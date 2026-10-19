// DESCRIPTION: Verilator: --rtl-fast bundles of small partitions
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// One small, independent register block with a memory written by
// non-blocking assignments; 'en' controls how often its inputs change
module blk #(
    parameter int K = 0
) (
    input clk,
    input en,
    input [31:0] seed,
    output logic [31:0] sum
);
  logic [31:0] state = 32'h1 + K;
  logic [7:0] mem[8];
  logic [7:0] rd = '0;
  always_ff @(posedge clk) begin
    if (en) begin
      state <= (state ^ (state << 13) ^ (state >> 7) ^ seed) + 32'(K);
      mem[state[2:0]] <= state[15:8] ^ state[31:24];
    end
    rd <= mem[seed[4:2]];
    sum <= sum + {24'b0, rd} + (state & 32'hff);
  end
endmodule

module t (
    input clk
);

  int cyc = 0;
  logic [31:0] lfsr = 32'h1;
  logic [31:0] sums[6];

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
    if (cyc % 10 == 0) begin
      $display("[%0d] %x %x %x %x %x %x", cyc, sums[0], sums[1], sums[2], sums[3], sums[4],
               sums[5]);
    end
    if (cyc == 90) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

  // Enables of different rates, so some blocks are skipped while others run
  for (genvar k = 0; k < 6; ++k) begin : g
    blk #(.K(k)) u (
        .clk(clk),
        .en(lfsr[k] & lfsr[k+8] & (k < 3 || cyc[k])),
        .seed(cyc[k+2] ? lfsr : 32'h0),
        .sum(sums[k])
    );
  end
endmodule
