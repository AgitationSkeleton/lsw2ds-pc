"""Read the pre-game countdown's own inputs, live, on both consoles.

WHAT THIS IS FOR, in plain english. Both consoles now reach the arena together
and stay there, healthy, but neither ever leaves state 42 - the pre-game - so
the round never begins. State 42's per-frame function is `FUN_0209b88c`, and it
is the thing that starts the round:

    v = (obj[0x38] + obj[0x3C]) - clock[0x4C8];
    if (v == 0x3C && ...) broadcast type 0x35 {0};   /* 60 to go */
    if (v == 0x1E && ...) broadcast type 0x35 {1};   /* 30 to go */
    if (v < 1)            broadcast type 0x12;       /* GO - case 0x12 asks
                                                        for state 44 */

Not one of those three message types has ever appeared in a run. So either the
function is returning before it gets there, or `v` never reaches the values it
tests. This reads every input to that expression so the answer is a reading
rather than an argument.

THE EARLY RETURNS, in order, because "it is not broadcasting" has four possible
causes and they need different fixes:

    obj[0x50] == 0                 disabled outright - it returns immediately
    *0x021092F4 == 0               no view manager - it DISABLES itself and
                                   clears obj[0x50]
    vtable+0x1C(...) != 0          a check this cannot evaluate from outside
    obj[0xB2] != live view mask    the seated players CHANGED: it tears the
                                   session down and asks for state 15

The last one matters most, because `+0x66` is the view mask that every player
count derives from, and the fork has a long history of that mask
moving during transitions.

    python tools/pregame_countdown.py            both consoles
    python tools/pregame_countdown.py --watch 60 sample once a second

CAVEAT (e): an unreadable address and a zero read the same over the debug
socket, so this reports the state object pointer first - if that is zero the
rest of the line means nothing and it says so.
"""

import json
import socket
import sys
import time

STATE_MANAGER = 0x02102900      # DAT_0209bacc, and statelog.h's kLsw2StateManager
STATE_CURRENT = 4               # manager + 4 -> the current state object
STATE_ID = 4                    # object  + 4 -> the state id
VIEW_HOLDER = 0x021092F4        # DAT_0209bac8
CLOCK = 0x02108D90              # DAT_0209bad0
CLOCK_FRAME = 0x4C8             # clock + 0x4C8, the value the deadline counts against
PREGAME_STATE = 42

# the state-42 object's own fields, from FUN_0209b88c
F_ENABLE = 0x50                 # byte; zero means the function returns at once
F_DEADLINE_A = 0x38             # word
F_DEADLINE_B = 0x3C             # word
F_LAST_MASK = 0xB2              # halfword; the view mask it remembers


def rd(port, addr, ln):
    """The same call the other tools make - `cpu: 9` and a `hex` reply field.

    Returns None when the port answered nothing, which is NOT the same as a
    zero read (caveat (e)); every caller below distinguishes them."""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    except OSError:
        return None
    try:
        f = s.makefile("rwb")
        f.write((json.dumps({"cmd": "read_mem", "cpu": 9,
                             "addr": addr, "len": ln}) + "\n").encode())
        f.flush()
        line = f.readline().decode()
    finally:
        s.close()
    try:
        h = json.loads(line).get("hex", "")
    except Exception:
        return None
    if not h:
        return None
    return bytes.fromhex(h)


def u32(b, off=0):
    if b is None or len(b) < off + 4:
        return None
    return int.from_bytes(b[off:off + 4], "little")


def u16(b, off=0):
    if b is None or len(b) < off + 2:
        return None
    return int.from_bytes(b[off:off + 2], "little")


