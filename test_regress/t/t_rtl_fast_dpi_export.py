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

# One partition per statement, so the reader of 'r' is gated on its own
test.setenv("VERILATOR_RTLFAST_PARTITION_SIZE", "1")
test.compile(make_top_shell=False,
             make_main=False,
             verilator_flags2=["--rtl-fast", "--stats", "--exe", test.pli_filename])

test.execute(expect_filename=test.golden_filename)

# The reader of 'r' is gated, and 'r' is compared at the start of the region
test.file_grep(test.stats, r'RtlFast, partitions gated\s+[1-9]')
test.file_grep(test.stats, r'RtlFast, outside variables compared\s+[1-9]')

test.passes()
