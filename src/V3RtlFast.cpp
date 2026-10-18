// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: --rtl-fast subset check and activity gating
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// --rtl-fast only supports synthesizable, synchronous RTL. checkSubset
// rejects everything else with an error, so the mode never silently
// produces a different result:
//   - timing controls (delays, statement event controls, wait, fork with join)
//   - classes (other than the std package)
//   - DPI imports and exports
//   - user C code ($c)
//
// gateActivity makes the 'nba' region activity driven. The nba statements
// of each trigger domain are reordered and cut into partitions. A
// 'gated' partition has a dirty flag and runs only when it is dirty.
// Whenever a variable changes value, every partition that reads it is
// marked dirty. Correctness rests on:
//   - A gated partition is deterministic: it has no side effects, and its
//     writes depend only on the values of its inputs, which are the
//     variables it may read before writing them.
//   - A variable written by more than one writer (another partition, or code
//     outside the nba region) is also an input of every partition writing it.
//   - Every value change of an input is detected: partitions compare the
//     written value with the old one at each assignment (or compare their
//     outputs before and after running), and variables written outside the
//     nba region are compared with a shadow copy whenever the region starts.
// If none of the inputs changed since the partition last ran, running it
// again would write the values its outputs already hold, so it is skipped.
//
// Statements with side effects ($display, $random, ...) are 'impure'. They
// keep their relative order. An impure 'if (c) ...' with a pure condition
// is split into a pure statement computing 'c' into a temporary, and the
// impure 'if' testing the temporary ('conditional statement'). A run of
// conditional statements forms a 'conditional group' with a dirty flag: it
// is marked dirty when one of its condition temporaries changes, and stays
// dirty while one of the conditions holds, so it is skipped only when all
// conditions are false. Other impure statements always run.
//
// Reordering: any order that keeps every pair of conflicting statements
// (one writes a variable the other reads or writes) and every pair of
// impure statements in their original order gives the same results. Among
// those, a topological order is chosen that follows a depth first walk of
// the data flow from the sinks, so statements feeding the same state are
// adjacent and land in the same partition.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3RtlFast.h"

#include "V3Stats.h"

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Subset checker

class RtlFastSubsetVisitor final : public VNVisitorConst {
    // STATE
    const AstPackage* const m_stdPackagep;  // The std package, which is always allowed

    void reject(const AstNode* nodep, const string& what) {
        nodep->v3error("Unsupported: --rtl-fast does not support " << what);
    }

    // VISITORS
    void visit(AstClass* nodep) override { reject(nodep, "classes"); }
    void visit(AstPackage* nodep) override {
        if (nodep == m_stdPackagep) return;
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeFTask* nodep) override {
        if (nodep->dpiImport() || nodep->dpiExport()) reject(nodep, "DPI functions or tasks");
        iterateChildrenConst(nodep);
    }
    void visit(AstCStmtUser* nodep) override { reject(nodep, "$c statements"); }
    void visit(AstCExprUser* nodep) override { reject(nodep, "$c expressions"); }
    void visit(AstFork* nodep) override { reject(nodep, "fork"); }
    void visit(AstNode* nodep) override {
        if (nodep->isTimingControl()) {
            reject(nodep, "timing controls (delays, event controls or wait)");
            return;
        }
        iterateChildrenConst(nodep);
    }

public:
    // CONSTRUCTORS
    explicit RtlFastSubsetVisitor(AstNetlist* nodep)
        : m_stdPackagep{nodep->stdPackagep()} {
        iterateConst(nodep);
    }
    ~RtlFastSubsetVisitor() override = default;
};

//######################################################################
// Activity gating

// Variable accesses of one statement, in execution order
class RtlFastAccessVisitor final : public VNVisitorConst {
    // STATE
    std::set<const AstVarScope*> m_def;  // Definitely fully written so far
    std::set<const AstVarScope*> m_inReadSet;  // Elements of m_inReads
    std::set<const AstVarScope*> m_readSet;  // Elements of m_reads
    std::set<const AstVarScope*> m_writeSet;  // Elements of m_writes
    std::set<const AstCFunc*> m_callStack;  // Functions being analyzed (recursion guard)
    int m_noDef = 0;  // Above 0 where writes are not definite (jumps, loops, callees)

public:
    std::vector<AstVarScope*> m_inReads;  // Variables read before definitely written
    std::vector<AstVarScope*> m_reads;  // Variables read
    std::vector<AstVarScope*> m_writes;  // Variables written
    std::vector<AstVarScope*> m_defs;  // Variables definitely fully written
    bool m_impure = false;  // Has side effects or non-deterministic results
    bool m_calls = false;  // Calls functions
    const AstNode* m_unknownp = nullptr;  // Node with effects that cannot be analyzed

private:
    static AstVarScope* stateVscp(const AstVarRef* refp) {
        AstVarScope* const vscp = refp->varScopep();
        if (!vscp || vscp->varp()->isFuncLocal()) return nullptr;
        return vscp;
    }
    void read(AstVarScope* vscp) {
        if (m_readSet.insert(vscp).second) m_reads.push_back(vscp);
        if (m_def.count(vscp)) return;
        if (m_inReadSet.insert(vscp).second) m_inReads.push_back(vscp);
    }
    void write(AstVarScope* vscp) {
        if (m_writeSet.insert(vscp).second) m_writes.push_back(vscp);
    }
    void unknown(const AstNode* nodep) {
        if (!m_unknownp) m_unknownp = nodep;
    }

