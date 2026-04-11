// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: NFA-based multi-cycle SVA assertion evaluation
//
// Converts multi-cycle SVA sequence/property expressions into NFA graphs,
// then emits module-level 1-bit state registers driven by AstAlways blocks.
// Overlapping evaluations are naturally supported because new triggers OR
// into the start node each cycle while in-progress evaluations advance
// through different NFA nodes.
//
// Runs BEFORE V3AssertProp and V3AssertPre. Assertions converted here are
// replaced with combinational accept/reject checks, so V3AssertProp sees
// no multi-cycle SExpr.
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2005-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"

#include "V3AssertNfa.h"
#include "V3Const.h"
#include "V3UniqueNames.h"

#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// NFA Graph Data Structures

namespace {

struct SvaNfaNode final {
    int id;
    bool isAccept = false;
    bool isReject = false;
    // Throughout scope: if non-empty, this node represents an attempt that is
    // alive inside one or more `expr throughout seq` scopes. If any of these
    // exprs drop (sampled value false) while this node is active, the
    // evaluation MUST reject per IEEE 1800-2023 16.9.9 (seq cannot complete
    // because expr1 stopped holding). These are owned clones.
    std::vector<AstNodeExpr*> throughoutConds;
    // Counter FSM node: if `isCounter` is true, this node is not a plain
    // register but a 2-register FSM (active + counter). It represents a
    // [0:counterRange] wait window where the consequent can be checked at any
    // cycle in [0, counterRange]. Used by `##[M:N]` when N-M exceeds the
    // register-chain threshold; the M-cycle prefix is still handled by a
    // regular delay chain. Non-overlapping (single attempt in flight) --
    // acceptable tradeoff per spec 3.2.1, since the alternative (one register
    // per cycle) explodes for large N-M.
    bool isCounter = false;
    int counterMin = 0;  // Start of [min,max] match window
    int counterMax = 0;  // Inclusive end of window; reject fires here
    // Liveness terminal: reached via an unbounded wait (`##[M:$]`, `[*]`,
    // etc.). Attempts ending here never fail in finite simulation time, so
    // reject must NOT fire from this source (IEEE weak semantics). Only
    // accept/cover are meaningful.
    bool isUnbounded = false;
};

struct SvaNfaEdge final {
    int fromId;
    int toId;
    AstNodeExpr* condp = nullptr;  // nullptr = unconditional; OWNED by NFA
    bool consumesCycle;  // true = Edge (##1), false = Link (##0/boolean)
    // rejectOnFail: if the source state is active and condp is false,
    // contribute to the reject signal. Used for "required first step"
    // boolean checks where failure at that cycle terminates the attempt
    // immediately (not a window retry). Set only on the outermost
    // required-step Link of a sequence build; nested / merged / optional
    // Links must NOT set this.
    bool rejectOnFail = false;
};

struct SvaNfa final {
    int startNode = -1;
    int acceptNode = -1;
    int rejectNode = -1;
    std::vector<SvaNfaNode> nodes;
    std::vector<SvaNfaEdge> edges;

    ~SvaNfa() {
        // Free all owned condp expression trees
        for (auto& edge : edges) {
            if (edge.condp) {
                edge.condp->deleteTree();
                edge.condp = nullptr;
            }
        }
        // Free per-node throughout condition clones
        for (auto& node : nodes) {
            for (auto* cp : node.throughoutConds) cp->deleteTree();
            node.throughoutConds.clear();
        }
    }

    int createNode() {
        const int id = static_cast<int>(nodes.size());
        nodes.push_back(SvaNfaNode{id, false, false});
        return id;
    }
    int createAcceptNode() {
        const int id = createNode();
        nodes[id].isAccept = true;
        acceptNode = id;
        return id;
    }
    int createRejectNode() {
        const int id = createNode();
        nodes[id].isReject = true;
        rejectNode = id;
        return id;
    }
    void addClockEdge(int from, int to, AstNodeExpr* condp = nullptr) {
        edges.push_back(SvaNfaEdge{from, to, condp, true});
    }
    void addLink(int from, int to, AstNodeExpr* condp = nullptr) {
        edges.push_back(SvaNfaEdge{from, to, condp, false});
    }
};

//######################################################################
// Builder result: terminal node + optional final condition.
// The final condition is the consequent expression that should be checked
// at the terminal node. It goes on the accept Link, not as a separate node.

struct BuildResult final {
    int termNode;  // Primary terminal node; accept Link here contributes to
                   // BOTH accept and reject in Phase 3 (non-liveness sources).
    AstNodeExpr* finalCondp;  // Final condition for accept/reject (nullptr = unconditional)
    // Additional mid-window source nodes for range delays with a pure
    // boolean RHS. Each of these gets its own Link to accept, marked
    // isUnbounded so Phase 3 treats them as accept-only (they represent
    // intermediate cycles of the [M,N] window where a match is still
    // possible later if this cycle fails).
    std::vector<int> midSources;
    bool valid() const { return termNode >= 0; }
    static BuildResult fail() { return {-1, nullptr, {}}; }
};

//######################################################################
// NFA Builder

class SvaNfaBuilder final {
    SvaNfa& m_nfa;
    std::vector<AstNodeExpr*> m_throughoutStack;
    // Once an unbounded wait (`##[M:$]`, `[*]`, `[->N]`, `[+]`) has been
    // built, every subsequently created node inherits liveness: attempts
    // reaching them never fail in finite simulation time. Sticky across
    // the rest of the sequence build -- required to suppress spurious
    // reject on `a |-> ##[+] b ##1 X` style patterns where the inner
    // terminal is still "reachable only via a liveness path".
    bool m_inUnboundedScope = false;

    AstNodeExpr* throughoutCond(AstNodeExpr* baseCond, FileLine* flp) {
        if (m_throughoutStack.empty()) return baseCond;
        // AND all throughout conditions (supports nesting)
        // Each must use $sampled values per IEEE 16.9.9
        AstNodeExpr* guardp = nullptr;
        for (AstNodeExpr* const condp : m_throughoutStack) {
            AstNodeExpr* const clonep = sampled(condp->cloneTreePure(false));
            if (!guardp) {
                guardp = clonep;
            } else {
                guardp = new AstAnd{flp, guardp, clonep};
                guardp->dtypeSetBit();
            }
        }
        if (baseCond) {
            guardp = new AstAnd{flp, baseCond, guardp};
            guardp->dtypeSetBit();
        }
        return guardp;
    }

    static int getConstInt(AstNodeExpr* exprp) {
        AstNodeExpr* const constp = V3Const::constifyEdit(exprp->cloneTreePure(false));
        const AstConst* const cp = VN_CAST(constp, Const);
        const int val = cp ? cp->toSInt() : -1;
        VL_DO_DANGLING(constp->deleteTree(), constp);
        return val;
    }

    // Wrap expression in AstSampled for correct IEEE concurrent assertion semantics
    static AstNodeExpr* sampled(AstNodeExpr* exprp) {
        // All conditions in NFA use $sampled values
        AstSampled* const sp = new AstSampled{exprp->fileline(), exprp};
        sp->dtypeFrom(exprp);
        return sp;
    }

    // Create a new NFA node and record the current throughout scope on it.
    // Every node created inside a `expr throughout seq` scope inherits the
    // guard so the emitter can turn "state alive AND guard dropped" into an
    // explicit reject (IEEE 16.9.9: the evaluation must fail when expr1 stops
    // holding, even if seq never reaches its terminal).
    int scopedCreateNode() {
        const int id = m_nfa.createNode();
        auto& node = m_nfa.nodes[id];
        for (AstNodeExpr* const cp : m_throughoutStack) {
            node.throughoutConds.push_back(cp->cloneTreePure(false));
        }
        if (m_inUnboundedScope) node.isUnbounded = true;
        return id;
    }

    // All builder-level edges/links MUST go through these helpers so the
    // current throughout stack is always AND'd into the condition. This is the
    // single invariant that makes nested/complex throughout RHS work (IEEE
    // 16.9.9: the throughout expression is checked at every tick of the
    // guarded sequence, including zero-delay links).
    void guardedLink(int from, int to, AstNodeExpr* condp, FileLine* flp) {
        m_nfa.addLink(from, to, throughoutCond(condp, flp));
    }
    void guardedLink(int from, int to, FileLine* flp) {
        m_nfa.addLink(from, to, throughoutCond(nullptr, flp));
    }
    void guardedEdge(int from, int to, AstNodeExpr* condp, FileLine* flp) {
        m_nfa.addClockEdge(from, to, throughoutCond(condp, flp));
    }
    void guardedEdge(int from, int to, FileLine* flp) {
        m_nfa.addClockEdge(from, to, throughoutCond(nullptr, flp));
    }

    int addDelayChain(int startNode, int n, FileLine* flp) {
        int current = startNode;
        for (int i = 0; i < n; ++i) {
            const int next = scopedCreateNode();
            guardedEdge(current, next, flp);
            current = next;
        }
        return current;
    }

    // Build NFA for an SExpr. Returns {termNode, finalCond}.
    // The finalCond is the RHS expression -- NOT yet added as a node.
    // isTopLevelStep: when true, the outermost required boolean check
    // (the preExpr Link) is marked as rejectOnFail so that "trigger
    // fires, first boolean fails" fires a reject even though the attempt
    // never leaves the start state.
    BuildResult buildSExpr(AstSExpr* sexprp, int entryNode,
                           bool isTopLevelStep = false) {
        AstDelay* const delayp = VN_CAST(sexprp->delayp(), Delay);
        if (!delayp || !delayp->isCycleDelay()) return BuildResult::fail();

        FileLine* const flp = sexprp->fileline();

        // Handle LHS (preExpr)
        int currentNode = entryNode;
        if (AstNodeExpr* const preExprp = sexprp->preExprp()) {
            const BuildResult pre = buildExpr(preExprp, currentNode);
            if (!pre.valid()) return BuildResult::fail();
            // If pre has a final condition, add it as a conditioned Link
            if (pre.finalCondp) {
                const int condNode = scopedCreateNode();
                guardedLink(pre.termNode, condNode,
                            sampled(pre.finalCondp->cloneTreePure(false)), flp);
                if (isTopLevelStep
                    && !m_nfa.nodes[pre.termNode].isUnbounded
                    && !m_inUnboundedScope) {
                    // Mark Link as rejectOnFail: trigger fires AND this
                    // required boolean is false -> immediate reject. Must
                    // NOT mark when the source is already a liveness state
                    // (e.g. `(##[+] b) ##1 X` parses as outer SExpr with
                    // preExpr = nested unbounded SExpr; the "first boolean"
                    // check is deferred to when the liveness wait ends).
                    m_nfa.edges.back().rejectOnFail = true;
                }
                currentNode = condNode;
            } else {
                currentNode = pre.termNode;
            }
        }

        // Handle delay
        std::vector<int> rangeMidSources;
        if (delayp->isRangeDelay()) {
            const int minDelay = getConstInt(delayp->lhsp());
            if (minDelay < 0) return BuildResult::fail();

            if (delayp->isUnbounded()) {
                // `##[M:$]`: wait M cycles, then self-loop waiting for the
                // match condition. Unbounded = liveness, so no reject.
                currentNode = addDelayChain(currentNode, minDelay, flp);
                guardedEdge(currentNode, currentNode, flp);
                m_nfa.nodes[currentNode].isUnbounded = true;
                m_inUnboundedScope = true;
            } else {
                const int maxDelay = getConstInt(delayp->rhsp());
                if (maxDelay < minDelay) return BuildResult::fail();
                if (minDelay == maxDelay) {
                    currentNode = addDelayChain(currentNode, minDelay, flp);
                } else {
                    const int range = maxDelay - minDelay;
                    currentNode = addDelayChain(currentNode, minDelay, flp);
                    // Decide between register-chain and counter-FSM:
                    // - Pure boolean RHS or nested SExpr RHS: register chain
                    //   (preserves overlapping under set-based semantics by
                    //   linking each mid-position as accept-only).
                    // - Very large ranges (chain would blow up Phase 1's
                    //   fixed point): fall back to counter FSM.
                    constexpr int kChainLimit = 256;
                    AstNodeExpr* const exprp = sexprp->exprp();
                    const bool nestedRhs = VN_IS(exprp, SExpr);
                    if (range > kChainLimit) {
                        // Large range: counter FSM (single attempt).
                        const int counterNodeId = scopedCreateNode();
                        m_nfa.nodes[counterNodeId].isCounter = true;
                        m_nfa.nodes[counterNodeId].counterMin = 0;
                        m_nfa.nodes[counterNodeId].counterMax = range;
                        guardedEdge(currentNode, counterNodeId, flp);
                        currentNode = counterNodeId;
                    } else if (nestedRhs) {
                        // Nested: merge all positions into mergeNode; the
                        // continuation sees "attempt active at some position
                        // in [M,N]". Reject semantics of the nested terminal
                        // are correct because it is a later, per-attempt
                        // terminal (not the range merge).
                        const int mergeNode = scopedCreateNode();
                        guardedLink(currentNode, mergeNode, flp);
                        for (int i = 0; i < range; ++i) {
                            const int nextNode = scopedCreateNode();
                            guardedEdge(currentNode, nextNode, flp);
                            guardedLink(nextNode, mergeNode, flp);
                            currentNode = nextNode;
                        }
                        currentNode = mergeNode;
                    } else {
                        // Pure boolean RHS: build chain without merge. Each
                        // mid-position directly links to accept (marked
                        // isUnbounded = accept-only). The LAST position is
                        // the primary termNode (contributes to reject).
                        //
                        // Edge gating: every range-chain edge fires only
                        // when sampled(exprp) is FALSE at this cycle. This
                        // prevents attempts that have already matched from
                        // propagating further down the chain, so w_N
                        // represents "an attempt reached the LAST cycle
                        // without having matched earlier". Without this
                        // gating, an attempt that matched at position M
                        // would still appear in w_(M+1), ..., w_N, and
                        // reject would fire spuriously when the current
                        // boolean is false while the attempt has already
                        // been accepted.
                        rangeMidSources.push_back(currentNode);  // P_M
                        for (int i = 0; i < range; ++i) {
                            const int nextNode = scopedCreateNode();
                            AstNodeExpr* const notExprp = new AstNot{flp,
                                sampled(exprp->cloneTreePure(false))};
                            notExprp->dtypeSetBit();
                            guardedEdge(currentNode, nextNode, notExprp, flp);
                            if (i < range - 1) {
                                rangeMidSources.push_back(nextNode);
                            }
                            currentNode = nextNode;
                        }
                        // currentNode = last position = P_N, the reject
                        // source.
                    }
                }
            }
        } else {
            const int delayCycles = getConstInt(delayp->lhsp());
            if (delayCycles < 0) return BuildResult::fail();
            currentNode = addDelayChain(currentNode, delayCycles, flp);
        }

        // Handle RHS -- return it as finalCond, NOT as a Link node
        AstNodeExpr* const exprp = sexprp->exprp();
        if (AstSExpr* const rhsSExprp = VN_CAST(exprp, SExpr)) {
            // Nested SExpr: recurse (its finalCond becomes ours).
            // rangeMidSources should be empty here because we picked the
            // mergeNode path for nested RHS.
            return buildSExpr(rhsSExprp, currentNode);
        }
        // Simple boolean RHS: this is the final condition.
        return {currentNode, exprp, std::move(rangeMidSources)};
    }

