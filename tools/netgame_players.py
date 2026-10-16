"""Read the per-player state whose disagreement IS the Communication Error, and
diff it between the two consoles.

WHAT THIS IS FOR, in plain english. It was established that the match-start
"Communication Error" is a DESYNC, not a radio fault: the netgame step returns
2, meaning `FUN_020af460` failed a comparison. This shows both sides of that
comparison on both consoles so the divergence can be SEEN.

THE LAYOUT, resolved from the vtable rather than guessed:

    netgame vtable 0x02100668
      +0x00 FUN_020af718   accessor(obj, n) -> a station; n == 0 gives
                           `param_1 + 0x90` WORDS, i.e. netgame + 0x240
      +0x04 FUN_020af328   returns `param_1 + 0x640`
      +0x0C FUN_020af330   the step, whose return lands at 0x021092FC
      +0x24 FUN_020af7ec   returns the byte at netgame + 0x6F4 - the gate that
                           decides whether the checksum is recomputed at all

`FUN_020ad21c` is handed the +0x640 sub-object and writes its total to +0x60 of
it, so the checksum is at **netgame + 0x6A0** and the value compared beside it
at **netgame + 0x6A8**. The local per-player records `FUN_020af460` checks
against are `param_1[(id & 7) * 2 + 0x1ad]` with `param_1` an `undefined4 *`,
so they start at **netgame + 0x6B4**, eight bytes each.

An earlier version of this tool read +0x60/+0x68 off the GLOBAL slot pointers
instead of off these objects. Those are different bases - the slot array at
0x02109358 holds the actors whose positions feed the sum, not the records that
are compared - and the numbers it printed were meaningless. Recorded because a
tool printing a plausible wrong number is the trap caveat (f) keeps catching.

WHAT THE SUMMED FIELDS ARE. `FUN_020ad21c` accumulates `slot->0xB0 + 0xB4 +
0xB8` over the live slots. Read from a running host those are 00019000,
00000327, FFFFD000 - which in the DS's 20.12 fixed point are 25.0, 0.197 and
-3.0, i.e. **a position**. So the "checksum" is the sum of the players'
coordinates: a cheap desync signature, exactly what a lockstep game compares.

WHY A DIFF AND NOT AN INTERPRETATION. Rather than decode every field, this
dumps the same windows from both consoles and marks the words that differ.
Divergence is the thing being looked for, and a diff finds it without needing
the meaning of each word first. Run it during a healthy lobby and again after
the failure - the consoles stay alive - and compare the two reports.
"""

import socket
import json
import sys

HOLDER = 0x021092F4
ERROR_WORD = HOLDER + 8
SLOT_ARRAY = 0x02109358   # DAT_020ad2a0 + 0x4C - the actors that feed the sum
MAX_SLOTS = 4
PARTS = (0xB0, 0xB4, 0xB8)

CHECKSUM = 0x6A0          # (netgame + 0x640) + 0x60
SEQUENCE = 0x6A8          # (netgame + 0x640) + 0x68
RECORDS = 0x6B4           # eight bytes per player
GATE = 0x6F4              # FUN_020af7ec - recompute the checksum at all?
STATION0 = 0x240          # accessor(obj, 0)

# THE SOURCE OF THE SEQUENCE, resolved rather than guessed. FUN_020ad21c ends
# with `*(ushort *)(param_1 + 0x68) = (ushort)*DAT_020ad2a4`, and DAT_020ad2a4
# reads 0x02104344 out of the ROM image - so the value published at +0x6A8 is
# the low halfword of a GLOBAL, not something the netgame object owns.
#
# Three functions zero that global: FUN_0207281c and FUN_02072ad8 (the
# view-manager allocators) and FUN_020af274, the netgame reset. Nothing found
# so far increments it, and the FUN_02062xxx cluster treats 0x02104344 as the
# base of a STRUCT with fields at +4, +8, +0x10 and +0x1c - so reading it
# alongside the published sequence is the way to find out what it actually
# tracks, instead of reasoning from the shape of the code.
SEQ_GLOBAL = 0x02104344

