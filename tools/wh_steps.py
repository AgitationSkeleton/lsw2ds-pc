"""Decode the wireless step signal the fork already prints, into named sites.

WHAT THIS IS FOR, in plain english. Every `[fork] WH state -> N ... step=XX`
line carries a byte the wireless layer writes at the END of whichever handler
just finished. So the step is not a status code - it is a RETURN ADDRESS in
disguise, and the sequence of them is a narrative of which wireless operations
ran and in what order.

That narrative is what the stalled-data-sharing question needs. Data sharing ends at the match
transition and never restarts, and the question is which handler last ran before
it stopped. Every log already recorded the answer; nothing was decoding it.

WHY THIS IS A SCAN AND NOT A PROBE. An observer override was written on the
event handler first, and it REGRESSED the host - 65 CPU faults and a session
that shut itself down, where the two runs either side of it were clean. It was
reverted. This gets the same information out of the image and out of
logs that already exist, without touching the guest at all.

HOW THE SITES WERE FOUND. The signal is `ctx + 0x48` where ctx is the WH session
authority at 0x0210B320. Scanning the ARM9 image for `STR rD,[rN,#0x48]` finds
179 sites, but they are overwhelmingly `[r13,#0x48]` - stack frames, not this
field. The real ones are a single dense cluster in 0x020ADE78..0x020AF070, each
storing a small distinct constant, which is the wireless module. Those are the
table below.

    python tools/wh_steps.py 2D 29 1C          decode some codes
    python tools/wh_steps.py --log path.log    decode a run's whole sequence

A CAVEAT THAT COST A CORRECTION. Naming the CONTAINING FUNCTION of each site is
less reliable than the site itself: the cached decompilation has overlapping
entries here, and a nearest-preceding-start lookup put the 0x2D at 0x020AE480
inside a spurious 8-byte stub at 0x020AE478 when it is really inside
FUN_020ae388 (0x020AE388, size 0x100). So the ADDRESS is the fact; a function
name beside it is a lead to verify by disassembly, exactly as caveat (r) says.
"""

import sys

# step code -> (site address, note). Read out of the image, not guessed.
STEPS = {
    0x2E: (0x020ADE78, ""),
    0x2F: (0x020ADED8, ""),
    0x30: (0x020ADF60, ""),
    0x31: (0x020ADF80, ""),
    0x32: (0x020AE004, ""),
    0x1C: (0x020AE020, "seen at teardown, and as the session stops"),
    0x1D: (0x020AE0CC, ""),
    0x20: (0x020AE0E8, ""),
    0x21: (0x020AE164, ""),
    0x28: (0x020AE17C, ""),
    0x29: (0x020AE2E4, "the lobby's steady cycle, with 2D"),
    0x2D: (0x020AE308, "AMBIGUOUS - two sites store 2D; see 0x020AE480"),
    0x2A: (0x020AE498, ""),
    0x12: (0x020AE4C4, ""),
    0x14: (0x020AE4E0, ""),
    0x15: (0x020AE58C, ""),
    0x10: (0x020AE5B8, ""),
    0x11: (0x020AE630, ""),
    0x16: (0x020AE874, ""),
    0x17: (0x020AE8DC, "two sites in one function"),
    0x22: (0x020AE91C, ""),
    0x23: (0x020AEB1C, ""),
    0x26: (0x020AECB0, "two sites"),
    0x0E: (0x020AED14, ""),
    0x0F: (0x020AEDB4, ""),
    0x0A: (0x020AEDD4, ""),
    0x0B: (0x020AEE9C, ""),
    0x09: (0x020AEEC4, ""),
    0x18: (0x020AEEE0, ""),
    0x19: (0x020AF070, ""),
}

# The one site whose containing function is established by disassembly rather
# than by the decompilation's overlapping entries: FUN_020ae388 is the wireless
# EVENT handler, its case 10 restarts data sharing, and its tail writes 2D.
SECOND_2D = (0x020AE480, "FUN_020ae388 tail - the EVENT handler, case 10 "
                         "restarts data sharing")


def describe(code):
    if code not in STEPS:
        return "  %02X  unknown - not one of the wireless module's stores" % code
    addr, note = STEPS[code]
    line = "  %02X  %08X" % (code, addr)
    if code == 0x2D:
        line += "  or %08X (%s)" % (SECOND_2D[0], SECOND_2D[1])
    elif note:
        line += "  %s" % note
    return line


def from_log(path):
    """Print the step sequence a run recorded, collapsing runs of repeats."""
    seq = []
    with open(path, "r", errors="ignore") as fh:
        for line in fh:
            i = line.find("step=")
            if i < 0:
                continue
            tok = line[i + 5:i + 7].strip()
            try:
                code = int(tok, 16)
            except ValueError:
                continue
            if seq and seq[-1][0] == code:
                seq[-1][1] += 1
            else:
                seq.append([code, 1])

    if not seq:
        print("no step= lines in %s" % path)
        return 1

    print("%d step transitions (repeats collapsed):" % len(seq))
    for code, n in seq:
        addr = STEPS.get(code, (0, ""))[0]
        print("  step %02X  x%-6d  %s" % (code, n, "%08X" % addr if addr else "?"))
    return 0


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        print("the wireless module's step sites:")
        for code in sorted(STEPS):
            print(describe(code))
        return 0
    if args[0] == "--log":
        if len(args) < 2:
            print("usage: wh_steps.py --log <path>")
            return 2
        return from_log(args[1])
    for a in args:
        try:
            print(describe(int(a, 16)))
        except ValueError:
            print("  %s is not a hex step code" % a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