    BuildResult buildConsRep(AstConsRep* repp, int entryNode) {
        FileLine* const flp = repp->fileline();
        AstNodeExpr* const exprp = repp->exprp();
        const int minN = getConstInt(repp->countp());
        if (minN < 0) return BuildResult::fail();

        int currentNode = entryNode;
        for (int i = 0; i < minN; ++i) {
            if (i > 0) {
                const int nextNode = scopedCreateNode();
                guardedEdge(currentNode, nextNode, flp);
                currentNode = nextNode;
            }
            // Add bool check as conditioned Link
            const int condNode = scopedCreateNode();
            guardedLink(currentNode, condNode,
                        sampled(exprp->cloneTreePure(false)), flp);
            currentNode = condNode;
        }

        if (repp->unbounded()) {
            if (minN == 0) {
                const int waitNode = scopedCreateNode();
                guardedEdge(currentNode, waitNode, flp);
                const int checkNode = scopedCreateNode();
                guardedLink(waitNode, checkNode,
                            sampled(exprp->cloneTreePure(false)), flp);
                guardedEdge(checkNode, waitNode, flp);
                guardedLink(currentNode, checkNode, flp);
                currentNode = checkNode;
            } else {
                const int loopBackNode = scopedCreateNode();
                guardedEdge(currentNode, loopBackNode, flp);
                const int reCheckNode = scopedCreateNode();
                guardedLink(loopBackNode, reCheckNode,
                            sampled(exprp->cloneTreePure(false)), flp);
                guardedEdge(reCheckNode, loopBackNode, flp);
                guardedLink(reCheckNode, currentNode, flp);
            }
            // Liveness terminal: the attempt can never explicitly fail in
            // bounded time.
            m_nfa.nodes[currentNode].isUnbounded = true;
            m_inUnboundedScope = true;
        } else if (repp->maxCountp()) {
            const int maxN = getConstInt(repp->maxCountp());
            if (maxN < minN) return BuildResult::fail();
            const int mergeNode = scopedCreateNode();
            guardedLink(currentNode, mergeNode, flp);
            for (int i = minN; i < maxN; ++i) {
                const int nextNode = scopedCreateNode();
                guardedEdge(currentNode, nextNode, flp);
                const int checkNode = scopedCreateNode();
                guardedLink(nextNode, checkNode,
                            sampled(exprp->cloneTreePure(false)), flp);
                guardedLink(checkNode, mergeNode, flp);
                currentNode = checkNode;
            }
            currentNode = mergeNode;
        }
        // finalCond = nullptr (already checked via Links)
        return {currentNode, nullptr};
    }

    BuildResult buildGotoRep(AstSGotoRep* repp, int entryNode) {
        FileLine* const flp = repp->fileline();
        AstNodeExpr* const exprp = repp->exprp();
        const int n = getConstInt(repp->countp());
        if (n <= 0) return BuildResult::fail();

        int currentNode = entryNode;
        for (int i = 0; i < n; ++i) {
            const int waitNode = scopedCreateNode();
            if (i == 0) {
                guardedLink(currentNode, waitNode, flp);
            } else {
                guardedEdge(currentNode, waitNode, flp);
            }
            AstNodeExpr* const notExprp
                = new AstNot{flp, exprp->cloneTreePure(false)};
            notExprp->dtypeSetBit();
            guardedEdge(waitNode, waitNode, sampled(notExprp), flp);
            const int matchNode = scopedCreateNode();
            guardedLink(waitNode, matchNode,
                        sampled(exprp->cloneTreePure(false)), flp);
            currentNode = matchNode;
        }
        // `[->N]` waits unboundedly for each match -- liveness terminal.
        m_nfa.nodes[currentNode].isUnbounded = true;
        m_inUnboundedScope = true;
        return {currentNode, nullptr};
    }

