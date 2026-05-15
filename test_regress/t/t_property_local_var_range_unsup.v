// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 PlanV GmbH
// SPDX-License-Identifier: CC0-1.0

module t (
    input clk
);

  int cyc;
  logic valid;

  // Range delay (##[1:3]) over a property-local match-item capture is
  // not yet supported: per-attempt storage is needed to disambiguate
  // overlapping in-flight attempts.
  property p_range;
    int prev;
    @(posedge clk) (valid,
    prev = cyc
    ) |-> ##[1:3] (cyc > prev);
  endproperty
  assert property (p_range);

  // Composite sequence operator (sequence `and`) under a captured local
  // variable reference is also out of scope for the v1 substitution.
  property p_composite;
    int snap;
    @(posedge clk) (valid,
    snap = cyc
    ) |-> (cyc == snap) and ##1 (cyc == snap + 1);
  endproperty
  assert property (p_composite);

  always @(posedge clk) begin
    cyc <= cyc + 1;
    valid <= (cyc == 2);
  end

endmodule
