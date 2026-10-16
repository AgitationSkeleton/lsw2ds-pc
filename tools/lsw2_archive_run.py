#!/usr/bin/env python3
"""Production entry point for the reusable LSW2 archive extractor."""

import lsw2_archive_fast  # installs the reusable decoder into the core module

core = lsw2_archive_fast.core

# Give each 64 MiB sandbox region a non-overlapping quarter of address space.
core.SOURCE_ADDRESS = 0x10000000
core.OUTPUT_ADDRESS = 0x20000000
core.STACK_ADDRESS = 0x30000000
core.RETURN_ADDRESS = 0x40000000

if __name__ == "__main__":
    raise SystemExit(core.main())
