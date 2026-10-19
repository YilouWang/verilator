// DESCRIPTION: Verilator: --rtl-fast with a DPI export writing state between evaluations
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
    input clk
);

  export "DPI-C" function set_r;

  int r = 0;
  int s = 0;
  int cyc = 0;

  // Called by the C++ main between evaluations
  function void set_r(input int v);
    r = v;
  endfunction

  // Reads 'r', which only set_r changes
  always_ff @(posedge clk) s <= r * 3 + 1;

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    $display("cyc=%0d r=%0d s=%0d", cyc, r, s);
    if (cyc == 20) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule
