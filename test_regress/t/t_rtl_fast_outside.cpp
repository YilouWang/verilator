// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: --rtl-fast change detection of logic outside the nba region
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>
#include VM_PREFIX_INCLUDE

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get()}};
    topp->clk = 0;
    topp->eval();
    for (int i = 0; i < 100 && !contextp->gotFinish(); ++i) {
        // Inputs change on some cycles only
        topp->a = (i / 3) * 7;
        if (i % 5 == 0) topp->b = i;
        topp->clk = 1;
        topp->eval();
        topp->clk = 0;
        topp->eval();
    }
    topp->final();
    return 0;
}
