"""Exercise the GUI's list logic without a person watching it.

    python test_master_gui.py

WHAT IS WORTH TESTING IN A WINDOW. Not the layout - nobody can assert a
sensible-looking pane - but the part that decides WHICH LOBBY A MENU CLICK ACTS
ON. The sidebar is rebuilt from scratch every second and the rows reorder as
lobbies heartbeat, so the mapping from a tree row to a lobby id, and the
preservation of which rows were expanded, are where a right-click ends up
kicking the wrong player. That is all reachable without showing anything.

THE ROOT IS WITHDRAWN BEFORE IT CAN MAP. Tk creates the window on construction
but does not put it on screen until the event loop runs; withdrawing on the next
line means nothing ever appears and no focus is taken. If Tk is not available at
all - a headless machine, a Python built without it - this exits 2 to say
"skipped", the same convention tools/run_unit_tests.sh already uses.
"""

from __future__ import annotations

import sys

FAILURES = []


def check(name, got, want):
    if got == want:
        print("  ok    %s" % name)
    else:
        print("  FAIL  %s\n          got  %r\n          want %r" % (name, got, want))
        FAILURES.append(name)


def lobby(sid, name, kind="bounty", players=1, roster=()):
    return {"id": sid, "name": name, "kind": kind, "code": "AB12",
            "address": "10.0.0.1", "port": 28553, "players": players,
            "max_players": 4, "roster": list(roster), "config": {},
            "created_at": 0}


def main() -> int:
    try:
        import tkinter as tk
    except Exception as exc:
        print("skipped: tkinter is not available (%s)" % exc)
        return 2

    try:
        root = tk.Tk()
        root.withdraw()
    except Exception as exc:
        print("skipped: no display (%s)" % exc)
        return 2

    import master_server_gui as gui

    clicks = []
    widget = gui.LobbyList(root, "test", lambda ev, target: clicks.append(target))

    print("rows map to the lobby they were drawn from")
    a = lobby("aaa", "Maclunkey", roster=[{"slot": 0, "name": "Han",
                                           "address": "10.0.0.2"},
                                          {"slot": 1, "name": "Greedo",
                                           "address": "10.0.0.3"}])
    b = lobby("bbb", "Cantina")
    widget.render([a, b])
    kinds = sorted(t[0] for t in widget.targets.values())
    check("two lobbies and two players are addressable",
          kinds, ["lobby", "lobby", "player", "player"])
    players = {t[3]["name"]: t[1] for t in widget.targets.values()
               if t[0] == "player"}
    check("both players belong to the lobby they were drawn under",
          players, {"Han": "aaa", "Greedo": "aaa"})

    print("a lobby with no roster still says so")
    empty = [widget.tree.item(c, "text")
             for c in widget.tree.get_children(
                 [k for k, v in widget.targets.items()
                  if v[0] == "lobby" and v[1] == "bbb"][0])]
    check("the placeholder row exists", empty, ["   (no roster reported)"])
    check("and is not clickable as a player",
          any(v[0] == "player" and v[1] == "bbb"
              for v in widget.targets.values()), False)

    print("expansion survives the once-a-second rebuild")
    node_a = [k for k, v in widget.targets.items()
              if v[0] == "lobby" and v[1] == "aaa"][0]
    widget.tree.item(node_a, open=True)
    # Lobbies reorder as they heartbeat - the listing is newest-first - so the
    # rebuild deliberately hands them back the other way round.
    widget.render([b, a])
    node_a = [k for k, v in widget.targets.items()
              if v[0] == "lobby" and v[1] == "aaa"][0]
    check("the row the operator opened is still open",
          bool(widget.tree.item(node_a, "open")), True)
    node_b = [k for k, v in widget.targets.items()
              if v[0] == "lobby" and v[1] == "bbb"][0]
    check("the one they did not is still closed",
          bool(widget.tree.item(node_b, "open")), False)

    print("selection follows the lobby, not the row position")
    widget.tree.selection_set(node_a)
    widget.render([a, b])
    picked = widget.targets[widget.tree.selection()[0]]
    check("still the same lobby after a reorder", (picked[0], picked[1]),
          ("lobby", "aaa"))

    print("a lobby that goes away takes its rows with it")
    widget.render([b])
    check("nothing still points at the closed lobby",
          any(v[1] == "aaa" for v in widget.targets.values()), False)

    root.destroy()
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
