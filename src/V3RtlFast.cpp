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
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3RtlFast.h"

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
// V3RtlFast class functions

void V3RtlFast::checkSubset(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    { RtlFastSubsetVisitor{nodep}; }
    V3Error::abortIfErrors();
}
