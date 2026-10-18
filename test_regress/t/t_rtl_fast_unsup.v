// DESCRIPTION: Verilator: --rtl-fast rejects input outside its subset
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2026 by Wilson Snyder.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

class Cls;
  int x;
endclass

module t (
    input clk
);
  import "DPI-C" function int dpi_f(int a);
  int cyc;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    #1 cyc <= 0;
    @(negedge clk);
    wait (cyc == 3);
    fork
      cyc <= 2;
    join
    $c("int x;");
  end
endmodule
