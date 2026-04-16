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
//  - Replace converted assertions with combinational match/reject checks
//    so V3AssertPre sees no multi-cycle SExpr (unsupported ones fall through).
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3AssertNfa.h"

#include "V3Const.h"
#include "V3Graph.h"
#include "V3Task.h"
#include "V3UniqueNames.h"

#include <set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// NFA Graph Data Structures (V3Graph-derived per upstream convention)

namespace {

class SvaStateVertex;

// Per-vertex algorithm data, stored via V3GraphVertex::userp() during lowering
struct SvaVertexData final {
    AstVar* stateVarp = nullptr;  // NBA state register for this vertex
    AstVar* counterActiveVarp = nullptr;  // Counter FSM active flag
    AstVar* counterCountVarp = nullptr;  // Counter FSM count register
    AstVar* doneLVarp = nullptr;  // SAnd LHS done-latch
    AstVar* doneRVarp = nullptr;  // SAnd RHS done-latch
    AstNodeExpr* stateSigp = nullptr;  // Combinational state signal (owned during lowering)
    bool needsReg = false;  // True if vertex has incoming clocked edge
};

// NFA state vertex -- one per NFA position in the sequence evaluation
class SvaStateVertex final : public V3GraphVertex {
    VL_RTTI_IMPL(SvaStateVertex, V3GraphVertex)
public:
    // True if this is the sequence-match terminal vertex
    bool m_isMatch = false;
    // Owned throughout-guard condition clones; IEEE 1800-2023 16.9.9
    std::vector<AstNodeExpr*> m_throughoutConds;
    // Counter FSM vertex for ##[M:N] when N-M > kChainLimit
    bool m_isCounter = false;
    int m_counterMin = 0;  // Counter window minimum
    int m_counterMax = 0;  // Counter window maximum
    // Liveness terminal (IEEE weak semantics): reject must not fire from this source
    bool m_isUnbounded = false;
    // Temporal sequence AND combiner; IEEE 1800-2023 16.9.5
    bool m_isAndCombiner = false;
    SvaStateVertex* m_andLhsTermp = nullptr;  // LHS sub-NFA terminal vertex
    SvaStateVertex* m_andRhsTermp = nullptr;  // RHS sub-NFA terminal vertex
    AstNodeExpr* m_andLhsCondp = nullptr;  // OWNED; LHS final condition (may be nullptr)
    AstNodeExpr* m_andRhsCondp = nullptr;  // OWNED; RHS final condition (may be nullptr)
    // Reject sink for SAnd rejectOnFail wiring; not a state-signal source
    bool m_isRejectSink = false;

    // CONSTRUCTORS
    explicit SvaStateVertex(V3Graph* graphp)
        : V3GraphVertex{graphp} {}
    ~SvaStateVertex() override {
        for (AstNodeExpr* cp : m_throughoutConds) cp->deleteTree();
        if (m_andLhsCondp) m_andLhsCondp->deleteTree();
        if (m_andRhsCondp) m_andRhsCondp->deleteTree();
    }
    // METHODS
    string name() const override { return "s" + cvtToStr(color()); }
    string dotColor() const override {
        if (m_isMatch) return "red";
        if (m_isCounter) return "blue";
        if (m_isAndCombiner) return "purple";
        return "black";
    }
    // Access per-vertex algorithm data (valid only during lowering phase)
    SvaVertexData* datap() const { return static_cast<SvaVertexData*>(userp()); }
};

// NFA transition edge -- clocked (##1) or combinational link (##0)
class SvaTransEdge final : public V3GraphEdge {
    VL_RTTI_IMPL(SvaTransEdge, V3GraphEdge)
public:
    AstNodeExpr* m_condp;  // Transition condition; nullptr = unconditional; OWNED
    bool m_consumesCycle;  // true = clocked edge (##1), false = link (##0/boolean)
    // Reject when source is active and condp is false; set only on
    // outermost required-step Link
    bool m_rejectOnFail = false;

    // CONSTRUCTORS
    SvaTransEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top, AstNodeExpr* condp,
                 bool consumesCycle)
        : V3GraphEdge{graphp, fromp, top, /*weight=*/1}
        , m_condp{condp}
        , m_consumesCycle{consumesCycle} {}
    ~SvaTransEdge() override {
        if (m_condp) m_condp->deleteTree();
    }
    // METHODS
    string dotLabel() const override { return m_consumesCycle ? "##1" : "link"; }
    string dotStyle() const override { return m_consumesCycle ? "" : "dashed"; }
    // Typed accessors for NFA vertices
    SvaStateVertex* fromVtxp() const { return static_cast<SvaStateVertex*>(fromp()); }
    SvaStateVertex* toVtxp() const { return static_cast<SvaStateVertex*>(top()); }
};

// NFA graph container
class SvaGraph final {
public:
    V3Graph m_graph;  // Owns all vertices and edges
    SvaStateVertex* m_startVertexp = nullptr;  // Trigger/start vertex
    SvaStateVertex* m_matchVertexp = nullptr;  // Sequence-match terminal vertex

    // Create a new state vertex
    SvaStateVertex* createStateVertex() { return new SvaStateVertex{&m_graph}; }
    // Create the match terminal vertex
    SvaStateVertex* createMatchVertex() {
        SvaStateVertex* const vtxp = createStateVertex();
        vtxp->m_isMatch = true;
        m_matchVertexp = vtxp;
        return vtxp;
    }
    // Add a clocked transition edge (##1)
    SvaTransEdge* addClockedEdge(SvaStateVertex* fromp, SvaStateVertex* top,
                                 AstNodeExpr* condp = nullptr) {
        return new SvaTransEdge{&m_graph, fromp, top, condp, /*consumesCycle=*/true};
    }
    // Add a combinational link (##0 / boolean condition)
    SvaTransEdge* addLink(SvaStateVertex* fromp, SvaStateVertex* top,
                          AstNodeExpr* condp = nullptr) {
        return new SvaTransEdge{&m_graph, fromp, top, condp, /*consumesCycle=*/false};
    }
    // Collect all edges into a flat vector for iteration.
    // Used by the lowering phase which needs global edge scans.
    std::vector<const SvaTransEdge*> allEdges() const {
        std::vector<const SvaTransEdge*> result;
        for (const V3GraphVertex& vtxr : m_graph.vertices()) {
            for (const V3GraphEdge& er : vtxr.outEdges()) {
                result.push_back(static_cast<const SvaTransEdge*>(&er));
            }
        }
        return result;
    }
};

//######################################################################
// Builder result: terminal vertex + optional final condition (match Link condition).
struct BuildResult final {
    SvaStateVertex* termVertexp;  // Primary terminal; contributes to both match and reject
    AstNodeExpr* finalCondp;  // nullptr = unconditional
    // Mid-window sources for range delays (pure boolean RHS): match-only (isUnbounded)
    std::vector<SvaStateVertex*> midSources;
    bool errorEmitted = false;  // Builder already emitted specific error; skip generic
    bool valid() const { return termVertexp != nullptr; }
    static BuildResult fail(bool errored = false) { return {nullptr, nullptr, {}, errored}; }
    static BuildResult failWithError() { return {nullptr, nullptr, {}, true}; }
};

//######################################################################
// NFA Builder

