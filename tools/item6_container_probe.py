"""Read the multiplayer player container on both consoles.

THE GOAL THIS SERVES, IN PLAIN ENGLISH. Starting the lobby should respawn the
player in the lobby the way starting a co-op game does, and it does not. An
earlier investigation closed the spawn-dispatcher route for it on two readings
taken in the lobby:

    [container + 0x290A]            = -1    the spawn index; FUN_02089e60 sees
                                            this and takes its SHORT path
    party record slot 0, character  = 100   kLsw2CharacterNone; the per-slot
                                            pass skips a slot that reads this

Both were read on a SOLO console. The container is the MULTIPLAYER container, so
"empty" might be a fact about the lobby or merely a fact about there being one
player - and that investigation cannot tell those apart, because it never seated a second
console. This reads the same fields with two consoles in the lobby.

WHERE THE ADDRESSES COME FROM, so none of them is a guess:

    container            0x02104DF4    the argument FUN_0208c888 passes to the
                                       spawn dispatcher and to FUN_020897f8
    spawn index  +0x290A               02089E6C ADD r1,r4,#0x2900
                                       02089E70 LDRSB r1,[r1,#0xA]
                                       02089E78 CMP r1,#-1 -> short path
    party record +0x21E4, stride 0x40  02089854 LDRSH r2,[r1,#0xE4]
                                       02089858 CMP r2,#0x64 (100) -> skip slot
    the enable byte +0x21FF             02089860 LDRBNE r1,[r1,#0x1FF]

READ-ONLY. Nothing here writes to the guest.
"""

from __future__ import annotations

import json
import socket
import sys

HOST = "127.0.0.1"

CONTAINER = 0x02104DF4
SPAWN_INDEX = 0x290A
PARTY_BASE = 0x21E4          # + slot * 0x40 -> the character halfword
PARTY_STRIDE = 0x40
ENABLE_BYTE = 0x21FF         # + slot * 0x40
ACTOR_ARRAY = 0x02109358
INPUT_MGR = 0x021027F8
MGR_COUNT = 0x29
MGR_LOCAL = 0x2A

CHARACTER_NONE = 100


def read(port: int, addr: int, ln: int) -> bytes:
    s = socket.create_connection((HOST, port), timeout=8.0)
    try:
        f = s.makefile("rwb")
        f.write((json.dumps({"cmd": "read_mem", "cpu": 9,
                             "addr": addr, "len": ln}) + "\n").encode())
        f.flush()
        return bytes.fromhex(json.loads(f.readline().decode()).get("hex", ""))
    finally:
        s.close()


def u8(port, addr):
    b = read(port, addr & ~3, 4)
    return b[addr & 3] if len(b) == 4 else None


def u16(port, addr):
    b = read(port, addr, 2)
    return int.from_bytes(b, "little") if len(b) == 2 else None


def u32(port, addr):
    b = read(port, addr, 4)
    return int.from_bytes(b, "little") if len(b) == 4 else None


def report(name: str, port: int) -> None:
    print("  %s (port %d)" % (name, port))
    try:
        cnt = u8(port, INPUT_MGR + MGR_COUNT)
        loc = u8(port, INPUT_MGR + MGR_LOCAL)
        print("     input manager: player count=%s  local index=%s" % (cnt, loc))

        raw = u8(port, CONTAINER + SPAWN_INDEX)
        spawn = None if raw is None else (raw - 256 if raw > 127 else raw)
        print("     spawn index [container+0x290A] = %s%s"
              % (spawn, "   (-1 -> FUN_02089e60 takes its SHORT path)"
                 if spawn == -1 else ""))

        actors = read(port, ACTOR_ARRAY, 16)
        for slot in range(4):
            ch = u16(port, CONTAINER + PARTY_BASE + slot * PARTY_STRIDE)
            en = u8(port, CONTAINER + ENABLE_BYTE + slot * PARTY_STRIDE)
            act = (int.from_bytes(actors[slot * 4:slot * 4 + 4], "little")
                   if len(actors) == 16 else 0)
            note = "  <- no character, this slot is skipped" if ch == CHARACTER_NONE else ""
            print("     slot %d: character=%s  enable=%s  actor=%08X%s"
                  % (slot, ch, en, act, note))
    except Exception as exc:                                 # noqa: BLE001
        # A refused read is NOT a zero - say so rather than printing blanks.
        print("     READ FAILED (%s) - this is NOT evidence the fields are empty"
              % exc)


def main() -> int:
    hp = int(sys.argv[1]) if len(sys.argv) > 1 else 19842
    cp = int(sys.argv[2]) if len(sys.argv) > 2 else 19843
    print("the player container %08X, read on both consoles:" % CONTAINER)
    report("host ", hp)
    report("child", cp)
    print()
    print("  The solo reading was spawn index -1 and slot 0 character 100.")
    print("  If they read the same here, that route is closed for real.")
    print("  If they are populated, the solo conclusion was about the player")
    print("  count rather than about the lobby, and needs correcting.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