    BuildResult buildThroughout(AstSThroughout* nodep, int entryNode) {
        // The entryNode may have been created outside this throughout scope
        // (e.g. the antecedent trigNode for `a |-> (cond throughout seq)`).
        // Mark it with the guard so that "cond is false at tick 0 when the
        // antecedent fires" is detected as a throughout-drop reject.
        m_nfa.nodes[entryNode].throughoutConds.push_back(
            nodep->lhsp()->cloneTreePure(false));

        // Push the guard onto the throughout stack. Every builder-level
        // edge/link created while this stack is non-empty is automatically
        // AND'd with the guard via guardedLink/guardedEdge. This invariant
        // makes nested repetition/SOr/SAnd/throughout/intersect RHS work
        // without per-node special casing.
        m_throughoutStack.push_back(nodep->lhsp());
        const BuildResult result = buildExpr(nodep->rhsp(), entryNode);
        m_throughoutStack.pop_back();
        // finalCondp is a pointer to an AST-linked expression; the emitter
        // clones it when emitting the reject check. The accept edge already
        // has the throughout guard AND'd in via guardedLink, so the "seq
        // reaches accept" signal is properly guarded. Nothing extra to do.
        return result;
    }

public:
    explicit SvaNfaBuilder(SvaNfa& nfa)
        : m_nfa{nfa} {}

    // Build NFA for any expression node. Returns {termNode, finalCond}.
    // isTopLevelStep: see buildSExpr -- only the outermost call from
    // processAssertion / build() should pass true. Recursive calls into
    // nested SExprs, merges, etc. must pass false (default).
    BuildResult buildExpr(AstNodeExpr* nodep, int entryNode,
                          bool isTopLevelStep = false) {
        if (AstSExpr* const sexprp = VN_CAST(nodep, SExpr)) {
            return buildSExpr(sexprp, entryNode, isTopLevelStep);
        }
        if (AstConsRep* const repp = VN_CAST(nodep, ConsRep)) {
            return buildConsRep(repp, entryNode);
        }
        if (AstSGotoRep* const repp = VN_CAST(nodep, SGotoRep)) {
            return buildGotoRep(repp, entryNode);
        }
        if (VN_IS(nodep, SIntersect)) {
            // Intersect requires product-state construction -- defer to V3AssertProp
            return BuildResult::fail();
        }
        if (AstSThroughout* const throughoutp = VN_CAST(nodep, SThroughout)) {
            return buildThroughout(throughoutp, entryNode);
        }
        if (AstSOr* const orp = VN_CAST(nodep, SOr)) {
            FileLine* const flp = orp->fileline();
            const BuildResult lhs = buildExpr(orp->lhsp(), entryNode);
            const BuildResult rhs = buildExpr(orp->rhsp(), entryNode);
            if (!lhs.valid() || !rhs.valid()) return BuildResult::fail();
            const int mergeNode = scopedCreateNode();
            if (lhs.finalCondp) {
                guardedLink(lhs.termNode, mergeNode,
                            sampled(lhs.finalCondp->cloneTreePure(false)), flp);
            } else {
                guardedLink(lhs.termNode, mergeNode, flp);
            }
            if (rhs.finalCondp) {
                guardedLink(rhs.termNode, mergeNode,
                            sampled(rhs.finalCondp->cloneTreePure(false)), flp);
            } else {
                guardedLink(rhs.termNode, mergeNode, flp);
            }
            return {mergeNode, nullptr};
        }
        if (AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            FileLine* const flp = orp->fileline();
            const BuildResult lhs = buildExpr(orp->lhsp(), entryNode);
            const BuildResult rhs = buildExpr(orp->rhsp(), entryNode);
            if (!lhs.valid() || !rhs.valid()) return BuildResult::fail();
            const int mergeNode = scopedCreateNode();
            if (lhs.finalCondp) {
                guardedLink(lhs.termNode, mergeNode,
                            sampled(lhs.finalCondp->cloneTreePure(false)), flp);
            } else {
                guardedLink(lhs.termNode, mergeNode, flp);
            }
            if (rhs.finalCondp) {
                guardedLink(rhs.termNode, mergeNode,
                            sampled(rhs.finalCondp->cloneTreePure(false)), flp);
            } else {
                guardedLink(rhs.termNode, mergeNode, flp);
            }
            return {mergeNode, nullptr};
        }
        if (VN_IS(nodep, SAnd)) {
            // Temporal sequence AND requires parallel NFA with done-latch --
            // not yet implemented. Defer to V3AssertProp.
            return BuildResult::fail();
        }
        if (VN_IS(nodep, LogAnd)) {
            // Boolean AND: treat as leaf with the whole expr as finalCond
            return {entryNode, nodep};
        }
        // Default: boolean leaf -- return as finalCond
        return {entryNode, nodep};
    }

    // Build complete NFA for a standalone sequence.
    BuildResult build(AstNodeExpr* exprp) {
        m_nfa.startNode = scopedCreateNode();
        return buildExpr(exprp, m_nfa.startNode, /*isTopLevelStep=*/true);
    }
};

//######################################################################
// NFA Emitter

class SvaNfaEmitter final {
    AstNodeModule* const m_modp;
    V3UniqueNames m_names{"__Vnfa"};

    static AstNodeExpr* andCond(FileLine* flp, AstNodeExpr* exprp, AstNodeExpr* condp) {
        if (!condp) return exprp;
        AstNodeExpr* const result = new AstAnd{flp, exprp, condp->cloneTreePure(false)};
        result->dtypeSetBit();
        return result;
    }
    static AstNodeExpr* orExprs(FileLine* flp, AstNodeExpr* a, AstNodeExpr* b) {
        if (!a) return b;
        if (!b) return a;
        AstNodeExpr* const result = new AstOr{flp, a, b};
        result->dtypeSetBit();
        return result;
    }

public:
    explicit SvaNfaEmitter(AstNodeModule* modp)
        : m_modp{modp} {}

    // Emit NFA as hardware. Two-phase: Links combinational, Edges registered.
    // acceptCondp = condition on the accept Link (from BuildResult::finalCondp).
    // Returns !reject expression for assert, or accept expression for cover.
    AstNodeExpr* emit(FileLine* flp, const SvaNfa& nfa,
                      AstNodeExpr* triggerExprp, AstSenTree* senTreep,
                      AstNodeExpr* acceptCondp, bool isCover,
                      AstNodeExpr* disableExprp = nullptr) {
        const std::string baseName = m_names.get("");
        const int N = static_cast<int>(nfa.nodes.size());

        // Identify registered nodes (targets of Edges)
        std::vector<bool> needsReg(N, false);
        for (const auto& edge : nfa.edges) {
            if (edge.consumesCycle && edge.toId != nfa.acceptNode
                && edge.toId != nfa.rejectNode) {
                needsReg[edge.toId] = true;
            }
        }

        // Create state registers.
        // Counter FSM nodes get their own pair of registers (active + counter)
        // and are NOT treated as plain registered state bits -- their Phase 2
        // update is emitted as a separate always block further down.
        std::vector<AstVar*> stateVars(N, nullptr);
        std::vector<AstVar*> counterActiveVars(N, nullptr);
        std::vector<AstVar*> counterCountVars(N, nullptr);
        AstNodeDType* const u32DType = m_modp->findBasicDType(VBasicDTypeKwd::UINT32);
        for (int i = 0; i < N; ++i) {
            if (nfa.nodes[i].isCounter) {
                const std::string base = baseName + "__c" + std::to_string(i);
                AstVar* const activep = new AstVar{flp, VVarType::MODULETEMP,
                                                   base + "_active",
                                                   m_modp->findBitDType()};
                activep->lifetime(VLifetime::STATIC_EXPLICIT);
                m_modp->addStmtsp(activep);
                counterActiveVars[i] = activep;
                AstVar* const cntp = new AstVar{flp, VVarType::MODULETEMP,
                                                base + "_cnt", u32DType};
                cntp->lifetime(VLifetime::STATIC_EXPLICIT);
                m_modp->addStmtsp(cntp);
                counterCountVars[i] = cntp;
                continue;
            }
            if (!needsReg[i]) continue;
            if (i == nfa.startNode || nfa.nodes[i].isAccept || nfa.nodes[i].isReject) continue;
            const std::string varName = baseName + "__s" + std::to_string(i);
            AstVar* const varp = new AstVar{flp, VVarType::MODULETEMP, varName,
                                            m_modp->findBitDType()};
            varp->lifetime(VLifetime::STATIC_EXPLICIT);
            m_modp->addStmtsp(varp);
            stateVars[i] = varp;
        }

        // Phase 1: Resolve Links (combinational state_sig)
        std::vector<AstNodeExpr*> stateSig(N, nullptr);
        stateSig[nfa.startNode] = triggerExprp->cloneTreePure(false);
        for (int i = 0; i < N; ++i) {
            if (stateVars[i]) {
                stateSig[i] = new AstVarRef{flp, stateVars[i], VAccess::READ};
            } else if (counterActiveVars[i]) {
                // Counter FSM: state is "active && counter >= counterMin".
                // An in-window cycle means the match condition may satisfy
                // the sequence. counter < counterMin means we are waiting
                // for the window to open (still active, but not yet
                // acceptable).
                AstVarRef* const activeRefp
                    = new AstVarRef{flp, counterActiveVars[i], VAccess::READ};
                if (nfa.nodes[i].counterMin == 0) {
                    stateSig[i] = activeRefp;
                } else {
                    AstGte* const gtep = new AstGte{flp,
                        new AstVarRef{flp, counterCountVars[i], VAccess::READ},
                        new AstConst{flp, AstConst::WidthedValue{}, 32,
                                     static_cast<uint32_t>(nfa.nodes[i].counterMin)}};
                    gtep->dtypeSetBit();
                    AstNodeExpr* const andp
                        = new AstAnd{flp, activeRefp, gtep};
                    andp->dtypeSetBit();
                    stateSig[i] = andp;
                }
            }
        }

        for (int pass = 0; pass < N; ++pass) {
            bool changed = false;
            for (const auto& edge : nfa.edges) {
                if (edge.consumesCycle) continue;
                if (!stateSig[edge.fromId]) continue;
                if (nfa.nodes[edge.toId].isAccept || nfa.nodes[edge.toId].isReject) continue;

                AstNodeExpr* const srcSig = stateSig[edge.fromId]->cloneTreePure(false);
                AstNodeExpr* const contribution = andCond(flp, srcSig, edge.condp);

                if (!stateSig[edge.toId]) {
                    stateSig[edge.toId] = contribution;
                    changed = true;
                } else if (!needsReg[edge.toId]) {
                    stateSig[edge.toId] = orExprs(flp, stateSig[edge.toId], contribution);
                    changed = true;
                } else {
                    // Link targets a registered node -- contribution unused, free it
                    contribution->deleteTree();
                }
            }
            if (!changed) break;
        }

        // Phase 2: Compute Edge activations → NBA
        AstNode* bodyp = nullptr;
        for (int i = 0; i < N; ++i) {
            if (!stateVars[i]) continue;

            AstNodeExpr* nextStatep = nullptr;
            for (const auto& edge : nfa.edges) {
                if (edge.toId != i) continue;
                if (!edge.consumesCycle) continue;
                if (!stateSig[edge.fromId]) continue;

                AstNodeExpr* srcSig = stateSig[edge.fromId]->cloneTreePure(false);
                srcSig = andCond(flp, srcSig, edge.condp);

                if (disableExprp) {
                    AstNodeExpr* const notDisp
                        = new AstNot{flp, disableExprp->cloneTreePure(false)};
                    notDisp->dtypeSetBit();
                    srcSig = new AstAnd{flp, srcSig, notDisp};
                    srcSig->dtypeSetBit();
                }
                nextStatep = orExprs(flp, nextStatep, srcSig);
            }

            if (!nextStatep) nextStatep = new AstConst{flp, AstConst::BitFalse{}};

            AstAssignDly* const assignp = new AstAssignDly{
                flp, new AstVarRef{flp, stateVars[i], VAccess::WRITE}, nextStatep};
            if (!bodyp) {
                bodyp = assignp;
            } else {
                bodyp->addNext(assignp);
            }
        }

        if (bodyp) {
            AstAlways* const alwaysp = new AstAlways{
                flp, VAlwaysKwd::ALWAYS, senTreep->cloneTree(false), bodyp};
            m_modp->addStmtsp(alwaysp);
        }

        // Phase 2b: Counter FSM always block.
        // For each counter node, emit:
        //   if (active) begin
        //     if (accepted_now || counter == range) active <= 0;
        //     else counter <= counter + 1;
        //   end else if (incoming) begin
        //     active <= 1; counter <= 0;
        //   end
        // where accepted_now = stateSig[counter] && $sampled(acceptCondp)
        //   and incoming = OR of stateSig[source] && edge.condp && !disable
        //                  for every Edge into this counter node.
        for (int ci = 0; ci < N; ++ci) {
            if (!counterActiveVars[ci]) continue;
            AstVar* const activep = counterActiveVars[ci];
            AstVar* const cntp = counterCountVars[ci];
            const uint32_t counterMax
                = static_cast<uint32_t>(nfa.nodes[ci].counterMax);

            // incoming = OR of edge-driven contributions from upstream states
            AstNodeExpr* incomingp = nullptr;
            for (const auto& edge : nfa.edges) {
                if (edge.toId != ci) continue;
                if (!edge.consumesCycle) continue;
                if (!stateSig[edge.fromId]) continue;
                AstNodeExpr* contrib
                    = stateSig[edge.fromId]->cloneTreePure(false);
                contrib = andCond(flp, contrib, edge.condp);
                if (disableExprp) {
                    AstNodeExpr* const notDisp
                        = new AstNot{flp, disableExprp->cloneTreePure(false)};
                    notDisp->dtypeSetBit();
                    contrib = new AstAnd{flp, contrib, notDisp};
                    contrib->dtypeSetBit();
                }
                incomingp = orExprs(flp, incomingp, contrib);
            }
            if (!incomingp) incomingp = new AstConst{flp, AstConst::BitFalse{}};

            // in_window = counter >= counterMin (active is implicit inside
            // the outer "if (active)" branch).
            AstNodeExpr* inWindowp = nullptr;
            if (nfa.nodes[ci].counterMin == 0) {
                inWindowp = new AstConst{flp, AstConst::BitTrue{}};
            } else {
                inWindowp = new AstGte{flp,
                    new AstVarRef{flp, cntp, VAccess::READ},
                    new AstConst{flp, AstConst::WidthedValue{}, 32,
                                 static_cast<uint32_t>(nfa.nodes[ci].counterMin)}};
                inWindowp->dtypeSetBit();
            }
            // accepted_now = in_window && $sampled(acceptCondp)
            AstNodeExpr* acceptedNowp = nullptr;
            if (acceptCondp) {
                AstSampled* const sampp
                    = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
                sampp->dtypeSetBit();
                acceptedNowp = new AstAnd{flp, inWindowp, sampp};
                acceptedNowp->dtypeSetBit();
            } else {
                acceptedNowp = inWindowp;
            }

            // counter_at_end = counter == counterMax
            AstNodeExpr* const counterAtEndp = new AstEq{flp,
                new AstVarRef{flp, cntp, VAccess::READ},
                new AstConst{flp, AstConst::WidthedValue{}, 32, counterMax}};
            counterAtEndp->dtypeSetBit();

            // done = accepted_now || counter == counterMax
            AstNodeExpr* const donep = new AstOr{flp, acceptedNowp, counterAtEndp};
            donep->dtypeSetBit();

            // then-branch: active <= 0
            AstAssignDly* const clearActivep = new AstAssignDly{flp,
                new AstVarRef{flp, activep, VAccess::WRITE},
                new AstConst{flp, AstConst::BitFalse{}}};
            // else-branch: counter <= counter + 1
            AstAdd* const addExprp = new AstAdd{flp,
                new AstVarRef{flp, cntp, VAccess::READ},
                new AstConst{flp, AstConst::WidthedValue{}, 32, 1u}};
            addExprp->dtypeFrom(cntp);
            AstAssignDly* const incCountp = new AstAssignDly{flp,
                new AstVarRef{flp, cntp, VAccess::WRITE}, addExprp};
            AstIf* const doneIfp
                = new AstIf{flp, donep, clearActivep, incCountp};

            // if (active) { doneIfp } else if (incoming) { active<=1; cnt<=0; }
            AstAssignDly* const setActivep = new AstAssignDly{flp,
                new AstVarRef{flp, activep, VAccess::WRITE},
                new AstConst{flp, AstConst::BitTrue{}}};
            AstAssignDly* const resetCountp = new AstAssignDly{flp,
                new AstVarRef{flp, cntp, VAccess::WRITE},
                new AstConst{flp, AstConst::WidthedValue{}, 32, 0u}};
            setActivep->addNext(resetCountp);
            AstIf* const startIfp
                = new AstIf{flp, incomingp, setActivep, nullptr};
            AstIf* const topIfp = new AstIf{flp,
                new AstVarRef{flp, activep, VAccess::READ},
                doneIfp, startIfp};

            AstAlways* const counterAlwaysp = new AstAlways{flp,
                VAlwaysKwd::ALWAYS, senTreep->cloneTree(false), topIfp};
            m_modp->addStmtsp(counterAlwaysp);
        }

        // Phase 3: Compute accept/reject from Links to accept node + acceptCondp
        // The accept node receives Links from terminal NFA nodes.
        // acceptCondp is the final boolean check from the builder.
        //
        // terminal_active = OR of (state_sig[source] && link_condition) for Links to accept
        // accept = terminal_active && acceptCondp
        // reject = terminal_active && !acceptCondp

        // terminalActivep: for accept/cover (any source contributes).
        // rejectBasep:     for reject (counter sources contribute only at
        //                  window end, not every in-window cycle).
        AstNodeExpr* terminalActivep = nullptr;
        AstNodeExpr* rejectBasep = nullptr;
        for (const auto& edge : nfa.edges) {
            if (edge.toId != nfa.acceptNode) continue;
            if (edge.consumesCycle) continue;
            if (!stateSig[edge.fromId]) continue;

            AstNodeExpr* srcSig = stateSig[edge.fromId]->cloneTreePure(false);
            srcSig = andCond(flp, srcSig, edge.condp);

            if (nfa.nodes[edge.fromId].isCounter) {
                const int ci = edge.fromId;
                // Accept: use srcSig as-is (active && link_cond)
                terminalActivep = orExprs(flp, terminalActivep,
                                          srcSig->cloneTreePure(false));
                // Reject base: srcSig && (counter == counterRange)
                AstNodeExpr* const atEndp = new AstEq{flp,
                    new AstVarRef{flp, counterCountVars[ci], VAccess::READ},
                    new AstConst{flp, AstConst::WidthedValue{}, 32,
                                 static_cast<uint32_t>(nfa.nodes[ci].counterMax)}};
                atEndp->dtypeSetBit();
                AstNodeExpr* const expireContribp
                    = new AstAnd{flp, srcSig, atEndp};
                expireContribp->dtypeSetBit();
                rejectBasep = orExprs(flp, rejectBasep, expireContribp);
            } else if (nfa.nodes[edge.fromId].isUnbounded) {
                // Liveness terminal: contributes to accept/cover only.
                // Reject never fires (IEEE weak semantics).
                terminalActivep
                    = orExprs(flp, terminalActivep, srcSig);
            } else {
                terminalActivep = orExprs(flp, terminalActivep,
                                          srcSig->cloneTreePure(false));
                rejectBasep = orExprs(flp, rejectBasep, srcSig);
            }
        }

        // If no Links to accept, check stateSig at the last registered node
        // that connects directly (this handles standalone sequences)
        if (!terminalActivep) {
            // Find the highest-numbered registered node
            for (int i = N - 1; i >= 0; --i) {
                if (stateVars[i] && stateSig[i]) {
                    terminalActivep = stateSig[i]->cloneTreePure(false);
                    break;
                }
            }
        }
        if (!terminalActivep) {
            terminalActivep = new AstConst{flp, AstConst::BitFalse{}};
        }

        // Phase 3a: "required-step" rejection.
        // For every Link marked rejectOnFail, reject fires when the source
        // state is active but the link condition is false. This catches
        // failures like `a |-> b ##...` where the antecedent fires but the
        // consequent's first boolean (b) is false -- the attempt never
        // leaves the start state, so no later terminal-based reject can
        // ever fire for it.
        AstNodeExpr* requiredStepRejectp = nullptr;
        for (const auto& edge : nfa.edges) {
            if (!edge.rejectOnFail) continue;
            if (edge.consumesCycle) continue;
            if (!stateSig[edge.fromId]) continue;
            if (!edge.condp) continue;
            AstNodeExpr* const srcSig
                = stateSig[edge.fromId]->cloneTreePure(false);
            AstNodeExpr* const notCondp
                = new AstNot{flp, edge.condp->cloneTreePure(false)};
            notCondp->dtypeSetBit();
            AstNodeExpr* const failp = new AstAnd{flp, srcSig, notCondp};
            failp->dtypeSetBit();
            requiredStepRejectp
                = orExprs(flp, requiredStepRejectp, failp);
        }

        // Phase 3b: Throughout-drop rejection.
        // For every node that was created inside a `expr throughout seq` scope,
        // emit fail_i = state_active[i] && !$sampled(AND of throughout exprs).
        // If any such fail fires, the evaluation MUST reject per IEEE 16.9.9 --
        // the seq can never reach its terminal because the guarded expression
        // stopped holding.
        // Uses registered state bit when available, else the combinational
        // stateSig (covers nodes reached only via Links).
        AstNodeExpr* throughoutRejectp = nullptr;
        for (int i = 0; i < N; ++i) {
            const auto& conds = nfa.nodes[i].throughoutConds;
            if (conds.empty()) continue;
            AstNodeExpr* stateExprp = nullptr;
            if (stateVars[i]) {
                stateExprp = new AstVarRef{flp, stateVars[i], VAccess::READ};
            } else if (stateSig[i]) {
                stateExprp = stateSig[i]->cloneTreePure(false);
            } else {
                continue;
            }
            AstNodeExpr* guardp = nullptr;
            for (AstNodeExpr* const cp : conds) {
                AstSampled* const sp = new AstSampled{flp, cp->cloneTreePure(false)};
                sp->dtypeSetBit();
                if (!guardp) {
                    guardp = sp;
                } else {
                    guardp = new AstAnd{flp, guardp, sp};
                    guardp->dtypeSetBit();
                }
            }
            AstNodeExpr* const notGuardp = new AstNot{flp, guardp};
            notGuardp->dtypeSetBit();
            AstNodeExpr* const failp = new AstAnd{flp, stateExprp, notGuardp};
            failp->dtypeSetBit();
            throughoutRejectp = orExprs(flp, throughoutRejectp, failp);
        }

        // Clean up ALL stateSig entries -- they've been cloned where needed
        // (Phase 2/3 always clone from stateSig before incorporating into AST)
        for (int i = 0; i < N; ++i) {
            if (stateSig[i]) {
                stateSig[i]->deleteTree();
                stateSig[i] = nullptr;
            }
        }
        // disable iff applied to throughout-drop and required-step rejects:
        // either kind of failure during disable is abandoned per IEEE 16.12.
        if (disableExprp) {
            if (throughoutRejectp) {
                AstNodeExpr* const notDisp
                    = new AstNot{flp, disableExprp->cloneTreePure(false)};
                notDisp->dtypeSetBit();
                throughoutRejectp = new AstAnd{flp, throughoutRejectp, notDisp};
                throughoutRejectp->dtypeSetBit();
            }
            if (requiredStepRejectp) {
                AstNodeExpr* const notDisp
                    = new AstNot{flp, disableExprp->cloneTreePure(false)};
                notDisp->dtypeSetBit();
                requiredStepRejectp
                    = new AstAnd{flp, requiredStepRejectp, notDisp};
                requiredStepRejectp->dtypeSetBit();
            }
        }

        // Clean up disableExprp if passed (was cloned in Phase 2, original not attached)
        if (disableExprp) {
            disableExprp->deleteTree();
            disableExprp = nullptr;
        }

        if (isCover) {
            // Cover property uses accept signal, not !reject. Reject-style
            // contributions are unused.
            if (throughoutRejectp) throughoutRejectp->deleteTree();
            if (rejectBasep) rejectBasep->deleteTree();
            if (requiredStepRejectp) requiredStepRejectp->deleteTree();
            if (acceptCondp) {
                AstNodeExpr* const sampledCondp
                    = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
                sampledCondp->dtypeFrom(acceptCondp);
                AstNodeExpr* const acceptp
                    = new AstAnd{flp, terminalActivep, sampledCondp};
                acceptp->dtypeSetBit();
                return acceptp;
            }
            return terminalActivep;
        }

        // Assert/assume: output = !reject
        // Base reject: a source whose reject window is "now" didn't match.
        // For non-counter sources that's "any cycle the attempt reaches the
        // accept Link"; for counter sources that's "window's last cycle".
        AstNodeExpr* rejectp = nullptr;
        if (acceptCondp && rejectBasep) {
            AstNodeExpr* const sampledCondp
                = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
            sampledCondp->dtypeFrom(acceptCondp);
            AstNodeExpr* const notCondp = new AstNot{flp, sampledCondp};
            notCondp->dtypeSetBit();
            rejectp = new AstAnd{flp, rejectBasep, notCondp};
            rejectp->dtypeSetBit();
        } else if (rejectBasep) {
            rejectBasep->deleteTree();
        }
        if (terminalActivep) terminalActivep->deleteTree();  // cover branch exited earlier

        // OR in throughout-drop reject
        if (throughoutRejectp) {
            rejectp = orExprs(flp, rejectp, throughoutRejectp);
            if (rejectp) rejectp->dtypeSetBit();
        }
        // OR in required-step reject
        if (requiredStepRejectp) {
            rejectp = orExprs(flp, rejectp, requiredStepRejectp);
            if (rejectp) rejectp->dtypeSetBit();
        }

        if (!rejectp) {
            // No condition: unconditional accept -> never rejects
            return new AstConst{flp, AstConst::BitTrue{}};
        }
        AstNodeExpr* const resultExprp = new AstNot{flp, rejectp};
        resultExprp->dtypeSetBit();
        return resultExprp;
    }
};

}  // namespace

