"""Read the WM layer's live state on both consoles, by chasing the cartridge's
own pointer chain.

WHAT THIS IS FOR, in plain english. It was established that the match-start
failure is an END that never completes: WH's step 0x10 issues WM command 9 -
end-parent - registers FUN_020ae608 as its completion callback, and waits for
that callback to stamp step 0x11. On the host the stamp never arrives and the
wireless layer sits in BUSY for the rest of the run.

That leaves one question the ARM9-side step signal cannot answer: did the ARM7
ACT on the command and only the reply go missing, or did the command never get
processed at all? Those need different fixes. The WM state distinguishes them,
because end-parent is precisely the operation that should move the state off
the parent value.

WHY THE STATE IS READABLE FROM HERE. WM keeps its status in a block that both
processors share, and the ARM9 reads it in its own precondition check:

    FUN_020d6618(1, 7, ...)  ->  **(ushort **)(*(int *)(DAT_020d66a8 + 4) + 4)

so the chain is: 0x02117E24 holds the WM context, the context's +4 holds the
status block, and the first halfword of that block is the state. The literal 7
in that check is the cartridge's own requirement for issuing end-parent, so it
is what the state reads while the console is hosting - measured from the ROM,
not assumed from an SDK header.

The callback table lives in the same context at +0x18, four bytes per slot
(FUN_020d6418), so slot 9's entry is +0x3C. Printing it says whether the
completion callback is still registered when the stall is observed - if it has
been cleared, the reply was consumed and something dropped it afterwards; if it
still points at FUN_020ae608, nothing ever came back for it.
"""

import socket
import json
import sys

WM_SYSTEM_PTR = 0x02117E24   # *(DAT_020d66a8 + 4) - the WM context pointer
CB_TABLE = 0x18              # FUN_020d6418: ctx + 0x18 + slot*4
END_PARENT_SLOT = 9          # the command WH's step 0x10 issues
STEP11_CALLBACK = 0x020AE608  # DAT_020ae604, read out of the ROM image


def rd(port, addr, ln):
    s = socket.create_connection(("127.0.0.1", port), timeout=10.0)
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


def word(port, addr):
    """One little-endian 32-bit word, or None if the read came back empty.

    An empty debug read decodes to 0, which is instrument caveat (f) and has
    been mistaken for a real zero before, so an unreadable address is reported
    as unreadable rather than as a value."""
    h = rd(port, addr, 4)
    if len(h) < 8:
        return None
    return int.from_bytes(bytes.fromhex(h[:8]), "little")


def half(port, addr):
    h = rd(port, addr, 2)
    if len(h) < 4:
        return None
    return int.from_bytes(bytes.fromhex(h[:4]), "little")


def show(port, name):
    # A ZERO POINTER AND AN UNREADABLE ONE ARE DIFFERENT ANSWERS, and this
    # function conflated them on its first outing. After a failed match start
    # the chase reported "WM context unreadable" - which reads as "the console
    # is gone" - when what had actually happened is that the pointer had been
    # written back to zero, i.e. WM was torn down. That is a finding, not a
    # failed read, and reporting it as a failed read would have buried it.
    # This is instrument caveat (f) - an empty debug read decodes to 0 - which
    # is a reason to separate the two cases, not to merge them.
    ctx = word(port, WM_SYSTEM_PTR)
    if ctx is None:
        print("%s  WM context pointer unreadable (console not up?)" % name)
        return
    if ctx == 0:
        print("%s  WM context pointer is ZERO - WM has been torn down" % name)
        return
    status = word(port, ctx + 4)
    state = half(port, status) if status else None
    cb = word(port, ctx + CB_TABLE + END_PARENT_SLOT * 4)

    print("%s  wmctx=%08X  status=%08X" % (name, ctx, status or 0))
    if state is None:
        print("        WM state   unreadable")
    else:
        note = "  <- the value the cartridge requires to issue end-parent" \
            if state == 7 else ""
        print("        WM state   %d%s" % (state, note))
    if cb is None:
        print("        cmd 9 cb   unreadable")
    else:
        note = "  = FUN_020ae608, the step-0x11 stamper (still registered)" \
            if cb == STEP11_CALLBACK else ""
        print("        cmd 9 cb   %08X%s" % (cb, note))


def main():
    ports = ((19842, "HOST "), (19843, "CHILD"))
    if len(sys.argv) > 1:
        ports = ((int(sys.argv[1]), "PORT%s" % sys.argv[1]),)
    for port, name in ports:
        show(port, name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
