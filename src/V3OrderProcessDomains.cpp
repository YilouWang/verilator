// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Block code ordering
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
//  Initial graph dependency builder for ordering
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Const.h"
#include "V3EmitV.h"
#include "V3File.h"
#include "V3OrderGraph.h"
#include "V3OrderInternal.h"
#include "V3SenTree.h"

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// ProcessDomains class

class V3OrderProcessDomains final {
    // NODE STATE
    //  AstNode::user4  -> Used by V3Const::constifyExpensiveEdit

    // STATE
    OrderGraph& m_graph;  // The ordering graph

    // This is a function provided by the invoker of the ordering that can provide additional
    // sensitivity expression that when triggered indicates the passed AstVarScope might have
    // changed external to the code being ordered.
    const V3Order::ExternalDomainsProvider m_externalDomains;

    SenTreeFinder m_finder;  // Global AstSenTree manager

    // Sentinel value indicating a vertex can be deleted. Never dereferenced, so any non-nullptr
    // value will do. Use something that wil crash quickly if used.
    AstSenTree* const m_deleteDomainp = reinterpret_cast<AstSenTree*>(1);
    // Logic that is never triggered and hence can be deleted
    std::vector<OrderLogicVertex*> m_logicpsToDelete;
    const string m_tag;  // Substring to add to generated names

    // METHODS

    // Dump a domain for debugging
    std::string debugDomain(AstSenTree* domainp) {
        return cvtToHex(domainp)
               + (domainp == m_deleteDomainp ? " [DEL]"
                  : domainp->hasCombo()      ? " [COMB]"
                  : domainp->isMulti()       ? " [MULT]"
                                             : "");
    }

    // Make a domain that merges the two domains, but don't yet simplify or add to global list
    AstSenTree* combineDomains(AstSenTree* ap, AstSenTree* bp) {
        if (ap == bp) return ap;
        if (ap == m_deleteDomainp) return bp;
        UASSERT_OBJ(bp != m_deleteDomainp, bp, "'bp' Should not be delete domain");
        // If either tree has a backp, it is an existing tree, otherwise it is an intermediae
        // created in this function.
        // Clone existing 'ap' tree, or reuse intermediate
        AstSenTree* const senTreep = ap->backp() ? ap->cloneTree(false) : ap;
        // Clone or move items from 'bp'
        if (bp->backp()) {
            senTreep->addSensesp(bp->sensesp()->cloneTree(true));
        } else {
            senTreep->addSensesp(bp->sensesp()->unlinkFrBackWithNext());
            VL_DO_DANGLING(bp->deleteTree(), bp);
        }
        return senTreep;
    }

    AstSenTree* simplifyDomain(AstSenTree* senTreep) {
        // If it has a back pointer, then it is already one of the existing global trees
        if (senTreep->backp()) return senTreep;
        V3Const::constifyExpensiveEdit(senTreep);  // Remove duplicates
        senTreep->multi(true);  // Comment that it was made from 2 domains
        AstSenTree* const resultp = m_finder.getSenTree(senTreep);
        VL_DO_DANGLING(senTreep->deleteTree(), senTreep);  // getSenTree clones, so delete this
        return resultp;
    }

    // The graph routines have already sorted the vertexes and edges into best->worst order
    // Assign clock domains to each signal.
    //     Sequential logic already hae their domain defined.
    //     Combo logic may be pushed into a seq domain if all its inputs are the same domain,
    //     else, if all inputs are from flops, it's end-of-sequential code
    //     else, it's full combo code
    void processDomains() {
        UINFO(2, "  Domains...");
        // Buffer to hold external sensitivities
        std::vector<AstSenTree*> externalDomainps;
        // For each vertex
        for (V3GraphVertex& it : m_graph.vertices()) {
            OrderEitherVertex* const vtxp = it.as<OrderEitherVertex>();
            UINFO(5, "    pdi: " << vtxp);
            // Sequential logic already has its domain set
            if (vtxp->domainp()) continue;

            AstSenTree* domainp = nullptr;
            // For logic, start with the explicit hybrid sensitivities
            OrderLogicVertex* const lvtxp = vtxp->cast<OrderLogicVertex>();
            if (lvtxp) domainp = lvtxp->hybridp();
            if (domainp) {
                UINFO(6, "      hybr d=" << debugDomain(domainp) << " " << vtxp);
                UASSERT(domainp->backp(), "Hybrid senTree should have backp");
            }

            // For each incoming edge, examine the source vertex
            for (V3GraphEdge& edge : vtxp->inEdges()) {
                OrderEitherVertex* const fromVtxp = edge.fromp()->as<OrderEitherVertex>();
                // Cut edge
                if (!edge.weight()) continue;
                //
                if (!fromVtxp->domainMatters()) continue;

                AstSenTree* fromDomainp = fromVtxp->domainp();

                UINFO(6, "      from d=" << debugDomain(fromDomainp) << " " << fromVtxp);
                UASSERT(fromDomainp == m_deleteDomainp || !fromDomainp->hasCombo(),
                        "There should be no need for combinational domains");
                UASSERT(fromDomainp == m_deleteDomainp || fromDomainp->backp(),
                        "Driver SenTree should have backp");

                // Add in any external domains of variables
                if (OrderVarVertex* const varVtxp = fromVtxp->cast<OrderVarVertex>()) {
                    AstVarScope* const vscp = varVtxp->vscp();
                    externalDomainps.clear();
                    m_externalDomains(vscp, externalDomainps);
                    for (AstSenTree* const externalDomainp : externalDomainps) {
                        UINFO(6, "      xtrn d=" << debugDomain(externalDomainp) << " " << fromVtxp
                                                 << " because of " << vscp);
                        UASSERT_OBJ(!externalDomainp->hasCombo(), vscp,
                                    "There should be no need for combinational domains");
                        UASSERT_OBJ(externalDomainp->backp(), vscp,
                                    "External SenTree should have backp()");
                        fromDomainp = combineDomains(fromDomainp, externalDomainp);
                    }
                }

                // Irrelevant input vertex (never triggered, not even externally)
                if (fromDomainp == m_deleteDomainp) continue;

                if (!domainp) {
                    // First input to this vertex that we are processing
                    domainp = fromDomainp;
                } else {
                    // Make a domain that merges the two domains
                    domainp = combineDomains(domainp, fromDomainp);
                }
            }

            // If nothing triggers this vertex, we can delete the corresponding logic
            if (!domainp) {
                domainp = m_deleteDomainp;
                if (lvtxp) m_logicpsToDelete.push_back(lvtxp);
            } else {
                // Simplify and create canonical global SenTree
                domainp = simplifyDomain(domainp);
            }

            // Set the domain of the vertex
            vtxp->domainp(domainp);

            UINFO(5, "      done d=" << debugDomain(domainp) << " " << vtxp);
        }
    }

    void processEdgeReport() {
        // Make report of all signal names and what clock edges they have
        const string filename = v3Global.debugFilename(m_tag + "_order_edges.txt");
        const std::unique_ptr<std::ofstream> logp{V3File::new_ofstream(filename)};
        if (logp->fail()) v3fatal("Can't write file: " << filename);

        std::deque<string> report;

        for (V3GraphVertex& vtx : m_graph.vertices()) {
            if (OrderVarVertex* const vvertexp = vtx.cast<OrderVarVertex>()) {
                string name(vvertexp->vscp()->prettyName());
                if (vtx.is<OrderVarPreVertex>()) {
                    name += " {PRE}";
                } else if (vtx.is<OrderVarPostVertex>()) {
                    name += " {POST}";
                } else if (vtx.is<OrderVarPordVertex>()) {
                    name += " {PORD}";
                }
                std::ostringstream os;
                os.setf(std::ios::left);
                os << "  " << cvtToHex(vvertexp->vscp()) << " " << std::setw(50) << name << " ";
                AstSenTree* const senTreep = vvertexp->domainp();
                if (senTreep == m_deleteDomainp) {
                    os << "DELETED";
                } else {
                    for (AstSenItem* senItemp = senTreep->sensesp(); senItemp;
                         senItemp = VN_AS(senItemp->nextp(), SenItem)) {
                        if (senItemp != senTreep->sensesp()) os << " or ";
                        V3EmitV::verilogForTree(senItemp, os);
                    }
                }
                report.push_back(os.str());
            }
        }

        *logp << "Signals and their clock domains:\n";
        stable_sort(report.begin(), report.end());
        for (const string& i : report) *logp << i << '\n';
    }

    // CONSTRUCTOR
    V3OrderProcessDomains(AstNetlist* netlistp, OrderGraph& graph, const string& tag,
                          const V3Order::ExternalDomainsProvider& externalDomains)
        : m_graph{graph}
        , m_externalDomains{externalDomains}
        , m_finder{netlistp}
        , m_tag{tag} {

        // Assign vertices to their sensitivity domains
        processDomains();
        if (dumpGraphLevel()) m_graph.dumpDotFilePrefixed(m_tag + "_orderg_domain");

        // Report domain assignments if requested
        if (dumpLevel()) processEdgeReport();

        // Delete logic that is never triggered
        for (OrderLogicVertex* const lVtxp : m_logicpsToDelete) {
            UASSERT_OBJ(lVtxp->domainp() == m_deleteDomainp, lVtxp,
                        "Should have been marked as deleted");
            lVtxp->nodep()->unlinkFrBack()->deleteTree();
            lVtxp->unlinkDelete(&m_graph);
        }
    }

    ~V3OrderProcessDomains() = default;

public:
    // Order the logic
    static void apply(AstNetlist* netlistp, OrderGraph& graph, const string& tag,
                      const V3Order::ExternalDomainsProvider& externalDomains) {
        V3OrderProcessDomains{netlistp, graph, tag, externalDomains};
    }
};

void V3Order::processDomains(AstNetlist* netlistp,  //
                             OrderGraph& graph,  //
                             const std::string& tag,  //
                             const ExternalDomainsProvider& externalDomains) {
    V3OrderProcessDomains::apply(netlistp, graph, tag, externalDomains);
}