//######################################################################
// Top-level visitor

class AssertNfaVisitor final : public VNVisitor {
    AstNodeModule* m_modp = nullptr;
    SvaNfaEmitter* m_emitterp = nullptr;

    static bool hasMultiCycleExpr(const AstNode* nodep) {
        bool found = false;
        nodep->foreach([&found](const AstSExpr*) { found = true; });
        if (!found) nodep->foreach([&found](const AstConsRep*) { found = true; });
        if (!found) nodep->foreach([&found](const AstSGotoRep*) { found = true; });
        if (!found) nodep->foreach([&found](const AstSIntersect*) { found = true; });
        if (!found) nodep->foreach([&found](const AstSThroughout*) { found = true; });
        if (!found) nodep->foreach([&found](const AstSAnd*) { found = true; });
        if (!found) nodep->foreach([&found](const AstSOr*) { found = true; });
        return found;
    }

    struct PropertyParts final {
        AstNodeExpr* triggerExprp = nullptr;
        AstNodeExpr* seqExprp = nullptr;
        bool isOverlapped = true;
        bool hasImplication = false;
    };

    static PropertyParts decomposeProperty(AstNode* propp) {
        PropertyParts parts;
        if (AstPropSpec* const specp = VN_CAST(propp, PropSpec)) {
            propp = specp->propp();
        }
        if (AstImplication* const implp = VN_CAST(propp, Implication)) {
            parts.hasImplication = true;
            parts.isOverlapped = implp->isOverlapped();
            parts.triggerExprp = implp->lhsp();
            parts.seqExprp = implp->rhsp();
        } else if (AstNodeExpr* const exprp = VN_CAST(propp, NodeExpr)) {
            parts.triggerExprp = nullptr;
            parts.seqExprp = exprp;
        }
        return parts;
    }

    void processAssertion(AstNodeCoverOrAssert* assertp) {
        if (assertp->immediate()) return;
        AstNode* const propp = assertp->propp();
        if (!hasMultiCycleExpr(propp)) return;

        const PropertyParts parts = decomposeProperty(propp);
        if (!parts.seqExprp) return;

        AstSenTree* senTreep = assertp->sentreep();
        bool senTreeOwned = false;  // True if we created senTreep locally
        AstPropSpec* const propSpecp = VN_CAST(assertp->propp(), PropSpec);
        AstNodeExpr* disableExprp = nullptr;
        if (propSpecp) {
            if (!senTreep && propSpecp->sensesp()) {
                senTreep = new AstSenTree{propSpecp->fileline(),
                                          propSpecp->sensesp()->cloneTree(true)};
                senTreeOwned = true;
            }
            disableExprp = propSpecp->disablep();
        }
        if (!senTreep) return;

        FileLine* const flp = assertp->fileline();
        const bool isCover = VN_IS(assertp, Cover);

        // Build NFA
        SvaNfa nfa;
        SvaNfaBuilder builder{nfa};

        BuildResult result = BuildResult::fail();
        if (parts.hasImplication) {
            nfa.startNode = nfa.createNode();

            if (parts.isOverlapped) {
                const int trigNode = nfa.createNode();
                // Sampled antecedent
                nfa.addLink(nfa.startNode, trigNode,
                            new AstSampled{flp, parts.triggerExprp->cloneTreePure(false)});
                result = builder.buildExpr(parts.seqExprp, trigNode,
                                           /*isTopLevelStep=*/true);
            } else {
                const int trigNode = nfa.createNode();
                nfa.addLink(nfa.startNode, trigNode,
                            new AstSampled{flp, parts.triggerExprp->cloneTreePure(false)});
                const int delayNode = nfa.createNode();
                nfa.addClockEdge(trigNode, delayNode);
                result = builder.buildExpr(parts.seqExprp, delayNode,
                                           /*isTopLevelStep=*/true);
            }

            if (result.valid()) {
                nfa.createAcceptNode();
                // Accept Link is unconditional. The finalCond is passed
                // separately to the emitter for accept/reject computation.
                nfa.addLink(result.termNode, nfa.acceptNode);
                // Range-delay mid-window positions: accept-only (Phase 3
                // skips reject contribution because we mark isUnbounded).
                for (int src : result.midSources) {
                    nfa.addLink(src, nfa.acceptNode);
                    nfa.nodes[src].isUnbounded = true;
                }
            }
        } else {
            // Standalone sequence
            result = builder.build(parts.seqExprp);
            if (result.valid()) {
                nfa.createAcceptNode();
                nfa.addLink(result.termNode, nfa.acceptNode);
                for (int src : result.midSources) {
                    nfa.addLink(src, nfa.acceptNode);
                    nfa.nodes[src].isUnbounded = true;
                }
            }
        }

        if (!result.valid()) {
            // NFA cannot handle this assertion -- emit UNSUPPORTED rather than
            // silently passing it through (there is no fallback anymore).
            assertp->v3warn(E_UNSUPPORTED,
                            "Unsupported: assertion contains SVA construct not yet"
                            " supported by NFA engine (e.g. intersect, sequence and,"
                            " complex throughout)");
            return;
        }

        // Emit NFA hardware
        AstNodeExpr* const alwaysTriggerp = new AstConst{flp, AstConst::BitTrue{}};
        AstNodeExpr* const outputExprp
            = m_emitterp->emit(flp, nfa, alwaysTriggerp, senTreep,
                               result.finalCondp, isCover,
                               disableExprp ? disableExprp->cloneTreePure(false)
                                            : nullptr);

        // Clean up locally-owned temporaries (emit cloned them)
        alwaysTriggerp->deleteTree();
        if (senTreeOwned) senTreep->deleteTree();

        // Replace inner property with output expression
        if (propSpecp) {
            AstNode* const innerPropp = propSpecp->propp();
            innerPropp->replaceWith(outputExprp);
            VL_DO_DANGLING(pushDeletep(innerPropp), innerPropp);
        } else {
            AstNode* const oldPropp = assertp->propp();
            oldPropp->replaceWith(outputExprp);
            VL_DO_DANGLING(pushDeletep(oldPropp), oldPropp);
        }

        UINFO(4, "NFA converted assertion at " << flp << ", "
                  << nfa.nodes.size() << " nodes, "
                  << nfa.edges.size() << " edges" << endl);
    }

    // VISITORS
    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        VL_RESTORER(m_emitterp);
        m_modp = nodep;
        SvaNfaEmitter emitter{nodep};
        m_emitterp = &emitter;
        iterateChildren(nodep);
    }
    void visit(AstAssert* nodep) override { processAssertion(nodep); }
    void visit(AstCover* nodep) override { processAssertion(nodep); }
    void visit(AstAssertIntrinsic* nodep) override {}
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit AssertNfaVisitor(AstNetlist* nodep) { iterate(nodep); }
};

//######################################################################
// Top entry point

void V3AssertNfa::assertNfaAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":" << endl);
    { AssertNfaVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("assertnfa", 0, dumpTreeEitherLevel() >= 3);
}