    // VISITORS
    void visit(AstVarRef* nodep) override {
        AstVarScope* const vscp = stateVscp(nodep);
        if (!vscp) return;
        if (nodep->access().isReadOrRW()) read(vscp);
        if (nodep->access().isWriteOrRW()) write(vscp);
    }
    void visit(AstNodeAssign* nodep) override {
        if (nodep->timingControlp()) unknown(nodep);
        iterateConst(nodep->rhsp());
        iterateConst(nodep->lhsp());
        if (const AstVarRef* const refp = VN_CAST(nodep->lhsp(), VarRef)) {
            AstVarScope* const vscp = stateVscp(refp);
            if (vscp && !m_noDef) m_def.insert(vscp);
        }
    }
    void visit(AstNodeIf* nodep) override {
        iterateConst(nodep->condp());
        const std::set<const AstVarScope*> before = m_def;
        iterateAndNextConstNull(nodep->thensp());
        const std::set<const AstVarScope*> thenDef = std::move(m_def);
        m_def = before;
        iterateAndNextConstNull(nodep->elsesp());
        // Definitely written after the 'if' only if written on both paths
        std::set<const AstVarScope*> both = before;
        for (const AstVarScope* const vscp : thenDef) {
            if (m_def.count(vscp)) both.insert(vscp);
        }
        m_def = std::move(both);
    }
    void visit(AstJumpBlock* nodep) override {
        ++m_noDef;
        iterateChildrenConst(nodep);
        --m_noDef;
    }
    void visit(AstLoop* nodep) override {
        ++m_noDef;
        iterateChildrenConst(nodep);
        --m_noDef;
    }
    void visit(AstNodeCCall* nodep) override {
        m_calls = true;
        iterateAndNextConstNull(nodep->argsp());
        AstCFunc* const funcp = nodep->funcp();
        if (funcp->dpiImportWrapper() || funcp->dpiExportDispatcher() || funcp->isCoroutine()
            || !m_callStack.insert(funcp).second) {
            unknown(nodep);
            return;
        }
        ++m_noDef;
        iterateAndNextConstNull(funcp->stmtsp());
        --m_noDef;
        m_callStack.erase(funcp);
    }
    void visit(AstCMethodHard* nodep) override {
        if (!nodep->isPure()) {
            // The subject may be modified, keep the statement in order
            m_impure = true;
            nodep->fromp()->foreach([&](const AstVarRef* refp) {
                if (AstVarScope* const vscp = stateVscp(refp)) {
                    read(vscp);
                    write(vscp);
                }
            });
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstCExpr* nodep) override {
        // A pure C expression has no effects, but its value may depend on
        // simulation context state (e.g. assertion control), so it is not skipped
        if (!nodep->isPure()) unknown(nodep);
        m_impure = true;
        iterateChildrenConst(nodep);
    }
    void visit(AstCStmt* nodep) override { unknown(nodep); }
    void visit(AstCStmtUser* nodep) override { unknown(nodep); }
    void visit(AstCExprUser* nodep) override { unknown(nodep); }
    void visit(AstVarXRef* nodep) override { unknown(nodep); }
    void visit(AstNodeFTaskRef* nodep) override { unknown(nodep); }
    void visit(AstNodeStmt* nodep) override {
        // Other statements ($display, $finish, ...) have effects beyond variables
        if (!VN_IS(nodep, StmtExpr) && !VN_IS(nodep, Comment) && !VN_IS(nodep, LoopTest)
            && !VN_IS(nodep, JumpGo) && !VN_IS(nodep, CReturn)) {
            m_impure = true;
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeExpr* nodep) override {
        if (!nodep->isPure() || !nodep->isPredictOptimizable() || nodep->isOutputter()) {
            m_impure = true;
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    // CONSTRUCTORS
    explicit RtlFastAccessVisitor(AstNode* nodep) {
        iterateConst(nodep);
        for (AstVarScope* const vscp : m_writes) {
            if (m_def.count(vscp)) m_defs.push_back(vscp);
        }
    }
    ~RtlFastAccessVisitor() override = default;
};

class RtlFastGate final {
    // Statements are merged into one gated partition up to this size (in AST nodes)
    static constexpr size_t PARTITION_SIZE = 200;
    // Size limit for groups of impure statements; these are mostly skipped as a
    // whole, but huge functions are slow to compile
    static constexpr size_t IMPURE_GROUP_SIZE = 4000;

    enum class Kind : uint8_t {
        GATED,  // Pure statements, run when an input changed
        COND,  // Conditional statements, run when a condition changed or holds
        ALWAYS  // Other impure statements, always run
    };
    struct Stmt final {
        AstNode* m_nodep = nullptr;  // The statement
        AstScope* m_scopep = nullptr;  // Scope of the function the statement was in
        size_t m_domain = 0;  // Index into m_domains
        std::vector<AstVarScope*> m_inReads;  // Read before definitely written
        std::vector<AstVarScope*> m_reads;  // Read
        std::vector<AstVarScope*> m_writes;  // Written
        std::vector<AstVarScope*> m_defs;  // Definitely fully written
        bool m_impure = false;  // Must not be skipped
        bool m_calls = false;  // Calls functions
        AstVarScope* m_condp = nullptr;  // Condition temporary of a conditional statement
        size_t m_size = 0;  // Node count
    };
    struct Part final {
        size_t m_domain = 0;  // Index into m_domains
        AstScope* m_scopep = nullptr;  // Scope of all statements
        Kind m_kind = Kind::GATED;
        bool m_calls = false;  // Some statement calls functions
        std::vector<size_t> m_stmts;  // Indices into m_stmts
        std::vector<AstVarScope*> m_ins;  // Inputs
        std::vector<AstVarScope*> m_outs;  // Written variables
        size_t m_size = 0;  // Node count
        int m_flag = -1;  // Dirty flag index if not ALWAYS
    };
    struct VarInfo final {
        std::vector<size_t> m_writerParts;  // Partitions writing the variable
        std::vector<int> m_consumerFlags;  // Dirty flags of partitions reading it
        bool m_outside = false;  // Written outside the nba region at run time
    };

    // NODE STATE
    //  AstVarScope::user1()  -> index + 1 into m_vars
    const VNUser1InUse m_inuser1;

    AstNetlist* const m_netlistp;
    std::vector<AstIf*> m_domains;  // The trigger checks in the nba region, in order
    std::vector<Stmt> m_stmts;  // The nba statements, in order
    std::vector<Part> m_parts;  // The partitions, in order
    std::vector<VarInfo> m_vars;
    std::vector<AstVarScope*> m_varps;  // Variable of each m_vars entry
    std::set<AstCFunc*> m_subFuncps;  // Functions holding the nba statements
    std::set<const AstVarScope*> m_condTemps;  // Condition temporaries
    AstVarScope* m_dirtyp = nullptr;  // The dirty flag array
    const AstNode* m_unknownp = nullptr;  // Node with effects that cannot be analyzed
    size_t m_partitionSize = PARTITION_SIZE;
    int m_nFlags = 0;
    size_t m_nTemps = 0;
    size_t m_nShadows = 0;
    size_t m_nWriteSites = 0;
    size_t m_nSnapshots = 0;

    VarInfo& varInfo(AstVarScope* vscp) {
        if (!vscp->user1()) {
            m_vars.emplace_back();
            m_varps.push_back(vscp);
            vscp->user1(static_cast<int>(m_vars.size()));
        }
        return m_vars[vscp->user1() - 1];
    }

    static AstCFunc* findFunc(AstNetlist* netlistp, const string& name) {
        AstCFunc* resultp = nullptr;
        netlistp->topScopep()->scopep()->foreach([&](AstCFunc* funcp) {
            if (funcp->name() == name) resultp = funcp;
        });
        return resultp;
    }

    AstVarScope* newTemp(const string& prefix, AstNodeDType* dtypep) {
        AstVarScope* const vscp = m_netlistp->topScopep()->createTemp(
            "__Vrtlfast_" + prefix + std::to_string(m_nTemps++), dtypep);
        vscp->varp()->noReset(true);
        vscp->varp()->isInternal(true);
        return vscp;
    }

    // Collect the ordered nba statements. Returns false if the region does not
    // have the expected shape.
    bool collect(AstCFunc* bodyp) {
        for (AstNode* nodep = bodyp->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstIf* const ifp = VN_CAST(nodep, If);
            if (!ifp || !VN_IS(ifp->condp(), Triggered) || ifp->elsesp()) return false;
            const size_t domain = m_domains.size();
            m_domains.push_back(ifp);
            for (AstNode* callp = ifp->thensp(); callp; callp = callp->nextp()) {
                const AstStmtExpr* const stmtp = VN_CAST(callp, StmtExpr);
                const AstCCall* const ccallp = stmtp ? VN_CAST(stmtp->exprp(), CCall) : nullptr;
                if (!ccallp || ccallp->argsp()) return false;
                AstCFunc* const funcp = ccallp->funcp();
                if (funcp->argsp() || funcp->isCoroutine() || funcp->rtnTypeVoid() != "void"
                    || !m_subFuncps.insert(funcp).second) {
                    return false;
                }
                for (AstNode* sp = funcp->stmtsp(); sp; sp = sp->nextp()) {
                    m_stmts.emplace_back();
                    m_stmts.back().m_nodep = sp;
                    m_stmts.back().m_scopep = funcp->scopep();
                    m_stmts.back().m_domain = domain;
                }
            }
        }
        return !m_stmts.empty();
    }

    // Mark variables written at run time outside the nba statements
    bool markOutsideWrites() {
        // Functions that only run before the first evaluation (or after the last)
        std::set<const AstCFunc*> preOnly;
        std::set<const AstCFunc*> runtime;
        const auto reach = [&](AstCFunc* rootp, std::set<const AstCFunc*>& seen) {
            std::vector<AstCFunc*> stack{rootp};
            while (!stack.empty()) {
                AstCFunc* const funcp = stack.back();
                stack.pop_back();
                if (!seen.insert(funcp).second) continue;
                // Statements of the nba region are analyzed separately
                if (m_subFuncps.count(funcp)) continue;
                funcp->foreach([&](AstNodeCCall* callp) { stack.push_back(callp->funcp()); });
            }
        };
        for (const VEval eval : {VEval::STATIC, VEval::INITIAL, VEval::STL, VEval::FINAL}) {
            reach(m_netlistp->evalFuncp(eval), preOnly);
        }
        for (const VEval eval : {VEval::SAMPLE, VEval::ICO, VEval::ACT, VEval::INACT, VEval::NBA,
                                 VEval::OBS, VEval::REACT, VEval::POSTPONED}) {
            reach(m_netlistp->evalFuncp(eval), runtime);
        }
        // Callees of the nba statements are analyzed with their callers
        std::set<const AstCFunc*> nbaCallees;
        for (AstCFunc* const funcp : m_subFuncps) {
            funcp->foreach([&](AstNodeCCall* callp) { reach(callp->funcp(), nbaCallees); });
        }
        bool ok = true;
        m_netlistp->foreach([&](AstCFunc* funcp) {
            if (m_subFuncps.count(funcp)) return;
            if (!runtime.count(funcp)) {
                if (preOnly.count(funcp)) return;
                if (nbaCallees.count(funcp)) return;
            }
            // Runs at run time outside the nba statements (or unknown when)
            funcp->foreach([&](AstNode* nodep) {
                if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
                    if (refp->access().isWriteOrRW() && refp->varScopep()) {
                        varInfo(refp->varScopep()).m_outside = true;
                    }
                } else if (AstCMethodHard* const methodp = VN_CAST(nodep, CMethodHard)) {
                    if (!methodp->isPure()) {
                        methodp->fromp()->foreach([&](const AstVarRef* refp) {
                            if (refp->varScopep()) varInfo(refp->varScopep()).m_outside = true;
                        });
                    }
                } else if (VN_IS(nodep, CStmtUser) || VN_IS(nodep, CExprUser)) {
                    ok = false;
                }
            });
        });
        // Variables the user can write between evaluations
        m_netlistp->foreach([&](AstVarScope* vscp) {
            const AstVar* const varp = vscp->varp();
            if (varp->isPrimaryInish() || varp->isSigUserRWPublic() || varp->isWrittenByDpi()
                || varp->isForced()) {
                varInfo(vscp).m_outside = true;
            }
        });
        return ok;
    }

    bool analyzeStmt(Stmt& stmt) {
        const RtlFastAccessVisitor visitor{stmt.m_nodep};
        if (visitor.m_unknownp) {
            m_unknownp = visitor.m_unknownp;
            return false;
        }
        stmt.m_inReads = visitor.m_inReads;
        stmt.m_reads = visitor.m_reads;
        stmt.m_writes = visitor.m_writes;
        stmt.m_defs = visitor.m_defs;
        stmt.m_impure = visitor.m_impure;
        stmt.m_calls = visitor.m_calls;
        stmt.m_size = stmt.m_nodep->nodeCount();
        stmt.m_condp = nullptr;
        if (const AstIf* const ifp = VN_CAST(stmt.m_nodep, If)) {
            const AstVarRef* const refp = VN_CAST(ifp->condp(), VarRef);
            if (stmt.m_impure && !ifp->elsesp() && refp && m_condTemps.count(refp->varScopep())) {
                stmt.m_condp = refp->varScopep();
            }
        }
        return true;
    }

    // Analyze the statements, splitting the pure condition off impure 'if's
    bool analyze() {
        std::vector<Stmt> stmts;
        stmts.reserve(m_stmts.size());
        for (Stmt& stmt : m_stmts) {
            if (!analyzeStmt(stmt)) return false;
            AstIf* const ifp = VN_CAST(stmt.m_nodep, If);
            if (stmt.m_impure && ifp && !ifp->elsesp() && !stmt.m_condp) {
                const RtlFastAccessVisitor condVisitor{ifp->condp()};
                if (!condVisitor.m_impure && !condVisitor.m_unknownp) {
                    FileLine* const flp = ifp->condp()->fileline();
                    AstNodeExpr* const condp = ifp->condp();
                    AstVarScope* const tempp = newTemp("cond", condp->dtypep());
                    m_condTemps.insert(tempp);
                    condp->replaceWith(new AstVarRef{flp, tempp, VAccess::READ});
                    AstAssign* const assignp
                        = new AstAssign{flp, new AstVarRef{flp, tempp, VAccess::WRITE}, condp};
                    ifp->addHereThisAsNext(assignp);
                    Stmt condStmt;
                    condStmt.m_nodep = assignp;
                    condStmt.m_scopep = stmt.m_scopep;
                    condStmt.m_domain = stmt.m_domain;
                    if (!analyzeStmt(condStmt) || !analyzeStmt(stmt)) return false;
                    stmts.push_back(std::move(condStmt));
                }
            }
            stmts.push_back(std::move(stmt));
        }
        m_stmts = std::move(stmts);
        return true;
    }

    // Reorder the statements of each domain (see top of file)
    void order() {
        std::vector<size_t> newOrder;
        newOrder.reserve(m_stmts.size());
        size_t begin = 0;
        while (begin < m_stmts.size()) {
            size_t end = begin;
            while (end < m_stmts.size() && m_stmts[end].m_domain == m_stmts[begin].m_domain) {
                ++end;
            }
            orderDomain(begin, end, newOrder);
            begin = end;
        }
        std::vector<Stmt> stmts;
        stmts.reserve(m_stmts.size());
        for (const size_t i : newOrder) stmts.push_back(std::move(m_stmts[i]));
        m_stmts = std::move(stmts);
    }
    void orderDomain(size_t begin, size_t end, std::vector<size_t>& newOrder) {
        const size_t n = end - begin;
        std::vector<std::vector<uint32_t>> succs(n);  // Order constraints
        std::vector<std::vector<uint32_t>> dataPreds(n);  // Data flow (read after write)
        std::vector<bool> hasDataSucc(n, false);
        std::vector<uint32_t> nPreds(n, 0);
        const auto addEdge = [&](uint32_t from, uint32_t to, bool data) {
            if (from == to) return;
            succs[from].push_back(to);
            ++nPreds[to];
            if (data) {
                dataPreds[to].push_back(from);
                hasDataSucc[from] = true;
            }
        };
        std::unordered_map<const AstVarScope*, uint32_t> lastWriter;
        std::unordered_map<const AstVarScope*, std::vector<uint32_t>> readers;
        int lastImpure = -1;
        for (uint32_t i = 0; i < n; ++i) {
            const Stmt& stmt = m_stmts[begin + i];
            for (const AstVarScope* const vscp : stmt.m_reads) {
                const auto it = lastWriter.find(vscp);
                if (it != lastWriter.end()) addEdge(it->second, i, true);
                readers[vscp].push_back(i);
            }
            for (const AstVarScope* const vscp : stmt.m_writes) {
                const auto it = lastWriter.find(vscp);
                if (it != lastWriter.end()) addEdge(it->second, i, false);
                std::vector<uint32_t>& rds = readers[vscp];
                for (const uint32_t r : rds) addEdge(r, i, false);
                rds.clear();
                lastWriter[vscp] = i;
            }
            if (stmt.m_impure) {
                if (lastImpure >= 0) addEdge(static_cast<uint32_t>(lastImpure), i, false);
                lastImpure = static_cast<int>(i);
            }
        }
        // Preferred order: depth first post-order over the data flow from the sinks
        std::vector<uint32_t> rank(n, 0);
        std::vector<bool> visited(n, false);
        uint32_t nextRank = 0;
        std::vector<std::pair<uint32_t, size_t>> stack;
        for (uint32_t root = 0; root < n; ++root) {
            if (hasDataSucc[root] || visited[root]) continue;
            visited[root] = true;
            stack.emplace_back(root, 0);
            while (!stack.empty()) {
                const uint32_t v = stack.back().first;
                size_t& next = stack.back().second;
                if (next < dataPreds[v].size()) {
                    const uint32_t p = dataPreds[v][next++];
                    if (!visited[p]) {
                        visited[p] = true;
                        stack.emplace_back(p, 0);
                    }
                    continue;
                }
                rank[v] = nextRank++;
                stack.pop_back();
            }
        }
        UASSERT(nextRank == n, "Statement not reached from a data flow sink");
        // Topological order closest to the preferred order; impure statements
        // as late as possible so they cluster
        using Entry = std::pair<uint32_t, uint32_t>;  // rank, statement
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pureReady;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> impureReady;
        const auto makeReady = [&](uint32_t i) {
            if (m_stmts[begin + i].m_impure) {
                impureReady.emplace(rank[i], i);
            } else {
                pureReady.emplace(rank[i], i);
            }
        };
        for (uint32_t i = 0; i < n; ++i) {
            if (!nPreds[i]) makeReady(i);
        }
        size_t emitted = 0;
        while (!pureReady.empty() || !impureReady.empty()) {
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>& queue
                = pureReady.empty() ? impureReady : pureReady;
            const uint32_t i = queue.top().second;
            queue.pop();
            newOrder.push_back(begin + i);
            ++emitted;
            for (const uint32_t s : succs[i]) {
                if (!--nPreds[s]) makeReady(s);
            }
        }
        UASSERT(emitted == n, "Cycle in statement order constraints");
    }

    static Kind kindOf(const Stmt& stmt) {
        if (!stmt.m_impure) return Kind::GATED;
        return stmt.m_condp ? Kind::COND : Kind::ALWAYS;
    }

    void partition() {
        Part* curp = nullptr;
        for (size_t i = 0; i < m_stmts.size(); ++i) {
            const Stmt& stmt = m_stmts[i];
            const Kind kind = kindOf(stmt);
            if (!curp || curp->m_domain != stmt.m_domain || curp->m_scopep != stmt.m_scopep
                || curp->m_kind != kind
                || curp->m_size >= (kind == Kind::GATED ? m_partitionSize : IMPURE_GROUP_SIZE)) {
                m_parts.emplace_back();
                curp = &m_parts.back();
                curp->m_domain = stmt.m_domain;
                curp->m_scopep = stmt.m_scopep;
                curp->m_kind = kind;
            }
            curp->m_stmts.push_back(i);
            curp->m_size += stmt.m_size;
            curp->m_calls |= stmt.m_calls;
        }
        // Inputs and outputs of each partition
        for (size_t p = 0; p < m_parts.size(); ++p) {
            Part& part = m_parts[p];
            std::set<const AstVarScope*> def;  // Definitely written so far
            std::set<const AstVarScope*> ins;
            std::set<const AstVarScope*> outs;
            for (const size_t i : part.m_stmts) {
                const Stmt& stmt = m_stmts[i];
                if (part.m_kind == Kind::COND) {
                    if (ins.insert(stmt.m_condp).second) part.m_ins.push_back(stmt.m_condp);
                } else if (part.m_kind == Kind::GATED) {
                    for (AstVarScope* const vscp : stmt.m_inReads) {
                        if (!def.count(vscp) && ins.insert(vscp).second) {
                            part.m_ins.push_back(vscp);
                        }
                    }
                }
                for (AstVarScope* const vscp : stmt.m_writes) {
                    if (outs.insert(vscp).second) {
                        part.m_outs.push_back(vscp);
                        varInfo(vscp).m_writerParts.push_back(p);
                    }
                }
                def.insert(stmt.m_defs.begin(), stmt.m_defs.end());
            }
        }
        // Variables with more than one writer are inputs of their gated writers
        for (size_t p = 0; p < m_parts.size(); ++p) {
            Part& part = m_parts[p];
            if (part.m_kind != Kind::GATED) continue;
            std::set<const AstVarScope*> ins{part.m_ins.begin(), part.m_ins.end()};
            for (AstVarScope* const vscp : part.m_outs) {
                const VarInfo& info = varInfo(vscp);
                if ((info.m_outside || info.m_writerParts.size() > 1) && ins.insert(vscp).second) {
                    part.m_ins.push_back(vscp);
                }
            }
        }
        // Flags and consumers
        for (Part& part : m_parts) {
            if (part.m_kind == Kind::ALWAYS) continue;
            part.m_flag = m_nFlags++;
            for (AstVarScope* const vscp : part.m_ins) {
                varInfo(vscp).m_consumerFlags.push_back(part.m_flag);
            }
        }
    }

    // Code generation helpers
    static AstNode* append(AstNode* listp, AstNode* newp) {
        return newp ? AstNode::addNext(listp, newp) : listp;
    }
    AstNodeExpr* flagRef(FileLine* flp, int flag, VAccess access) {
        return new AstArraySel{flp, new AstVarRef{flp, m_dirtyp, access}, flag};
    }
    AstNodeStmt* setFlag(FileLine* flp, int flag, bool value) {
        return new AstAssign{flp, flagRef(flp, flag, VAccess::WRITE),
                             new AstConst{flp, AstConst::BitTrue{}, value}};
    }
    // Statements marking all consumers of a variable dirty
    AstNodeStmt* markConsumers(FileLine* flp, const VarInfo& info) {
        AstNodeStmt* stmtsp = nullptr;
        for (const int flag : info.m_consumerFlags) {
            stmtsp = AstNode::addNext(stmtsp, setFlag(flp, flag, true));
        }
        return stmtsp;
    }
    static bool comparable(AstNodeDType* dtypep) {
        const AstBasicDType* const basicp = VN_CAST(dtypep->skipRefp(), BasicDType);
        return basicp && !basicp->isString() && !basicp->isDouble() && !basicp->isEvent()
               && !basicp->isOpaque();
    }
    // The variable reference an assignment target writes, if the target is
    // a variable or a selection with pure indices
    static AstVarRef* simpleTarget(AstNodeExpr* lhsp) {
        if (AstVarRef* const refp = VN_CAST(lhsp, VarRef)) return refp;
        if (AstArraySel* const selp = VN_CAST(lhsp, ArraySel)) {
            if (!pureExpr(selp->bitp())) return nullptr;
            return simpleTarget(selp->fromp());
        }
        if (AstSel* const selp = VN_CAST(lhsp, Sel)) {
            if (!pureExpr(selp->lsbp())) return nullptr;
            return simpleTarget(selp->fromp());
        }
        return nullptr;
    }
    static bool pureExpr(AstNodeExpr* exprp) {
        const RtlFastAccessVisitor visitor{exprp};
        return !visitor.m_impure && !visitor.m_unknownp && !visitor.m_calls;
    }
    // Compare at each assignment: 'lhs = rhs' becomes
    // 'tmp = rhs; if (lhs != tmp) { lhs = tmp; mark consumers }'
    void addWriteSiteCompare(AstNodeAssign* assignp, const VarInfo& info) {
        FileLine* const flp = assignp->fileline();
        AstNodeExpr* const lhsp = assignp->lhsp();
        AstNodeExpr* const oldp = lhsp->cloneTree(false);
        oldp->foreach([](AstVarRef* refp) { refp->access(VAccess::READ); });
        AstNodeExpr* newp;
        AstNode* prep = nullptr;
        if (VN_IS(assignp->rhsp(), VarRef) || VN_IS(assignp->rhsp(), Const)) {
            newp = assignp->rhsp()->cloneTree(false);
        } else {
            AstVarScope* const tempp = newTemp("wr", lhsp->dtypep());
            prep = new AstAssign{flp, new AstVarRef{flp, tempp, VAccess::WRITE},
                                 assignp->rhsp()->unlinkFrBack()};
            assignp->rhsp(new AstVarRef{flp, tempp, VAccess::READ});
            newp = new AstVarRef{flp, tempp, VAccess::READ};
        }
        AstIf* const ifp = new AstIf{flp, new AstNeq{flp, oldp, newp}};
        assignp->replaceWith(ifp);
        ifp->addThensp(assignp);
        ifp->addThensp(markConsumers(flp, info));
        if (prep) ifp->addHereThisAsNext(prep);
        ++m_nWriteSites;
    }
    // Add change detection of the outputs of a partition to its function
    void addChangeDetect(FileLine* flp, const Part& part, AstCFunc* funcp) {
        AstNode* const stmtsp = funcp->stmtsp();
        // Write sites of each output, if all are simple assignments
        std::map<const AstVarScope*, std::vector<AstNodeAssign*>> sites;
        std::set<const AstVarRef*> siteRefs;
        if (!part.m_calls) {
            for (AstNode* nodep = stmtsp; nodep; nodep = nodep->nextp()) {
                nodep->foreach([&](AstNodeAssign* assignp) {
                    AstVarRef* const refp = simpleTarget(assignp->lhsp());
                    if (!refp || !refp->varScopep() || !comparable(assignp->lhsp()->dtypep())) {
                        return;
                    }
                    sites[refp->varScopep()].push_back(assignp);
                    siteRefs.insert(refp);
                });
            }
            for (AstNode* nodep = stmtsp; nodep; nodep = nodep->nextp()) {
                nodep->foreach([&](const AstVarRef* refp) {
                    if (refp->access().isWriteOrRW() && !siteRefs.count(refp)) {
                        sites.erase(refp->varScopep());
                    }
                });
            }
        }
        AstNode* prep = nullptr;
        AstNode* postp = nullptr;
        for (AstVarScope* const vscp : part.m_outs) {
            const VarInfo& info = varInfo(vscp);
            if (info.m_consumerFlags.empty()) continue;
            const auto it = sites.find(vscp);
            if (it != sites.end()) {
                for (AstNodeAssign* const assignp : it->second) addWriteSiteCompare(assignp, info);
                continue;
            }
            if (!comparable(vscp->dtypep())) {
                // Conservatively changed whenever the writer runs
                postp = append(postp, markConsumers(flp, info));
                continue;
            }
            AstVarScope* const oldp = newTemp("old", vscp->dtypep());
            prep = AstNode::addNext(prep, new AstAssign{flp, new AstVarRef{flp, oldp, VAccess::WRITE},
                                                        new AstVarRef{flp, vscp, VAccess::READ}});
            AstIf* const ifp
                = new AstIf{flp, new AstNeq{flp, new AstVarRef{flp, vscp, VAccess::READ},
                                            new AstVarRef{flp, oldp, VAccess::READ}}};
            ifp->addThensp(markConsumers(flp, info));
            postp = AstNode::addNext(postp, ifp);
            ++m_nSnapshots;
        }
        if (prep) funcp->stmtsp()->addHereThisAsNext(prep);
        if (postp) funcp->addStmtsp(postp);
    }

    AstCFunc* newFunc(AstScope* scopep, const string& name) {
        AstCFunc* const funcp = new AstCFunc{scopep->fileline(), name, scopep, ""};
        funcp->isStatic(false);
        funcp->isLoose(true);
        funcp->slow(false);
        scopep->addBlocksp(funcp);
        return funcp;
    }

    void generate(AstCFunc* bodyp) {
        FileLine* const flp = bodyp->fileline();
        // The dirty flags
        AstNodeDType* const bitp = m_netlistp->findBitDType();
        AstUnpackArrayDType* const arrayp
            = new AstUnpackArrayDType{flp, bitp, new AstRange{flp, m_nFlags - 1, 0}};
        m_netlistp->typeTablep()->addTypesp(arrayp);
        m_dirtyp = m_netlistp->topScopep()->createTemp("__Vrtlfast_dirty", arrayp);
        m_dirtyp->varp()->noReset(true);
        m_dirtyp->varp()->isInternal(true);
        // All partitions start dirty
        AstCFunc* const staticp = m_netlistp->evalFuncp(VEval::STATIC);
        for (int flag = 0; flag < m_nFlags; ++flag) staticp->addStmtsp(setFlag(flp, flag, true));
        // Detach the domain bodies (calls to the original functions)
        for (AstIf* const ifp : m_domains) {
            if (AstNode* const callsp = ifp->thensp()) {
                VL_DO_DANGLING(callsp->unlinkFrBackWithNext()->deleteTree(), callsp);
            }
        }
        // Build the partition functions
        std::map<AstScope*, size_t> funcNums;
        for (Part& part : m_parts) {
            AstNode* stmtsp = nullptr;
            for (const size_t i : part.m_stmts) {
                const Stmt& stmt = m_stmts[i];
                stmtsp = AstNode::addNext(stmtsp, stmt.m_nodep->unlinkFrBack());
                if (part.m_kind == Kind::COND) {
                    // Stay dirty while the condition holds
                    AstIf* const ifp = VN_AS(stmt.m_nodep, If);
                    if (AstNode* const thensp = ifp->thensp()) {
                        thensp->addHereThisAsNext(setFlag(flp, part.m_flag, true));
                    }
                }
            }
            const char* const kindName = part.m_kind == Kind::GATED  ? "part"
                                         : part.m_kind == Kind::COND ? "cond"
                                                                     : "always";
            const string name = string{"_nba_rtlfast_"} + kindName + "__"
                                + part.m_scopep->nameDotless() + "__"
                                + std::to_string(funcNums[part.m_scopep]++);
            AstCFunc* const funcp = newFunc(part.m_scopep, name);
            funcp->addStmtsp(stmtsp);
            addChangeDetect(flp, part, funcp);
            AstCCall* const callp = new AstCCall{flp, funcp};
            callp->dtypeSetVoid();
            AstIf* const domainp = m_domains[part.m_domain];
            if (part.m_kind != Kind::ALWAYS) {
                AstIf* const ifp = new AstIf{flp, flagRef(flp, part.m_flag, VAccess::READ)};
                ifp->addThensp(setFlag(flp, part.m_flag, false));
                ifp->addThensp(callp->makeStmt());
                domainp->addThensp(ifp);
            } else {
                domainp->addThensp(callp->makeStmt());
            }
        }
        // Remove the now empty original functions
        for (AstCFunc* const funcp : m_subFuncps) {
            UASSERT_OBJ(!funcp->stmtsp(), funcp, "Statements left behind");
            VL_DO_DANGLING(funcp->unlinkFrBack()->deleteTree(), funcp);
        }
        // Variables changed outside the region: compare with a shadow copy
        AstNode* prep = nullptr;
        AstNode* postp = nullptr;
        for (size_t v = 0; v < m_vars.size(); ++v) {
            const VarInfo& info = m_vars[v];
            if (!info.m_outside || info.m_consumerFlags.empty()) continue;
            AstVarScope* const vscp = m_varps[v];
            if (!comparable(vscp->dtypep())) {
                prep = append(prep, markConsumers(flp, info));
                continue;
            }
            AstVarScope* const shadowp = newTemp("shadow", vscp->dtypep());
            AstIf* const ifp
                = new AstIf{flp, new AstNeq{flp, new AstVarRef{flp, vscp, VAccess::READ},
                                            new AstVarRef{flp, shadowp, VAccess::READ}}};
            ifp->addThensp(markConsumers(flp, info));
            prep = AstNode::addNext(prep, ifp);
            postp = AstNode::addNext(
                postp, new AstAssign{flp, new AstVarRef{flp, shadowp, VAccess::WRITE},
                                     new AstVarRef{flp, vscp, VAccess::READ}});
            ++m_nShadows;
        }
        if (prep) bodyp->stmtsp()->addHereThisAsNext(prep);
        if (postp) bodyp->addStmtsp(postp);
    }

public:
    explicit RtlFastGate(AstNetlist* netlistp)
        : m_netlistp{netlistp} {
        // Development knob for partition size experiments
        if (const char* const envp = std::getenv("VERILATOR_RTLFAST_PARTITION_SIZE")) {
            m_partitionSize = std::max(1, std::atoi(envp));
        }
        AstCFunc* const bodyp = findFunc(netlistp, "_eval_body__nba");
        if (!bodyp || !collect(bodyp)) {
            UINFO(1, "rtl-fast: nba region has an unexpected shape, not gated");
            V3Stats::addStat("RtlFast, gating disabled (shape)", 1);
            return;
        }
        if (!analyze()) {
            UINFO(1, "rtl-fast: cannot analyze " << m_unknownp << ", not gated");
            V3Stats::addStat("RtlFast, gating disabled (unknown effects)", 1);
            return;
        }
        if (!markOutsideWrites()) {
            V3Stats::addStat("RtlFast, gating disabled (user C code)", 1);
            return;
        }
        order();
        partition();
        if (!m_nFlags) return;
        generate(bodyp);
        size_t nOuts = 0;
        size_t nMarks = 0;
        std::map<Kind, size_t> nParts;
        for (const Part& part : m_parts) {
            ++nParts[part.m_kind];
            for (AstVarScope* const vscp : part.m_outs) {
                const VarInfo& info = varInfo(vscp);
                if (info.m_consumerFlags.empty()) continue;
                ++nOuts;
                nMarks += info.m_consumerFlags.size();
            }
        }
        V3Stats::addStat("RtlFast, nba statements", m_stmts.size());
        V3Stats::addStat("RtlFast, condition temporaries", m_condTemps.size());
        V3Stats::addStat("RtlFast, partitions gated", nParts[Kind::GATED]);
        V3Stats::addStat("RtlFast, partitions conditional", nParts[Kind::COND]);
        V3Stats::addStat("RtlFast, partitions always run", nParts[Kind::ALWAYS]);
        V3Stats::addStat("RtlFast, partition outputs compared", nOuts);
        V3Stats::addStat("RtlFast, outputs compared at write sites", m_nWriteSites);
        V3Stats::addStat("RtlFast, outputs compared by snapshot", m_nSnapshots);
        V3Stats::addStat("RtlFast, consumer marks", nMarks);
        V3Stats::addStat("RtlFast, outside variables compared", m_nShadows);
    }
};

//######################################################################
// V3RtlFast class functions

void V3RtlFast::checkSubset(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    { RtlFastSubsetVisitor{nodep}; }
    V3Error::abortIfErrors();
}

void V3RtlFast::gateActivity(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    { RtlFastGate{nodep}; }
    V3Global::dumpCheckGlobalTree("rtlfast", 0, dumpTreeEitherLevel() >= 3);
}
