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

class All;
  rand bit [3:0] mode;
  rand bit [3:0] a;
  rand bit [3:0] b;
  rand bit [3:0] c;
  rand bit [7:0] arr [4];
  rand bit [3:0] uarr [3];

  // Form 1: expr -> if (cond) stmt;
  constraint c_if {
    (mode == 4'd0) -> if (a == 4'h1) b == 4'h7;
  }

  // Form 2: expr -> if (cond) stmt; else stmt;
  constraint c_if_else {
    (mode == 4'd1) -> if (a == 4'h1) b == 4'ha;
                      else            b == 4'hb;
  }

  // Form 3: expr -> foreach (...) stmt;
  constraint c_foreach {
    (mode == 4'd2) -> foreach (arr[i]) arr[i] < 8'h40;
  }

  // Form 4: expr -> unique { static_array };
  constraint c_unique {
    (mode == 4'd3) -> unique { uarr };
  }

  // Form 5: expr -> soft expr;
  constraint c_soft {
    (mode == 4'd4) -> soft b == 4'hd;
  }

  // Form 6: expr -> { brace block }
  constraint c_brace {
    (mode == 4'd5) -> { b == 4'h2; c == 4'h3; }
  }

  // Form 7: expr -> expr;  (legacy single-expression form)
  constraint c_expr {
    (mode == 4'd6) -> b == 4'h9;
  }
endclass

// Separate class to verify `expr -> disable soft var;` actually takes effect
// conditionally.  When override==0, the soft `x == 4'h5` must hold.  When
// override==1, the implication fires the disable AND forces a hard
// `x == 4'hc`, overriding the soft.  ConstraintExprVisitor's pre-pass
// hoists the disable into `if (override==1) randomizer.disable_soft(x);`
// executed before the solver on every randomize().
class DisSoft;
  bit override_flag;
  rand bit [3:0] x;
  constraint c_soft_x { soft x == 4'h5; }
  constraint c_override {
    (override_flag == 1'b1) -> disable soft x ;
    (override_flag == 1'b1) -> x == 4'hc ;
  }
endclass

module t;
  All obj;
  int ok;

  initial begin
    obj = new();

    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd0; a == 4'h1; };
      `checkd(ok, 1);
      `checkh(obj.b, 4'h7);
    end

    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd1; a == 4'h1; };
      `checkd(ok, 1);
      `checkh(obj.b, 4'ha);
    end
    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd1; a == 4'h2; };
      `checkd(ok, 1);
      `checkh(obj.b, 4'hb);
    end

    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd2; };
      `checkd(ok, 1);
      foreach (obj.arr[i]) `checkh(obj.arr[i] < 8'h40, 1'b1);
    end

    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd3; };
      `checkd(ok, 1);
      `checkh(obj.uarr[0] == obj.uarr[1], 1'b0);
      `checkh(obj.uarr[0] == obj.uarr[2], 1'b0);
      `checkh(obj.uarr[1] == obj.uarr[2], 1'b0);
    end

    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd4; };
      `checkd(ok, 1);
      `checkh(obj.b, 4'hd);
    end

    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd5; };
      `checkd(ok, 1);
      `checkh(obj.b, 4'h2);
      `checkh(obj.c, 4'h3);
    end

    repeat (10) begin
      ok = obj.randomize() with { mode == 4'd6; };
      `checkd(ok, 1);
      `checkh(obj.b, 4'h9);
    end

    begin
      DisSoft ds;
      int dok;
      ds = new();

      // override=0: soft x==5 wins
      ds.override_flag = 1'b0;
      repeat (10) begin
        dok = ds.randomize();
        `checkd(dok, 1);
        `checkh(ds.x, 4'h5);
      end

      // override=1: hoisted runtime disable_soft fires; hard x==c wins
      ds.override_flag = 1'b1;
      repeat (10) begin
        dok = ds.randomize();
        `checkd(dok, 1);
        `checkh(ds.x, 4'hc);
      end
    end

    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
