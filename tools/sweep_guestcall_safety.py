"""Can this guest function be called synchronously from the host?

THE GOAL THIS SERVES, IN PLAIN ENGLISH. The port calls a handful of the
cartridge's own functions from the host thread (`guestcall.h`). Most are safe.
One class is not, and it has already been expensive:

    coopjoin.h, on 0x020B111C: "IT CANNOT BE CALLED FROM THE HOST ...
    FUN_02072bc8 dispatches the WH object's vtable+0x50 - WH_End - and if that
    returns zero it POLLS it in a do-while until it does not. A synchronous call
    injected from the host can never come back out of that loop."

That fact was written down in a file already in this repo, and a whole round
of work was spent building toward a call that would have hung the guest. The gate that
caught it (`dup_constants`) caught it BY ACCIDENT, because the addresses
collided with names that already existed.

So this makes the question askable on purpose, and cheaply, BEFORE the code is
written rather than after the console stops responding.

WHAT IT LOOKS FOR. A function is unsafe to call synchronously if it - or
anything it reaches - waits for something that only advances when the guest's
own thread runs. It reports exactly one thing as a finding: a loop that spins on
the RETURN VALUE of a call.

    02072C2C  BLX r1
    02072C30  CMP r0,#0x0        <- the call's own result
    02072C34  BEQ 02072C20       <- and back round on it

HOW MUCH TO TRUST IT - MEASURED, NOT ASSERTED. Baselined against the EIGHT
functions this port already calls from the host and that demonstrably work:

    a POSITIVE is WEAK. 0x020B90E4 (the music selector, host-called every day)
    reports eight of them at depth 3. Ordinary code retries things. A finding
    means "go and read this site", never "do not call this".

    a ZERO is MEANINGFUL, because the detector is demonstrably sensitive: it
    finds the real WH_End poll unaided, with the known-unsafe list switched off
    (`--selftest`). Zero across a hundred reachable functions is evidence.

Two earlier versions of this file were wrong and are worth knowing about, since
both produced confident output:

  * the first reported ANY indirect call inside a backward branch. The baseline
    above showed up to three per known-good function - a list walk calling each
    element's virtual method has exactly that shape. That signal is not evidence
    and is now only shown under --verbose, in lower case.
  * the second scanned forward from a call for a test of r0 WITHOUT FOLLOWING
    CONTROL FLOW, and so reported FUN_0208a738 - on the spawn path this tool was
    built to clear - because a `CMP r0,#0x2` sat two instructions past a `BLX`
    with an unconditional `B` in between. It was reading the listing, not the
    program.

WHAT QUESTION THIS IS, AND WHAT IT IS NOT. It is about a call the HOST starts -
`lsw2_guest_call`, which runs guest code synchronously from the host thread and
cannot return until that code does. A polling loop there is fatal because
nothing else advances while it spins.

It is **not** the question to ask about a log-and-delegate OVERRIDE. An override
runs on the guest's own thread, in the middle of the guest's own execution, at a
moment the guest itself chose - a wait inside it is just the game waiting, which
is what the game was going to do anyway. Running this sweep on an override
target and worrying about the result is a category error, and an easy one: the
tool will happily print findings for any address given to it.

WHAT IT DOES NOT DO. It does not prove safety. A wait can be written without a
loop the decoder recognises, and a callee reached only through a vtable is
invisible to a direct-call graph - the same blind spot that made
`FUN_0204b450`'s call sites invisible to an earlier caller scan. Depth is not
free either: past about depth 4 the graph is most of the program and the
known-good baseline lights up too. A clean report means "no return-value spin
was FOUND on the direct-call graph to this depth", which is a reason to proceed
carefully, not a guarantee.

USAGE
    python tools/sweep_guestcall_safety.py 0x02089E60
    python tools/sweep_guestcall_safety.py 0x02089E60 --depth 6 --verbose
    python tools/sweep_guestcall_safety.py --selftest
"""

from __future__ import annotations

import argparse
import bisect
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from armdis import dis  # noqa: E402

IMG_PATH = os.path.join(ROOT, "out", "raw", "arm9_section_0_02000000.bin")
FUNCS_PATH = os.path.join(ROOT, "tools", "ds_funcs.txt")
BASE = 0x02000000

# Known-unsafe leaves, with the reason. These are facts already established in
# the port and are named so a report says WHY rather than just "reachable".
KNOWN_UNSAFE = {
    0x020B111C: "kLsw2WirelessEnd - the session teardown; coopjoin.h: cannot be "
                "called from the host",
    0x02072BC8: "polls the WH object's vtable+0x50 (WH_End) in a do-while",
}


def load():
    with open(IMG_PATH, "rb") as fh:
        img = fh.read()
    with open(FUNCS_PATH) as fh:
        addrs = sorted(int(line.strip(), 16) for line in fh if line.strip())
    return img, addrs


def word_at(img, addr):
    off = addr - BASE
    if off < 0 or off + 4 > len(img):
        return None
    return struct.unpack_from("<I", img, off)[0]


def func_end(addrs, f):
    i = bisect.bisect_right(addrs, f)
    return addrs[i] if i < len(addrs) else f + 0x400


BL_RE = re.compile(r"^BL (?:0x)?([0-9A-Fa-f]{8})$")
BLX_REG_RE = re.compile(r"^BLX r(\d+)$")
BRANCH_RE = re.compile(r"^B([A-Z]{0,2}) (?:0x)?([0-9A-Fa-f]{8})$")

# A flag-setter on r0 - the shape that tests a call's RETURN VALUE.
CMP_R0_RE = re.compile(r"^(?:CMP|CMN|TST|TEQ) r0,")
# Anything that writes r0. Used to prove the value tested IS the call's result.
WRITES_R0_RE = re.compile(r"^(?:MOV|MVN|LDR|LDRB|LDRH|LDRSB|LDRSH|ADD|SUB|RSB|AND|"
                          r"ORR|EOR|BIC|MUL|MLA|SMULL|UMULL|ADC|SBC|LSL|LSR|ASR)"
                          r"[A-Z]{0,2}S? r0,")

# Any branch at all - it ends the fall-through path from a call.
ANY_BRANCH_RE = re.compile(r"^(?:B|BL|BLX|BX)[A-Z]{0,2} ")

# How many instructions may sit between the call and the test of its result.
# The measured poll has ZERO; a couple of slack covers a MOV or a reload.
SPIN_TEST_WINDOW = 3


def analyse(img, addrs, f):
    """Return (direct_calls, loop_ranges, indirect_sites, direct_sites)."""
    end = func_end(addrs, f)
    calls, branches, indirect, direct = [], [], [], []
    for a in range(f, end, 4):
        w = word_at(img, a)
        if w is None:
            break
        t = dis(a)
        m = BL_RE.match(t)
        if m:
            tgt = int(m.group(1), 16)
            calls.append(tgt)
            direct.append((a, tgt))
            continue
        if BLX_REG_RE.match(t):
            indirect.append(a)
            continue
        m = BRANCH_RE.match(t)
        if m:
            cond, tgt = m.group(1), int(m.group(2), 16)
            # A BACKWARD branch inside this function is a loop. An UNCONDITIONAL
            # one cannot be a spin-until-it-returns, so the condition is kept.
            if f <= tgt <= a:
                branches.append((tgt, a, cond))
    return calls, branches, indirect, direct


def in_any_loop(addr, loops):
    return any(lo <= addr <= hi for lo, hi, _cond in loops)


def find_spins(f, loops):
    """The ONLY finding this tool treats as a verdict: a loop that spins on the
    RETURN VALUE of a call.

    THE DISCRIMINATOR, and why it had to exist. The first version reported any
    indirect call inside a backward branch, and a baseline of the EIGHT
    functions this port already calls from the host successfully showed up to
    THREE such findings each. So that signal is not evidence of anything; a list
    walk that calls each element's virtual method has exactly that shape.

    The measured poll does something a list walk never does - it tests r0, the
    call's own result, and branches back on THAT:

        02072C2C  BLX r1
        02072C30  CMP r0,#0x0        <- the return value
        02072C34  BEQ 02072C20       <- spin

    while the false positives follow their call with loads and stores and are
    driven round by a pointer, not by what the call returned. So a spin requires
    all of: a call inside the loop, a flag-setter on r0 within a few
    instructions of it, nothing else writing r0 in between, and a CONDITIONAL
    backward branch. """
    spins = []
    for lo, hi, cond in loops:
        if not cond:                      # unconditional B - not a wait
            continue
        for s in range(lo, hi + 1, 4):
            t = dis(s)
            if not (BLX_REG_RE.match(t) or BL_RE.match(t)):
                continue
            for k in range(1, SPIN_TEST_WINDOW + 1):
                c = s + 4 * k
                if c > hi:
                    break
                tc = dis(c)
                # A BRANCH ENDS THE FALL-THROUGH PATH, and forgetting that was
                # this tool's first real bug. FUN_0208a738 was reported as a
                # spin because a `CMP r0,#0x2` sat two instructions after a
                # `BLX r2` - with an UNCONDITIONAL `B` in between, so the
                # comparison is on a different path entirely and the loop is
                # driven by a counter. Scanning straight through a branch is
                # reading the listing rather than the control flow.
                if ANY_BRANCH_RE.match(tc):
                    break
                if CMP_R0_RE.match(tc):
                    spins.append((s, c, hi, t))
                    break
                if WRITES_R0_RE.match(tc):
                    break                 # the tested value is not the result
    return spins


def sweep(root, max_depth, verbose):
    img, addrs = load()
    seen, findings, order = {}, [], []
    stack = [(root, 0, [root])]
    while stack:
        f, depth, path = stack.pop()
        if f in seen and seen[f] <= depth:
            continue
        seen[f] = depth
        order.append(f)

        if f in KNOWN_UNSAFE and f != root:
            findings.append(("KNOWN", f, None, path, KNOWN_UNSAFE[f]))
            continue

        if f < BASE or f >= BASE + len(img):
            continue

        calls, loops, indirect, direct = analyse(img, addrs, f)

        for s, c, hi, call_text in find_spins(f, loops):
            findings.append(("SPIN-ON-RETURN", f, s, path,
                             "%s at %08X, its result tested at %08X, branched "
                             "back at %08X - a wait" % (call_text, s, c, hi)))

        if verbose:
            for site in indirect:
                if in_any_loop(site, loops):
                    findings.append(("indirect-in-loop", f, site, path,
                                     "an indirect call inside a backward branch "
                                     "- NOT evidence on its own, see --help"))

        if depth >= max_depth:
            continue
        for tgt in calls:
            if tgt not in seen:
                stack.append((tgt, depth + 1, path + [tgt]))

    return findings, order


def fmt_path(path):
    return " -> ".join("FUN_%08x" % p for p in path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", help="function address, e.g. 0x02089E60")
    ap.add_argument("--depth", type=int, default=5)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true",
                    help="check the tool against the case that motivated it")
    args = ap.parse_args()

    if args.selftest:
        # FUN_020b111c must come back UNSAFE through FUN_02072bc8. If this stops
        # holding, the detector has drifted and its clean reports mean nothing.
        findings, _ = sweep(0x02072BC8, 1, False)
        hit = [f for f in findings if f[0] == "SPIN-ON-RETURN"]
        print("selftest: FUN_02072bc8 (the measured WH_End poll) ->", len(hit), "SPIN finding(s)")
        for kind, f, site, path, why in hit[:5]:
            print("   %-17s FUN_%08x %s" % (kind, f, why))
        ok = bool(hit)
        print("SELFTEST", "PASSED" if ok else "FAILED - the detector is blind to "
                                              "the very case it was built for")
        return 0 if ok else 1

    if not args.root:
        ap.error("give a function address, or --selftest")
    root = int(args.root, 0)
    findings, order = sweep(root, args.depth, args.verbose)

    print("guest-call safety sweep of FUN_%08x  (depth %d, %d functions reached)"
          % (root, args.depth, len(order)))
    print()
    if not findings:
        print("  no polling loop found on the direct-call graph.")
    else:
        rank = {"KNOWN": 0, "SPIN-ON-RETURN": 1, "indirect-in-loop": 2}
        for kind, f, site, path, why in sorted(findings, key=lambda x: rank[x[0]]):
            where = " at %08X" % site if site else ""
            print("  [%s] FUN_%08x%s" % (kind, f, where))
            print("      %s" % why)
            print("      via %s" % fmt_path(path))
            print()

    print("  NOTE: a clean report means no polling loop was FOUND on the")
    print("  DIRECT-call graph. Vtable calls are invisible to it, which is the")
    print("  same blind spot that once hid FUN_0204b450's call sites.")
    print("  It is a reason to proceed carefully, not a guarantee.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
