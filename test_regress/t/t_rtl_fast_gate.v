// DESCRIPTION: Verilator: --rtl-fast activity gating
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
    input clk
);

  int cyc = 0;
  logic [31:0] crc = 32'h1;
  logic [7:0] mem[16];
  logic [7:0] rd;
  logic [15:0] acc;
  logic [95:0] wide;
  logic rst_n = 1'b0;

  // Combinational logic feeding several registers
  wire [7:0] mix = crc[7:0] ^ crc[23:16];
  wire hit = mix[2:0] == 3'b101;

  // Asynchronous reset domain
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) acc <= '0;
    else if (hit) acc <= acc + {8'b0, rd};
  end

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    crc <= {crc[30:0], crc[31] ^ crc[21] ^ crc[1] ^ crc[0]};
    if (crc[3]) mem[crc[7:4]] <= crc[15:8];
    rd <= mem[crc[11:8]];
    if (crc[4]) wide <= {wide[63:0], crc};
    // Conditional side effect
    if (hit) $display("[%0d] crc=%x rd=%x acc=%x wide=%x", cyc, crc, rd, acc, wide);
    if (cyc == 5) rst_n <= 1'b1;
    if (cyc == 90) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule
