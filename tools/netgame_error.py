"""Read the word that decides Communication Error, and name the accessor that
produced it.

WHAT THIS IS FOR, in plain english. Earlier analysis reduced the match-start
failure to one word. The tail of FUN_020b07e8 does exactly this:

    LDR r0,[pc] -> 021092F4      the netgame object holder
    LDR r0,[r0,#8]               the word at 0x021092FC
    CMP r0,#0
    {EQ} return                  zero: nothing is wrong, go home
    ...  BL FUN_02072cd0         non-zero: shut networking down,
         request state 15, then state 38 = Communication Error

So a single non-zero word is the whole failure. It is written a few
instructions earlier from the netgame object's own vtable slot +0xC
(FUN_020b0bdc), which makes that slot the thing that decides.

WHY THIS IS A READ AND NOT A WATCH. The write watch was pointed at the word
first and its 32-entry cap was spent during boot: the field is written 0 over
and over from pc=020B09A0 while everything is healthy, so the one interesting
store never got a slot. The value survives in memory after the error, because
nothing on the failure path clears it - so reading it afterwards is both
cheaper and more reliable than trying to catch the store.

Resolving the vtable slot turns the value into a name: with the object's vtable
in hand, slot +0xC is a code address that can be looked up in the corpus.
"""

import socket
import json
import sys

HOLDER = 0x021092F4          # DAT_020b0bc8 / DAT_020b0c58 - holds the object
ERROR_WORD = HOLDER + 8      # 0x021092FC - the word the tail tests
WIRELESS_OBJ = 0x0210B348    # DAT_020b0c5c - the wireless session object
ACCESSOR_SLOT = 0x0C         # the vtable slot FUN_020b0bdc returns

# WHAT THE VALUE ACTUALLY MEANS, and the first version of this tool got it
# wrong in a way worth recording. It mapped the word through the WM error enum
# and printed "2 (OPERATING)", which is a real WM code and reads like a
# diagnosis - but this word is not a WM error at all. It is the return of
# FUN_020af330, the netgame step, which has exactly three values:
#
#     0  healthy - the tail returns early and nothing happens
#     1  FUN_020af594 returned 0: the per-frame data exchange did not run
#     2  FUN_020af460 returned 0: the CONSISTENCY CHECK across players failed
#
# Mislabelling 2 as "OPERATING" pointed at the wireless layer when the value
# actually says the opposite - the link was fine and the two consoles disagreed
# about the game state. Same trap as caveat (f): a plausible label on a number
# is worse than no label.
STEP_RESULT = {
    0: "healthy",
    1: "FUN_020af594 returned 0 - the data exchange did not run",
    2: "FUN_020af460 returned 0 - the cross-player consistency check FAILED",
}


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
    """One little-endian word, or None if the read came back empty.

    An empty debug read decodes to 0 - instrument caveat (f) - and wm_state.py
    shipped with that conflation and nearly buried a finding, so zero and
    unreadable are kept apart here from the start."""
    h = rd(port, addr, 4)
    if len(h) < 8:
        return None
    return int.from_bytes(bytes.fromhex(h[:8]), "little")


def show(port, name):
    err = word(port, ERROR_WORD)
    if err is None:
        print("%s  error word unreadable (console not up?)" % name)
        return
    tag = STEP_RESULT.get(err, "unknown step result")
    verdict = "OK - the tail returns early" if err == 0 \
        else "NON-ZERO -> shut networking down, request 15 then 38"
    print("%s  [%08X] = %08X  (%s)  %s"
          % (name, ERROR_WORD, err, tag, verdict))

    obj = word(port, HOLDER)
    if not obj:
        print("        netgame object not allocated")
    else:
        vt = word(port, obj)
        fn = word(port, vt + ACCESSOR_SLOT) if vt else None
        print("        netgame obj=%08X vtable=%08X  slot+0x%02X -> %s"
              % (obj, vt or 0, ACCESSOR_SLOT,
                 ("FUN_%08x" % fn) if fn else "unreadable"))

    wobj = word(port, WIRELESS_OBJ)
    if wobj:
        wvt = word(port, wobj)
        print("        wireless obj=%08X vtable=%08X" % (wobj, wvt or 0))


def main():
    ports = ((19842, "HOST "), (19843, "CHILD"))
    if len(sys.argv) > 1:
        ports = ((int(sys.argv[1]), "PORT%s" % sys.argv[1]),)
    for port, name in ports:
        show(port, name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
