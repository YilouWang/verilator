// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: --rtl-fast with a DPI export writing state between evaluations
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>
#include VM_PREFIX_INCLUDE
#include "Vt_rtl_fast_dpi_export__Dpi.h"
#include "svdpi.h"

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get()}};
    topp->clk = 0;
    topp->eval();
    svSetScope(svGetScopeFromName("TOP.t"));
    for (int i = 0; i < 100 && !contextp->gotFinish(); ++i) {
        // Change 'r' now and then, never from the design itself
        if (i % 3 == 1) set_r(i * 7);
        topp->clk = 1;
        topp->eval();
        topp->clk = 0;
        topp->eval();
    }
    topp->final();
    return 0;
}
