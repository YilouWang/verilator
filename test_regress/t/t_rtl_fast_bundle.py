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

test.compile(verilator_flags2=["--rtl-fast", "--stats"])

test.execute(expect_filename=test.golden_filename)

test.file_grep(test.stats, r'RtlFast, partitions in bundles\s+[1-9]')
test.file_grep(test.stats, r'RtlFast, block temporaries kept\s+[1-9]')

# Partition functions are optimized for code size
test.file_grep(test.obj_dir + "/" + test.vm_prefix + "___024root__0.cpp", r'VL_ATTR_OPTSIZE void')

test.passes()