def report(port, label):
    mgr = rd(port, STATE_MANAGER + STATE_CURRENT, 4)
    obj = u32(mgr)
    if obj is None:
        print("%-5s  the debug port answered nothing - is it running?" % label)
        return
    if obj == 0:
        print("%-5s  state object pointer is ZERO (no current state)" % label)
        return

    sid = u32(rd(port, obj + STATE_ID, 4))
    if sid != PREGAME_STATE:
        print("%-5s  obj=%08X  state=%s - NOT the pre-game, nothing to read"
              % (label, obj, sid))
        return

    enable = rd(port, obj + F_ENABLE, 1)
    a = u32(rd(port, obj + F_DEADLINE_A, 4))
    b = u32(rd(port, obj + F_DEADLINE_B, 4))
    mask = u16(rd(port, obj + F_LAST_MASK, 2))
    now = u32(rd(port, CLOCK + CLOCK_FRAME, 4))
    vm = u32(rd(port, VIEW_HOLDER, 4))

    en = enable[0] if enable else None
    v = None
    if a is not None and b is not None and now is not None:
        v = (a + b - now) & 0xFFFFFFFF
        if v >= 0x80000000:
            v -= 0x100000000

    # WHAT ARMS IT, AND WHICH OBJECT IT ARMS.
    #
    # The countdown is armed by FUN_0209bb34, and the image has exactly one
    # caller - 0x02099F00, in state 41's class - guarded twice:
    #
    #     LDR  r0,[0x027E02A8] ; TST r0,#0x40 ; BEQ past the arm
    #     LDR  r0,[0x02106D90+0xAD0] ; LDRB r1,[r0,#0x50] ; CMP r1,#0 ; BNE skip
    #     BL   FUN_0209bb34
    #
    # So it arms whatever object sits at [0x02107860], which SHOULD be this same
    # state-42 object. Reporting both makes a mismatch visible instead of
    # inferred - and a mismatch would mean the arm is arming something else.
    gate = u32(rd(port, 0x027E02A8, 4))
    arm_obj = u32(rd(port, 0x02107860, 4))
    arm_en = None
    arm_id = None
    if arm_obj:
        e = rd(port, arm_obj + F_ENABLE, 1)
        arm_en = e[0] if e else None
        arm_id = u32(rd(port, arm_obj + STATE_ID, 4))

    # THE VTABLE OF EACH, because "same class" was an ASSUMPTION and it decides
    # everything. An earlier reading called 02159728 "genuinely a state-42-class instance"
    # on the strength of its first word being 020FFA78 - the address where
    # FUN_0209b88c is stored. That only follows if 020FFA78 is the vtable BASE
    # rather than a slot part-way into it. Printing both objects' first word
    # settles it by comparison instead of by argument: if they differ, they are
    # not the same class and the whole "two instances" reading is wrong.
    vt_state = u32(rd(port, obj, 4))
    vt_arm = u32(rd(port, arm_obj, 4)) if arm_obj else None

    print("%-5s  obj=%08X state=42  enable[0x50]=%s  view mgr=%08X" %
          (label, obj, en, vm or 0))
    print("       vtable of the LIVE state object = %s" %
          (None if vt_state is None else "%08X" % vt_state))
    print("       vtable of the ARM TARGET        = %s   %s" %
          (None if vt_arm is None else "%08X" % vt_arm,
           "SAME CLASS" if (vt_arm is not None and vt_arm == vt_state)
           else "*** DIFFERENT CLASS - not two instances of one thing ***"))
    print("       arm gate [027E02A8]=%s  bit 0x40 %s" %
          (None if gate is None else "%08X" % gate,
           "SET - the arm is reachable" if (gate or 0) & 0x40
           else "CLEAR - THE ARM IS SKIPPED"))
    print("       arm target [02107860]=%s  its state id=%s  its enable=%s%s" %
          (None if arm_obj is None else "%08X" % arm_obj, arm_id, arm_en,
           "" if arm_obj == obj else "   <- NOT THIS STATE OBJECT"))
    print("       deadline = %s + %s = %s   clock = %s   -> COUNTDOWN v = %s"
          % (a, b, None if a is None or b is None else a + b, now, v))
    print("       remembered view mask[0xB2] = %s" % mask)

    # Say what the function would do with these numbers, rather than leaving the
    # reader to re-derive it.
    if en == 0:
        print("       => DISABLED: FUN_0209b88c returns immediately")
    elif vm == 0:
        print("       => no view manager: it clears enable and returns")
    elif v is None:
        print("       => could not read the countdown inputs")
    elif v == 0x3C:
        print("       => would broadcast type 0x35 {0} THIS FRAME")
    elif v == 0x1E:
        print("       => would broadcast type 0x35 {1} THIS FRAME")
    elif v < 1:
        print("       => would broadcast type 0x12 - THE ROUND STARTS")
    else:
        print("       => counting: %d frames to go (%.1f s at 60fps), passes 60 "
              "at v=0x3C and 30 at v=0x1E" % (v, v / 60.0))

    # AND THE SAME FIELDS ON THE REGISTERED OBJECT, which is the one that matters.
    #
    # An earlier reading took the countdown's fields off the STATE MANAGER's current
    # object and found them zero/uninitialised. That was the wrong object.
    # FUN_0209b61c - the only function that installs vtable 020FFA78, and the
    # only constructor of the countdown's class - has exactly ONE caller in the
    # image, the factory at 0208589C, and it registers its single instance at
    # [0x02107860]. There is no second instance to be had, so the state
    # machine's state-42 object is a DIFFERENT class and its +0x50 means nothing.
    if arm_obj:
        a2 = u32(rd(port, arm_obj + F_DEADLINE_A, 4))
        b2 = u32(rd(port, arm_obj + F_DEADLINE_B, 4))
        m2 = u16(rd(port, arm_obj + F_LAST_MASK, 2))
        now2 = u32(rd(port, CLOCK + CLOCK_FRAME, 4))
        v2 = None
        if a2 is not None and b2 is not None and now2 is not None:
            v2 = (a2 + b2 - now2) & 0xFFFFFFFF
            if v2 >= 0x80000000:
                v2 -= 0x100000000
        print("       REGISTERED OBJECT %08X: enable=%s  deadline=%s+%s  "
              "clock=%s  -> v=%s  mask[0xB2]=%s"
              % (arm_obj, arm_en, a2, b2, now2, v2, m2))
        if arm_en:
            if v2 is None:
                print("         (could not read its countdown inputs)")
            elif v2 == 0x3C:
                print("         => it would broadcast 0x35 {0} THIS FRAME")
            elif v2 == 0x1E:
                print("         => it would broadcast 0x35 {1} THIS FRAME")
            elif v2 < 1:
                print("         => v < 1: it would broadcast 0x12 - THE ROUND "
                      "STARTS. If no 0x12 is in the log, FUN_0209b88c is NOT "
                      "BEING CALLED on this object.")
            else:
                print("         => counting: %d frames (%.1f s) to go" %
                      (v2, v2 / 60.0))
        else:
            print("         => ARMED FLAG IS CLEAR on the registered object too")



def main():
    watch = 0
    if len(sys.argv) >= 3 and sys.argv[1] == "--watch":
        watch = int(sys.argv[2])

    first = True
    while first or watch > 0:
        first = False
        for port, label in ((19842, "HOST"), (19843, "CHILD")):
            report(port, label)
        if watch <= 0:
            break
        print("")
        time.sleep(1.0)
        watch -= 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