STEP_RESULT = {0: "healthy",
               1: "the data exchange did not run",
               2: "the cross-player consistency check FAILED"}


def rd(port, addr, ln):
    # A CONSOLE THAT IS NOT UP YET IS NOT AN ERROR. --watch is armed before the
    # emulators finish booting so it cannot miss the transition, so a refused
    # connection is the normal state for the first minute and must read as
    # "nothing yet" rather than raising.
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    except OSError:
        return ""
    f = s.makefile("rwb")
    f.write((json.dumps({"cmd": "read_mem", "cpu": 9,
                         "addr": addr, "len": ln}) + "\n").encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    try:
        return json.loads(line).get("hex", "")
    except Exception:
        return ""


def words(port, addr, n):
    """n little-endian words, or None for the whole read when it came back
    short. Zero and unreadable stay distinct - caveat (f)."""
    h = rd(port, addr, n * 4)
    if len(h) < n * 8:
        return None
    return [int.from_bytes(bytes.fromhex(h[i * 8:i * 8 + 8]), "little")
            for i in range(n)]


def one(port, addr):
    w = words(port, addr, 1)
    return None if w is None else w[0]


def narrow(port, addr, nbytes):
    """A byte or halfword. THE WIDTH MATTERS AND READING IT WRONG CRIES WOLF.

    The first version read the gate and the record sequence as full words. The
    gate is a byte (FUN_020af7ec returns *(u8 *)(obj + 0x6F4)) and the sequence
    is a short, so the upper bytes are neighbouring data - uninitialised on one
    console and not the other. The diff duly reported "gate host=00000001
    child=0FD40E01" when the byte both sides actually compare is 01 on each.
    FUN_020ad2a8 reads a short at +0 and an int at +4; nothing else is
    compared, so nothing else may be reported as a difference."""
    h = rd(port, addr, nbytes)
    if len(h) < nbytes * 2:
        return None
    return int.from_bytes(bytes.fromhex(h[:nbytes * 2]), "little")


def gather(port):
    st = {"err": one(port, ERROR_WORD), "obj": one(port, HOLDER)}
    if not st["obj"]:
        return st
    o = st["obj"]
    st["checksum"] = one(port, o + CHECKSUM)
    st["sequence"] = narrow(port, o + SEQUENCE, 2)
    st["gate"] = narrow(port, o + GATE, 1)
    # Only what FUN_020ad2a8 actually compares: a short at +0, an int at +4.
    st["records"] = []
    for i in range(MAX_SLOTS):
        st["records"].append(narrow(port, o + RECORDS + i * 8, 2))
        st["records"].append(one(port, o + RECORDS + i * 8 + 4))
    st["station0"] = words(port, o + STATION0, 8)
    st["seqglobal"] = one(port, SEQ_GLOBAL)
    st["slots"] = []
    for i in range(MAX_SLOTS):
        p = one(port, SLOT_ARRAY + i * 4)
        st["slots"].append((p, [one(port, p + q) for q in PARTS] if p else None))
    return st


def fx(v):
    """20.12 fixed point, which is what the summed fields turned out to be."""
    if v is None:
        return "?"
    s = v - (1 << 32) if v & 0x80000000 else v
    return "%9.3f" % (s / 4096.0)


def sig(st):
    """The values FUN_020ad2a8 actually compares, as a comparable tuple."""
    return (st.get("err"), st.get("checksum"), st.get("sequence"),
            st.get("seqglobal"),
            tuple(st.get("records") or ()),
            tuple(p for p, _ in (st.get("slots") or ())))


def watch(seconds):
    """Poll both consoles and print only when something CHANGES.

    Needed because the netgame object is freed the moment the error path runs,
    so the divergence cannot be read afterwards - the sample has to be taken
    while the transition is happening. Prints the last state before the object
    disappears, which is the one that matters."""
    import time
    ports = [(19842, "HOST "), (19843, "CHILD")]
    last = {}
    end = time.time() + seconds
    n = 0
    while time.time() < end:
        n += 1
        # PACE IT. Each sample is roughly a dozen synchronous debug reads per
        # console, and hammering those stalls the emulator - which would change
        # the very timing being measured. A quarter of a second is far finer
        # than the transition being watched and costs the guest almost nothing.
        time.sleep(0.25)
        for port, name in ports:
            st = gather(port)
            k = sig(st)
            if k != last.get(name):
                last[name] = k
                print("[%4d] %s step=%s obj=%08X sum=%s seq=%s rec0={%s %s} "
                      "global=%s"
                      % (n, name,
                         st.get("err"), st.get("obj") or 0,
                         "?" if st.get("checksum") is None
                         else "%08X" % st["checksum"],
                         "?" if st.get("sequence") is None
                         else "%04X" % st["sequence"],
                         "?" if not st.get("records")
                         else "seq=%04X" % (st["records"][0] or 0),
                         "?" if not st.get("records")
                         else "sum=%08X" % (st["records"][1] or 0),
                         "?" if st.get("seqglobal") is None
                         else "%08X" % st["seqglobal"]))
                sys.stdout.flush()
    return 0


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--watch":
        return watch(float(sys.argv[2]))
    ports = [(19842, "HOST "), (19843, "CHILD")]
    if len(sys.argv) > 1:
        ports = [(int(sys.argv[1]), "PORT%s" % sys.argv[1])]
    got = [(n, gather(p)) for p, n in ports]

    for name, st in got:
        if st.get("err") is None:
            print("%s  unreadable (console not up?)" % name)
            continue
        print("%s  step=%s (%s)  netgame=%08X  gate[+6F4]=%s"
              % (name, st["err"], STEP_RESULT.get(st["err"], "?"),
                 st["obj"] or 0,
                 "?" if st.get("gate") is None else "%02X" % st["gate"]))
        print("        seq global[02104344]=%s"
              % ("?" if st.get("seqglobal") is None
                 else "%08X" % st["seqglobal"]))
        print("        checksum[+6A0]=%s  sequence[+6A8]=%s"
              % ("?" if st.get("checksum") is None else "%08X" % st["checksum"],
                 "?" if st.get("sequence") is None else "%04X" % st["sequence"]))
        for i in range(MAX_SLOTS):
            # THE OBJECT IS FREED ON THE ERROR PATH, so a post-mortem read finds
            # a null holder and none of these keys. That is a finding, not a
            # crash: the error word survives but the state it was computed from
            # does not, which is why --watch below exists.
            if not st.get("slots"):
                print("        netgame object is GONE - freed by the error path;"
                      " the compared state cannot be read after the fact")
                break
            p, parts = st["slots"][i]
            rec = st.get("records") or []
            r = rec[i * 2:i * 2 + 2] if len(rec) >= i * 2 + 2 else [None, None]
            line = "        slot %d  ptr=%s  local{%s %s}" % (
                i, "%08X" % p if p else "--------",
                "?" if r[0] is None else "seq=%04X" % r[0],
                "?" if r[1] is None else "sum=%08X" % r[1])
            if parts:
                line += "  pos(%s,%s,%s)" % tuple(fx(v) for v in parts)
            print(line)

    if len(got) == 2:
        print("--- words that DIFFER between the consoles ---")
        a, b = got[0][1], got[1][1]
        any_diff = False
        for key in ("checksum", "sequence", "gate"):
            if a.get(key) != b.get(key):
                any_diff = True
                print("    %-10s host=%s  child=%s" % (
                    key,
                    "?" if a.get(key) is None else "%08X" % a[key],
                    "?" if b.get(key) is None else "%08X" % b[key]))
        for label, k in (("records", "records"), ("station0", "station0")):
            xa, xb = a.get(k) or [], b.get(k) or []
            for i in range(min(len(xa), len(xb))):
                if xa[i] != xb[i]:
                    any_diff = True
                    print("    %s[%d]  host=%08X  child=%08X"
                          % (label, i, xa[i], xb[i]))
        if not any_diff:
            print("    none - the two consoles agree on every word read here")
    return 0


if __name__ == "__main__":
    sys.exit(main())
