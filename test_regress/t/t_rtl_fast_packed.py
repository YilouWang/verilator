#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

# Gate the small test design
test.setenv("VERILATOR_RTLFAST_MIN_NODES", "0")

# One partition per statement, so the aggregates are partition outputs
test.setenv("VERILATOR_RTLFAST_PARTITION_SIZE", "1")
test.compile(verilator_flags2=["--rtl-fast", "--stats"])

test.execute(expect_filename=test.golden_filename)

# Packed arrays, structs and unions are compared, not assumed changed
test.file_grep(test.stats, r'RtlFast, outputs compared at write sites\s+[1-9]')
test.file_grep(test.stats, r'RtlFast, outputs not compared\s+0')

test.passes()
