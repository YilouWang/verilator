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
};

struct SvaNfaEdge final {
    int fromId;
    int toId;
    AstNodeExpr* condp = nullptr;  // nullptr = unconditional; OWNED by NFA
    bool consumesCycle;  // true = Edge (##1), false = Link (##0/boolean)
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
    int termNode;           // The last NFA node of the built sub-graph
    AstNodeExpr* finalCondp;  // Final condition for accept/reject (nullptr = unconditional)
    bool valid() const { return termNode >= 0; }
    static BuildResult fail() { return {-1, nullptr}; }
};

//######################################################################
// NFA Builder

class SvaNfaBuilder final {
    SvaNfa& m_nfa;
    std::vector<AstNodeExpr*> m_throughoutStack;

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

    int addDelayChain(int startNode, int n, FileLine* flp) {
        int current = startNode;
        for (int i = 0; i < n; ++i) {
            const int next = m_nfa.createNode();
            m_nfa.addClockEdge(current, next, throughoutCond(nullptr, flp));
            current = next;
        }
        return current;
    }

    // Build NFA for an SExpr. Returns {termNode, finalCond}.
    // The finalCond is the RHS expression -- NOT yet added as a node.
    BuildResult buildSExpr(AstSExpr* sexprp, int entryNode) {
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
                const int condNode = m_nfa.createNode();
                m_nfa.addLink(pre.termNode, condNode,
                              sampled(pre.finalCondp->cloneTreePure(false)));
                currentNode = condNode;
            } else {
                currentNode = pre.termNode;
            }
        }

        // Handle delay
        if (delayp->isRangeDelay()) {
            const int minDelay = getConstInt(delayp->lhsp());
            if (minDelay < 0) return BuildResult::fail();
            currentNode = addDelayChain(currentNode, minDelay, flp);

            if (delayp->isUnbounded()) {
                m_nfa.addClockEdge(currentNode, currentNode,
                                   throughoutCond(nullptr, flp));
            } else {
                const int maxDelay = getConstInt(delayp->rhsp());
                if (maxDelay < minDelay) return BuildResult::fail();
                const int range = maxDelay - minDelay;
                if (range > 64) return BuildResult::fail();

                const int mergeNode = m_nfa.createNode();
                m_nfa.addLink(currentNode, mergeNode);
                for (int i = 0; i < range; ++i) {
                    const int nextNode = m_nfa.createNode();
                    m_nfa.addClockEdge(currentNode, nextNode,
                                       throughoutCond(nullptr, flp));
                    m_nfa.addLink(nextNode, mergeNode);
                    currentNode = nextNode;
                }
                currentNode = mergeNode;
            }
        } else {
            const int delayCycles = getConstInt(delayp->lhsp());
            if (delayCycles < 0) return BuildResult::fail();
            currentNode = addDelayChain(currentNode, delayCycles, flp);
        }

        // Handle RHS -- return it as finalCond, NOT as a Link node
        AstNodeExpr* const exprp = sexprp->exprp();
        if (AstSExpr* const rhsSExprp = VN_CAST(exprp, SExpr)) {
            // Nested SExpr: recurse (its finalCond becomes ours)
            return buildSExpr(rhsSExprp, currentNode);
        }
        // Simple boolean RHS: this is the final condition
        return {currentNode, exprp};
    }

    BuildResult buildConsRep(AstConsRep* repp, int entryNode) {
        FileLine* const flp = repp->fileline();
        AstNodeExpr* const exprp = repp->exprp();
        const int minN = getConstInt(repp->countp());
        if (minN < 0) return BuildResult::fail();

        int currentNode = entryNode;
        for (int i = 0; i < minN; ++i) {
            if (i > 0) {
                const int nextNode = m_nfa.createNode();
                m_nfa.addClockEdge(currentNode, nextNode,
                                   throughoutCond(nullptr, flp));
                currentNode = nextNode;
            }
            // Add bool check as conditioned Link
            const int condNode = m_nfa.createNode();
            m_nfa.addLink(currentNode, condNode,
                          sampled(exprp->cloneTreePure(false)));
            currentNode = condNode;
        }

        if (repp->unbounded()) {
            if (minN == 0) {
                const int waitNode = m_nfa.createNode();
                m_nfa.addClockEdge(currentNode, waitNode,
                                   throughoutCond(nullptr, flp));
                const int checkNode = m_nfa.createNode();
                m_nfa.addLink(waitNode, checkNode,
                              sampled(exprp->cloneTreePure(false)));
                m_nfa.addClockEdge(checkNode, waitNode,
                                   throughoutCond(nullptr, flp));
                m_nfa.addLink(currentNode, checkNode);
                currentNode = checkNode;
            } else {
                const int loopBackNode = m_nfa.createNode();
                m_nfa.addClockEdge(currentNode, loopBackNode,
                                   throughoutCond(nullptr, flp));
                const int reCheckNode = m_nfa.createNode();
                m_nfa.addLink(loopBackNode, reCheckNode,
                              sampled(exprp->cloneTreePure(false)));
                m_nfa.addClockEdge(reCheckNode, loopBackNode,
                                   throughoutCond(nullptr, flp));
                m_nfa.addLink(reCheckNode, currentNode);
            }
        } else if (repp->maxCountp()) {
            const int maxN = getConstInt(repp->maxCountp());
            if (maxN < minN) return BuildResult::fail();
            const int mergeNode = m_nfa.createNode();
            m_nfa.addLink(currentNode, mergeNode);
            for (int i = minN; i < maxN; ++i) {
                const int nextNode = m_nfa.createNode();
                m_nfa.addClockEdge(currentNode, nextNode,
                                   throughoutCond(nullptr, flp));
                const int checkNode = m_nfa.createNode();
                m_nfa.addLink(nextNode, checkNode,
                              sampled(exprp->cloneTreePure(false)));
                m_nfa.addLink(checkNode, mergeNode);
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
            const int waitNode = m_nfa.createNode();
            if (i == 0) {
                m_nfa.addLink(currentNode, waitNode);
            } else {
                m_nfa.addClockEdge(currentNode, waitNode,
                                   throughoutCond(nullptr, flp));
            }
            AstNodeExpr* const notExprp
                = new AstNot{flp, exprp->cloneTreePure(false)};
            notExprp->dtypeSetBit();
            m_nfa.addClockEdge(waitNode, waitNode,
                               throughoutCond(sampled(notExprp), flp));
            const int matchNode = m_nfa.createNode();
            m_nfa.addLink(waitNode, matchNode,
                          throughoutCond(sampled(exprp->cloneTreePure(false)), flp));
            currentNode = matchNode;
        }
        return {currentNode, nullptr};
    }

    BuildResult buildThroughout(AstSThroughout* nodep, int entryNode) {
        // Only handle throughout with simple delay sequences in RHS.
        // Complex RHS (range delays, repetition, nested throughout, SOr/SAnd)
        // may not be correctly guarded -- defer to V3AssertProp.
        AstNodeExpr* const rhsp = nodep->rhsp();
        bool hasComplex = false;
        rhsp->foreach([&hasComplex](const AstConsRep*) { hasComplex = true; });
        if (!hasComplex) rhsp->foreach([&hasComplex](const AstSGotoRep*) { hasComplex = true; });
        if (!hasComplex) rhsp->foreach([&hasComplex](const AstSThroughout*) { hasComplex = true; });
        if (!hasComplex) rhsp->foreach([&hasComplex](const AstSIntersect*) { hasComplex = true; });
        if (!hasComplex) rhsp->foreach([&hasComplex](const AstSOr*) { hasComplex = true; });
        if (!hasComplex) rhsp->foreach([&hasComplex](const AstSAnd*) { hasComplex = true; });
        if (hasComplex) return BuildResult::fail();

        m_throughoutStack.push_back(nodep->lhsp());
        const BuildResult result = buildExpr(rhsp, entryNode);
        m_throughoutStack.pop_back();
        return result;
    }

public:
    explicit SvaNfaBuilder(SvaNfa& nfa)
        : m_nfa{nfa} {}

    // Build NFA for any expression node. Returns {termNode, finalCond}.
    BuildResult buildExpr(AstNodeExpr* nodep, int entryNode) {
        if (AstSExpr* const sexprp = VN_CAST(nodep, SExpr)) {
            return buildSExpr(sexprp, entryNode);
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
            const BuildResult lhs = buildExpr(orp->lhsp(), entryNode);
            const BuildResult rhs = buildExpr(orp->rhsp(), entryNode);
            if (!lhs.valid() || !rhs.valid()) return BuildResult::fail();
            // Merge: both branches connect to a merge node
            const int mergeNode = m_nfa.createNode();
            // If either branch has a finalCond, add it as a conditioned Link
            if (lhs.finalCondp) {
                m_nfa.addLink(lhs.termNode, mergeNode,
                              sampled(lhs.finalCondp->cloneTreePure(false)));
            } else {
                m_nfa.addLink(lhs.termNode, mergeNode);
            }
            if (rhs.finalCondp) {
                m_nfa.addLink(rhs.termNode, mergeNode,
                              sampled(rhs.finalCondp->cloneTreePure(false)));
            } else {
                m_nfa.addLink(rhs.termNode, mergeNode);
            }
            return {mergeNode, nullptr};
        }
        if (AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            const BuildResult lhs = buildExpr(orp->lhsp(), entryNode);
            const BuildResult rhs = buildExpr(orp->rhsp(), entryNode);
            if (!lhs.valid() || !rhs.valid()) return BuildResult::fail();
            const int mergeNode = m_nfa.createNode();
            if (lhs.finalCondp) {
                m_nfa.addLink(lhs.termNode, mergeNode,
                              sampled(lhs.finalCondp->cloneTreePure(false)));
            } else {
                m_nfa.addLink(lhs.termNode, mergeNode);
            }
            if (rhs.finalCondp) {
                m_nfa.addLink(rhs.termNode, mergeNode,
                              sampled(rhs.finalCondp->cloneTreePure(false)));
            } else {
                m_nfa.addLink(rhs.termNode, mergeNode);
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
        m_nfa.startNode = m_nfa.createNode();
        return buildExpr(exprp, m_nfa.startNode);
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

        // Create state registers
        std::vector<AstVar*> stateVars(N, nullptr);
        for (int i = 0; i < N; ++i) {
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

        // Phase 3: Compute accept/reject from Links to accept node + acceptCondp
        // The accept node receives Links from terminal NFA nodes.
        // acceptCondp is the final boolean check from the builder.
        //
        // terminal_active = OR of (state_sig[source] && link_condition) for Links to accept
        // accept = terminal_active && acceptCondp
        // reject = terminal_active && !acceptCondp

        AstNodeExpr* terminalActivep = nullptr;
        for (const auto& edge : nfa.edges) {
            if (edge.toId != nfa.acceptNode) continue;
            if (edge.consumesCycle) continue;
            if (!stateSig[edge.fromId]) continue;

            AstNodeExpr* srcSig = stateSig[edge.fromId]->cloneTreePure(false);
            srcSig = andCond(flp, srcSig, edge.condp);
            terminalActivep = orExprs(flp, terminalActivep, srcSig);
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

        // Clean up ALL stateSig entries -- they've been cloned where needed
        // (Phase 2/3 always clone from stateSig before incorporating into AST)
        for (int i = 0; i < N; ++i) {
            if (stateSig[i]) {
                stateSig[i]->deleteTree();
                stateSig[i] = nullptr;
            }
        }
        // Clean up disableExprp if passed (was cloned in Phase 2, original not attached)
        if (disableExprp) {
            disableExprp->deleteTree();
            disableExprp = nullptr;
        }

        if (isCover) {
            // Cover property uses accept signal, not !reject
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
        if (acceptCondp) {
            // reject = terminal_active && !$sampled(acceptCond)
            AstNodeExpr* const sampledCondp
                = new AstSampled{flp, acceptCondp->cloneTreePure(false)};
            sampledCondp->dtypeFrom(acceptCondp);
            AstNodeExpr* const notCondp = new AstNot{flp, sampledCondp};
            notCondp->dtypeSetBit();
            AstNodeExpr* const rejectp
                = new AstAnd{flp, terminalActivep, notCondp};
            rejectp->dtypeSetBit();
            AstNodeExpr* const resultExprp = new AstNot{flp, rejectp};
            resultExprp->dtypeSetBit();
            return resultExprp;
        }
        // No condition: unconditional accept → never rejects
        return new AstConst{flp, AstConst::BitTrue{}};
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
                result = builder.buildExpr(parts.seqExprp, trigNode);
            } else {
                const int trigNode = nfa.createNode();
                nfa.addLink(nfa.startNode, trigNode,
                            new AstSampled{flp, parts.triggerExprp->cloneTreePure(false)});
                const int delayNode = nfa.createNode();
                nfa.addClockEdge(trigNode, delayNode);
                result = builder.buildExpr(parts.seqExprp, delayNode);
            }

            if (result.valid()) {
                nfa.createAcceptNode();
                // Accept Link is unconditional. The finalCond is passed
                // separately to the emitter for accept/reject computation.
                nfa.addLink(result.termNode, nfa.acceptNode);
            }
        } else {
            // Standalone sequence
            result = builder.build(parts.seqExprp);
            if (result.valid()) {
                nfa.createAcceptNode();
                nfa.addLink(result.termNode, nfa.acceptNode);
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
