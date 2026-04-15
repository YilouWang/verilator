// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: NFA-based multi-cycle SVA assertion evaluation
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
// V3AssertNfa's Transformations:
//
//  - Convert multi-cycle SVA sequences/properties into NFA graphs.
//  - Emit module-level state registers driven by AstAlways blocks.
//  - Replace converted assertions with combinational accept/reject checks
//    so V3AssertProp/V3AssertPre see no multi-cycle SExpr.
//
//*************************************************************************

#include "V3PchAstNoMT.h"

#include "V3AssertNfa.h"

#include "V3Const.h"
#include "V3Task.h"
#include "V3UniqueNames.h"

#include <set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// NFA Graph Data Structures

namespace {

struct SvaNfaNode final {
    int id;
    bool isAccept = false;
    // Throughout scope: if non-empty, this node represents an attempt that is
    // alive inside one or more `expr throughout seq` scopes. If any of these
    // exprs drop (sampled value false) while this node is active, the
    // evaluation MUST reject per IEEE 1800-2023 16.9.9 (seq cannot complete
    // because expr1 stopped holding). These are owned clones.
    std::vector<AstNodeExpr*> throughoutConds;
    // Counter FSM node: represents a ##[M:N] wait window when N-M > kChainLimit
    bool isCounter = false;
    int counterMin = 0;  // Start of [min,max] match window
    int counterMax = 0;  // Inclusive end of window; reject fires here
    // Liveness terminal: reached via an unbounded wait (`##[M:$]`, `[*]`,
    // etc.). Attempts ending here never fail in finite simulation time, so
    // reject must NOT fire from this source (IEEE weak semantics). Only
    // accept/cover are meaningful.
    bool isUnbounded = false;
    // Temporal sequence AND combiner per IEEE 1800-2023 16.9.5
    bool isAndCombiner = false;
    int andLhsTermId = -1;
    int andRhsTermId = -1;
    AstNodeExpr* andLhsCondp = nullptr;  // OWNED; may be null
    AstNodeExpr* andRhsCondp = nullptr;  // OWNED; may be null
    // Reject sink for SAnd rejectOnFail wiring; not a stateSig source
    bool isRejectSink = false;
};

struct SvaNfaEdge final {
    int fromId;
    int toId;
    AstNodeExpr* condp = nullptr;  // nullptr = unconditional; OWNED by NFA
    bool consumesCycle;  // true = Edge (##1), false = Link (##0/boolean)
    // Reject when source is active and condp is false. Set only on the
    // outermost required-step Link; nested / optional Links must not set this.
    bool rejectOnFail = false;
};

struct SvaNfa final {
    int startNode = -1;
    int acceptNode = -1;
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
        // Free per-node throughout condition clones and SAnd combiner operands
        for (auto& node : nodes) {
            for (auto* cp : node.throughoutConds) cp->deleteTree();
            node.throughoutConds.clear();
            if (node.andLhsCondp) {
                node.andLhsCondp->deleteTree();
                node.andLhsCondp = nullptr;
            }
            if (node.andRhsCondp) {
                node.andRhsCondp->deleteTree();
                node.andRhsCondp = nullptr;
            }
        }
    }

    int createNode() {
        const int id = static_cast<int>(nodes.size());
        SvaNfaNode node;
        node.id = id;
        nodes.push_back(std::move(node));
        return id;
    }
    int createAcceptNode() {
        const int id = createNode();
        nodes[id].isAccept = true;
        acceptNode = id;
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
    // Set when the builder already emitted a specific v3error for this
    // failure. processAssertion should skip the generic UNSUPPORTED message.
    bool errorEmitted = false;
    bool valid() const { return termNode >= 0; }
    static BuildResult fail(bool errored = false) { return {-1, nullptr, {}, errored}; }
    static BuildResult failWithError() { return {-1, nullptr, {}, true}; }
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

    AstNodeExpr* throughoutCond(AstNodeExpr* baseCondp, FileLine* flp) {
        if (m_throughoutStack.empty()) return baseCondp;
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
        if (baseCondp) {
            guardp = new AstAnd{flp, baseCondp, guardp};
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

    // Static fixed-length analysis for an SVA sub-expression. Returns the
    // number of clock ticks the expression consumes from entry to terminal,
    // or -1 if the length is not statically determined (variable / unbounded
    // / unsupported construct). Used by SIntersect lowering to verify the
    // IEEE 1800-2023 16.9.6 equal-length precondition before reusing the
    // SAnd combiner.
    //
    // Supported:
    //  - Boolean leaf (default)              -> 0
    //  - AstSExpr with fixed cycle delay     -> pre + N + body
    //  - AstSExpr with range delay M==N      -> pre + N + body
    //  - AstSThroughout                      -> length of rhsp (the seq)
    // Unsupported (returns -1):
    //  - Range delays with M != N            -> variable
    //  - Unbounded waits ##[M:$]             -> infinite/variable
    //  - ConsRep / SGotoRep / SAnd / SOr     -> defer (rare in intersect)
    //  - SIntersect nested in SIntersect     -> defer
    static int fixedLength(AstNodeExpr* nodep) {
        if (!nodep) return 0;
        if (AstSExpr* const sexprp = VN_CAST(nodep, SExpr)) {
            AstDelay* const delayp = VN_CAST(sexprp->delayp(), Delay);
            if (!delayp || !delayp->isCycleDelay()) return -1;
            int delayCycles = -1;
            if (delayp->isRangeDelay()) {
                if (delayp->isUnbounded()) return -1;
                const int minD = getConstInt(delayp->lhsp());
                const int maxD = getConstInt(delayp->rhsp());
                if (minD < 0 || maxD < 0 || minD != maxD) return -1;
                delayCycles = minD;
            } else {
                delayCycles = getConstInt(delayp->lhsp());
                if (delayCycles < 0) return -1;
            }
            int preLen = 0;
            if (AstNodeExpr* const prep = sexprp->preExprp()) {
                preLen = fixedLength(prep);
                if (preLen < 0) return -1;
            }
            const int bodyLen = fixedLength(sexprp->exprp());
            if (bodyLen < 0) return -1;
            return preLen + delayCycles + bodyLen;
        }
        if (AstSThroughout* const throughp = VN_CAST(nodep, SThroughout)) {
            return fixedLength(throughp->rhsp());
        }
        if (VN_IS(nodep, SConsRep) || VN_IS(nodep, SGotoRep) || VN_IS(nodep, SAnd)
            || VN_IS(nodep, SOr) || VN_IS(nodep, SIntersect)) {
            // Conservatively variable -- can be tightened in a follow-up.
            return -1;
        }
        // Plain boolean expression (no SVA constructs) -- 0 cycles.
        return 0;
    }

    static AstNodeExpr* sampled(AstNodeExpr* exprp) {
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
    BuildResult buildSExpr(AstSExpr* sexprp, int entryNode, bool isTopLevelStep = false) {
        AstDelay* const delayp = VN_CAST(sexprp->delayp(), Delay);
        if (!delayp || !delayp->isCycleDelay()) return BuildResult::fail();

        FileLine* const flp = sexprp->fileline();

        // Handle LHS (preExpr)
        int currentNode = entryNode;
        if (AstNodeExpr* const preExprp = sexprp->preExprp()) {
            const BuildResult pre = buildExpr(preExprp, currentNode, isTopLevelStep);
            if (!pre.valid()) return BuildResult::fail(pre.errorEmitted);
            // If pre has a final condition, add it as a conditioned Link
            if (pre.finalCondp) {
                const int condNode = scopedCreateNode();
                guardedLink(pre.termNode, condNode, sampled(pre.finalCondp->cloneTreePure(false)),
                            flp);
                if (isTopLevelStep && !m_nfa.nodes[pre.termNode].isUnbounded
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
            if (minDelay < 0) {
                delayp->v3error("Range delay minimum is not a non-negative"
                                " elaboration-time constant"
                                " (IEEE 1800-2023 16.7)");
                return BuildResult::failWithError();
            }

            if (delayp->isUnbounded()) {
                // `##[M:$]`: wait M cycles, then self-loop waiting for the
                // match condition. Unbounded = liveness, so no reject.
                currentNode = addDelayChain(currentNode, minDelay, flp);
                guardedEdge(currentNode, currentNode, flp);
                m_nfa.nodes[currentNode].isUnbounded = true;
                m_inUnboundedScope = true;
            } else {
                const int maxDelay = getConstInt(delayp->rhsp());
                if (maxDelay < 0) {
                    delayp->v3error("Range delay maximum is not a non-negative"
                                    " elaboration-time constant"
                                    " (IEEE 1800-2023 16.7)");
                    return BuildResult::failWithError();
                }
                if (maxDelay < minDelay) {
                    delayp->v3error("Range delay maximum must be >= minimum"
                                    " (IEEE 1800-2023 16.7)");
                    return BuildResult::failWithError();
                }
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
                            AstNodeExpr* const notExprp
                                = new AstNot{flp, sampled(exprp->cloneTreePure(false))};
                            notExprp->dtypeSetBit();
                            guardedEdge(currentNode, nextNode, notExprp, flp);
                            if (i < range - 1) { rangeMidSources.push_back(nextNode); }
                            currentNode = nextNode;
                        }
                        // currentNode = last position = P_N, the reject
                        // source.
                    }
                }
            }
        } else {
            const int delayCycles = getConstInt(delayp->lhsp());
            if (delayCycles < 0) {
                delayp->v3error("Delay value is not a non-negative"
                                " elaboration-time constant"
                                " (IEEE 1800-2023 16.7)");
                return BuildResult::failWithError();
            }
            currentNode = addDelayChain(currentNode, delayCycles, flp);
        }

        // Handle RHS. If it is itself a multi-cycle construct (nested
        // SExpr, SAnd, SOr, ConsRep, etc.) recurse via buildExpr so the
        // construct is properly compiled into the NFA. Only a *plain*
        // boolean leaf is returned as finalCondp -- otherwise the emitter
        // would try to use the multi-cycle subtree as a sampled boolean,
        // which produces invalid AST and silently broken assertions.
        AstNodeExpr* const exprp = sexprp->exprp();
        if (VN_IS(exprp, SExpr) || VN_IS(exprp, SAnd) || VN_IS(exprp, SOr)
            || VN_IS(exprp, SConsRep) || VN_IS(exprp, SGotoRep) || VN_IS(exprp, SThroughout)
            || VN_IS(exprp, SIntersect) || VN_IS(exprp, SNonConsRep)) {
            // rangeMidSources should be empty here because the chain path
            // is only taken for pure-boolean RHS.
            // Pass isTopLevelStep through so that required-step boolean checks
            // inside nested RHS sequences (e.g. A ##1 (B ##1 C)) are correctly
            // marked as rejectOnFail when this is a top-level evaluation.
            return buildExpr(exprp, currentNode, isTopLevelStep);
        }
        // Simple boolean RHS: this is the final condition.
        return {currentNode, exprp, std::move(rangeMidSources)};
    }

    BuildResult buildConsRep(AstSConsRep* repp, int entryNode, bool isTopLevelStep = false) {
        FileLine* const flp = repp->fileline();
        AstNodeExpr* const exprp = repp->exprp();
        // Multi-cycle sequence expression as the repeated element is not yet
        // supported (e.g. `(a ##2 b) [*3]`). The NFA builder would need to
        // recursively build a sub-NFA for exprp and compose it N times, which
        // requires a clone-and-rewire pass that is not yet implemented.
        // Treating it as a boolean via sampled() produces invalid AST and
        // silently broken assertions, so bail early.
        if (VN_IS(exprp, SExpr) || VN_IS(exprp, SThroughout) || VN_IS(exprp, SAnd)
            || VN_IS(exprp, SOr) || VN_IS(exprp, SConsRep) || VN_IS(exprp, SGotoRep)
            || VN_IS(exprp, SIntersect) || VN_IS(exprp, SNonConsRep)) {
            repp->v3warn(E_UNSUPPORTED, "Unsupported: multi-cycle sequence expression inside"
                                        " consecutive repetition (IEEE 1800-2023 16.9.2)");
            return BuildResult::failWithError();
        }
        const int minN = getConstInt(repp->countp());
        if (minN < 0) return BuildResult::fail();
        // Guard against excessively large exact repetitions that would
        // create O(N) nodes and O(N^2) Phase 1 fixed-point iterations.
        // A counter-based FSM for ConsRep is a future enhancement
        // (unlike range delay counters, ConsRep counters need "expr must
        // hold every cycle" semantics). For now, bail on large N.
        constexpr int kConsRepLimit = 256;
        if (minN > kConsRepLimit && !repp->unbounded() && !repp->maxCountp()) {
            return BuildResult::fail();
        }

        int currentNode = entryNode;
        for (int i = 0; i < minN; ++i) {
            if (i > 0) {
                const int nextNode = scopedCreateNode();
                guardedEdge(currentNode, nextNode, flp);
                currentNode = nextNode;
            }
            // Add bool check as conditioned Link.
            // Mark first and last boolean Links as rejectOnFail so that
            // standalone ConsRep (no implication wrapper) generates correct
            // reject signals:
            //   first Link: "expr false at start -> immediate fail"
            //   last Link:  "partial match, expr false at final step -> fail"
            // For ConsRep inside an implication antecedent, the rejects are
            // harmless (they fire in addition to the consequent reject).
            const int condNode = scopedCreateNode();
            guardedLink(currentNode, condNode, sampled(exprp->cloneTreePure(false)), flp);
            if (isTopLevelStep && (i == 0 || i == minN - 1)) {
                m_nfa.edges.back().rejectOnFail = true;
            }
            currentNode = condNode;
        }

        if (repp->unbounded()) {
            if (minN == 0) {
                const int waitNode = scopedCreateNode();
                guardedEdge(currentNode, waitNode, flp);
                const int checkNode = scopedCreateNode();
                guardedLink(waitNode, checkNode, sampled(exprp->cloneTreePure(false)), flp);
                guardedEdge(checkNode, waitNode, flp);
                guardedLink(currentNode, checkNode, flp);
                currentNode = checkNode;
            } else {
                const int loopBackNode = scopedCreateNode();
                guardedEdge(currentNode, loopBackNode, flp);
                const int reCheckNode = scopedCreateNode();
                guardedLink(loopBackNode, reCheckNode, sampled(exprp->cloneTreePure(false)), flp);
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
                guardedLink(nextNode, checkNode, sampled(exprp->cloneTreePure(false)), flp);
                guardedLink(checkNode, mergeNode, flp);
                currentNode = checkNode;
            }
            currentNode = mergeNode;
        }
        // finalCond = nullptr (already checked via Links)
        return {currentNode, nullptr, {}};
    }

    BuildResult buildGotoRep(AstSGotoRep* repp, int entryNode) {
        FileLine* const flp = repp->fileline();
        AstNodeExpr* const exprp = repp->exprp();
        const int n = getConstInt(repp->countp());
        if (n <= 0) return BuildResult::fail();

        int currentNode = entryNode;
        for (int i = 0; i < n; ++i) {
            const int waitNode = scopedCreateNode();
            // Edge (consume 1 cycle) for ALL iterations including the
            // first. The IEEE expansion `(!expr [*0:$]) ##1 expr` has
            // a `##1` before each match check. Using a Link for i==0
            // was incorrect: (a) it allowed same-cycle matching (too
            // early), and (b) the Link contribution was discarded in
            // Phase 2 because waitNode is registered (self-loop Edge),
            // so the goto rep NFA never activated.
            guardedEdge(currentNode, waitNode, flp);
            AstNodeExpr* const notExprp = new AstNot{flp, exprp->cloneTreePure(false)};
            notExprp->dtypeSetBit();
            guardedEdge(waitNode, waitNode, sampled(notExprp), flp);
            const int matchNode = scopedCreateNode();
            guardedLink(waitNode, matchNode, sampled(exprp->cloneTreePure(false)), flp);
            currentNode = matchNode;
        }
        // `[->N]` waits unboundedly for each match -- liveness terminal.
        m_nfa.nodes[currentNode].isUnbounded = true;
        m_inUnboundedScope = true;
        return {currentNode, nullptr, {}};
    }

    // Shared sub-routine for SAnd and (lowered) SIntersect: build two
    // disjoint sub-NFAs from the same entry node and aggregate via a
    // done-latch combiner (IEEE 1800-2023 16.9.5).
    BuildResult buildAndCombiner(AstNodeExpr* lhsExprp, AstNodeExpr* rhsExprp, int entryNode,
                                 FileLine* flp) {
        // Snapshot-restore m_inUnboundedScope around each sub-build so an
        // LHS liveness wait does not spuriously mark RHS nodes as unbounded
        // (and vice versa). The combiner inherits liveness only if at
        // least one side's terminal is unbounded.
        const bool savedScope = m_inUnboundedScope;
        const BuildResult lhs = buildExpr(lhsExprp, entryNode);
        const bool lhsScope = m_inUnboundedScope;
        m_inUnboundedScope = savedScope;
        const BuildResult rhs = buildExpr(rhsExprp, entryNode);
        const bool rhsScope = m_inUnboundedScope;
        m_inUnboundedScope = savedScope || lhsScope || rhsScope;
        if (!lhs.valid() || !rhs.valid()) {
            return BuildResult::fail(lhs.errorEmitted || rhs.errorEmitted);
        }

        // Both operands are single-cycle (termNode == entryNode): use a
        // simple boolean AND.  The done-latch combiner incorrectly
        // persists a previous cycle's result and would fire when the two
        // operands were true on DIFFERENT cycles.
        if (lhs.termNode == entryNode && rhs.termNode == entryNode) {
            AstNodeExpr* condp = nullptr;
            if (lhs.finalCondp && rhs.finalCondp) {
                condp = new AstAnd{flp, lhs.finalCondp->cloneTreePure(false),
                                   rhs.finalCondp->cloneTreePure(false)};
                condp->dtypeSetBit();
            } else if (lhs.finalCondp) {
                condp = lhs.finalCondp;
            } else {
                condp = rhs.finalCondp;
            }
            return {entryNode, condp, {}};
        }
        // Range-delay mid-window sources in either sub-branch would need
        // to be folded into the latch's "accept-now" signal, which the
        // current combiner does not support. Defer (UNSUPPORTED).
        if (!lhs.midSources.empty() || !rhs.midSources.empty()) { return BuildResult::fail(); }
        const int combNode = scopedCreateNode();
        SvaNfaNode& cn = m_nfa.nodes[combNode];
        cn.isAndCombiner = true;
        cn.andLhsTermId = lhs.termNode;
        cn.andRhsTermId = rhs.termNode;
        if (lhs.finalCondp) { cn.andLhsCondp = lhs.finalCondp->cloneTreePure(false); }
        if (rhs.finalCondp) { cn.andRhsCondp = rhs.finalCondp->cloneTreePure(false); }
        // Liveness propagates: if either sub-sequence is liveness-only
        // (unbounded wait), the whole AND is liveness-only too.
        if (m_nfa.nodes[lhs.termNode].isUnbounded || m_nfa.nodes[rhs.termNode].isUnbounded) {
            cn.isUnbounded = true;
        }
        // Wire sub-sequence terminal-boolean rejects so each side can
        // fail the whole AND independently when its required terminal
        // expression drops. The combiner is accept-only, so without
        // these explicit links the sub-sequence "reached terminal but
        // boolean false" case would be silently swallowed. The Links
        // target a dedicated reject sink so that Phase 1 propagation
        // does not pollute the combiner's accept formula.
        //
        // Skip rejectOnFail wiring entirely when either sub-side is
        // liveness-only -- a liveness sub-sequence is never required to
        // terminate, so failing its boolean check is not a definitive
        // SAnd failure.
        // Wire sub-sequence rejectOnFail only for multi-cycle sub-
        // sequences (termNode != entryNode). For single-cycle boolean
        // operands the termNode IS the entryNode (often the always-active
        // start node), so a rejectOnFail Link would fire on every cycle
        // where the boolean is false -- producing spurious rejects when
        // the combiner is used as an antecedent (IEEE vacuous pass).
        if (!cn.isUnbounded) {
            bool needSink = false;
            const bool lhsMultiCycle = (lhs.termNode != entryNode);
            const bool rhsMultiCycle = (rhs.termNode != entryNode);
            if (lhs.finalCondp && lhsMultiCycle && !m_nfa.nodes[lhs.termNode].isUnbounded) {
                needSink = true;
            }
            if (rhs.finalCondp && rhsMultiCycle && !m_nfa.nodes[rhs.termNode].isUnbounded) {
                needSink = true;
            }
            if (needSink) {
                const int sinkNode = m_nfa.createNode();
                m_nfa.nodes[sinkNode].isRejectSink = true;
                if (lhs.finalCondp && lhsMultiCycle && !m_nfa.nodes[lhs.termNode].isUnbounded) {
                    m_nfa.addLink(lhs.termNode, sinkNode,
                                  sampled(lhs.finalCondp->cloneTreePure(false)));
                    m_nfa.edges.back().rejectOnFail = true;
                }
                if (rhs.finalCondp && rhsMultiCycle && !m_nfa.nodes[rhs.termNode].isUnbounded) {
                    m_nfa.addLink(rhs.termNode, sinkNode,
                                  sampled(rhs.finalCondp->cloneTreePure(false)));
                    m_nfa.edges.back().rejectOnFail = true;
                }
            }
        }
        return {combNode, nullptr, {}};
    }

    BuildResult buildThroughout(AstSThroughout* nodep, int entryNode,
                                bool isTopLevelStep = false) {
        // The entryNode may have been created outside this throughout scope
        // (e.g. the antecedent trigNode for `a |-> (cond throughout seq)`).
        // Mark it with the guard so that "cond is false at tick 0 when the
        // antecedent fires" is detected as a throughout-drop reject.
        m_nfa.nodes[entryNode].throughoutConds.push_back(nodep->lhsp()->cloneTreePure(false));

        // Push the guard onto the throughout stack. Every builder-level
        // edge/link created while this stack is non-empty is automatically
        // AND'd with the guard via guardedLink/guardedEdge. This invariant
        // makes nested repetition/SOr/SAnd/throughout/intersect RHS work
        // without per-node special casing.
        m_throughoutStack.push_back(nodep->lhsp());
        // Pass isTopLevelStep so that the outermost required boolean check
        // inside the throughout body (e.g. preExpr of b ##[1:2] c) is
        // marked rejectOnFail. Without this, a trigger that fires when
        // the first boolean is false would silently not reject.
        const BuildResult result = buildExpr(nodep->rhsp(), entryNode, isTopLevelStep);
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

    // Reset builder scope between antecedent and consequent builds so that
    // liveness from the antecedent (e.g. `a[+]`, `a[->N]`) does not leak
    // into the consequent. Consequent nodes must start with a clean scope
    // because "reaching the antecedent terminal" is a definitive event --
    // the consequent MUST be checked (not deferred by liveness).
    void resetScope() {
        m_inUnboundedScope = false;
        // throughout stack should already be empty between top-level builds,
        // but clear it defensively.
        m_throughoutStack.clear();
    }

    // Build NFA for any expression node. Returns {termNode, finalCond}.
    // isTopLevelStep: see buildSExpr -- only the outermost call from
    // processAssertion / build() should pass true. Recursive calls into
    // nested SExprs, merges, etc. must pass false (default).
    BuildResult buildExpr(AstNodeExpr* nodep, int entryNode, bool isTopLevelStep = false) {
        if (AstSExpr* const sexprp = VN_CAST(nodep, SExpr)) {
            return buildSExpr(sexprp, entryNode, isTopLevelStep);
        }
        if (AstSConsRep* const repp = VN_CAST(nodep, SConsRep)) {
            return buildConsRep(repp, entryNode, isTopLevelStep);
        }
        if (AstSGotoRep* const repp = VN_CAST(nodep, SGotoRep)) {
            return buildGotoRep(repp, entryNode);
        }
        if (AstSThroughout* const throughoutp = VN_CAST(nodep, SThroughout)) {
            return buildThroughout(throughoutp, entryNode, isTopLevelStep);
        }
        if (AstSOr* const orp = VN_CAST(nodep, SOr)) {
            FileLine* const flp = orp->fileline();
            const BuildResult lhs = buildExpr(orp->lhsp(), entryNode);
            const BuildResult rhs = buildExpr(orp->rhsp(), entryNode);
            if (!lhs.valid() || !rhs.valid()) {
                return BuildResult::fail(lhs.errorEmitted || rhs.errorEmitted);
            }
            const int mergeNode = scopedCreateNode();
            if (lhs.finalCondp) {
                guardedLink(lhs.termNode, mergeNode, sampled(lhs.finalCondp->cloneTreePure(false)),
                            flp);
            } else {
                guardedLink(lhs.termNode, mergeNode, flp);
            }
            if (rhs.finalCondp) {
                guardedLink(rhs.termNode, mergeNode, sampled(rhs.finalCondp->cloneTreePure(false)),
                            flp);
            } else {
                guardedLink(rhs.termNode, mergeNode, flp);
            }
            return {mergeNode, nullptr, {}};
        }
        if (AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            FileLine* const flp = orp->fileline();
            const BuildResult lhs = buildExpr(orp->lhsp(), entryNode);
            const BuildResult rhs = buildExpr(orp->rhsp(), entryNode);
            if (!lhs.valid() || !rhs.valid()) {
                return BuildResult::fail(lhs.errorEmitted || rhs.errorEmitted);
            }
            const int mergeNode = scopedCreateNode();
            if (lhs.finalCondp) {
                guardedLink(lhs.termNode, mergeNode, sampled(lhs.finalCondp->cloneTreePure(false)),
                            flp);
            } else {
                guardedLink(lhs.termNode, mergeNode, flp);
            }
            if (rhs.finalCondp) {
                guardedLink(rhs.termNode, mergeNode, sampled(rhs.finalCondp->cloneTreePure(false)),
                            flp);
            } else {
                guardedLink(rhs.termNode, mergeNode, flp);
            }
            return {mergeNode, nullptr, {}};
        }
        if (AstSAnd* const andp = VN_CAST(nodep, SAnd)) {
            return buildAndCombiner(andp->lhsp(), andp->rhsp(), entryNode, andp->fileline());
        }
        if (AstSIntersect* const intp = VN_CAST(nodep, SIntersect)) {
            // IEEE 1800-2023 16.9.6: SAnd with the additional constraint
            // that both operands match with EQUAL length. For
            // statically-fixed equal-length sub-sequences this is identical
            // to SAnd because both terminate at the same cycle. Variable
            // length intersect requires per-attempt cycle counters
            // (IEEE 1800-2023 16.9.6) which is left for a follow-up.
            const int lhsLen = fixedLength(intp->lhsp());
            const int rhsLen = fixedLength(intp->rhsp());
            if (lhsLen < 0 || rhsLen < 0) return BuildResult::fail();
            if (lhsLen != rhsLen) {
                intp->v3error("Intersect sequence length mismatch: left " + std::to_string(lhsLen)
                              + " cycles, right " + std::to_string(rhsLen)
                              + " cycles (IEEE 1800-2023 16.9.6)");
                return BuildResult::failWithError();
            }
            return buildAndCombiner(intp->lhsp(), intp->rhsp(), entryNode, intp->fileline());
        }
        if (VN_IS(nodep, SNonConsRep)) {
            // Nonconsecutive repetition [=N] is not yet implemented in the
            // NFA engine. Return fail so the caller emits UNSUPPORTED.
            return BuildResult::fail();
        }
        if (VN_IS(nodep, LogAnd)) {
            // Boolean AND: treat as leaf with the whole expr as finalCond
            return {entryNode, nodep, {}};
        }
        // Default: boolean leaf -- return as finalCond
        return {entryNode, nodep, {}};
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
        AstNodeExpr* const resultp = new AstAnd{flp, exprp, condp->cloneTreePure(false)};
        resultp->dtypeSetBit();
        return resultp;
    }
    static AstNodeExpr* orExprs(FileLine* flp, AstNodeExpr* ap, AstNodeExpr* bp) {
        if (!ap) return bp;
        if (!bp) return ap;
        AstNodeExpr* const resultp = new AstOr{flp, ap, bp};
        resultp->dtypeSetBit();
        return resultp;
    }

public:
    explicit SvaNfaEmitter(AstNodeModule* modp)
        : m_modp{modp} {}

    // Emit NFA as hardware. Two-phase: Links combinational, Edges registered.
    // acceptCondp = condition on the accept Link (from BuildResult::finalCondp).
    // Returns !reject expression for assert, or accept expression for cover.
    // outAcceptpp: if non-null, stores the accept expression (caller owns).
    AstNodeExpr* emit(FileLine* flp, const SvaNfa& nfa, AstNodeExpr* triggerExprp,
                      AstSenTree* senTreep, AstNodeExpr* acceptCondp, bool isCover,
                      AstNodeExpr* disableExprp = nullptr, bool negated = false,
                      AstNodeExpr** outAcceptpp = nullptr, AstVar* disableCntVarp = nullptr,
                      AstVar* snapshotVarp = nullptr,
                      std::vector<AstNodeExpr*>* outRequiredStepSrcsp = nullptr) {
        const std::string baseName = m_names.get("");
        const int N = static_cast<int>(nfa.nodes.size());

        // Identify registered nodes (targets of Edges)
        std::vector<bool> needsReg(N, false);
        for (const auto& edge : nfa.edges) {
            if (edge.consumesCycle && edge.toId != nfa.acceptNode
                && !nfa.nodes[edge.toId].isRejectSink) {
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
        // SAnd combiner: per-combiner done-latch register pair.
        std::vector<AstVar*> doneLVars(N, nullptr);
        std::vector<AstVar*> doneRVars(N, nullptr);
        AstNodeDType* const u32DTypep = m_modp->findBasicDType(VBasicDTypeKwd::UINT32);
        for (int i = 0; i < N; ++i) {
            if (nfa.nodes[i].isAndCombiner) {
                const std::string base = baseName + "__a" + std::to_string(i);
                AstVar* const lp = new AstVar{flp, VVarType::MODULETEMP, base + "_doneL",
                                              m_modp->findBitDType()};
                lp->lifetime(VLifetime::STATIC_EXPLICIT);
                m_modp->addStmtsp(lp);
                doneLVars[i] = lp;
                AstVar* const rp = new AstVar{flp, VVarType::MODULETEMP, base + "_doneR",
                                              m_modp->findBitDType()};
                rp->lifetime(VLifetime::STATIC_EXPLICIT);
                m_modp->addStmtsp(rp);
                doneRVars[i] = rp;
                continue;
            }
            if (nfa.nodes[i].isCounter) {
                const std::string base = baseName + "__c" + std::to_string(i);
                AstVar* const activep = new AstVar{flp, VVarType::MODULETEMP, base + "_active",
                                                   m_modp->findBitDType()};
                activep->lifetime(VLifetime::STATIC_EXPLICIT);
                m_modp->addStmtsp(activep);
                counterActiveVars[i] = activep;
                AstVar* const cntp
                    = new AstVar{flp, VVarType::MODULETEMP, base + "_cnt", u32DTypep};
                cntp->lifetime(VLifetime::STATIC_EXPLICIT);
                m_modp->addStmtsp(cntp);
                counterCountVars[i] = cntp;
                continue;
            }
            if (!needsReg[i]) continue;
            if (i == nfa.startNode || nfa.nodes[i].isAccept) continue;
            const std::string varName = baseName + "__s" + std::to_string(i);
            AstVar* const varp
                = new AstVar{flp, VVarType::MODULETEMP, varName, m_modp->findBitDType()};
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
                    AstGte* const gtep
                        = new AstGte{flp, new AstVarRef{flp, counterCountVars[i], VAccess::READ},
                                     new AstConst{flp, AstConst::WidthedValue{}, 32,
                                                  static_cast<uint32_t>(nfa.nodes[i].counterMin)}};
                    gtep->dtypeSetBit();
                    AstNodeExpr* const andp = new AstAnd{flp, activeRefp, gtep};
                    andp->dtypeSetBit();
                    stateSig[i] = andp;
                }
            }
        }

        // Helper: build `state && $sampled(cond)` (cond may be null, in which
        // case returns just the state ref clone). Used by SAnd combiner to
        // derive per-side "accept this cycle" signals.
        auto buildAcceptNow = [flp](AstNodeExpr* stateExprp, AstNodeExpr* condp) -> AstNodeExpr* {
            AstNodeExpr* const statep = stateExprp->cloneTreePure(false);
            if (!condp) return statep;
            AstSampled* const sampp = new AstSampled{flp, condp->cloneTreePure(false)};
            sampp->dtypeSetBit();
            AstNodeExpr* const andp = new AstAnd{flp, statep, sampp};
            andp->dtypeSetBit();
            return andp;
        };

        for (int pass = 0; pass < 2 * N + 2; ++pass) {
            bool changed = false;
            // Try to seed any SAnd combiner whose sub-NFAs are now ready.
            // This must happen inside the Phase 1 fixed-point because the
            // combiner itself is a Link source (downstream Links and the
            // accept Link read stateSig[combNode]) and, dually, the
            // sub-NFA termNodes may themselves be Link-propagated (not
            // registered), so their stateSigs only become available after
            // at least one propagation pass.
            for (int i = 0; i < N; ++i) {
                const auto& node = nfa.nodes[i];
                if (!node.isAndCombiner) continue;
                if (stateSig[i]) continue;
                const int l = node.andLhsTermId;
                const int r = node.andRhsTermId;
                if (l < 0 || r < 0) continue;
                if (!stateSig[l] || !stateSig[r]) continue;

                AstNodeExpr* const acceptLforOrp = buildAcceptNow(stateSig[l], node.andLhsCondp);
                AstNodeExpr* const acceptRforOrp = buildAcceptNow(stateSig[r], node.andRhsCondp);
                AstNodeExpr* const acceptLforOnep = buildAcceptNow(stateSig[l], node.andLhsCondp);
                AstNodeExpr* const acceptRforOnep = buildAcceptNow(stateSig[r], node.andRhsCondp);

                AstNodeExpr* const doneLRefp = new AstVarRef{flp, doneLVars[i], VAccess::READ};
                AstNodeExpr* const doneLOrp = new AstOr{flp, doneLRefp, acceptLforOrp};
                doneLOrp->dtypeSetBit();
                AstNodeExpr* const doneRRefp = new AstVarRef{flp, doneRVars[i], VAccess::READ};
                AstNodeExpr* const doneRightOrp = new AstOr{flp, doneRRefp, acceptRforOrp};
                doneRightOrp->dtypeSetBit();
                AstNodeExpr* const bothDonep = new AstAnd{flp, doneLOrp, doneRightOrp};
                bothDonep->dtypeSetBit();
                AstNodeExpr* const oneNowp = new AstOr{flp, acceptLforOnep, acceptRforOnep};
                oneNowp->dtypeSetBit();
                AstNodeExpr* const acceptp = new AstAnd{flp, bothDonep, oneNowp};
                acceptp->dtypeSetBit();
                stateSig[i] = acceptp;
                changed = true;
            }

            for (const auto& edge : nfa.edges) {
                if (edge.consumesCycle) continue;
                if (!stateSig[edge.fromId]) continue;
                if (nfa.nodes[edge.toId].isAccept) continue;
                // Reject sinks exist only as Phase 3a rejectOnFail targets;
                // they have no "active" semantics and must not appear in
                // stateSig propagation.
                if (nfa.nodes[edge.toId].isRejectSink) continue;

                AstNodeExpr* const srcSigp = stateSig[edge.fromId]->cloneTreePure(false);
                AstNodeExpr* const contributionp = andCond(flp, srcSigp, edge.condp);

                if (!stateSig[edge.toId]) {
                    stateSig[edge.toId] = contributionp;
                    changed = true;
                } else if (!needsReg[edge.toId]) {
                    stateSig[edge.toId] = orExprs(flp, stateSig[edge.toId], contributionp);
                    changed = true;
                } else {
                    // Link targets a registered node -- contribution unused, free it
                    contributionp->deleteTree();
                }
            }
            if (!changed) break;
        }

        // Phase 2: Compute Edge activations -> NBA
        AstNode* bodyp = nullptr;
        for (int i = 0; i < N; ++i) {
            if (!stateVars[i]) continue;

            AstNodeExpr* nextStatep = nullptr;
            for (const auto& edge : nfa.edges) {
                if (edge.toId != i) continue;
                if (!edge.consumesCycle) continue;
                if (!stateSig[edge.fromId]) continue;

                AstNodeExpr* srcSigp = stateSig[edge.fromId]->cloneTreePure(false);
                srcSigp = andCond(flp, srcSigp, edge.condp);

                // Gate state register NBA with !disable unless the snapshot
                // mechanism is active (which handles disable iff timing
                // correctly via disableCnt comparison at the terminal).
                if (disableExprp && !snapshotVarp) {
                    AstNodeExpr* const notDisp
                        = new AstNot{flp, disableExprp->cloneTreePure(false)};
                    notDisp->dtypeSetBit();
                    srcSigp = new AstAnd{flp, srcSigp, notDisp};
                    srcSigp->dtypeSetBit();
                }
                nextStatep = orExprs(flp, nextStatep, srcSigp);
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
            // If using the snapshot mechanism, capture the disableCnt at
            // each posedge in the same Phase-2 always block. This captures
            // the counter BEFORE any reactive re-evaluation triggered by
            // signal changes in this clock's NBA round (e.g., ++cyc in
            // always @(clk) updating the disable expression). The terminal
            // check then compares snapshotVarp vs disableCntVarp to decide
            // whether disable fired during the evaluation.
            if (snapshotVarp && disableCntVarp) {
                AstAssignDly* const snapAssignp
                    = new AstAssignDly{flp, new AstVarRef{flp, snapshotVarp, VAccess::WRITE},
                                       new AstVarRef{flp, disableCntVarp, VAccess::READ}};
                bodyp->addNext(snapAssignp);
            }
            AstAlways* const alwaysp
                = new AstAlways{flp, VAlwaysKwd::ALWAYS, senTreep->cloneTree(false), bodyp};
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
            const uint32_t counterMax = static_cast<uint32_t>(nfa.nodes[ci].counterMax);

            // incoming = OR of edge-driven contributions from upstream states
            AstNodeExpr* incomingp = nullptr;
            for (const auto& edge : nfa.edges) {
                if (edge.toId != ci) continue;
                if (!edge.consumesCycle) continue;
                if (!stateSig[edge.fromId]) continue;
                AstNodeExpr* contribp = stateSig[edge.fromId]->cloneTreePure(false);
                contribp = andCond(flp, contribp, edge.condp);
                if (disableExprp) {
                    AstNodeExpr* const notDisp
                        = new AstNot{flp, disableExprp->cloneTreePure(false)};
                    notDisp->dtypeSetBit();
                    contribp = new AstAnd{flp, contribp, notDisp};
                    contribp->dtypeSetBit();
                }
                incomingp = orExprs(flp, incomingp, contribp);
            }
            if (!incomingp) incomingp = new AstConst{flp, AstConst::BitFalse{}};

            // in_window = counter >= counterMin (active is implicit inside
            // the outer "if (active)" branch).
            AstNodeExpr* inWindowp = nullptr;
            if (nfa.nodes[ci].counterMin == 0) {
                inWindowp = new AstConst{flp, AstConst::BitTrue{}};
            } else {
                inWindowp
                    = new AstGte{flp, new AstVarRef{flp, cntp, VAccess::READ},
                                 new AstConst{flp, AstConst::WidthedValue{}, 32,
                                              static_cast<uint32_t>(nfa.nodes[ci].counterMin)}};
                inWindowp->dtypeSetBit();
            }
            // accepted_now = in_window && $sampled(acceptCondp)
            AstNodeExpr* acceptedNowp = nullptr;
            if (acceptCondp) {
                AstSampled* const sampp = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
                sampp->dtypeSetBit();
                acceptedNowp = new AstAnd{flp, inWindowp, sampp};
                acceptedNowp->dtypeSetBit();
            } else {
                acceptedNowp = inWindowp;
            }

            // counter_at_end = counter == counterMax
            AstNodeExpr* const counterAtEndp
                = new AstEq{flp, new AstVarRef{flp, cntp, VAccess::READ},
                            new AstConst{flp, AstConst::WidthedValue{}, 32, counterMax}};
            counterAtEndp->dtypeSetBit();

            // done = accepted_now || counter == counterMax
            AstNodeExpr* const donep = new AstOr{flp, acceptedNowp, counterAtEndp};
            donep->dtypeSetBit();

            // then-branch: active <= 0
            AstAssignDly* const clearActivep
                = new AstAssignDly{flp, new AstVarRef{flp, activep, VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitFalse{}}};
            // else-branch: counter <= counter + 1
            AstAdd* const addExprp
                = new AstAdd{flp, new AstVarRef{flp, cntp, VAccess::READ},
                             new AstConst{flp, AstConst::WidthedValue{}, 32, 1u}};
            addExprp->dtypeFrom(cntp);
            AstAssignDly* const incCountp
                = new AstAssignDly{flp, new AstVarRef{flp, cntp, VAccess::WRITE}, addExprp};
            AstIf* const doneIfp = new AstIf{flp, donep, clearActivep, incCountp};

            // if (active) { doneIfp } else if (incoming) { active<=1; cnt<=0; }
            AstAssignDly* const setActivep
                = new AstAssignDly{flp, new AstVarRef{flp, activep, VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitTrue{}}};
            AstAssignDly* const resetCountp
                = new AstAssignDly{flp, new AstVarRef{flp, cntp, VAccess::WRITE},
                                   new AstConst{flp, AstConst::WidthedValue{}, 32, 0u}};
            setActivep->addNext(resetCountp);
            AstIf* const startIfp = new AstIf{flp, incomingp, setActivep, nullptr};
            AstIf* const topIfp
                = new AstIf{flp, new AstVarRef{flp, activep, VAccess::READ}, doneIfp, startIfp};

            AstAlways* const counterAlwaysp
                = new AstAlways{flp, VAlwaysKwd::ALWAYS, senTreep->cloneTree(false), topIfp};
            m_modp->addStmtsp(counterAlwaysp);
        }

        // Phase 2c: SAnd combiner done-latch always block.
        // For each combiner i, emit:
        //   always @(senTree) begin
        //     if (stateSig[i])               // accept fires this cycle
        //       begin doneL <= 0; doneR <= 0; end
        //     else begin
        //       if (acceptLNow && !disable) doneL <= 1;
        //       if (acceptRNow && !disable) doneR <= 1;
        //     end
        //   end
        // NBA semantics ensure the read of doneL/doneR inside stateSig[i]
        // uses pre-update values, matching IEEE 16.9.5 end-time semantics.
        for (int ai = 0; ai < N; ++ai) {
            if (!doneLVars[ai]) continue;
            const auto& node = nfa.nodes[ai];
            const int l = node.andLhsTermId;
            const int r = node.andRhsTermId;
            if (!stateSig[l] || !stateSig[r] || !stateSig[ai]) continue;

            AstAssignDly* const clearLp
                = new AstAssignDly{flp, new AstVarRef{flp, doneLVars[ai], VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitFalse{}}};
            AstAssignDly* const clearRp
                = new AstAssignDly{flp, new AstVarRef{flp, doneRVars[ai], VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitFalse{}}};
            clearLp->addNext(clearRp);

            AstNodeExpr* const acceptLNowp = buildAcceptNow(stateSig[l], node.andLhsCondp);
            AstNodeExpr* const acceptRNowp = buildAcceptNow(stateSig[r], node.andRhsCondp);
            AstNodeExpr* gateLp = acceptLNowp;
            AstNodeExpr* gateRp = acceptRNowp;
            if (disableExprp) {
                AstNodeExpr* const notDisLp = new AstNot{flp, disableExprp->cloneTreePure(false)};
                notDisLp->dtypeSetBit();
                gateLp = new AstAnd{flp, gateLp, notDisLp};
                gateLp->dtypeSetBit();
                AstNodeExpr* const notDisRp = new AstNot{flp, disableExprp->cloneTreePure(false)};
                notDisRp->dtypeSetBit();
                gateRp = new AstAnd{flp, gateRp, notDisRp};
                gateRp->dtypeSetBit();
            }
            AstAssignDly* const setLp
                = new AstAssignDly{flp, new AstVarRef{flp, doneLVars[ai], VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitTrue{}}};
            AstIf* const setLIfp = new AstIf{flp, gateLp, setLp, nullptr};
            AstAssignDly* const setRp
                = new AstAssignDly{flp, new AstVarRef{flp, doneRVars[ai], VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitTrue{}}};
            AstIf* const setRIfp = new AstIf{flp, gateRp, setRp, nullptr};
            setLIfp->addNext(setRIfp);

            AstIf* const topp
                = new AstIf{flp, stateSig[ai]->cloneTreePure(false), clearLp, setLIfp};
            AstAlways* const combAlwaysp
                = new AstAlways{flp, VAlwaysKwd::ALWAYS, senTreep->cloneTree(false), topp};
            m_modp->addStmtsp(combAlwaysp);
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
        // Snapshot-based not-disabled expression: (snapshotVarp == disableCntVarp).
        // Built once and cloned into each terminal contribution.
        AstNodeExpr* snapshotOkp = nullptr;
        if (snapshotVarp && disableCntVarp) {
            snapshotOkp = new AstEq{flp, new AstVarRef{flp, snapshotVarp, VAccess::READ},
                                    new AstVarRef{flp, disableCntVarp, VAccess::READ}};
            snapshotOkp->dtypeSetBit();
        }

        AstNodeExpr* terminalActivep = nullptr;
        AstNodeExpr* rejectBasep = nullptr;
        for (const auto& edge : nfa.edges) {
            if (edge.toId != nfa.acceptNode) continue;
            if (edge.consumesCycle) continue;
            if (!stateSig[edge.fromId]) continue;

            AstNodeExpr* srcSigp = stateSig[edge.fromId]->cloneTreePure(false);
            srcSigp = andCond(flp, srcSigp, edge.condp);
            // Gate terminal contribution with snapshot check if active.
            if (snapshotOkp) {
                srcSigp = new AstAnd{flp, srcSigp, snapshotOkp->cloneTreePure(false)};
                srcSigp->dtypeSetBit();
            }

            if (nfa.nodes[edge.fromId].isCounter) {
                const int ci = edge.fromId;
                // Accept: use srcSigp as-is (active && link_cond)
                terminalActivep = orExprs(flp, terminalActivep, srcSigp->cloneTreePure(false));
                // Reject base: srcSigp && (counter == counterRange)
                AstNodeExpr* const atEndp
                    = new AstEq{flp, new AstVarRef{flp, counterCountVars[ci], VAccess::READ},
                                new AstConst{flp, AstConst::WidthedValue{}, 32,
                                             static_cast<uint32_t>(nfa.nodes[ci].counterMax)}};
                atEndp->dtypeSetBit();
                AstNodeExpr* const expireContribp = new AstAnd{flp, srcSigp, atEndp};
                expireContribp->dtypeSetBit();
                rejectBasep = orExprs(flp, rejectBasep, expireContribp);
            } else if (nfa.nodes[edge.fromId].isUnbounded
                       || nfa.nodes[edge.fromId].isAndCombiner) {
                // Liveness terminal OR SAnd combiner: contributes to
                // accept/cover only. A combiner's stateSig is a richer
                // expression (the done-latch formula) that is TRUE only on
                // the exact cycle the aggregated sequence matches, so
                // treating "formula false" as reject would be wrong -- the
                // constituent sub-NFAs already emit their own rejects.
                terminalActivep = orExprs(flp, terminalActivep, srcSigp);
            } else {
                terminalActivep = orExprs(flp, terminalActivep, srcSigp->cloneTreePure(false));
                rejectBasep = orExprs(flp, rejectBasep, srcSigp);
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
        if (!terminalActivep) { terminalActivep = new AstConst{flp, AstConst::BitFalse{}}; }

        // Phase 3a: "required-step" rejection.
        // For every Link marked rejectOnFail, reject fires when the source
        // state is active but the link condition is false. This catches
        // failures like `a |-> b ##...` where the antecedent fires but the
        // consequent's first boolean (b) is false -- the attempt never
        // leaves the start state, so no later terminal-based reject can
        // ever fire for it.
        //
        // When outRequiredStepSrcsp is non-null, individual sources are
        // also collected so that the caller can emit extra fail-handler
        // fires for the case where two or more independent evaluation
        // threads each hit a required-step failure in the same clock cycle.
        // The main reject expression (requiredStepRejectp) is always built
        // as the OR of all sources and remains the primary fail trigger.
        AstNodeExpr* requiredStepRejectp = nullptr;
        for (const auto& edge : nfa.edges) {
            if (!edge.rejectOnFail) continue;
            if (edge.consumesCycle) continue;
            if (!stateSig[edge.fromId]) continue;
            if (!edge.condp) continue;
            AstNodeExpr* const srcSigp = stateSig[edge.fromId]->cloneTreePure(false);
            AstNodeExpr* const notCondp = new AstNot{flp, edge.condp->cloneTreePure(false)};
            notCondp->dtypeSetBit();
            AstNodeExpr* const failp = new AstAnd{flp, srcSigp, notCondp};
            failp->dtypeSetBit();
            if (outRequiredStepSrcsp) {
                // Also collect individually for extra-fire logic in caller.
                outRequiredStepSrcsp->push_back(failp->cloneTreePure(false));
            }
            requiredStepRejectp = orExprs(flp, requiredStepRejectp, failp);
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
            // Skip SAnd combiner: its stateSig is an accept formula
            // (fires only at completion), not an in-progress marker, so
            // throughout-drop detection for the AND itself is already
            // covered by the sub-sequences' own combiner-internal nodes.
            if (nfa.nodes[i].isAndCombiner) continue;
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
                AstNodeExpr* const notDisp = new AstNot{flp, disableExprp->cloneTreePure(false)};
                notDisp->dtypeSetBit();
                throughoutRejectp = new AstAnd{flp, throughoutRejectp, notDisp};
                throughoutRejectp->dtypeSetBit();
            }
            if (requiredStepRejectp) {
                AstNodeExpr* const notDisp = new AstNot{flp, disableExprp->cloneTreePure(false)};
                notDisp->dtypeSetBit();
                requiredStepRejectp = new AstAnd{flp, requiredStepRejectp, notDisp};
                requiredStepRejectp->dtypeSetBit();
            }
        }

        // Clean up snapshotOkp template expression (was cloned per-use)
        if (snapshotOkp) {
            snapshotOkp->deleteTree();
            snapshotOkp = nullptr;
        }

        // Clean up disableExprp if passed (was cloned in Phase 2, original not attached)
        if (disableExprp) {
            disableExprp->deleteTree();
            disableExprp = nullptr;
        }

        // Property negation (IEEE 1800-2023 16.12.1 `not`): invert
        // accept/reject semantics.
        // Assert(not P): pass when P fails -> return !accept.
        // Cover(not P): fire when P fails -> return reject.
        if (negated) {
            if (isCover) {
                // Negated cover: return reject signal (P failed)
                if (terminalActivep) terminalActivep->deleteTree();
                AstNodeExpr* negRejectp = nullptr;
                if (acceptCondp && rejectBasep) {
                    AstNodeExpr* const sampledCondp
                        = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
                    sampledCondp->dtypeFrom(acceptCondp);
                    AstNodeExpr* const notCondp = new AstNot{flp, sampledCondp};
                    notCondp->dtypeSetBit();
                    negRejectp = new AstAnd{flp, rejectBasep, notCondp};
                    negRejectp->dtypeSetBit();
                } else if (rejectBasep) {
                    rejectBasep->deleteTree();
                }
                if (throughoutRejectp) {
                    negRejectp = orExprs(flp, negRejectp, throughoutRejectp);
                    if (negRejectp) negRejectp->dtypeSetBit();
                }
                if (requiredStepRejectp) {
                    negRejectp = orExprs(flp, negRejectp, requiredStepRejectp);
                    if (negRejectp) negRejectp->dtypeSetBit();
                }
                return negRejectp ? negRejectp : new AstConst{flp, AstConst::BitFalse{}};
            }
            // Negated assert/assume: output = !accept (assertion fails when inner P matches).
            // For pass-handler gating: NOT-P passes when inner P REJECTS -- i.e., when
            // reject_of_inner_P fires. Using the raw !accept would cause vacuous passes
            // (fires every cycle where no instance has completed, not just when P rejected).
            AstNodeExpr* acceptp = terminalActivep;
            if (acceptCondp) {
                AstNodeExpr* const sampledCondp
                    = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
                sampledCondp->dtypeFrom(acceptCondp);
                acceptp = new AstAnd{flp, acceptp, sampledCondp};
                acceptp->dtypeSetBit();
            }
            // Build reject_of_inner_P for passsp gating before deleting the signals.
            if (outAcceptpp) {
                AstNodeExpr* notPAcceptp = nullptr;
                if (acceptCondp && rejectBasep) {
                    AstNodeExpr* const sampledCondp
                        = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
                    sampledCondp->dtypeFrom(acceptCondp);
                    AstNodeExpr* const notCondp = new AstNot{flp, sampledCondp};
                    notCondp->dtypeSetBit();
                    notPAcceptp = new AstAnd{flp, rejectBasep->cloneTreePure(false), notCondp};
                    notPAcceptp->dtypeSetBit();
                } else if (rejectBasep) {
                    notPAcceptp = rejectBasep->cloneTreePure(false);
                }
                if (throughoutRejectp)
                    notPAcceptp
                        = orExprs(flp, notPAcceptp, throughoutRejectp->cloneTreePure(false));
                if (requiredStepRejectp)
                    notPAcceptp
                        = orExprs(flp, notPAcceptp, requiredStepRejectp->cloneTreePure(false));
                *outAcceptpp = notPAcceptp;
            }
            if (throughoutRejectp) throughoutRejectp->deleteTree();
            if (rejectBasep) rejectBasep->deleteTree();
            if (requiredStepRejectp) requiredStepRejectp->deleteTree();
            AstNodeExpr* const resultExprp = new AstNot{flp, acceptp};
            resultExprp->dtypeSetBit();
            return resultExprp;
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
                AstNodeExpr* const acceptp = new AstAnd{flp, terminalActivep, sampledCondp};
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
        // Optionally return accept expression for pass-handler gating.
        if (outAcceptpp) {
            AstNodeExpr* acceptExprp = terminalActivep->cloneTreePure(false);
            if (acceptCondp) {
                AstNodeExpr* const sp = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
                sp->dtypeSetBit();
                acceptExprp = new AstAnd{flp, acceptExprp, sp};
                acceptExprp->dtypeSetBit();
            }
            *outAcceptpp = acceptExprp;
        }
        if (terminalActivep) terminalActivep->deleteTree();

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
    V3UniqueNames m_propVarNames{"__Vpropvar"};
    V3UniqueNames m_disableCntNames{"__VnfaDis"};
    // Recursion guard for inlineNamedProperty: tracks properties currently
    // being inlined on the call stack to detect self-referential definitions.
    std::set<const AstProperty*> m_inliningProps;

    // Extract body expression from an AstSequence definition.
    // Skips port AstVar nodes at the front of stmtsp().
    static AstNodeExpr* getSequenceBodyExprp(const AstSequence* seqp) {
        AstNode* bodyp = seqp->stmtsp();
        while (bodyp && VN_IS(bodyp, Var)) bodyp = bodyp->nextp();
        return VN_CAST(bodyp, NodeExpr);
    }

    // Extract AstPropSpec body from an AstProperty definition.
    // Skips port AstVar and AstInitial* nodes at the front of stmtsp().
    static AstPropSpec* getPropertyExprp(const AstProperty* propp) {
        AstNode* propExprp = propp->stmtsp();
        while (propExprp
               && (VN_IS(propExprp, Var) || VN_IS(propExprp, InitialStaticStmt)
                   || VN_IS(propExprp, InitialAutomaticStmt))) {
            propExprp = propExprp->nextp();
        }
        return VN_CAST(propExprp, PropSpec);
    }

    // Inline a named property call (FuncRef -> Property) into the
    // assertion's PropSpec. Mirrors V3AssertPre::substitutePropertyCall.
    void inlineNamedProperty(AstPropSpec* outerSpecp, AstFuncRef* funcrefp,
                             const AstProperty* propyp) {
        // Recursion guard: self-referential or mutually-recursive properties
        // would cause infinite inlining. IEEE 1800-2023 16.12.1 requires
        // that property definitions not be recursive.
        if (m_inliningProps.count(propyp)) {
            funcrefp->v3error("Recursive property reference not allowed"
                              " (IEEE 1800-2023 16.12.1)");
            return;
        }
        m_inliningProps.insert(propyp);
        struct Guard final {
            std::set<const AstProperty*>& setr;
            const AstProperty* keyp;
            ~Guard() { setr.erase(keyp); }
        } guard{m_inliningProps, propyp};
        AstPropSpec* propExprp = getPropertyExprp(propyp);
        UASSERT_OBJ(propExprp, funcrefp, "Property has no body PropSpec");
        propExprp = propExprp->cloneTree(false);

        // Build substitution map for formal port parameters
        const V3TaskConnects tconnects = V3Task::taskConnects(funcrefp, propyp->stmtsp());
        std::unordered_map<const AstVar*, AstNodeExpr*> portMap;
        for (const auto& tconnect : tconnects) {
            portMap[tconnect.first] = tconnect.second->exprp();
        }

        // Promote property-local variables (non-port Vars, IEEE 16.10) to
        // module-level __Vpropvar temps.
        std::unordered_map<const AstVar*, AstVar*> localVarMap;
        for (AstNode* stmtp = propyp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (!varp->isIO()) {
                    const string newName = m_propVarNames.get(varp);
                    AstVar* const newVarp = new AstVar{varp->fileline(), VVarType::MODULETEMP,
                                                       newName, varp->dtypep()};
                    newVarp->lifetime(VLifetime::STATIC_EXPLICIT);
                    m_modp->addStmtsp(newVarp);
                    localVarMap[varp] = newVarp;
                }
            }
        }

        // Single traversal: substitute ports and update local var refs
        propExprp->foreach([&](AstVarRef* refp) {
            {
                const auto portIt = portMap.find(refp->varp());
                if (portIt != portMap.end()) {
                    refp->replaceWith(portIt->second->cloneTree(false));
                    VL_DO_DANGLING(pushDeletep(refp), refp);
                    return;
                }
            }
            {
                const auto localIt = localVarMap.find(refp->varp());
                if (localIt != localVarMap.end()) { refp->varp(localIt->second); }
            }
        });

        // Clean up argument expressions owned by FuncRef
        for (const auto& tconnect : tconnects) {
            pushDeletep(tconnect.second->exprp()->unlinkFrBack());
        }

        // Merge disable iff (IEEE 1800-2023 16.12.1)
        if (outerSpecp->disablep() && propExprp->disablep()) {
            outerSpecp->v3error("disable iff expression before property call "
                                "and in its body is not legal");
            pushDeletep(propExprp->disablep()->unlinkFrBack());
        }
        if (outerSpecp->disablep()) {
            propExprp->disablep(outerSpecp->disablep()->unlinkFrBack());
        }

        // Merge clock events
        if (outerSpecp->sensesp() && propExprp->sensesp()) {
            outerSpecp->v3warn(E_UNSUPPORTED,
                               "Unsupported: Clock event before property call and in its body");
            pushDeletep(propExprp->sensesp()->unlinkFrBack());
        }
        if (outerSpecp->sensesp()) {
            AstSenItem* const sensesp = outerSpecp->sensesp();
            sensesp->unlinkFrBack();
            propExprp->sensesp(sensesp);
        }

        // Replace outer PropSpec with inlined inner PropSpec
        outerSpecp->replaceWith(propExprp);
        VL_DO_DANGLING(pushDeletep(outerSpecp), outerSpecp);
    }

    // Inline a named sequence call (FuncRef -> Sequence) into the
    // assertion's expression tree. Mirrors V3AssertPre::substituteSequenceCall.
    void inlineSequenceRef(AstFuncRef* funcrefp, const AstSequence* seqp) {
        AstNodeExpr* const bodyExprp = getSequenceBodyExprp(seqp);
        UASSERT_OBJ(bodyExprp, funcrefp, "Sequence has no body expression");
        AstNodeExpr* const clonedp = bodyExprp->cloneTree(false);

        // Build substitution map for formal port parameters
        const V3TaskConnects tconnects = V3Task::taskConnects(funcrefp, seqp->stmtsp());
        std::unordered_map<const AstVar*, AstNodeExpr*> portMap;
        for (const auto& tconnect : tconnects) {
            portMap[tconnect.first] = tconnect.second->exprp();
        }
        clonedp->foreach([&](AstVarRef* refp) {
            const auto it = portMap.find(refp->varp());
            if (it != portMap.end()) {
                refp->replaceWith(it->second->cloneTree(false));
                VL_DO_DANGLING(pushDeletep(refp), refp);
            }
        });
        // Clean up argument expressions owned by FuncRef
        for (const auto& tconnect : tconnects) {
            pushDeletep(tconnect.second->exprp()->unlinkFrBack());
        }
        funcrefp->replaceWith(clonedp);
        VL_DO_DANGLING(pushDeletep(funcrefp), funcrefp);
    }

    // Inline all named sequence references in the assertion subtree.
    // Must run before hasMultiCycleExpr() so that multi-cycle sequence
    // bodies are visible for NFA conversion. Without this, named sequences
    // would pass through to V3AssertPre (which runs after V3AssertNfa),
    // and their multi-cycle bodies would never be compiled by the NFA engine.
    void inlineAllSequenceRefs(AstNode* rootp) {
        bool changed = true;
        while (changed) {
            changed = false;
            rootp->foreach([&](AstFuncRef* funcrefp) {
                if (changed) return;
                if (const AstSequence* const seqp = VN_CAST(funcrefp->taskp(), Sequence)) {
                    inlineSequenceRef(funcrefp, seqp);
                    changed = true;
                }
            });
        }
    }

    static bool hasMultiCycleExpr(const AstNode* nodep) {
        return !nodep->forall([](const AstNode* np) {
            return !VN_IS(np, SExpr) && !VN_IS(np, SConsRep) && !VN_IS(np, SGotoRep)
                   && !VN_IS(np, SIntersect) && !VN_IS(np, SThroughout) && !VN_IS(np, SAnd)
                   && !VN_IS(np, SOr) && !VN_IS(np, SNonConsRep);
        });
    }

    struct PropertyParts final {
        AstNodeExpr* triggerExprp = nullptr;
        AstNodeExpr* seqExprp = nullptr;
        bool isOverlapped = true;
        bool hasImplication = false;
    };

    static PropertyParts decomposeProperty(AstNode* propp) {
        PropertyParts parts;
        if (AstPropSpec* const specp = VN_CAST(propp, PropSpec)) { propp = specp->propp(); }
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

        // Inline named property calls (FuncRef -> Property) so that
        // hasMultiCycleExpr can see the property body.
        if (AstPropSpec* const specp = VN_CAST(assertp->propp(), PropSpec)) {
            if (AstFuncRef* const funcrefp = VN_CAST(specp->propp(), FuncRef)) {
                if (const AstProperty* const propyp = VN_CAST(funcrefp->taskp(), Property)) {
                    inlineNamedProperty(specp, funcrefp, propyp);
                    // specp is now dangling -- re-read propp() below
                }
            }
        }

        // Inline named sequence calls (FuncRef -> Sequence) anywhere in
        // the assertion tree. Must run before hasMultiCycleExpr() so that
        // multi-cycle sequence bodies are visible for NFA conversion.
        inlineAllSequenceRefs(assertp->propp());

        AstNode* const propp = assertp->propp();
        if (!hasMultiCycleExpr(propp)) return;

        const PropertyParts parts = decomposeProperty(propp);
        if (!parts.seqExprp) return;

        // Unwrap property-level `not` (IEEE 1800-2023 16.12.1).
        // Odd LogNot count -> negated semantics in emitter.
        AstNodeExpr* seqBodyp = parts.seqExprp;
        bool negated = false;
        while (AstLogNot* const notp = VN_CAST(seqBodyp, LogNot)) {
            negated = !negated;
            seqBodyp = notp->lhsp();
        }

        AstSenTree* senTreep = assertp->sentreep();
        bool senTreeOwned = false;  // True if we created senTreep locally
        AstPropSpec* const propSpecp = VN_CAST(assertp->propp(), PropSpec);
        AstNodeExpr* disableExprp = nullptr;
        if (propSpecp) {
            if (!senTreep && propSpecp->sensesp()) {
                senTreep
                    = new AstSenTree{propSpecp->fileline(), propSpecp->sensesp()->cloneTree(true)};
                senTreeOwned = true;
            }
            disableExprp = propSpecp->disablep();
        }
        if (!senTreep) return;

        FileLine* const flp = assertp->fileline();
        const bool isCover = VN_IS(assertp, Cover);

        // For standalone sequences with a non-constant disable iff, use the
        // IEEE-correct disableCnt snapshot mechanism instead of the direct
        // !disable gate on state NBAs. The !disable gate misses the disable
        // cycle when the disable expression depends on variables updated via
        // NBA (e.g. `always @(clk) ++cyc`) because the NFA's state registers
        // are also updated in the NBA region (before the reactive re-evaluation
        // that would capture the updated disable value).
        //
        // The disableCnt mechanism: a counter increments on the rising edge of
        // the disable expression (in a reactive re-evaluation triggered after
        // NBA updates). A snapshot register captures the counter value at each
        // evaluation start (in Phase-2 NBA, before the reactive increment).
        // The terminal check compares snapshot vs counter: if they differ,
        // disable fired during the evaluation => kill it.
        AstVar* disableCntVarp = nullptr;
        AstVar* snapshotVarp = nullptr;
        // Detect $sampled inside the disable expression: cannot use @(posedge
        // disableExpr) as a sensitivity if disableExpr contains $sampled.
        // Fall back to the basic !disable gate for such cases.
        const bool disableHasSampled
            = disableExprp && disableExprp->exists([](const AstSampled*) { return true; });
        if (disableExprp && !parts.hasImplication && !VN_IS(disableExprp, Const)
            && !disableHasSampled) {
            AstNodeDType* const u32DTypep = m_modp->findBasicDType(VBasicDTypeKwd::UINT32);
            const std::string cntName = m_disableCntNames.get("");
            disableCntVarp = new AstVar{flp, VVarType::MODULETEMP, cntName, u32DTypep};
            disableCntVarp->lifetime(VLifetime::STATIC_EXPLICIT);
            m_modp->addStmtsp(disableCntVarp);

            // always @(posedge disableExpr) disableCnt++
            AstNodeExpr* const rdRefp = new AstVarRef{flp, disableCntVarp, VAccess::READ};
            AstNodeExpr* const wrRefp = new AstVarRef{flp, disableCntVarp, VAccess::WRITE};
            AstNodeExpr* const incrExprp
                = new AstAdd{flp, rdRefp, new AstConst{flp, AstConst::WidthedValue{}, 32, 1u}};
            incrExprp->dtypeFrom(disableCntVarp);
            AstAssign* const incrAssignp = new AstAssign{flp, wrRefp, incrExprp};
            AstSenItem* const senItemp
                = new AstSenItem{flp, VEdgeType::ET_POSEDGE, disableExprp->cloneTreePure(false)};
            AstSenTree* const disSenp = new AstSenTree{flp, senItemp};
            AstAlways* const disAlwaysp
                = new AstAlways{flp, VAlwaysKwd::ALWAYS, disSenp, incrAssignp};
            m_modp->addStmtsp(disAlwaysp);

            // Create snapshot register (captured in Phase-2 NBA alongside
            // state register updates, before any reactive re-evaluation)
            const std::string snapName = m_disableCntNames.get("") + "__snap";
            snapshotVarp = new AstVar{flp, VVarType::MODULETEMP, snapName, u32DTypep};
            snapshotVarp->lifetime(VLifetime::STATIC_EXPLICIT);
            m_modp->addStmtsp(snapshotVarp);

            // Remove propSpec disable so V3Assert doesn't add its own !disable
            // wrapper (which uses the active-region value and would be wrong).
            // The snapshot mechanism handles disable semantics instead.
            if (propSpecp && propSpecp->disablep()) {
                AstNodeExpr* const oldDisp = propSpecp->disablep();
                oldDisp->unlinkFrBack();
                // disableExprp now points to the unlinked expression. It is
                // used via cloneTreePure() in emit(). The original must be
                // deleted explicitly after emit() returns (see below).
            }
        }
        // Track whether disableExprp was unlinked so we can delete it later.
        // Nodes that remain in the AST are freed when the property tree is
        // deleted; unlinked nodes must be freed manually.
        const bool disableExprUnlinked = disableCntVarp && disableExprp;

        // Build NFA
        SvaNfa nfa;
        SvaNfaBuilder builder{nfa};

        BuildResult result = BuildResult::fail();
        if (parts.hasImplication) {
            nfa.startNode = nfa.createNode();

            // Build antecedent. For simple boolean triggers this produces
            // {startNode, boolExpr} (no NFA nodes added). For multi-cycle
            // antecedents (ConsRep, SExpr, SGotoRep, ...) it builds a real
            // NFA sub-graph whose terminal is the "match" point.
            const BuildResult antResult = builder.buildExpr(parts.triggerExprp, nfa.startNode);
            if (!antResult.valid()) {
                if (!antResult.errorEmitted) {
                    assertp->v3warn(E_UNSUPPORTED, "Unsupported: assertion antecedent contains SVA"
                                                   " construct not yet supported by NFA engine");
                }
                if (senTreeOwned) senTreep->deleteTree();
                if (disableExprUnlinked) disableExprp->deleteTree();
                // Replace property with a safe placeholder so downstream passes
                // don't crash on the leftover SExpr nodes (--debug-check, ASAN).
                if (propSpecp) {
                    AstNode* const innerPropp = propSpecp->propp();
                    innerPropp->replaceWith(new AstConst{flp, AstConst::BitFalse{}});
                    VL_DO_DANGLING(pushDeletep(innerPropp), innerPropp);
                }
                return;
            }

            // Create a clean trigger node that does NOT inherit the
            // antecedent's liveness scope. Reaching the antecedent
            // terminal is a definitive event -- the consequent MUST
            // fire accept/reject, even if the antecedent was unbounded.
            // Use raw nfa.createNode() (not builder's scopedCreateNode)
            // so the node starts with isUnbounded=false.
            const int trigNode = nfa.createNode();
            if (antResult.finalCondp) {
                nfa.addLink(antResult.termNode, trigNode,
                            new AstSampled{flp, antResult.finalCondp->cloneTreePure(false)});
                // Delete fresh finalCondp (no AST parent = freshly allocated).
                // Nodes in the original tree are freed with innerPropp later.
                if (!antResult.finalCondp->backp()) antResult.finalCondp->deleteTree();
            } else {
                nfa.addLink(antResult.termNode, trigNode);
            }

            // Reset builder scope: clear liveness and throughout state
            // from the antecedent build so the consequent gets a fresh
            // context. A `[+]` antecedent must not make consequent nodes
            // liveness-only.
            builder.resetScope();

            if (parts.isOverlapped) {
                result = builder.buildExpr(seqBodyp, trigNode,
                                           /*isTopLevelStep=*/true);
            } else {
                const int delayNode = nfa.createNode();
                nfa.addClockEdge(trigNode, delayNode);
                result = builder.buildExpr(seqBodyp, delayNode,
                                           /*isTopLevelStep=*/true);
            }

            if (result.valid()) {
                nfa.createAcceptNode();
                // Accept Link is unconditional. The finalCond is passed
                // separately to the emitter for accept/reject computation.
                nfa.addLink(result.termNode, nfa.acceptNode);
                // Range-delay mid-window positions: accept-only (Phase 3
                // skips reject contribution because we mark isUnbounded).
                // Apply the node's stored throughout conditions to the link
                // so that a mid-position cannot accept while a throughout
                // guard is violated (otherwise throughout-drop fires as reject
                // but the simultaneous unconditional accept would erroneously
                // fire pass action).
                for (int src : result.midSources) {
                    AstNodeExpr* condp = nullptr;
                    for (AstNodeExpr* const tc : nfa.nodes[src].throughoutConds) {
                        AstNodeExpr* const tcClone = tc->cloneTreePure(false);
                        condp = condp ? new AstAnd{flp, condp, tcClone} : tcClone;
                        if (condp->width() != 1) condp->dtypeSetBit();
                    }
                    nfa.addLink(src, nfa.acceptNode, condp);
                    nfa.nodes[src].isUnbounded = true;
                }
            }
        } else {
            // Standalone sequence
            result = builder.build(seqBodyp);
            if (result.valid()) {
                nfa.createAcceptNode();
                nfa.addLink(result.termNode, nfa.acceptNode);
                for (int src : result.midSources) {
                    AstNodeExpr* condp = nullptr;
                    for (AstNodeExpr* const tc : nfa.nodes[src].throughoutConds) {
                        AstNodeExpr* const tcClone = tc->cloneTreePure(false);
                        condp = condp ? new AstAnd{flp, condp, tcClone} : tcClone;
                        if (condp->width() != 1) condp->dtypeSetBit();
                    }
                    nfa.addLink(src, nfa.acceptNode, condp);
                    nfa.nodes[src].isUnbounded = true;
                }
            }
        }

        if (!result.valid()) {
            // NFA cannot handle this assertion -- emit UNSUPPORTED rather than
            // silently passing it through (there is no fallback anymore).
            // Skip generic message if the builder already emitted a specific error.
            if (!result.errorEmitted) {
                assertp->v3warn(E_UNSUPPORTED,
                                "Unsupported: assertion contains SVA construct not yet"
                                " supported by NFA engine (e.g. intersect, sequence and,"
                                " complex throughout)");
            }
            if (senTreeOwned) senTreep->deleteTree();
            if (disableExprUnlinked) disableExprp->deleteTree();
            // Replace property with a safe placeholder so downstream passes
            // don't crash on the leftover SExpr nodes (--debug-check, ASAN).
            if (propSpecp) {
                AstNode* const innerPropp = propSpecp->propp();
                innerPropp->replaceWith(new AstConst{flp, AstConst::BitFalse{}});
                VL_DO_DANGLING(pushDeletep(innerPropp), innerPropp);
            } else {
                AstNode* const oldPropp = assertp->propp();
                oldPropp->replaceWith(new AstConst{flp, AstConst::BitFalse{}});
                VL_DO_DANGLING(pushDeletep(oldPropp), oldPropp);
            }
            return;
        }

        // For standalone sequences (no implication) with pass handlers,
        // request the accept expression so we can gate the pass handler.
        // Without this, `!reject == 1` on non-terminal cycles causes
        // vacuous-pass firings that don't occur in Questa.
        AstAssert* const assertAssertp = VN_CAST(assertp, Assert);
        const bool needAccept
            = !isCover && !parts.hasImplication && assertAssertp && assertAssertp->passsp();
        AstNodeExpr* acceptExprp = nullptr;

        // Collect individual required-step reject sources when there is a
        // fail handler to invoke. Each source represents a distinct evaluation
        // thread that failed its required first (or mid-sequence) boolean
        // check. Using individual sources instead of a single OR lets the
        // fail handler fire once per failing thread in the same cycle.
        AstAssert* const assertWithFailp = VN_CAST(assertp, Assert);
        const bool needPerSrcFail
            = !isCover && !parts.hasImplication && assertWithFailp && assertWithFailp->failsp();
        std::vector<AstNodeExpr*> requiredStepSrcs;

        // Emit NFA hardware
        AstNodeExpr* const alwaysTriggerp = new AstConst{flp, AstConst::BitTrue{}};
        AstNodeExpr* const outputExprp
            = m_emitterp->emit(flp, nfa, alwaysTriggerp, senTreep, result.finalCondp, isCover,
                               disableExprp ? disableExprp->cloneTreePure(false) : nullptr,
                               negated, needAccept ? &acceptExprp : nullptr, disableCntVarp,
                               snapshotVarp, needPerSrcFail ? &requiredStepSrcs : nullptr);

        // Save senTree clone for extra-fail always blocks before senTreep may
        // be deleted. Only needed when >= 2 required-step sources exist (single
        // source is handled correctly by the main assertion's OR'd reject).
        AstSenTree* const perSrcSenTreep
            = (requiredStepSrcs.size() >= 2) ? senTreep->cloneTree(false) : nullptr;

        // Clean up locally-owned temporaries (emit cloned them)
        alwaysTriggerp->deleteTree();
        if (senTreeOwned) senTreep->deleteTree();
        // Delete unlinked disableExprp (was unlinked from propSpecp for the
        // disableCnt mechanism; emit() only uses clones, so original is ours).
        if (disableExprUnlinked) disableExprp->deleteTree();
        // Delete fresh finalCondp nodes not attached to the original AST.
        // emit() always cloneTreePure()s finalCondp, so we own any freshly
        // allocated node (backp()==nullptr means it has no AST parent).
        if (result.finalCondp && !result.finalCondp->backp()) { result.finalCondp->deleteTree(); }

        // Gate pass action with accept signal to prevent vacuous-pass firings.
        // For standalone sequences (no implication) the assertion output is
        // `!reject`, which is vacuously 1 on every cycle where no instance has
        // completed. Without gating, the pass handler would fire spuriously on
        // every such cycle. Wrapping it in `if (accept_expr)` ensures it fires
        // only when an instance actually completed successfully this cycle.
        // For negated properties, accept_expr is reject_of_inner_P (see emit()).
        if (needAccept && acceptExprp) {
            AstNode* passsp = assertAssertp->passsp();
            if (passsp) {
                passsp->unlinkFrBackWithNext();
                // Gate pass handler: fires only when accept_expr is true.
                // Use cloneTree (not cloneTreePure) for statement nodes that
                // may contain side-effecting calls such as $display.
                assertAssertp->addPasssp(new AstIf{flp, acceptExprp->cloneTreePure(false),
                                                   passsp->cloneTree(false), nullptr});
                // Fail-handler prefix: fires when reject=1 && accept=1 (different
                // overlapping instances: one fails, another passes). Without this,
                // the pass action is suppressed whenever reject=1, producing
                // {fails:1, passs:0} instead of {fails:1, passs:1} (IEEE 16.12).
                if (AstNode* const failsp = assertAssertp->failsp()) {
                    failsp->addHereThisAsNext(
                        new AstIf{flp, acceptExprp, passsp->cloneTree(false), nullptr});
                } else {
                    acceptExprp->deleteTree();
                }
                VL_DO_DANGLING(pushDeletep(passsp), passsp);
            } else {
                acceptExprp->deleteTree();
            }
        }

        // Extra fail-handler fires for simultaneous required-step failures.
        //
        // The main assertion expression already includes ALL required-step
        // reject sources (via requiredStepRejectp = OR of all sources) and
        // fires the fail handler once when any source triggers. When two or
        // more INDEPENDENT evaluation threads each hit a required-step
        // failure in the same clock cycle, the fail handler must fire once
        // per failing thread (IEEE 1800-2023 concurrent assertion semantics).
        //
        // Exactly N fires when N sources are simultaneously active
        if (requiredStepSrcs.size() >= 2 && assertWithFailp && assertWithFailp->failsp()
            && perSrcSenTreep) {
            AstNode* const failsp = assertWithFailp->failsp();
            // Build cumulative OR of sources seen so far.
            // All srcp entries are independently owned; clone them for each use.
            AstNodeExpr* cumulativeOrp = requiredStepSrcs[0]->cloneTreePure(false);
            for (size_t i = 1; i < requiredStepSrcs.size(); ++i) {
                AstNodeExpr* const srcp = requiredStepSrcs[i];
                // Extra always block: fires when src_i AND any previous src.
                // This represents a second (or later) simultaneous thread fail.
                AstNodeExpr* const condp = new AstAnd{flp, srcp->cloneTreePure(false),
                                                      cumulativeOrp->cloneTreePure(false)};
                condp->dtypeSetBit();
                // cloneTree(true) copies the full sibling list so multi-statement
                // else blocks are reproduced completely.
                AstNode* const failClonep = failsp->cloneTree(true);
                AstIf* const ifp = new AstIf{flp, condp, failClonep, nullptr};
                AstAlways* const alwaysp = new AstAlways{flp, VAlwaysKwd::ALWAYS,
                                                         perSrcSenTreep->cloneTree(false), ifp};
                m_modp->addStmtsp(alwaysp);
                // Extend cumulative OR to include this source for next iteration.
                AstNodeExpr* const extOrp
                    = new AstOr{flp, cumulativeOrp, srcp->cloneTreePure(false)};
                extOrp->dtypeSetBit();
                cumulativeOrp = extOrp;
            }
            // Free all collected sources and the cumulative OR.
            for (AstNodeExpr* const srcp : requiredStepSrcs) srcp->deleteTree();
            cumulativeOrp->deleteTree();
            perSrcSenTreep->deleteTree();
        } else {
            // Single source or no fail handler: no extra fires needed.
            for (AstNodeExpr* const srcp : requiredStepSrcs) srcp->deleteTree();
            if (perSrcSenTreep) perSrcSenTreep->deleteTree();
        }

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

        UINFO(4, "NFA converted assertion at " << flp << ", " << nfa.nodes.size() << " nodes, "
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
    void visit(AstRestrict* nodep) override {
        // Restrict property is ignored by simulators (IEEE 1800-2023 16.12.2).
        // Remove here so temporal SExpr don't leak to V3AssertPre.
        VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
    }
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
