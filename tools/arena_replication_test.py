"""Is this a real two-player match, or two consoles running side by side?

THE GOAL THIS SERVES, IN PLAIN ENGLISH. The requirement is that the players
are actually connected and present in the game's real arena multiplayer system,
playing an actual deathmatch - not merely each warped to the same map.

Two consoles can both reach the arena, both show a character, and share nothing
at all. This asks the question that tells those apart: **drive one console and
watch the OTHER one's copy of that player.** If the far console's actor moves,
the input crossed the wire; if the two agree on the position afterwards, the
world is shared rather than merely similar.

Both directions are tested, because "the host can drive" and "the joiner can
drive" are different claims and only one of them was ever in doubt.

WHAT THE READINGS MEAN.

    the input manager 0x021027F8   four 10-byte per-slot records, buttons in
                                   the first halfword of each, the player count
                                   at +0x29 and the local player index at +0x2A
                                   (from FUN_0205d140 and FUN_0205ce64)

In a networked match (`count >= 2`) every slot's buttons are pulled from the
session object and are therefore expected to appear on BOTH consoles. Seeing
the driven slot light up on the far console is the wire working, one layer below
the actor.

INSTRUMENT NOTES, both learned the hard way in earlier runs.

  * The debug server SERIALIZES connections per console, but the two consoles
    are separate servers, so one link to each is fine - and necessary, since the
    whole point is to read one while driving the other.
  * A stalled console reads a constant and is indistinguishable from "nothing
    replicated". Liveness is checked with `dispatch_stats.arm9.dispatch_total`,
    the recompiler's own dispatch count - NOT `frontend_stats.frames`, which
    exists and reads 0 forever on a healthy console.
"""

from __future__ import annotations

import json
import socket
import sys
import time

HOST = "127.0.0.1"
ACTOR_ARRAY = 0x02109358
INPUT_MGR = 0x021027F8
MGR_STRIDE = 10
COMM_ERROR = 0x021092FC
OFF_POS = 0xB0


class Link:
    def __init__(self, port: int, name: str, timeout: float = 8.0):
        self.name = name
        self.port = port
        self.sock = socket.create_connection((HOST, port), timeout=timeout)
        self.f = self.sock.makefile("rwb")

    def cmd(self, **kw):
        self.f.write((json.dumps(kw) + "\n").encode())
        self.f.flush()
        return json.loads(self.f.readline().decode())

    def read(self, addr: int, ln: int) -> bytes:
        return bytes.fromhex(self.cmd(cmd="read_mem", cpu=9, addr=addr, len=ln).get("hex", ""))

    def actors(self):
        b = self.read(ACTOR_ARRAY, 16)
        return [int.from_bytes(b[i * 4:i * 4 + 4], "little") for i in range(4)]

    def pos(self, actor: int):
        b = self.read(actor + OFF_POS, 12)
        if len(b) != 12:
            return None
        return tuple(int.from_bytes(b[i:i + 4], "little", signed=True) for i in (0, 4, 8))

    def mgr(self):
        b = self.read(INPUT_MGR, 44)
        if len(b) < 44:
            return None, None, None
        btn = [int.from_bytes(b[s * MGR_STRIDE:s * MGR_STRIDE + 2], "little") for s in range(4)]
        return btn, b[0x29], b[0x2A]

    def comm_error(self) -> int:
        b = self.read(COMM_ERROR, 4)
        return int.from_bytes(b, "little") if len(b) == 4 else -1

    def ticks(self) -> int:
        try:
            return int(self.cmd(cmd="dispatch_stats")["arm9"]["dispatch_total"])
        except Exception:                                    # noqa: BLE001
            return -1

    def key(self, name: str, down: bool):
        return self.cmd(cmd="frontend_input", action="key", key=name, down=down)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def alive(link: Link, dwell: float = 0.5) -> bool:
    a = link.ticks()
    time.sleep(dwell)
    return a >= 0 and link.ticks() > a


def drive(driver: Link, watcher: Link, slot: int, actor: int, key: str = "up",
          hold_s: float = 1.6):
    """Hold a key on `driver`; report what `watcher` sees for that player."""
    if not (alive(driver) and alive(watcher)):
        print("  VOID: a console is not stepping")
        return None

    before_d, before_w = driver.pos(actor), watcher.pos(actor)
    driver.key(key, True)
    lit_far = False
    t_end = time.time() + hold_s
    while time.time() < t_end:
        time.sleep(0.3)
        wb, _, _ = watcher.mgr()
        db, _, _ = driver.mgr()
        if wb and wb[slot]:
            lit_far = True
        print("     driver mgr=%s   watcher mgr=%s   watcher sees actor%d at %s"
              % (["%04X" % v for v in (db or [])], ["%04X" % v for v in (wb or [])],
                 slot, watcher.pos(actor)))
    driver.key(key, False)
    time.sleep(0.5)

    after_d, after_w = driver.pos(actor), watcher.pos(actor)
    moved_far = before_w != after_w and None not in (before_w, after_w)
    agree = after_d == after_w and after_d is not None
    print("     %s drove slot %d: far console saw it move = %s | buttons reached the far"
          " console = %s | positions agree afterwards = %s"
          % (driver.name, slot, moved_far, lit_far, agree))
    print("       driver  %s -> %s" % (before_d, after_d))
    print("       watcher %s -> %s" % (before_w, after_w))
    return {"moved_far": moved_far, "buttons_far": lit_far, "agree": agree}


def main() -> int:
    hp = int(sys.argv[1]) if len(sys.argv) > 1 else 19842
    cp = int(sys.argv[2]) if len(sys.argv) > 2 else 19843
    host = Link(hp, "host")
    child = Link(cp, "child")
    results = []
    try:
        actors = host.actors()
        hb, hcount, hlocal = host.mgr()
        cb, ccount, clocal = child.mgr()
        print("actors: %s" % " ".join("%08X" % a for a in actors))
        print("host  player count=%s local index=%s comm-error=%08X"
              % (hcount, hlocal, host.comm_error()))
        print("child player count=%s local index=%s comm-error=%08X"
              % (ccount, clocal, child.comm_error()))
        if hcount is None or hcount < 2:
            print("NOT a two-player match on the host (count=%s) - nothing to replicate"
                  % hcount)
            return 2
        print()
        print("---- host drives its own player (slot %d) ----" % hlocal)
        results.append(drive(host, child, hlocal, actors[hlocal]))
        print()
        print("---- child drives its own player (slot %d) ----" % clocal)
        results.append(drive(child, host, clocal, actors[clocal]))
        print()
        ok = [r for r in results if r]
        verdict = bool(ok) and len(ok) == 2 and all(
            r["moved_far"] and r["buttons_far"] and r["agree"] for r in ok)
        print("VERDICT: a real shared two-player match = %s" % verdict)
        print("  (each player drove their own character, the far console saw it,"
              " and both consoles agreed on the position afterwards)")
        print("comm-error afterwards: host=%08X child=%08X"
              % (host.comm_error(), child.comm_error()))
    finally:
        for lk in (host, child):
            for k in ("up", "down", "left", "right"):
                try:
                    lk.key(k, False)
                except Exception:                            # noqa: BLE001
                    pass
            lk.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
