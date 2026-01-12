# Minimal Test Case: unique Constraint Bug with 200 Elements

## Problem Description

The `unique` constraint implementation in Verilator produces **duplicate values** when applied to arrays with 200 or more elements, while smaller arrays (up to 100 elements) work correctly.

## Test Case

**File**: `t_constraint_unique_200.{v,sv}`

This is a minimal reproduction case that:
- Declares a single class with one 200-element array
- Applies `unique {arr}` constraint
- Checks all 200 values for duplicates
- Prints array samples for debugging

## Expected Behavior (QuestaSim 2022.3)

```
PASSED: All 200 values are unique
*** TEST PASSED ***
*-* All Finished *-*
```

**Result**: ✅ All 200 values are unique, no duplicates found

## Actual Behavior (Verilator PR)

```
DUPLICATE FOUND: arr[X] == arr[Y] == 0x????
FAILED: Found 1 duplicate pairs in 200-element array
%Error: Assertion failed
```

**Result**: ❌ At least one duplicate value detected

## Test Results Summary

| Array Size | Verilator PR | QuestaSim | Status |
|-----------|--------------|-----------|--------|
| 50        | ✅ Pass      | ✅ Pass   | OK     |
| 100       | ✅ Pass      | ✅ Pass   | OK     |
| **200**   | ❌ **Fail**  | ✅ Pass   | **BUG** |
| 256       | ❌ Fail      | ✅ Pass   | BUG    |

**Conclusion**: The bug appears at array size between 100 and 200.

## How to Run

### With Verilator (Reproduces Bug)
```bash
cd verilator
export VERILATOR_ROOT=/path/to/verilator
./test_regress/t/t_constraint_unique_200.py
```

### With QuestaSim (Expected Behavior)
```bash
cd test_regress/t
vlog -work test_work t_constraint_unique_200.sv
vsim -work test_work -c t_constraint_unique_200 -do "run -all; quit -f"
```

## Debug Information

The test includes a `print_array()` function that displays:
- First 20 array elements (indices 0-19)
- Last 20 array elements (indices 180-199)

This helps identify patterns in the generated values and locate where duplicates occur.

## Suspected Root Causes

1. **SMT constraint generation incomplete**:
   - Missing `distinct` constraints for some array element pairs
   - Loop boundary issues in constraint generation code

2. **Solver result parsing error**:
   - Buffer overflow or indexing error when reading 200+ values
   - Some array indices being skipped or overwritten

3. **Hard-coded size limits**:
   - Possible limit in V3Randomize.cpp or verilated_random.h
   - Array size threshold that triggers different code path

## Recommended Investigation

1. Compare SMT-lib2 output for 100-element vs 200-element arrays
2. Check V3Randomize.cpp for loops generating `distinct` constraints
3. Verify verilated_random.h solver result parsing for large arrays
4. Search for hard-coded constants like 100, 128, 256 in randomization code

## Test File Details

- **Lines of code**: ~70 (minimal)
- **Classes**: 1 (Test200Elements)
- **Constraints**: 1 (unique constraint only)
- **No other complexity**: No additional constraints, no inheritance, no nested structures
- **Pure reproduction**: Focuses solely on the 200-element unique bug
