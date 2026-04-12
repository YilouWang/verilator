// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 PlanV GmbH
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while(0);
`define checkd(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0d exp=%0d\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0);
// verilog_format: on

module t (
    input clk
);

  int cyc;
  reg [63:0] crc;

  // Derive signals from non-adjacent CRC bits
  wire a = crc[0];
  wire b = crc[4];
  wire c = crc[8];
  wire d = crc[12];

  int count_fail1 = 0;
  int count_fail2 = 0;
  int count_fail3 = 0;
  int count_fail4 = 0;
  int count_fail5 = 0;
  int count_fail6 = 0;
  int count_fail7 = 0;
  int count_fail8 = 0;
  int count_fail9 = 0;
  int count_fail10 = 0;
  int count_fail11 = 0;

  // Test 1: a[*3] |-> b
  assert property (@(posedge clk) a [* 3] |-> b)
  else count_fail1 <= count_fail1 + 1;

  // Test 2: a[*1] |-> c
  assert property (@(posedge clk) a [* 1] |-> c)
  else count_fail2 <= count_fail2 + 1;

  // Test 3: a[*2] |=> d
  assert property (@(posedge clk) a [* 2] |=> d)
  else count_fail3 <= count_fail3 + 1;

  // Test 4: b[*2] standalone
  assert property (@(posedge clk) b [* 2])
  else count_fail4 <= count_fail4 + 1;

  // Tests 5-9,11 disabled: parser-level UNSUPPORTED for [*N>256], [*M:N],
  // [+], [*] in assertion context (boolean_abbrev grammar limitation).
  // Test 5: a[*10000] -- NFA ConsRep guard (N>256, no counter FSM yet)
  // Test 6: a[*1:3] ##1 b -- parser: [*M:N] boolean abbrev
  // Test 7: a[+] ##1 b -- parser: [+] boolean abbrev
  // Test 8: a[+] |-> b -- parser: [+] boolean abbrev
  // Test 9: a[*] |-> b -- parser: [*] boolean abbrev
  // Test 11: a[*] ##1 b -- parser: [*] boolean abbrev

  // Test 10: a[*1] ##1 b -- trivial [*1] in SExpr (restored to plain SExpr)
  assert property (@(posedge clk) a [* 1] ##1 b)
  else count_fail10 <= count_fail10 + 1;

  always @(posedge clk) begin
`ifdef TEST_VERBOSE
    $write("[%0t] cyc==%0d crc=%x a=%b b=%b c=%b d=%b\n", $time, cyc, crc, a, b, c, d);
`endif
    cyc <= cyc + 1;
    crc <= {crc[62:0], crc[63] ^ crc[2] ^ crc[0]};
    if (cyc == 0) begin
      crc <= 64'h5aef0c8d_d70a4497;
    end
    else if (cyc == 99) begin
      `checkh(crc, 64'hc77bb9b3784ea091);
      `checkd(count_fail1, 5);    // Questa: 5
      `checkd(count_fail2, 25);   // Questa: 25
      `checkd(count_fail3, 9);    // Questa: 9
      `checkd(count_fail4, 49);   // Questa: 49
      `checkd(count_fail5, 0);    // disabled (NFA ConsRep guard N>256)
      `checkd(count_fail6, 0);    // disabled (parser [*M:N] boolean abbrev)
      `checkd(count_fail7, 0);    // disabled (parser [+] boolean abbrev)
      `checkd(count_fail8, 0);    // disabled (parser [+] boolean abbrev)
      `checkd(count_fail9, 0);    // disabled (parser [*] boolean abbrev)
      `checkd(count_fail10, 59);  // Questa: 59
      `checkd(count_fail11, 0);   // disabled (parser [*] boolean abbrev)
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule
