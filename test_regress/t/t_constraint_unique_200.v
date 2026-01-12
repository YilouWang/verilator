// DESCRIPTION: Verilator: Minimal test case for unique constraint bug with 200 elements
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2026.
// SPDX-License-Identifier: CC0-1.0

// Minimal test to reproduce the duplicate value bug in 200-element array

class Test200Elements;
  rand bit [15:0] arr[200];

  constraint unique_c {
    unique {arr};
  }

  function bit check();
    int duplicate_count = 0;

    // Check for duplicates
    for (int i = 0; i < 200; i++) begin
      for (int j = i + 1; j < 200; j++) begin
        if (arr[i] == arr[j]) begin
          $error("DUPLICATE FOUND: arr[%0d] == arr[%0d] == 0x%h", i, j, arr[i]);
          duplicate_count++;
        end
      end
    end

    if (duplicate_count > 0) begin
      $display("FAILED: Found %0d duplicate pairs in 200-element array", duplicate_count);
      return 0;
    end

    $display("PASSED: All 200 values are unique");
    return 1;
  endfunction

  function void print_array();
    $display("Array values (first 20 and last 20):");
    for (int i = 0; i < 20; i++) begin
      $display("  arr[%3d] = 0x%04h", i, arr[i]);
    end
    $display("  ...");
    for (int i = 180; i < 200; i++) begin
      $display("  arr[%3d] = 0x%04h", i, arr[i]);
    end
  endfunction
endclass

module t_constraint_unique_200;
  initial begin
    Test200Elements test;

    test = new();

    /* verilator lint_off WIDTHTRUNC */
    if (!test.randomize()) begin
      $fatal(1, "FAILED: randomize() returned 0");
    end
    /* verilator lint_on WIDTHTRUNC */

    // Print sample of array values for debugging
    test.print_array();

    if (!test.check()) begin
      $fatal(1, "FAILED: Found duplicate values in 200-element array");
    end

    $display("*** TEST PASSED ***");
    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
