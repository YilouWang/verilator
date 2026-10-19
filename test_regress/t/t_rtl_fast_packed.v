// DESCRIPTION: Verilator: --rtl-fast change detection of packed aggregates
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

typedef struct packed {
  logic [3:0] a;
  logic [11:0] b;
} pair_t;

typedef union packed {
  logic [15:0] w;
  pair_t p;
} word_t;

module t (
    input clk
);

  int cyc = 0;
  logic [31:0] lfsr = 32'h1;
  // Written now and then, read every cycle by another partition
  logic [7:0][3:0] arr = '0;
  pair_t pr = '0;
  word_t wd = '0;
  logic [31:0] sum = 32'h0;

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
    if (lfsr[1:0] == 2'h0) arr[lfsr[6:4]] <= lfsr[10:7];
    if (lfsr[3:2] == 2'h1) pr <= '{a: lfsr[3:0], b: lfsr[15:4]};
    if (lfsr[5:4] == 2'h2) wd.w <= lfsr[31:16];
    sum <= sum + {28'b0, arr[cyc[2:0]]} + {20'b0, pr.b} + {16'b0, wd.p.b, wd.p.a};
    if (cyc % 5 == 0) $display("[%0d] %x %x %x %x", cyc, arr, pr, wd, sum);
    if (cyc == 90) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule
