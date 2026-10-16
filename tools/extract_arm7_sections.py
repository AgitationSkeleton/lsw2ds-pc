"""Split the ARM7 binary into the sections the console actually loads.

WHY IT MATTERS. out/raw/arm7.bin is the ARM7 binary as one flat block, and
Ghidra decompiled it that way - 930 functions, all between 0x02380100 and
0x023A7ED8. But the ARM7 calls code that is not in that range:

    thunk_EXT_FUN_03804414
    thunk_EXT_FUN_038043e2
    thunk_EXT_FUN_038043c8

0x0380xxxx is ARM7 WRAM. That code lives inside the binary on the cartridge and
is at a different address once running - the same situation as the ARM9's ITCM
and DTCM blocks, which WERE split out (out/raw has arm9_section_0_02000000,
_1_01ff8000 and _2_027e0000). The ARM7 never was, so 32KB of it has never been
decompiled at the address it actually runs at.

I first assumed this was the standard NDS autoload mechanism. IT IS NOT: this
binary carries no module-params structure, and no literal anywhere in it
references either end of the copy, so the code that performs the copy has not
been found. The mapping is established by measurement instead - see the note
above WRAM_BLOCK_OFFSET - and the autoload path below is kept because it costs
nothing and would be the right answer for a binary that does have one.

That matters right now: the handler the ARM7 registers for IPC FIFO channel 8 is
0x03804484, and the port is stuck waiting for a reply that handler is supposed
to send.

HOW THE SECTIONS ARE FOUND. An NDS binary carries a module-params structure
ending in two magic words, 0xDEC00621 and 0x2106C0DE. The fields before them
give the bounds of the autoload LIST and the address the autoload DATA starts
at. Each list entry is (destination address, size, bss size), and the data for
successive entries follows on from autoload_start in order.

Nothing here is guessed: if the magic is absent, or the bounds fall outside the
binary, the tool says so and writes nothing rather than inventing a layout.
"""

from __future__ import annotations

import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "out/raw"

NITROCODE_BE = 0xDEC00621
NITROCODE_LE = 0x2106C0DE


def find_module_params(data: bytes) -> int | None:
    """Offset of the module-params structure, or None."""
    for off in range(0, len(data) - 8, 4):
        a, b = struct.unpack_from("<2I", data, off)
        if a == NITROCODE_BE and b == NITROCODE_LE:
            start = off - 24
            return start if start >= 0 else None
    return None


# THE WRAM BLOCK, AND WHAT PROVES IT.
#
# This binary has no module-params structure, so it has no autoload list in the
# standard sense - and no literal in it references either end of the copy, so
# the code that performs the copy has NOT been located. What is established is
# the mapping itself, and it is established by measurement rather than by
# reading the loader:
#
#   arm7.bin offset 0x1B0 corresponds to 0x037F8000, and the next 64KB are
#   BYTE-IDENTICAL to SharedWRAM followed by ARM7WRAM in a savestate of the
#   real console - 100% over the first 48KB. Past that the agreement collapses
#   to 0-13%, which is where the code ends and runtime data begins.
#
# The anchor that started it: the ARM7 registers 0x03804484 as its IPC FIFO
# handler for channel 8, that address holds `PUSH {lr}` on the real machine,
# and the same 64 bytes appear in arm7.bin at 0xC634 - a delta of exactly
# 0x81B0 from 0x4484.
#
# Ghidra decompiled these 32KB at 0x023881B0 instead, so every call INTO them
# came out as `thunk_EXT_FUN_0380xxxx` with no body - 178 distinct WRAM
# addresses the corpus has nothing for.
# CORRECTION: the block is 64KB and starts lower than first measured.
#
# The first pass anchored on 0x03804484 and found arm7.bin+0x81B0 -> 0x03800000
# over 0x8000 bytes. That was right but incomplete: the code in that block calls
# func_0x037fe3a4, func_0x037fcc6c and func_0x037fe9ec, at 0x037Fxxxx, which is
# SHARED WRAM sitting directly below ARM7 WRAM. Testing the obvious candidate -
# 0x81B0 minus 0x8000 - against SharedWRAM in the same savestate gives 8192 of
# 8192 words identical, 100% across all eight 4KB blocks.
#
# So it is one contiguous 64KB region, and extracting it whole matters: split in
# two, every call between the halves is an unresolved external.
#
# 64KB IS RIGHT AND 96KB IS WRONG - a negative result, recorded because the
# wrong answer was reached with a method that looked identical to the right one.
#
# ARM7 WRAM runs to 0x0380FFFF, so this block stops halfway through it, and the
# obvious guess is that the upper 32KB is simply the next 32KB of the file. A
# search for the block's first bytes in a savestate appeared to confirm it: the
# next three 32KB chunks matched 100%, 100%, 100%.
#
# They matched because the search found THE ARM7 BINARY SITTING IN MAIN RAM at
# 0x02380000, where the loader put it - the binary correlated against a copy of
# itself. The signature occurs twice in the savestate; the second occurrence is
# the real WRAM, and there the numbers are
#
#   0x037F8000-0x037FFFFF   100.0%
#   0x03800000-0x03807FFF   100.0%
#   0x03808000-0x0380FFFF     1.6%     <- collapses
#
# which is the original 48KB-then-collapse result, and says the block is 64KB.
# The upper half of ARM7 WRAM is runtime data, not code copied from the ROM: on
# the real console it is 2,636 non-zero bytes out of 32,768, and the address
# that prompted the whole search reads as zeros.
#
# THE CHECK THAT CATCHES THIS: a correct correlation STOPS. If the match runs
# past the end of the region you are testing - here 0x03810000, beyond ARM7
# WRAM entirely - you are not looking at that region. Take every occurrence of
# the signature, not the first.
WRAM_BLOCK_OFFSET = 0x1B0
WRAM_BLOCK_ADDR = 0x037F8000
WRAM_BLOCK_SIZE = 0x10000


