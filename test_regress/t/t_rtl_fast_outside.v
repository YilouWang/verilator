// DESCRIPTION: Verilator: --rtl-fast change detection of logic outside the nba region
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
    input clk,
    input [7:0] a,
    input [7:0] b
);

  // Combinational logic of the inputs, evaluated outside the nba region
  wire [7:0] s = a + b;
  wire [7:0] m = s ^ 8'h5a;

  logic [15:0] q = 0;
  int cyc = 0;

  // Gated, reads 'm' only
  always_ff @(posedge clk) q <= {8'h0, m} * 16'd3;

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    $display("cyc=%0d m=%0d q=%0d", cyc, m, q);
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule
