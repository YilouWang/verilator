// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Implementation of assertion properties
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
//
// V3AssertProp is now a no-op in the multi-cycle SVA pipeline. Multi-cycle
// SVA assertions (sequence/property with cycle delays, repetition, throughout,
// sequence and/or/intersect) are compiled by V3AssertNfa, which runs before
// this pass and replaces the assertion's property with a combinational
// accept/reject expression. Constructs the NFA engine cannot handle today
// (e.g. property-local variables in cyclic NFAs, variable-length intersect)
// are reported as UNSUPPORTED by V3AssertNfa.
//
// The legacy DFA/PExpr lowering visitors that previously lived here have
// been removed. The pass entry point is retained because Verilator.cpp still
// schedules it, and the dump-tree call is useful as a checkpoint between
// V3AssertNfa and V3AssertPre.
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3AssertProp.h"

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Top AssertProp class

void V3AssertProp::assertPropAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": (NFA handles multi-cycle -- no legacy lowering)" << endl);
    V3Global::dumpCheckGlobalTree("assertproperties", 0, dumpTreeEitherLevel() >= 3);
}
