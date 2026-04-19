// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 PlanV GmbH
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got='h%x exp='h%x\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0)
`define checkd(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0d exp=%0d\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0)
// verilog_format: on

typedef enum bit [1:0] { TXN_READ, TXN_WRITE } txn_e;

class ImplBlock;
  rand bit [3:0]  size;
  rand bit [31:0] address;
  rand txn_e      txn_type;

  // Legacy form: expr -> expr ; (already worked before this PR)
  constraint c_legacy {
    txn_type inside {TXN_READ, TXN_WRITE} -> address % (1 << size) == 0;
  }

  // Mirrors the issue #7300 reproducer (the corrected version that VCS accepts).
  constraint c_addr2 {
    (size == 0) -> {
      if (txn_type inside {TXN_READ})
        address % (1 << 4) == 0;
    }
  }

  // Multi-statement brace block on RHS.
  constraint c_multi {
    (size == 4'd1) -> {
      address[0]  == 1'b0;
      address[31] == 1'b0;
    }
  }
endclass

class Nested;
  rand bit [3:0] a;
  rand bit [3:0] b;
  rand bit [3:0] c;
  rand bit [7:0] arr [4];

  // Nested implication inside a brace block.
  constraint c_nested {
    (a == 4'h0) -> {
      (b == 4'h0) -> { c == 4'h5; }
    }
  }

  // if/else inside RHS block.
  constraint c_if_block {
    (a == 4'h1) -> {
      if (b inside {[0:7]})
        c == 4'ha;
      else
        c == 4'hb;
    }
  }

  // foreach inside RHS block.
  constraint c_foreach_block {
    (a == 4'h2) -> {
      foreach (arr[i]) arr[i] < 8'h80;
    }
  }

  // soft constraint inside RHS block.
  constraint c_soft_in_block {
    (a == 4'h4) -> {
      soft b == 4'hd;
    }
  }
endclass

module t;
  ImplBlock obj1;
  Nested    obj2;
  int ok;

  initial begin
    obj1 = new();
    obj2 = new();

    repeat (40) begin
      ok = obj1.randomize();
      `checkd(ok, 1);
      `checkh(obj1.address & ((1 << obj1.size) - 1), 32'h0);
      if (obj1.size == 0 && obj1.txn_type == TXN_READ)
        `checkh(obj1.address[3:0], 4'h0);
      if (obj1.size == 4'd1) begin
        `checkh(obj1.address[0], 1'b0);
        `checkh(obj1.address[31], 1'b0);
      end
    end

    repeat (40) begin
      ok = obj2.randomize();
      `checkd(ok, 1);
      if (obj2.a == 4'h0 && obj2.b == 4'h0)
        `checkh(obj2.c, 4'h5);
      if (obj2.a == 4'h1) begin
        if (obj2.b <= 4'h7) `checkh(obj2.c, 4'ha);
        else                `checkh(obj2.c, 4'hb);
      end
      if (obj2.a == 4'h2) begin
        foreach (obj2.arr[i]) `checkh(obj2.arr[i] < 8'h80, 1'b1);
      end
      if (obj2.a == 4'h4)
        `checkh(obj2.b, 4'hd);
    end

    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