class SvaNfaBuilder final {
    SvaGraph& m_graph;  // NFA graph being built
    std::vector<AstNodeExpr*> m_throughoutStack;  // Active throughout guards (IEEE 16.9.9)
    bool m_inUnboundedScope = false;  // Sticky: nodes created after inherit liveness

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
            }
        }
        if (baseCondp) { guardp = new AstAnd{flp, baseCondp, guardp}; }
        return guardp;
    }

    static int getConstInt(AstNodeExpr* exprp) {
        AstNodeExpr* const constp = V3Const::constifyEdit(exprp->cloneTreePure(false));
        const AstConst* const cp = VN_CAST(constp, Const);
        const int val = cp ? cp->toSInt() : -1;
        VL_DO_DANGLING(constp->deleteTree(), constp);
        return val;
    }

    // Static fixed-length analysis: clock ticks from entry to terminal, or -1.
    // Used by SIntersect to verify IEEE 1800-2023 16.9.6 equal-length precondition.
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

    // Create vertex and inherit throughout guards from current scope (IEEE 16.9.9).
    SvaStateVertex* scopedCreateVertex() {
        SvaStateVertex* const vtxp = m_graph.createStateVertex();
        for (AstNodeExpr* const cp : m_throughoutStack) {
            vtxp->m_throughoutConds.push_back(cp->cloneTreePure(false));
        }
        if (m_inUnboundedScope) vtxp->m_isUnbounded = true;
        return vtxp;
    }

    // AND current throughout stack into every edge/link (IEEE 16.9.9 invariant).
    SvaTransEdge* guardedLink(SvaStateVertex* fromp, SvaStateVertex* top, AstNodeExpr* condp,
                              FileLine* flp) {
        return m_graph.addLink(fromp, top, throughoutCond(condp, flp));
    }
    SvaTransEdge* guardedLink(SvaStateVertex* fromp, SvaStateVertex* top, FileLine* flp) {
        return m_graph.addLink(fromp, top, throughoutCond(nullptr, flp));
    }
    SvaTransEdge* guardedEdge(SvaStateVertex* fromp, SvaStateVertex* top, AstNodeExpr* condp,
                              FileLine* flp) {
        return m_graph.addClockedEdge(fromp, top, throughoutCond(condp, flp));
    }
    SvaTransEdge* guardedEdge(SvaStateVertex* fromp, SvaStateVertex* top, FileLine* flp) {
        return m_graph.addClockedEdge(fromp, top, throughoutCond(nullptr, flp));
    }

    SvaStateVertex* addDelayChain(SvaStateVertex* startp, int n, FileLine* flp) {
        SvaStateVertex* currentp = startp;
        for (int i = 0; i < n; ++i) {
            SvaStateVertex* const nextp = scopedCreateVertex();
            guardedEdge(currentp, nextp, flp);
            currentp = nextp;
        }
        return currentp;
    }

    // Build NFA for an SExpr. finalCond = RHS (not yet added as a vertex).
    // isTopLevelStep: marks outermost required boolean check as rejectOnFail.
    BuildResult buildSExpr(AstSExpr* sexprp, SvaStateVertex* entryVtxp,
                           bool isTopLevelStep = false) {
        AstDelay* const delayp = VN_CAST(sexprp->delayp(), Delay);
        if (!delayp || !delayp->isCycleDelay()) return BuildResult::fail();

        FileLine* const flp = sexprp->fileline();

        // Handle LHS (preExpr)
        SvaStateVertex* currentp = entryVtxp;
        if (AstNodeExpr* const preExprp = sexprp->preExprp()) {
            const BuildResult pre = buildExpr(preExprp, currentp, isTopLevelStep);
            if (!pre.valid()) return BuildResult::fail(pre.errorEmitted);
            if (pre.finalCondp) {
                SvaStateVertex* const condVtxp = scopedCreateVertex();
                SvaTransEdge* const edgep = guardedLink(
                    pre.termVertexp, condVtxp, sampled(pre.finalCondp->cloneTreePure(false)), flp);
                if (isTopLevelStep && !pre.termVertexp->m_isUnbounded && !m_inUnboundedScope) {
                    // Do not mark liveness sources: first boolean check is deferred.
                    edgep->m_rejectOnFail = true;
                }
                currentp = condVtxp;
            } else {
                currentp = pre.termVertexp;
            }
        }

        // Handle delay
        std::vector<SvaStateVertex*> rangeMidSources;
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
                currentp = addDelayChain(currentp, minDelay, flp);
                guardedEdge(currentp, currentp, flp);
                currentp->m_isUnbounded = true;
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
                    currentp = addDelayChain(currentp, minDelay, flp);
                } else {
                    const int range = maxDelay - minDelay;
                    currentp = addDelayChain(currentp, minDelay, flp);
                    constexpr int kChainLimit = 256;
                    AstNodeExpr* const exprp = sexprp->exprp();
                    const bool nestedRhs = VN_IS(exprp, SExpr);
                    if (range > kChainLimit) {
                        // Large range: counter FSM. Overlapping triggers during an
                        // active count are dropped (non-overlapping semantics only).
                        SvaStateVertex* const counterVtxp = scopedCreateVertex();
                        counterVtxp->m_isCounter = true;
                        counterVtxp->m_counterMin = 0;
                        counterVtxp->m_counterMax = range;
                        guardedEdge(currentp, counterVtxp, flp);
                        currentp = counterVtxp;
                    } else if (nestedRhs) {
                        // Merge all [M,N] positions; continuation is per-attempt.
                        SvaStateVertex* const mergeVtxp = scopedCreateVertex();
                        guardedLink(currentp, mergeVtxp, flp);
                        for (int i = 0; i < range; ++i) {
                            SvaStateVertex* const nextVtxp = scopedCreateVertex();
                            guardedEdge(currentp, nextVtxp, flp);
                            guardedLink(nextVtxp, mergeVtxp, flp);
                            currentp = nextVtxp;
                        }
                        currentp = mergeVtxp;
                    } else {
                        // Pure boolean RHS: register chain. Each mid-position links
                        // to match (match-only); last position is the reject source.
                        rangeMidSources.push_back(currentp);
                        for (int i = 0; i < range; ++i) {
                            SvaStateVertex* const nextVtxp = scopedCreateVertex();
                            AstNodeExpr* const notExprp
                                = new AstNot{flp, sampled(exprp->cloneTreePure(false))};
                            notExprp->dtypeSetBit();
                            guardedEdge(currentp, nextVtxp, notExprp, flp);
                            if (i < range - 1) rangeMidSources.push_back(nextVtxp);
                            currentp = nextVtxp;
                        }
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
            currentp = addDelayChain(currentp, delayCycles, flp);
        }

        // Multi-cycle RHS: recurse (only plain boolean is returned as finalCondp).
        AstNodeExpr* const exprp = sexprp->exprp();
        if (VN_IS(exprp, SExpr) || VN_IS(exprp, SAnd) || VN_IS(exprp, SOr)
            || VN_IS(exprp, SConsRep) || VN_IS(exprp, SGotoRep) || VN_IS(exprp, SThroughout)
            || VN_IS(exprp, SIntersect) || VN_IS(exprp, SNonConsRep)) {
            return buildExpr(exprp, currentp, isTopLevelStep);
        }
        return {currentp, exprp, std::move(rangeMidSources)};
    }

    BuildResult buildConsRep(AstSConsRep* repp, SvaStateVertex* entryVtxp,
                             bool isTopLevelStep = false) {
        FileLine* const flp = repp->fileline();
        AstNodeExpr* const exprp = repp->exprp();
        // Multi-cycle expr in ConsRep not yet supported; bail to avoid invalid AST.
        if (VN_IS(exprp, SExpr) || VN_IS(exprp, SThroughout) || VN_IS(exprp, SAnd)
            || VN_IS(exprp, SOr) || VN_IS(exprp, SConsRep) || VN_IS(exprp, SGotoRep)
            || VN_IS(exprp, SIntersect) || VN_IS(exprp, SNonConsRep)) {
            repp->v3warn(E_UNSUPPORTED, "Unsupported: multi-cycle sequence expression inside"
                                        " consecutive repetition (IEEE 1800-2023 16.9.2)");
            return BuildResult::failWithError();
        }
        const int minN = getConstInt(repp->countp());
        if (minN < 0) return BuildResult::fail();
        // Bail on large exact repetitions (no counter FSM for ConsRep yet).
        constexpr int kConsRepLimit = 256;
        if (minN > kConsRepLimit && !repp->unbounded() && !repp->maxCountp()) {
            return BuildResult::fail();
        }

        SvaStateVertex* currentp = entryVtxp;
        for (int i = 0; i < minN; ++i) {
            if (i > 0) {
                SvaStateVertex* const nextp = scopedCreateVertex();
                guardedEdge(currentp, nextp, flp);
                currentp = nextp;
            }
            // Mark first and last boolean Links as rejectOnFail for correct
            // reject on standalone ConsRep.
            SvaStateVertex* const condVtxp = scopedCreateVertex();
            SvaTransEdge* const linkp
                = guardedLink(currentp, condVtxp, sampled(exprp->cloneTreePure(false)), flp);
            if (isTopLevelStep && (i == 0 || i == minN - 1)) { linkp->m_rejectOnFail = true; }
            currentp = condVtxp;
        }

        if (repp->unbounded()) {
            if (minN == 0) {
                SvaStateVertex* const waitVtxp = scopedCreateVertex();
                guardedEdge(currentp, waitVtxp, flp);
                SvaStateVertex* const checkVtxp = scopedCreateVertex();
                guardedLink(waitVtxp, checkVtxp, sampled(exprp->cloneTreePure(false)), flp);
                guardedEdge(checkVtxp, waitVtxp, flp);
                guardedLink(currentp, checkVtxp, flp);
                currentp = checkVtxp;
            } else {
                SvaStateVertex* const loopBackVtxp = scopedCreateVertex();
                guardedEdge(currentp, loopBackVtxp, flp);
                SvaStateVertex* const reCheckVtxp = scopedCreateVertex();
                guardedLink(loopBackVtxp, reCheckVtxp, sampled(exprp->cloneTreePure(false)), flp);
                guardedEdge(reCheckVtxp, loopBackVtxp, flp);
                guardedLink(reCheckVtxp, currentp, flp);
            }
            currentp->m_isUnbounded = true;
            m_inUnboundedScope = true;
        } else if (repp->maxCountp()) {
            const int maxN = getConstInt(repp->maxCountp());
            if (maxN < minN) return BuildResult::fail();
            SvaStateVertex* const mergeVtxp = scopedCreateVertex();
            guardedLink(currentp, mergeVtxp, flp);
            for (int i = minN; i < maxN; ++i) {
                SvaStateVertex* const nextVtxp = scopedCreateVertex();
                guardedEdge(currentp, nextVtxp, flp);
                SvaStateVertex* const checkVtxp = scopedCreateVertex();
                guardedLink(nextVtxp, checkVtxp, sampled(exprp->cloneTreePure(false)), flp);
                guardedLink(checkVtxp, mergeVtxp, flp);
                currentp = checkVtxp;
            }
            currentp = mergeVtxp;
        }
        // finalCond = nullptr (already checked via Links)
        return {currentp, nullptr, {}};
    }

    BuildResult buildGotoRep(AstSGotoRep* repp, SvaStateVertex* entryVtxp) {
        FileLine* const flp = repp->fileline();
        AstNodeExpr* const exprp = repp->exprp();
        const int n = getConstInt(repp->countp());
        if (n <= 0) return BuildResult::fail();

        SvaStateVertex* currentp = entryVtxp;
        for (int i = 0; i < n; ++i) {
            SvaStateVertex* const waitVtxp = scopedCreateVertex();
            // Edge (not Link) for all iterations: IEEE expansion ##1 before each
            // match. A Link at i==0 was wrong -- it allowed same-cycle matching
            // and was discarded by Phase 2 (waitNode has a self-loop Edge).
            guardedEdge(currentp, waitVtxp, flp);
            AstNodeExpr* const notExprp = new AstNot{flp, exprp->cloneTreePure(false)};
            notExprp->dtypeSetBit();
            guardedEdge(waitVtxp, waitVtxp, sampled(notExprp), flp);
            SvaStateVertex* const matchVtxp = scopedCreateVertex();
            guardedLink(waitVtxp, matchVtxp, sampled(exprp->cloneTreePure(false)), flp);
            currentp = matchVtxp;
        }
        currentp->m_isUnbounded = true;  // [->N] waits unboundedly
        m_inUnboundedScope = true;
        return {currentp, nullptr, {}};
    }

    // Build merge vertex for SOr / LogOr: both branches feed into one vertex.
    BuildResult buildOrMerge(AstNodeExpr* lhsp, AstNodeExpr* rhsp, SvaStateVertex* entryVtxp,
                             FileLine* flp) {
        const BuildResult lhs = buildExpr(lhsp, entryVtxp);
        const BuildResult rhs = buildExpr(rhsp, entryVtxp);
        if (!lhs.valid() || !rhs.valid()) {
            return BuildResult::fail(lhs.errorEmitted || rhs.errorEmitted);
        }
        SvaStateVertex* const mergeVtxp = scopedCreateVertex();
        if (lhs.finalCondp) {
            guardedLink(lhs.termVertexp, mergeVtxp, sampled(lhs.finalCondp->cloneTreePure(false)),
                        flp);
        } else {
            guardedLink(lhs.termVertexp, mergeVtxp, flp);
        }
        if (rhs.finalCondp) {
            guardedLink(rhs.termVertexp, mergeVtxp, sampled(rhs.finalCondp->cloneTreePure(false)),
                        flp);
        } else {
            guardedLink(rhs.termVertexp, mergeVtxp, flp);
        }
        return {mergeVtxp, nullptr, {}};
    }

    // Build done-latch combiner for SAnd/SIntersect (IEEE 1800-2023 16.9.5).
    BuildResult buildAndCombiner(AstNodeExpr* lhsExprp, AstNodeExpr* rhsExprp,
                                 SvaStateVertex* entryVtxp, FileLine* flp) {
        // Snapshot-restore scope so LHS liveness does not leak into RHS.
        const bool savedScope = m_inUnboundedScope;
        const BuildResult lhs = buildExpr(lhsExprp, entryVtxp);
        const bool lhsScope = m_inUnboundedScope;
        m_inUnboundedScope = savedScope;
        const BuildResult rhs = buildExpr(rhsExprp, entryVtxp);
        const bool rhsScope = m_inUnboundedScope;
        m_inUnboundedScope = savedScope || lhsScope || rhsScope;
        if (!lhs.valid() || !rhs.valid()) {
            return BuildResult::fail(lhs.errorEmitted || rhs.errorEmitted);
        }

        // Single-cycle operands: use boolean AND (done-latch would fire across cycles).
        if (lhs.termVertexp == entryVtxp && rhs.termVertexp == entryVtxp) {
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
            return {entryVtxp, condp, {}};
        }
        // Range-delay mid-window sources in either sub-branch would need
        // to be folded into the latch's match-now signal, which the
        // current combiner does not support. Defer (UNSUPPORTED).
        if (!lhs.midSources.empty() || !rhs.midSources.empty()) return BuildResult::fail();
        SvaStateVertex* const combVtxp = scopedCreateVertex();
        combVtxp->m_isAndCombiner = true;
        combVtxp->m_andLhsTermp = lhs.termVertexp;
        combVtxp->m_andRhsTermp = rhs.termVertexp;
        if (lhs.finalCondp) combVtxp->m_andLhsCondp = lhs.finalCondp->cloneTreePure(false);
        if (rhs.finalCondp) combVtxp->m_andRhsCondp = rhs.finalCondp->cloneTreePure(false);
        if (lhs.termVertexp->m_isUnbounded || rhs.termVertexp->m_isUnbounded) {
            combVtxp->m_isUnbounded = true;
        }
        // Wire terminal-boolean rejects to a dedicated sink so each side can fail
        // the AND independently. Skip for liveness or single-cycle operands
        // (single-cycle termVertexp == entryVtxp would fire every cycle).
        if (!combVtxp->m_isUnbounded) {
            bool needSink = false;
            const bool lhsMultiCycle = (lhs.termVertexp != entryVtxp);
            const bool rhsMultiCycle = (rhs.termVertexp != entryVtxp);
            if (lhs.finalCondp && lhsMultiCycle && !lhs.termVertexp->m_isUnbounded) {
                needSink = true;
            }
            if (rhs.finalCondp && rhsMultiCycle && !rhs.termVertexp->m_isUnbounded) {
                needSink = true;
            }
            if (needSink) {
                SvaStateVertex* const sinkVtxp = m_graph.createStateVertex();
                sinkVtxp->m_isRejectSink = true;
                if (lhs.finalCondp && lhsMultiCycle && !lhs.termVertexp->m_isUnbounded) {
                    SvaTransEdge* const ep = m_graph.addLink(
                        lhs.termVertexp, sinkVtxp, sampled(lhs.finalCondp->cloneTreePure(false)));
                    ep->m_rejectOnFail = true;
                }
                if (rhs.finalCondp && rhsMultiCycle && !rhs.termVertexp->m_isUnbounded) {
                    SvaTransEdge* const ep = m_graph.addLink(
                        rhs.termVertexp, sinkVtxp, sampled(rhs.finalCondp->cloneTreePure(false)));
                    ep->m_rejectOnFail = true;
                }
            }
        }
        return {combVtxp, nullptr, {}};
    }

    BuildResult buildThroughout(AstSThroughout* nodep, SvaStateVertex* entryVtxp,
                                bool isTopLevelStep = false) {
        // Mark entryVtxp so "cond false at tick 0" is detected as throughout-drop.
        entryVtxp->m_throughoutConds.push_back(nodep->lhsp()->cloneTreePure(false));
        m_throughoutStack.push_back(nodep->lhsp());
        const BuildResult result = buildExpr(nodep->rhsp(), entryVtxp, isTopLevelStep);
        m_throughoutStack.pop_back();
        return result;
    }

public:
    explicit SvaNfaBuilder(SvaGraph& graph)
        : m_graph{graph} {}

    // Reset scope between antecedent and consequent: liveness must not leak.
    void resetScope() {
        m_inUnboundedScope = false;
        m_throughoutStack.clear();
    }

    BuildResult buildExpr(AstNodeExpr* nodep, SvaStateVertex* entryVtxp,
                          bool isTopLevelStep = false) {
        if (AstSExpr* const sexprp = VN_CAST(nodep, SExpr)) {
            return buildSExpr(sexprp, entryVtxp, isTopLevelStep);
        }
        if (AstSConsRep* const repp = VN_CAST(nodep, SConsRep)) {
            return buildConsRep(repp, entryVtxp, isTopLevelStep);
        }
        if (AstSGotoRep* const repp = VN_CAST(nodep, SGotoRep)) {
            return buildGotoRep(repp, entryVtxp);
        }
        if (AstSThroughout* const throughoutp = VN_CAST(nodep, SThroughout)) {
            return buildThroughout(throughoutp, entryVtxp, isTopLevelStep);
        }
        if (AstSOr* const orp = VN_CAST(nodep, SOr)) {
            return buildOrMerge(orp->lhsp(), orp->rhsp(), entryVtxp, orp->fileline());
        }
        if (AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            return buildOrMerge(orp->lhsp(), orp->rhsp(), entryVtxp, orp->fileline());
        }
        if (AstSAnd* const andp = VN_CAST(nodep, SAnd)) {
            return buildAndCombiner(andp->lhsp(), andp->rhsp(), entryVtxp, andp->fileline());
        }
        if (AstSIntersect* const intp = VN_CAST(nodep, SIntersect)) {
            // IEEE 1800-2023 16.9.6: SAnd with equal-length constraint.
            // Variable-length intersect deferred to follow-up.
            const int lhsLen = fixedLength(intp->lhsp());
            const int rhsLen = fixedLength(intp->rhsp());
            if (lhsLen < 0 || rhsLen < 0) return BuildResult::fail();
            if (lhsLen != rhsLen) {
                intp->v3error("Intersect sequence length mismatch: left " + std::to_string(lhsLen)
                              + " cycles, right " + std::to_string(rhsLen)
                              + " cycles (IEEE 1800-2023 16.9.6)");
                return BuildResult::failWithError();
            }
            return buildAndCombiner(intp->lhsp(), intp->rhsp(), entryVtxp, intp->fileline());
        }
        if (VN_IS(nodep, SNonConsRep)) return BuildResult::fail();
        // Boolean leaf (including LogAnd): return as finalCond
        return {entryVtxp, nodep, {}};
    }

    BuildResult build(AstNodeExpr* exprp) {
        m_graph.m_startVertexp = scopedCreateVertex();
        return buildExpr(exprp, m_graph.m_startVertexp, /*isTopLevelStep=*/true);
    }
};

//######################################################################
// NFA Lowering (converts NFA graph to synthesizable AstAlways blocks)

class SvaNfaLowering final {
    AstNodeModule* const m_modp;  // Module to add state vars and always blocks to
    V3UniqueNames m_names{"__Vnfa"};

    // Build a match-now expression: stateSig[i] && $sampled(condp)
    static AstNodeExpr* buildMatchNow(FileLine* flp, AstNodeExpr* stateExprp, AstNodeExpr* condp) {
        AstNodeExpr* const statep = stateExprp->cloneTreePure(false);
        if (!condp) return statep;
        AstSampled* const sampp = new AstSampled{flp, condp->cloneTreePure(false)};
        sampp->dtypeFrom(condp);
        return new AstAnd{flp, statep, sampp};
    }
    static AstNodeExpr* andCond(FileLine* flp, AstNodeExpr* exprp, AstNodeExpr* condp) {
        if (!condp) return exprp;
        return new AstAnd{flp, exprp, condp->cloneTreePure(false)};
    }
    static AstNodeExpr* orExprs(FileLine* flp, AstNodeExpr* ap, AstNodeExpr* bp) {
        if (!ap) return bp;
        if (!bp) return ap;
        return new AstOr{flp, ap, bp};
    }

public:
    explicit SvaNfaLowering(AstNodeModule* modp)
        : m_modp{modp} {}

    // Lower NFA graph to synthesizable AstAlways blocks with state registers.
    // Links are combinational; Edges are registered (NBA).
    // Returns !reject for assert/assume, or match for cover.
    AstNodeExpr* lower(FileLine* flp, SvaGraph& graph, AstNodeExpr* triggerExprp,
                       AstSenTree* senTreep, AstNodeExpr* matchCondp, bool isCover,
                       AstNodeExpr* disableExprp = nullptr, bool negated = false,
                       AstNodeExpr** outMatchpp = nullptr, AstVar* disableCntVarp = nullptr,
                       AstVar* snapshotVarp = nullptr,
                       std::vector<AstNodeExpr*>* outRequiredStepSrcsp = nullptr) {
        const std::string baseName = m_names.get("");

        // Number vertices with sequential colors for array indexing.
        int N = 0;
        for (V3GraphVertex& vtxr : graph.m_graph.vertices()) { vtxr.color(N++); }
        // Build vertex lookup array (color → vertex pointer).
        std::vector<SvaStateVertex*> vtx(N, nullptr);
        for (V3GraphVertex& vtxr : graph.m_graph.vertices()) {
            vtx[vtxr.color()] = static_cast<SvaStateVertex*>(&vtxr);
        }
        const int startIdx = graph.m_startVertexp->color();
        const int matchIdx = graph.m_matchVertexp ? graph.m_matchVertexp->color() : -1;
        const std::vector<const SvaTransEdge*> edges = graph.allEdges();

        // Identify registered vertices (targets of clocked edges)
        std::vector<bool> needsReg(N, false);
        for (int i = 0; i < N; ++i) {
            for (const V3GraphEdge& er : vtx[i]->outEdges()) {
                const SvaTransEdge& te = static_cast<const SvaTransEdge&>(er);
                const int toIdx = te.toVtxp()->color();
                if (te.m_consumesCycle && toIdx != matchIdx && !te.toVtxp()->m_isRejectSink) {
                    needsReg[toIdx] = true;
                }
            }
        }

        std::vector<AstVar*> stateVars(N, nullptr);
        std::vector<AstVar*> counterActiveVars(N, nullptr);
        std::vector<AstVar*> counterCountVars(N, nullptr);
        std::vector<AstVar*> doneLVars(N, nullptr);
        std::vector<AstVar*> doneRVars(N, nullptr);
        AstNodeDType* const u32DTypep = m_modp->findBasicDType(VBasicDTypeKwd::UINT32);
        for (int i = 0; i < N; ++i) {
            if (vtx[i]->m_isAndCombiner) {
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
            if (vtx[i]->m_isCounter) {
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
            if (i == startIdx || vtx[i]->m_isMatch) continue;
            const std::string varName = baseName + "__s" + std::to_string(i);
            AstVar* const varp
                = new AstVar{flp, VVarType::MODULETEMP, varName, m_modp->findBitDType()};
            varp->lifetime(VLifetime::STATIC_EXPLICIT);
            m_modp->addStmtsp(varp);
            stateVars[i] = varp;
        }

        // Phase 1: Resolve Links (combinational state_sig)
        std::vector<AstNodeExpr*> stateSig(N, nullptr);
        stateSig[startIdx] = triggerExprp->cloneTreePure(false);
        for (int i = 0; i < N; ++i) {
            if (stateVars[i]) {
                stateSig[i] = new AstVarRef{flp, stateVars[i], VAccess::READ};
            } else if (counterActiveVars[i]) {
                AstVarRef* const activeRefp
                    = new AstVarRef{flp, counterActiveVars[i], VAccess::READ};
                if (vtx[i]->m_counterMin == 0) {
                    stateSig[i] = activeRefp;
                } else {
                    AstGte* const gtep
                        = new AstGte{flp, new AstVarRef{flp, counterCountVars[i], VAccess::READ},
                                     new AstConst{flp, AstConst::WidthedValue{}, 32,
                                                  static_cast<uint32_t>(vtx[i]->m_counterMin)}};
                    gtep->dtypeSetBit();
                    AstNodeExpr* const andp = new AstAnd{flp, activeRefp, gtep};
                    andp->dtypeSetBit();
                    stateSig[i] = andp;
                }
            }
        }

        // Fixed-point propagation along zero-delay (Link) edges.
        // Worst case: longest chain of Link edges is N hops; SAnd combiner
        // seeding adds one extra round; factor-of-2 covers reverse-order
        // dependencies.  Static NFA size is bounded by kChainLimit (256)
        // plus small per-operator overhead, so this is O(N^2) at compile
        // time only -- not simulation time.
        for (int pass = 0; pass < 2 * N + 2; ++pass) {
            bool changed = false;
            // Seed SAnd combiners inside the fixed-point (sub-NFA termNodes
            // may be Link-propagated and only available after a pass).
            for (int i = 0; i < N; ++i) {
                if (!vtx[i]->m_isAndCombiner) continue;
                if (stateSig[i]) continue;
                const int l = vtx[i]->m_andLhsTermp ? vtx[i]->m_andLhsTermp->color() : -1;
                const int r = vtx[i]->m_andRhsTermp ? vtx[i]->m_andRhsTermp->color() : -1;
                if (l < 0 || r < 0) continue;
                if (!stateSig[l] || !stateSig[r]) continue;

                AstNodeExpr* const matchLNowp
                    = buildMatchNow(flp, stateSig[l], vtx[i]->m_andLhsCondp);
                AstNodeExpr* const matchRNowp
                    = buildMatchNow(flp, stateSig[r], vtx[i]->m_andRhsCondp);

                AstNodeExpr* const doneLRefp = new AstVarRef{flp, doneLVars[i], VAccess::READ};
                AstNodeExpr* const doneLOrp = new AstOr{flp, doneLRefp, matchLNowp};
                doneLOrp->dtypeSetBit();
                AstNodeExpr* const doneRRefp = new AstVarRef{flp, doneRVars[i], VAccess::READ};
                AstNodeExpr* const doneROrp = new AstOr{flp, doneRRefp, matchRNowp};
                doneROrp->dtypeSetBit();
                AstNodeExpr* const bothDonep = new AstAnd{flp, doneLOrp, doneROrp};
                bothDonep->dtypeSetBit();
                AstNodeExpr* const oneNowp = new AstOr{flp, matchLNowp->cloneTreePure(false),
                                                       matchRNowp->cloneTreePure(false)};
                oneNowp->dtypeSetBit();
                AstNodeExpr* const matchedp = new AstAnd{flp, bothDonep, oneNowp};
                matchedp->dtypeSetBit();
                stateSig[i] = matchedp;
                changed = true;
            }

            for (int fi = 0; fi < N; ++fi) {
                if (!stateSig[fi]) continue;
                for (const V3GraphEdge& er : vtx[fi]->outEdges()) {
                    const SvaTransEdge& te = static_cast<const SvaTransEdge&>(er);
                    if (te.m_consumesCycle) continue;
                    const int ti = te.toVtxp()->color();
                    if (te.toVtxp()->m_isMatch) continue;
                    if (te.toVtxp()->m_isRejectSink) continue;

                    AstNodeExpr* const srcSigp = stateSig[fi]->cloneTreePure(false);
                    AstNodeExpr* const contributionp = andCond(flp, srcSigp, te.m_condp);

                    if (!stateSig[ti]) {
                        stateSig[ti] = contributionp;
                        changed = true;
                    } else if (!needsReg[ti]) {
                        stateSig[ti] = orExprs(flp, stateSig[ti], contributionp);
                        changed = true;
                    } else {
                        contributionp->deleteTree();
                    }
                }
            }
            if (!changed) break;
        }

        // Phase 2: Compute Edge activations -> NBA
        AstNode* bodyp = nullptr;
        for (int i = 0; i < N; ++i) {
            if (!stateVars[i]) continue;

            AstNodeExpr* nextStatep = nullptr;
            for (const V3GraphEdge& er : vtx[i]->inEdges()) {
                const SvaTransEdge& te = static_cast<const SvaTransEdge&>(er);
                if (!te.m_consumesCycle) continue;
                const int fromIdx = te.fromVtxp()->color();
                if (!stateSig[fromIdx]) continue;

                AstNodeExpr* srcSigp = stateSig[fromIdx]->cloneTreePure(false);
                srcSigp = andCond(flp, srcSigp, te.m_condp);

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
            // Capture disableCnt in Phase-2 NBA before any reactive re-evaluation.
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
        // if (active) { if (done) active<=0; else counter<=counter+1; }
        // else if (incoming) { active<=1; counter<=0; }
        for (int ci = 0; ci < N; ++ci) {
            if (!counterActiveVars[ci]) continue;
            AstVar* const activep = counterActiveVars[ci];
            AstVar* const cntp = counterCountVars[ci];
            const uint32_t counterMax = static_cast<uint32_t>(vtx[ci]->m_counterMax);

            AstNodeExpr* incomingp = nullptr;
            for (const SvaTransEdge* const tep : edges) {
                if (tep->toVtxp()->color() != ci) continue;
                if (!tep->m_consumesCycle) continue;
                const int fi = tep->fromVtxp()->color();
                if (!stateSig[fi]) continue;
                AstNodeExpr* contribp = stateSig[fi]->cloneTreePure(false);
                contribp = andCond(flp, contribp, tep->m_condp);
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

            AstNodeExpr* inWindowp = nullptr;
            if (vtx[ci]->m_counterMin == 0) {
                inWindowp = new AstConst{flp, AstConst::BitTrue{}};
            } else {
                inWindowp = new AstGte{flp, new AstVarRef{flp, cntp, VAccess::READ},
                                       new AstConst{flp, AstConst::WidthedValue{}, 32,
                                                    static_cast<uint32_t>(vtx[ci]->m_counterMin)}};
                inWindowp->dtypeSetBit();
            }
            AstNodeExpr* acceptedNowp = nullptr;
            if (matchCondp) {
                AstSampled* const sampp = new AstSampled{flp, matchCondp->cloneTreePure(false)};
                sampp->dtypeSetBit();
                acceptedNowp = new AstAnd{flp, inWindowp, sampp};
                acceptedNowp->dtypeSetBit();
            } else {
                acceptedNowp = inWindowp;
            }

            AstNodeExpr* const counterAtEndp
                = new AstEq{flp, new AstVarRef{flp, cntp, VAccess::READ},
                            new AstConst{flp, AstConst::WidthedValue{}, 32, counterMax}};
            counterAtEndp->dtypeSetBit();

            AstNodeExpr* const donep = new AstOr{flp, acceptedNowp, counterAtEndp};
            donep->dtypeSetBit();

            AstAssignDly* const clearActivep
                = new AstAssignDly{flp, new AstVarRef{flp, activep, VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitFalse{}}};
            AstAdd* const addExprp
                = new AstAdd{flp, new AstVarRef{flp, cntp, VAccess::READ},
                             new AstConst{flp, AstConst::WidthedValue{}, 32, 1u}};
            addExprp->dtypeFrom(cntp);
            AstAssignDly* const incCountp
                = new AstAssignDly{flp, new AstVarRef{flp, cntp, VAccess::WRITE}, addExprp};
            AstIf* const doneIfp = new AstIf{flp, donep, clearActivep, incCountp};

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
        // NBA semantics ensure doneL/doneR read pre-update values (IEEE 16.9.5).
        for (int ai = 0; ai < N; ++ai) {
            if (!doneLVars[ai]) continue;
            const SvaStateVertex* const avp = vtx[ai];
            const int l = avp->m_andLhsTermp ? avp->m_andLhsTermp->color() : -1;
            const int r = avp->m_andRhsTermp ? avp->m_andRhsTermp->color() : -1;
            if (l < 0 || r < 0) continue;
            if (!stateSig[l] || !stateSig[r] || !stateSig[ai]) continue;

            AstAssignDly* const clearLp
                = new AstAssignDly{flp, new AstVarRef{flp, doneLVars[ai], VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitFalse{}}};
            AstAssignDly* const clearRp
                = new AstAssignDly{flp, new AstVarRef{flp, doneRVars[ai], VAccess::WRITE},
                                   new AstConst{flp, AstConst::BitFalse{}}};
            clearLp->addNext(clearRp);

            AstNodeExpr* const matchLNowp = buildMatchNow(flp, stateSig[l], avp->m_andLhsCondp);
            AstNodeExpr* const matchRNowp = buildMatchNow(flp, stateSig[r], avp->m_andRhsCondp);
            AstNodeExpr* gateLp = matchLNowp;
            AstNodeExpr* gateRp = matchRNowp;
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

        // Phase 3: Compute accept/reject from terminal Links.
        // rejectBasep: counter sources only at window end; others on every terminal cycle.
        AstNodeExpr* snapshotOkp = nullptr;
        if (snapshotVarp && disableCntVarp) {
            snapshotOkp = new AstEq{flp, new AstVarRef{flp, snapshotVarp, VAccess::READ},
                                    new AstVarRef{flp, disableCntVarp, VAccess::READ}};
            snapshotOkp->dtypeSetBit();
        }

        AstNodeExpr* terminalActivep = nullptr;
        AstNodeExpr* rejectBasep = nullptr;
        for (const SvaTransEdge* const tep : edges) {
            if (tep->toVtxp() != graph.m_matchVertexp) continue;
            if (tep->m_consumesCycle) continue;
            const int fi = tep->fromVtxp()->color();
            if (!stateSig[fi]) continue;

            AstNodeExpr* srcSigp = stateSig[fi]->cloneTreePure(false);
            srcSigp = andCond(flp, srcSigp, tep->m_condp);
            if (snapshotOkp) {
                srcSigp = new AstAnd{flp, srcSigp, snapshotOkp->cloneTreePure(false)};
                srcSigp->dtypeSetBit();
            }

            if (tep->fromVtxp()->m_isCounter) {
                terminalActivep = orExprs(flp, terminalActivep, srcSigp->cloneTreePure(false));
                AstNodeExpr* const atEndp = new AstEq{
                    flp, new AstVarRef{flp, counterCountVars[fi], VAccess::READ},
                    new AstConst{flp, AstConst::WidthedValue{}, 32,
                                 static_cast<uint32_t>(tep->fromVtxp()->m_counterMax)}};
                atEndp->dtypeSetBit();
                AstNodeExpr* const expireContribp = new AstAnd{flp, srcSigp, atEndp};
                expireContribp->dtypeSetBit();
                rejectBasep = orExprs(flp, rejectBasep, expireContribp);
            } else if (tep->fromVtxp()->m_isUnbounded || tep->fromVtxp()->m_isAndCombiner) {
                // Liveness or SAnd combiner: match only; sub-NFAs own their rejects.
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

        // Phase 3a: required-step rejection.
        // Fires when source is active but link condition is false.
        // E.g. `a |-> b ##...`: antecedent fires but b is false -- the attempt never
        // leaves the start state, so no terminal-based reject can fire.
        AstNodeExpr* requiredStepRejectp = nullptr;
        for (const SvaTransEdge* const tep : edges) {
            if (!tep->m_rejectOnFail) continue;
            if (tep->m_consumesCycle) continue;
            const int fi = tep->fromVtxp()->color();
            if (!stateSig[fi]) continue;
            if (!tep->m_condp) continue;
            AstNodeExpr* const srcSigp = stateSig[fi]->cloneTreePure(false);
            AstNodeExpr* const notCondp = new AstNot{flp, tep->m_condp->cloneTreePure(false)};
            notCondp->dtypeSetBit();
            AstNodeExpr* const failp = new AstAnd{flp, srcSigp, notCondp};
            failp->dtypeSetBit();
            if (outRequiredStepSrcsp) {
                outRequiredStepSrcsp->push_back(failp->cloneTreePure(false));
            }
            requiredStepRejectp = orExprs(flp, requiredStepRejectp, failp);
        }

        // Phase 3b: Throughout-drop rejection (IEEE 16.9.9).
        // fail_i = state_active[i] && !$sampled(AND of throughout exprs).
        AstNodeExpr* throughoutRejectp = nullptr;
        for (int i = 0; i < N; ++i) {
            const auto& conds = vtx[i]->m_throughoutConds;
            if (conds.empty()) continue;
            // SAnd combiner stateSig fires only at completion; sub-sequences own their vertices.
            if (vtx[i]->m_isAndCombiner) continue;
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

        if (snapshotOkp) {
            snapshotOkp->deleteTree();
            snapshotOkp = nullptr;
        }

        if (disableExprp) {
            disableExprp->deleteTree();
            disableExprp = nullptr;
        }

        // Property negation (IEEE 1800-2023 16.12.1 `not`): invert accept/reject.
        if (negated) {
            if (isCover) {
                if (terminalActivep) terminalActivep->deleteTree();
                AstNodeExpr* negRejectp = nullptr;
                if (matchCondp && rejectBasep) {
                    AstNodeExpr* const sampledCondp
                        = new AstSampled{flp, matchCondp->cloneTreePure(false)};
                    sampledCondp->dtypeFrom(matchCondp);
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
            // Negated assert/assume: output = !accept.
            AstNodeExpr* acceptp = terminalActivep;
            if (matchCondp) {
                AstNodeExpr* const sampledCondp
                    = new AstSampled{flp, matchCondp->cloneTreePure(false)};
                sampledCondp->dtypeFrom(matchCondp);
                acceptp = new AstAnd{flp, acceptp, sampledCondp};
                acceptp->dtypeSetBit();
            }
            if (outMatchpp) {
                AstNodeExpr* notPAcceptp = nullptr;
                if (matchCondp && rejectBasep) {
                    AstNodeExpr* const sampledCondp
                        = new AstSampled{flp, matchCondp->cloneTreePure(false)};
                    sampledCondp->dtypeFrom(matchCondp);
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
                *outMatchpp = notPAcceptp;
            }
            if (throughoutRejectp) throughoutRejectp->deleteTree();
            if (rejectBasep) rejectBasep->deleteTree();
            if (requiredStepRejectp) requiredStepRejectp->deleteTree();
            AstNodeExpr* const resultExprp = new AstNot{flp, acceptp};
            resultExprp->dtypeSetBit();
            return resultExprp;
        }

        if (isCover) {
            if (throughoutRejectp) throughoutRejectp->deleteTree();
            if (rejectBasep) rejectBasep->deleteTree();
            if (requiredStepRejectp) requiredStepRejectp->deleteTree();
            if (matchCondp) {
                AstNodeExpr* const sampledCondp
                    = new AstSampled{flp, matchCondp->cloneTreePure(false)};
                sampledCondp->dtypeFrom(matchCondp);
                AstNodeExpr* const acceptp = new AstAnd{flp, terminalActivep, sampledCondp};
                acceptp->dtypeSetBit();
                return acceptp;
            }
            return terminalActivep;
        }

        // Assert/assume: output = !reject
        AstNodeExpr* rejectp = nullptr;
        if (matchCondp && rejectBasep) {
            AstNodeExpr* const sampledCondp
                = new AstSampled{flp, matchCondp->cloneTreePure(false)};
            sampledCondp->dtypeFrom(matchCondp);
            AstNodeExpr* const notCondp = new AstNot{flp, sampledCondp};
            notCondp->dtypeSetBit();
            rejectp = new AstAnd{flp, rejectBasep, notCondp};
            rejectp->dtypeSetBit();
        } else if (rejectBasep) {
            rejectBasep->deleteTree();
        }
        if (outMatchpp) {
            AstNodeExpr* acceptExprp = terminalActivep->cloneTreePure(false);
            if (matchCondp) {
                AstNodeExpr* const sp = new AstSampled{flp, matchCondp->cloneTreePure(false)};
                sp->dtypeSetBit();
                acceptExprp = new AstAnd{flp, acceptExprp, sp};
                acceptExprp->dtypeSetBit();
            }
            *outMatchpp = acceptExprp;
        }
        if (terminalActivep) terminalActivep->deleteTree();

        if (throughoutRejectp) {
            rejectp = orExprs(flp, rejectp, throughoutRejectp);
            if (rejectp) rejectp->dtypeSetBit();
        }
        if (requiredStepRejectp) {
            rejectp = orExprs(flp, rejectp, requiredStepRejectp);
            if (rejectp) rejectp->dtypeSetBit();
        }
        if (!rejectp) { return new AstConst{flp, AstConst::BitTrue{}}; }
        AstNodeExpr* const resultExprp = new AstNot{flp, rejectp};
        resultExprp->dtypeSetBit();
        return resultExprp;
    }
};

}  // namespace

//######################################################################
// Top-level visitor

class AssertNfaVisitor final : public VNVisitor {
    // STATE
    AstNodeModule* m_modp = nullptr;  // Current module being processed
    SvaNfaLowering* m_loweringp = nullptr;  // NFA-to-hardware lowering engine
    V3UniqueNames m_propVarNames{"__Vpropvar"};  // Property-local variable names
    V3UniqueNames m_disableCntNames{"__VnfaDis"};  // Disable-iff counter names
    std::set<const AstProperty*> m_inliningProps;  // Recursion guard for inlineNamedProperty

    // Wire match vertex and mid-window sources for a successful NFA build.
    static void wireMatchAndMidSources(SvaGraph& graph, const BuildResult& result, FileLine* flp) {
        graph.createMatchVertex();
        graph.addLink(result.termVertexp, graph.m_matchVertexp);
        for (SvaStateVertex* srcVtxp : result.midSources) {
            AstNodeExpr* condp = nullptr;
            for (AstNodeExpr* const tc : srcVtxp->m_throughoutConds) {
                AstNodeExpr* const tcClone = tc->cloneTreePure(false);
                condp = condp ? new AstAnd{flp, condp, tcClone} : tcClone;
                if (condp->width() != 1) condp->dtypeSetBit();
            }
            graph.addLink(srcVtxp, graph.m_matchVertexp, condp);
            srcVtxp->m_isUnbounded = true;
        }
    }

    static AstNodeExpr* getSequenceBodyExprp(const AstSequence* seqp) {
        AstNode* bodyp = seqp->stmtsp();
        while (bodyp && VN_IS(bodyp, Var)) bodyp = bodyp->nextp();
        return VN_CAST(bodyp, NodeExpr);
    }

    static AstPropSpec* getPropertySpecp(const AstProperty* propp) {
        AstNode* stmtp = propp->stmtsp();
        while (stmtp
               && (VN_IS(stmtp, Var) || VN_IS(stmtp, InitialStaticStmt)
                   || VN_IS(stmtp, InitialAutomaticStmt))) {
            stmtp = stmtp->nextp();
        }
        return VN_CAST(stmtp, PropSpec);
    }

    void inlineNamedProperty(AstPropSpec* outerSpecp, AstFuncRef* funcrefp,
                             const AstProperty* propyp) {
        // Recursion guard: IEEE 1800-2023 16.12.1 forbids recursive properties.
        if (m_inliningProps.count(propyp)) {
            funcrefp->v3error("Illegal recursive property reference"
                              " (IEEE 1800-2023 16.12.1)");
            return;
        }
        m_inliningProps.insert(propyp);
        struct Guard final {
            std::set<const AstProperty*>& setr;
            const AstProperty* keyp;
            ~Guard() { setr.erase(keyp); }
        } guard{m_inliningProps, propyp};
        AstPropSpec* propSpecp = getPropertySpecp(propyp);
        UASSERT_OBJ(propSpecp, funcrefp, "Property has no body PropSpec");
        propSpecp = propSpecp->cloneTree(false);

        const V3TaskConnects tconnects = V3Task::taskConnects(funcrefp, propyp->stmtsp());
        std::unordered_map<const AstVar*, AstNodeExpr*> portMap;
        for (const auto& tconnect : tconnects) {
            portMap[tconnect.first] = tconnect.second->exprp();
        }

        // Promote property-local variables to module-level temps (IEEE 16.10).
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

        propSpecp->foreach([&](AstVarRef* refp) {
            const auto portIt = portMap.find(refp->varp());
            if (portIt != portMap.end()) {
                refp->replaceWith(portIt->second->cloneTree(false));
                VL_DO_DANGLING(pushDeletep(refp), refp);
                return;
            }
            const auto localIt = localVarMap.find(refp->varp());
            if (localIt != localVarMap.end()) refp->varp(localIt->second);
        });

        for (const auto& tconnect : tconnects) {
            pushDeletep(tconnect.second->exprp()->unlinkFrBack());
        }

        // Merge disable iff (IEEE 1800-2023 16.12.1)
        if (outerSpecp->disablep() && propSpecp->disablep()) {
            outerSpecp->v3error("disable iff expression before property call "
                                "and in its body is not legal");
            pushDeletep(propSpecp->disablep()->unlinkFrBack());
        }
        if (outerSpecp->disablep()) {
            propSpecp->disablep(outerSpecp->disablep()->unlinkFrBack());
        }

        if (outerSpecp->sensesp() && propSpecp->sensesp()) {
            outerSpecp->v3warn(E_UNSUPPORTED,
                               "Unsupported: Clock event before property call and in its body");
            pushDeletep(propSpecp->sensesp()->unlinkFrBack());
        }
        if (outerSpecp->sensesp()) {
            AstSenItem* const sensesp = outerSpecp->sensesp();
            sensesp->unlinkFrBack();
            propSpecp->sensesp(sensesp);
        }

        outerSpecp->replaceWith(propSpecp);
        VL_DO_DANGLING(pushDeletep(outerSpecp), outerSpecp);
    }

    void inlineSequenceRef(AstFuncRef* funcrefp, AstSequence* seqp) {
        AstNodeExpr* const bodyExprp = getSequenceBodyExprp(seqp);
        UASSERT_OBJ(bodyExprp, funcrefp, "Sequence has no body expression");
        AstNodeExpr* const clonedp = bodyExprp->cloneTree(false);

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
        for (const auto& tconnect : tconnects) {
            pushDeletep(tconnect.second->exprp()->unlinkFrBack());
        }
        funcrefp->replaceWith(clonedp);
        VL_DO_DANGLING(pushDeletep(funcrefp), funcrefp);
        // Clear referenced flag so V3AssertPre cleanup does not emit
        // spurious UNSUPPORTED for sequences that were already inlined here.
        seqp->isReferenced(false);
    }

    // Must run before hasMultiCycleExpr() so NFA sees sequence bodies.
    void inlineAllSequenceRefs(AstNode* rootp) {
        bool changed = true;
        while (changed) {
            changed = false;
            rootp->foreach([&](AstFuncRef* funcrefp) {
                if (changed) return;
                if (AstSequence* const seqp = VN_CAST(funcrefp->taskp(), Sequence)) {
                    inlineSequenceRef(funcrefp, seqp);
                    changed = true;
                }
            });
        }
    }

    static bool hasMultiCycleExpr(const AstNode* nodep) {
        return nodep->exists([](const AstNode* np) {
            if (const auto* const ep = VN_CAST(np, NodeExpr)) return ep->isMultiCycleSva();
            return false;
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

        if (AstPropSpec* const specp = VN_CAST(assertp->propp(), PropSpec)) {
            if (AstFuncRef* const funcrefp = VN_CAST(specp->propp(), FuncRef)) {
                if (const AstProperty* const propyp = VN_CAST(funcrefp->taskp(), Property)) {
                    inlineNamedProperty(specp, funcrefp, propyp);
                }
            }
        }

        inlineAllSequenceRefs(assertp->propp());

        AstNode* const propp = assertp->propp();
        if (!hasMultiCycleExpr(propp)) return;

        const PropertyParts parts = decomposeProperty(propp);
        if (!parts.seqExprp) return;

        // Unwrap `not` (IEEE 1800-2023 16.12.1); odd count -> negated semantics.
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

        SvaGraph graph;
        SvaNfaBuilder builder{graph};

        BuildResult result = BuildResult::fail();
        if (parts.hasImplication) {
            graph.m_startVertexp = graph.createStateVertex();

            const BuildResult antResult
                = builder.buildExpr(parts.triggerExprp, graph.m_startVertexp);
            if (!antResult.valid()) {
                // Fall through to V3AssertPre for unsupported constructs.
                // Only replace with BitFalse on real semantic errors.
                if (antResult.errorEmitted) {
                    if (propSpecp) {
                        AstNode* const innerPropp = propSpecp->propp();
                        innerPropp->replaceWith(new AstConst{flp, AstConst::BitFalse{}});
                        VL_DO_DANGLING(pushDeletep(innerPropp), innerPropp);
                    }
                }
                if (senTreeOwned) senTreep->deleteTree();
                return;
            }

            // Use raw createStateVertex() (not scopedCreateVertex) so trigVtxp starts
            // without liveness.  Reaching the antecedent terminal is a definitive event.
            SvaStateVertex* const trigVtxp = graph.createStateVertex();
            if (antResult.finalCondp) {
                graph.addLink(antResult.termVertexp, trigVtxp,
                              new AstSampled{flp, antResult.finalCondp->cloneTreePure(false)});
                if (!antResult.finalCondp->backp()) antResult.finalCondp->deleteTree();
            } else {
                graph.addLink(antResult.termVertexp, trigVtxp);
            }
            builder.resetScope();

            if (parts.isOverlapped) {
                result = builder.buildExpr(seqBodyp, trigVtxp,
                                           /*isTopLevelStep=*/true);
            } else {
                SvaStateVertex* const delayVtxp = graph.createStateVertex();
                graph.addClockedEdge(trigVtxp, delayVtxp);
                result = builder.buildExpr(seqBodyp, delayVtxp,
                                           /*isTopLevelStep=*/true);
            }

            if (result.valid()) wireMatchAndMidSources(graph, result, flp);
        } else {
            result = builder.build(seqBodyp);
            if (result.valid()) wireMatchAndMidSources(graph, result, flp);
        }

        if (!result.valid()) {
            // Fall through to V3AssertPre for unsupported constructs.
            // Only replace with BitFalse on real semantic errors.
            if (result.errorEmitted) {
                if (propSpecp) {
                    AstNode* const innerPropp = propSpecp->propp();
                    innerPropp->replaceWith(new AstConst{flp, AstConst::BitFalse{}});
                    VL_DO_DANGLING(pushDeletep(innerPropp), innerPropp);
                } else {
                    AstNode* const oldPropp = assertp->propp();
                    oldPropp->replaceWith(new AstConst{flp, AstConst::BitFalse{}});
                    VL_DO_DANGLING(pushDeletep(oldPropp), oldPropp);
                }
            }
            if (senTreeOwned) senTreep->deleteTree();
            return;
        }

        // Build succeeded. Now create snapshot mechanism for disable iff if needed.
        // Done here (not before build) so failed builds don't pollute the AST.
        AstVar* disableCntVarp = nullptr;
        AstVar* snapshotVarp = nullptr;
        const bool disableHasSampled
            = disableExprp && disableExprp->exists([](const AstSampled*) { return true; });
        if (disableExprp && !parts.hasImplication && !VN_IS(disableExprp, Const)
            && !disableHasSampled) {
            AstNodeDType* const u32DTypep = m_modp->findBasicDType(VBasicDTypeKwd::UINT32);
            const std::string cntName = m_disableCntNames.get("");
            disableCntVarp = new AstVar{flp, VVarType::MODULETEMP, cntName, u32DTypep};
            disableCntVarp->lifetime(VLifetime::STATIC_EXPLICIT);
            m_modp->addStmtsp(disableCntVarp);

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

            const std::string snapName = m_disableCntNames.get("") + "__snap";
            snapshotVarp = new AstVar{flp, VVarType::MODULETEMP, snapName, u32DTypep};
            snapshotVarp->lifetime(VLifetime::STATIC_EXPLICIT);
            m_modp->addStmtsp(snapshotVarp);

            if (propSpecp && propSpecp->disablep()) {
                AstNodeExpr* const oldDisp = propSpecp->disablep();
                oldDisp->unlinkFrBack();
            }
        }
        const bool disableExprUnlinked = disableCntVarp && disableExprp;

        AstAssert* const assertAssertp = VN_CAST(assertp, Assert);
        const bool needAccept
            = !isCover && !parts.hasImplication && assertAssertp && assertAssertp->passsp();
        AstNodeExpr* matchExprp = nullptr;

        AstAssert* const assertWithFailp = VN_CAST(assertp, Assert);
        const bool needPerSrcFail
            = !isCover && !parts.hasImplication && assertWithFailp && assertWithFailp->failsp();
        std::vector<AstNodeExpr*> requiredStepSrcs;

        AstNodeExpr* const alwaysTriggerp = new AstConst{flp, AstConst::BitTrue{}};
        AstNodeExpr* const outputExprp
            = m_loweringp->lower(flp, graph, alwaysTriggerp, senTreep, result.finalCondp, isCover,
                                 disableExprp ? disableExprp->cloneTreePure(false) : nullptr,
                                 negated, needAccept ? &matchExprp : nullptr, disableCntVarp,
                                 snapshotVarp, needPerSrcFail ? &requiredStepSrcs : nullptr);

        AstSenTree* const perSrcSenTreep
            = (requiredStepSrcs.size() >= 2) ? senTreep->cloneTree(false) : nullptr;

        alwaysTriggerp->deleteTree();
        if (senTreeOwned) senTreep->deleteTree();
        if (disableExprUnlinked) disableExprp->deleteTree();
        if (result.finalCondp && !result.finalCondp->backp()) { result.finalCondp->deleteTree(); }

        // Gate pass handler on match to prevent vacuous-pass firings.
        if (needAccept && matchExprp) {
            AstNode* passsp = assertAssertp->passsp();
            if (passsp) {
                passsp->unlinkFrBackWithNext();
                assertAssertp->addPasssp(new AstIf{flp, matchExprp->cloneTreePure(false),
                                                   passsp->cloneTree(false), nullptr});
                // Fail-handler prefix for overlapping instances (IEEE 16.12):
                // fires when reject=1 && match=1 in the same cycle.
                if (AstNode* const failsp = assertAssertp->failsp()) {
                    failsp->addHereThisAsNext(
                        new AstIf{flp, matchExprp, passsp->cloneTree(false), nullptr});
                } else {
                    matchExprp->deleteTree();
                }
                VL_DO_DANGLING(pushDeletep(passsp), passsp);
            } else {
                matchExprp->deleteTree();
            }
        }

        // Extra fail-handler fires for simultaneous required-step failures
        // (IEEE 1800-2023: fail handler fires once per failing thread).
        if (requiredStepSrcs.size() >= 2 && assertWithFailp && assertWithFailp->failsp()
            && perSrcSenTreep) {
            AstNode* const failsp = assertWithFailp->failsp();
            AstNodeExpr* cumulativeOrp = requiredStepSrcs[0]->cloneTreePure(false);
            for (size_t i = 1; i < requiredStepSrcs.size(); ++i) {
                AstNodeExpr* const srcp = requiredStepSrcs[i];
                AstNodeExpr* const condp = new AstAnd{flp, srcp->cloneTreePure(false),
                                                      cumulativeOrp->cloneTreePure(false)};
                condp->dtypeSetBit();
                AstNode* const failClonep = failsp->cloneTree(true);
                AstIf* const ifp = new AstIf{flp, condp, failClonep, nullptr};
                AstAlways* const alwaysp = new AstAlways{flp, VAlwaysKwd::ALWAYS,
                                                         perSrcSenTreep->cloneTree(false), ifp};
                m_modp->addStmtsp(alwaysp);
                AstNodeExpr* const extOrp
                    = new AstOr{flp, cumulativeOrp, srcp->cloneTreePure(false)};
                extOrp->dtypeSetBit();
                cumulativeOrp = extOrp;
            }
            for (AstNodeExpr* const srcp : requiredStepSrcs) srcp->deleteTree();
            cumulativeOrp->deleteTree();
            perSrcSenTreep->deleteTree();
        } else {
            for (AstNodeExpr* const srcp : requiredStepSrcs) srcp->deleteTree();
            if (perSrcSenTreep) perSrcSenTreep->deleteTree();
        }

        if (propSpecp) {
            AstNode* const innerPropp = propSpecp->propp();
            innerPropp->replaceWith(outputExprp);
            VL_DO_DANGLING(pushDeletep(innerPropp), innerPropp);
        } else {
            AstNode* const oldPropp = assertp->propp();
            oldPropp->replaceWith(outputExprp);
            VL_DO_DANGLING(pushDeletep(oldPropp), oldPropp);
        }

        UINFO(4, "NFA converted assertion at " << flp << endl);
    }

    // VISITORS
    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        VL_RESTORER(m_loweringp);
        m_modp = nodep;
        SvaNfaLowering lowering{nodep};
        m_loweringp = &lowering;
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
