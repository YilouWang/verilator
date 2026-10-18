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

#ifndef VERILATOR_V3RTLFAST_H_
#define VERILATOR_V3RTLFAST_H_

#include "config_build.h"
#include "verilatedos.h"

class AstNetlist;

//============================================================================

class V3RtlFast final {
public:
    // Reject input outside the synthesizable synchronous subset (after linking)
    static void checkSubset(AstNetlist* nodep) VL_MT_DISABLED;
    // Gate the 'nba' region logic on input activity (after scheduling)
    static void gateActivity(AstNetlist* nodep) VL_MT_DISABLED;
};

#endif  // Guard