def emit_wram_block(binary: bytes, ram: int) -> None:
    end = WRAM_BLOCK_OFFSET + WRAM_BLOCK_SIZE
    if end > len(binary):
        print("the WRAM block runs past the end of the binary - not writing it")
        return
    blob = binary[WRAM_BLOCK_OFFSET:end]
    p = OUT / ("arm7_wram_%08x.bin" % WRAM_BLOCK_ADDR)
    p.write_bytes(blob)
    print("wrote %s  0x%08X  %d bytes"
          % (p.name, WRAM_BLOCK_ADDR, len(blob)))
    print("  (from arm7.bin +0x%X; see the note above WRAM_BLOCK_OFFSET)"
          % WRAM_BLOCK_OFFSET)


def main() -> int:
    roms = sorted(ROOT.glob("rom/*.nds"))
    if not roms:
        print("no ROM to read")
        return 1
    rom = roms[0].read_bytes()

    off, entry, ram, size = struct.unpack_from("<4I", rom, 0x30)
    binary = rom[off:off + size]
    print("ARM7 binary: %d bytes, loads at 0x%08X, entry 0x%08X"
          % (size, ram, entry))

    # Whatever the mechanism, the WRAM block is at a fixed offset and its
    # contents are provable. See WRAM_BLOCK below.
    emit_wram_block(binary, ram)

    mp = find_module_params(binary)
    if mp is None:
        print()
        print("no module-params signature: this binary has no standard autoload")
        print("list, so the static section is the whole of it.")
        return 0

    (list_start, list_end, data_start, bss_end,
     compressed_end, sdk) = struct.unpack_from("<6I", binary, mp)
    print("module params at +0x%X" % mp)
    print("  autoload list  0x%08X .. 0x%08X" % (list_start, list_end))
    print("  autoload data  0x%08X" % data_start)
    print("  SDK version    0x%08X" % sdk)

    def to_off(addr: int) -> int:
        return addr - ram

    for name, addr in (("list start", list_start), ("list end", list_end),
                       ("data start", data_start)):
        o = to_off(addr)
        if not (0 <= o <= len(binary)):
            print("  %s is outside the binary - refusing to guess" % name)
            return 1

    # The static section is everything up to where the autoload data begins.
    static_len = to_off(data_start)
    static = binary[:static_len]
    p = OUT / ("arm7_section_0_%08x.bin" % ram)
    p.write_bytes(static)
    print("wrote %s  0x%08X  %d bytes" % (p.name, ram, len(static)))

    pos = to_off(data_start)
    n = 0
    for e in range(to_off(list_start), to_off(list_end), 12):
        dest, dsize, bsize = struct.unpack_from("<3I", binary, e)
        if pos + dsize > len(binary):
            print("  entry %d claims %d bytes past the end - stopping" % (n, dsize))
            break
        blob = binary[pos:pos + dsize]
        q = OUT / ("arm7_section_%d_%08x.bin" % (n + 1, dest))
        q.write_bytes(blob)
        print("wrote %s  0x%08X  %d bytes (+%d bss)"
              % (q.name, dest, dsize, bsize))
        pos += dsize
        n += 1

    if n == 0:
        print("the autoload list is empty - the ARM7 has no relocated sections")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
