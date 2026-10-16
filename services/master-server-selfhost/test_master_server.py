"""Exercise the master server without the game.

The whole point of writing this service before the netcode is that it can be
proved on its own. Run it and the contract is either honoured or it is not - no
ROM, no window, no play session.

    python test_master_server.py

Starts the service on a free port in-process, runs every endpoint, and checks
the parts that are easy to get wrong rather than the parts that are easy to
test: the token check, the sanitiser, the player clamp, code normalisation, and
the fact that a host cannot choose its own address.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import tempfile
import threading
import time
import urllib.error
import urllib.request

import banlist
import eventlog
import filelog
import master_server as ms


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def call(method, url, body=None, headers=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers=headers or {})
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


FAILURES = []


def check(name, got, want):
    if got == want:
        print("  ok    %s" % name)
    else:
        print("  FAIL  %s\n          got  %r\n          want %r" % (name, got, want))
        FAILURES.append(name)


def main() -> int:
    port = free_port()
    workdir = tempfile.mkdtemp(prefix="lsw2-master-test-")
    bans_path = os.path.join(workdir, "bans.txt")

    args = ms.build_parser().parse_args(
        ["--port", str(port), "--host", "127.0.0.1", "--db", ":memory:",
         "--bans", bans_path, "--logs", os.path.join(workdir, "logs")])
    service = ms.Service(args)
    service.attach_file_log()
    service.announce()
    httpd = service.build()
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % port
    time.sleep(0.2)

    print("sanitiser (pure, no server needed)")
    # Stripped to "scriptxscript" and THEN cut to 10 - the order matters, and
    # this expectation originally forgot the cut.
    check("strips markup", ms.sanitize_name("<script>x</script>"), "scriptxscr")
    check("strips quotes and semicolons", ms.sanitize_name("a';DROP TABLE--"),
          "aDROP TABLE--"[:ms.MAX_NAME])
    check("collapses whitespace", ms.sanitize_name("  a    b  "), "a b")
    check("empty becomes PLAYER", ms.sanitize_name(""), "PLAYER")
    check("all-illegal becomes PLAYER", ms.sanitize_name("!!!"), "PLAYER")
    check("cuts to 10", ms.sanitize_name("Maclunkey12345"), "Maclunkey1")
    check("keeps the default name intact", ms.sanitize_name("Maclunkey"), "Maclunkey")

    print("room code normalisation")
    check("folds I and L to 1", ms.normalise_code("il"), "11")
    check("folds O to 0", ms.normalise_code("o0"), "00")
    check("uppercases", ms.normalise_code("ab"), "AB")

    print("health")
    status, body = call("GET", base + "/v1/health")
    check("health responds", (status, body.get("ok")), (200, True))

    print("register")
    status, reg = call("POST", base + "/v1/servers",
                       {"name": "Maclunkey", "port": 28600, "players": 1})
    check("created", status, 201)
    check("returns a token", isinstance(reg.get("token"), str) and len(reg["token"]) > 10, True)
    check("returns a 4-character code", len(reg.get("code", "")), ms.CODE_LENGTH)
    check("code uses the safe alphabet",
          all(c in ms.CROCKFORD for c in reg.get("code", "")), True)

    print("listing")
    status, body = call("GET", base + "/v1/servers")
    servers = body.get("servers", [])
    check("one server listed", len(servers), 1)
    check("name survived", servers[0]["name"], "Maclunkey")
    check("port is the host's", servers[0]["port"], 28600)
    check("address is the SOCKET's, not the body's",
          servers[0]["address"], "127.0.0.1")
    check("no ping field - the client measures that",
          "ping" in servers[0], False)

    print("a host cannot advertise someone else's address")
    status, other = call("POST", base + "/v1/servers",
                         {"name": "Spoofer", "port": 28601,
                          "address": "203.0.113.7"})
    check("still created", status, 201)
    _, body = call("GET", base + "/v1/servers")
    spoof = [s for s in body["servers"] if s["name"] == "Spoofer"][0]
    check("body address ignored", spoof["address"], "127.0.0.1")

    print("room code resolution")
    status, got = call("GET", base + "/v1/codes/" + reg["code"])
    check("resolves", (status, got.get("port")), (200, 28600))
    status, _ = call("GET", base + "/v1/codes/ZZZZ0000")
    check("unknown code is 404", status, 404)

    print("heartbeat and the token check")
    status, _ = call("PUT", base + "/v1/servers/" + reg["id"],
                     {"token": "wrong", "players": 3})
    check("bad token refused", status, 403)
    status, _ = call("PUT", base + "/v1/servers/" + reg["id"],
                     {"token": reg["token"], "players": 3})
    check("good token accepted", status, 200)
    _, body = call("GET", base + "/v1/servers")
    mine = [s for s in body["servers"] if s["name"] == "Maclunkey"][0]
    check("player count updated", mine["players"], 3)

    print("the player count is clamped to the arena's four slots")
    call("PUT", base + "/v1/servers/" + reg["id"],
         {"token": reg["token"], "players": 99})
    _, body = call("GET", base + "/v1/servers")
    mine = [s for s in body["servers"] if s["name"] == "Maclunkey"][0]
    check("99 clamps to 4", mine["players"], 4)
    call("PUT", base + "/v1/servers/" + reg["id"],
         {"token": reg["token"], "players": -5})
    _, body = call("GET", base + "/v1/servers")
    mine = [s for s in body["servers"] if s["name"] == "Maclunkey"][0]
    check("-5 clamps to 0", mine["players"], 0)

    print("bad input is refused rather than stored")
    status, _ = call("POST", base + "/v1/servers", {"name": "x"})
    check("missing port refused", status, 400)
    status, _ = call("POST", base + "/v1/servers", {"name": "x", "port": 99999})
    check("out-of-range port refused", status, 400)

    print("shutdown")
    status, _ = call("DELETE", base + "/v1/servers/" + reg["id"],
                     headers={"X-Token": "wrong"})
    check("bad token refused", status, 403)
    status, _ = call("DELETE", base + "/v1/servers/" + reg["id"],
                     headers={"X-Token": reg["token"]})
    check("removed", status, 200)
    _, body = call("GET", base + "/v1/servers")
    check("gone from the list",
          [s for s in body["servers"] if s["name"] == "Maclunkey"], [])

    print("expiry")
    service.store.db.execute(
        "UPDATE servers SET expires_at = ? WHERE 1", (int(time.time()) - 1,))
    service.store.db.commit()
    _, body = call("GET", base + "/v1/servers")
    check("expired advertisements are swept", body["servers"], [])

    # ------------------------------------------------------------------------
    # Lobby kinds, rosters, kicks, bans and the event log.
    # ------------------------------------------------------------------------

    print("upgrading a database written by the previous release")
    # THIS IS THE CASE EVERY OTHER TEST MISSES, because they all open :memory:
    # and get today's schema. A service upgraded in place opens yesterday's, and
    # the first attempt at this died on startup with "no such column: kind" -
    # CREATE TABLE IF NOT EXISTS did nothing, and the new index ran before the
    # ALTER that would have made it possible.
    legacy = os.path.join(workdir, "legacy.sqlite3")
    import sqlite3
    old_db = sqlite3.connect(legacy)
    old_db.executescript("""
        CREATE TABLE servers (
          id TEXT PRIMARY KEY, token_hash TEXT NOT NULL, code TEXT UNIQUE,
          name TEXT NOT NULL, address TEXT NOT NULL, port INTEGER NOT NULL,
          players INTEGER NOT NULL DEFAULT 1,
          max_players INTEGER NOT NULL DEFAULT 4,
          config_json TEXT NOT NULL DEFAULT '{}',
          updated_at INTEGER NOT NULL, expires_at INTEGER NOT NULL);
        CREATE INDEX servers_expires_at ON servers(expires_at);
        CREATE INDEX servers_code ON servers(code);
    """)
    now = int(time.time())
    old_db.execute(
        "INSERT INTO servers (id, token_hash, code, name, address, port,"
        " players, max_players, config_json, updated_at, expires_at)"
        " VALUES ('old','h','ZZ99','Legacy','10.0.0.9',28553,2,4,'{}',?,?)",
        (now, now + 600))
    old_db.commit()
    old_db.close()

    upgraded = ms.Store(legacy)
    rows = upgraded.listing()
    check("the old row survived the upgrade", len(rows), 1)
    check("and is treated as a bounty lobby", rows[0]["kind"], ms.KIND_BOUNTY)
    check("with an empty roster rather than a crash", rows[0]["roster"], [])
    check("opening it twice is also fine", len(ms.Store(legacy).listing()), 1)
    upgraded.db.close()

    print("lobby kinds - bounty and coop share one directory")
    check("unknown kind falls back to bounty", ms.normalise_kind("nonsense"),
          ms.KIND_BOUNTY)
    check("missing kind falls back to bounty", ms.normalise_kind(None),
          ms.KIND_BOUNTY)
    check("case is folded", ms.normalise_kind("COOP"), ms.KIND_COOP)
    _, hunter = call("POST", base + "/v1/servers",
                     {"name": "Hunter", "port": 28610, "kind": "bounty"})
    _, coop = call("POST", base + "/v1/servers",
                   {"name": "Cantina", "port": 28611, "kind": "coop"})
    _, body = call("GET", base + "/v1/servers")
    check("both listed together", len(body["servers"]), 2)
    _, body = call("GET", base + "/v1/servers?kind=coop")
    check("filtered to coop", [s["name"] for s in body["servers"]], ["Cantina"])
    _, body = call("GET", base + "/v1/servers?kind=bounty")
    check("filtered to bounty", [s["name"] for s in body["servers"]], ["Hunter"])

    print("rosters - what the GUI expands a lobby to show")
    check("a non-list roster is dropped", ms.sanitize_roster("hello"), [])
    check("names are sanitised like any other",
          ms.sanitize_roster([{"name": "<b>Han</b>"}])[0]["name"], "bHanb")
    # Scrubbing characters would have let "10.0.0.5; rm -rf" through as
    # "10.0.0.5f" - the f in "rf" is a hex digit. Addresses are parsed, not
    # filtered.
    check("a mangled address is dropped whole, not trimmed to look valid",
          ms.sanitize_roster([{"name": "a", "address": "10.0.0.5; rm -rf"}])[0]["address"],
          "")
    check("a real v4 address survives", ms.valid_address("10.0.0.5"), "10.0.0.5")
    check("a real v6 address survives", ms.valid_address("::1"), "::1")
    check("999.1.1.1 is not an address", ms.valid_address("999.1.1.1"), "")
    check("a hostname is not an address", ms.valid_address("evil.example"), "")
    check("longer than the arena is cut to four slots",
          len(ms.sanitize_roster([{"name": "a"}] * 9)), ms.MAX_PLAYERS)
    call("PUT", base + "/v1/servers/" + coop["id"],
         {"token": coop["token"], "players": 2,
          "roster": [{"slot": 0, "name": "Maclunkey", "address": "10.0.0.2"},
                     {"slot": 1, "name": "Han", "address": "10.0.0.3"}]})
    _, body = call("GET", base + "/v1/servers?kind=coop")
    roster = body["servers"][0]["roster"]
    check("roster reaches the list", [p["name"] for p in roster],
          ["Maclunkey", "Han"])
    check("THE PUBLIC LIST HIDES PLAYER IPs",
          any("address" in p for p in roster), False)
    check("the operator's view keeps them",
          [p["address"] for p in service.store.find(coop["id"])["roster"]],
          ["10.0.0.2", "10.0.0.3"])

    # A leave is a roster that came back shorter - the directory never sees the
    # disconnect itself. Exercise it here so the log assertions below have one.
    call("PUT", base + "/v1/servers/" + coop["id"],
         {"token": coop["token"], "players": 1,
          "roster": [{"slot": 0, "name": "Maclunkey", "address": "10.0.0.2"}]})
    check("the shrunken roster is stored",
          [p["name"] for p in service.store.find(coop["id"])["roster"]],
          ["Maclunkey"])
    call("PUT", base + "/v1/servers/" + coop["id"],
         {"token": coop["token"], "players": 2,
          "roster": [{"slot": 0, "name": "Maclunkey", "address": "10.0.0.2"},
                     {"slot": 1, "name": "Han", "address": "10.0.0.3"}]})

    print("kick - the directory can only ask, on the next heartbeat")
    check("kick queued", service.kick(coop["id"], 1, "Han", "10.0.0.3"), True)
    check("kick for an unknown lobby is refused",
          service.kick("nope", 0), False)
    _, beat = call("PUT", base + "/v1/servers/" + coop["id"],
                   {"token": coop["token"], "players": 2})
    check("the directive rides the heartbeat",
          beat.get("directives"), [{"action": "kick", "slot": 1,
                                    "address": "10.0.0.3"}])
    _, beat = call("PUT", base + "/v1/servers/" + coop["id"],
                   {"token": coop["token"], "players": 1})
    check("and is delivered exactly once", "directives" in beat, False)

    print("close - the operator's Kick on a whole lobby")
    check("closed", service.close_lobby(hunter["id"]), True)
    _, body = call("GET", base + "/v1/servers?kind=bounty")
    check("gone from the list", body["servers"], [])
    check("closing it twice is refused", service.close_lobby(hunter["id"]), False)

    print("bans - a plain text file, permanent, edited by hand")
    fresh = banlist.BanList(os.path.join(workdir, "missing.txt"))
    check("a missing file is an empty list, not an error", fresh.entries(), [])
    with open(bans_path, "w", encoding="utf-8") as fh:
        fh.write("# griefers\n203.0.113.9\n\n  198.51.100.4  # spam\n")
    listed = banlist.BanList(bans_path)
    check("comments and blanks are ignored",
          listed.entries(), ["198.51.100.4", "203.0.113.9"])
    check("a listed address is banned", listed.is_banned("203.0.113.9"), True)
    check("an unlisted one is not", listed.is_banned("203.0.113.10"), False)
    check("appending a new one works", listed.add("192.0.2.5", "test"), True)
    check("appending it twice does not", listed.add("192.0.2.5"), False)
    check("the note is kept out of the entry",
          listed.is_banned("192.0.2.5"), True)
    with open(bans_path, encoding="utf-8") as fh:
        text = fh.read()
    check("the hand-written comment survived a write", "# griefers" in text, True)

    print("a ban is lifted by editing the file, with no restart")
    with open(bans_path, "w", encoding="utf-8") as fh:
        fh.write("203.0.113.9\n")
    os.utime(bans_path, (time.time() + 2, time.time() + 2))
    check("the edit is picked up", listed.is_banned("192.0.2.5"), False)
    check("and what is left still bans", listed.is_banned("203.0.113.9"), True)

    print("a banned address is refused at the door")
    check("the coop lobby is still up before the ban",
          service.store.find(coop["id"]) is not None, True)
    check("a non-address is never written to the ban file",
          service.ban("evil.example"), False)
    check("banning is recorded", service.ban("127.0.0.1", "test"), True)
    status, body = call("GET", base + "/v1/health")
    check("even health is refused", (status, body.get("error")), (403, "banned"))
    status, _ = call("POST", base + "/v1/servers", {"name": "x", "port": 28620})
    check("registration is refused", status, 403)

    print("a refused request does not poison the connection")
    # HTTP/1.1 KEEP-ALIVE MEANS A REFUSAL MUST STILL READ THE BODY. A POST whose
    # body is never drained leaves those bytes on the socket, and the NEXT
    # request on that connection fails - which is why this first showed up as an
    # intermittent ConnectionAbortedError one request too late, and read as a
    # network flake rather than as a bug in the refusal.
    for _ in range(6):
        status, _ = call("POST", base + "/v1/servers",
                         {"name": "x" * 200, "port": 28621, "config": {"pad": "y" * 400}})
        if status != 403:
            break
    check("a refused POST with a body stays refused", status, 403)
    status, body = call("GET", base + "/v1/health")
    check("and the connection still works afterwards",
          (status, body.get("error")), (403, "banned"))

    print("banning a host takes its lobby down with it")
    # A lobby nobody may heartbeat would otherwise sit in the list for the full
    # TTL looking joinable.
    check("the banned host's lobby is gone",
          service.store.find(coop["id"]), None)

    with open(bans_path, "w", encoding="utf-8") as fh:
        fh.write("")
    os.utime(bans_path, (time.time() + 4, time.time() + 4))
    status, _ = call("GET", base + "/v1/health")
    check("unbanning restores service", status, 200)

    print("requests actually reach the log")
    # THE GAP THAT HID A REAL BUG. Every earlier check here asserted on the
    # HTTP reply, so a service that answered correctly and logged nothing would
    # have passed the whole suite - and the operator's console log is entirely
    # about the logging. Assert the lines exist, not just the responses.
    recent = [r for r in service.log.tail(400)]
    check("registering a lobby is logged",
          any(r.category == "lobby" and "started a new coop game" in r.message
              for r in recent), True)
    check("the line names the address it came from",
          any(r.category == "lobby" and r.message.startswith("127.0.0.1")
              for r in recent), True)
    check("a list request is logged", any(r.category == "list" for r in recent),
          True)
    check("a room code lookup is logged",
          any(r.category == "code" for r in recent), True)
    check("a join is logged",
          any(r.category == "player" and "joined" in r.message
              for r in recent), True)
    check("a leave is logged",
          any(r.category == "player" and "left" in r.message
              for r in recent), True)
    check("a kick is logged",
          any(r.category == "admin" and "kick queued" in r.message
              for r in recent), True)
    check("a ban is logged",
          any(r.category == "ban" and "banned" in r.message
              for r in recent), True)
    check("a refused request from a banned address is logged",
          any(r.category == "ban" and "banned 127.0.0.1" not in r.message
              and "refused" in r.message for r in recent), True)

    print("a second copy must not quietly share the port")
    # On Windows SO_REUSEADDR lets a second LIVE listener bind a port that is
    # already served, and connections split between the two at random - which
    # looks like "the log is broken" from whichever copy loses the packet.
    doubled = False
    try:
        rival = ms.Directory(("127.0.0.1", port), ms.Handler)
        rival.server_close()
        doubled = True
    except OSError:
        pass
    check("binding the busy port is refused", doubled, False)

    print("the log the GUI and the files both read")
    log = eventlog.EventLog(capacity=3)
    seen = []
    log.add_sink(seen.append)
    log.info("t", "one")
    check("a sink receives records", seen[0].message, "one")
    log.info("t", "two")
    log.info("t", "three")
    log.info("t", "four")
    check("the ring buffer is bounded", [r.message for r in log.tail()],
          ["two", "three", "four"])
    late = []
    log.add_sink(late.append)
    check("a late sink is replayed the backlog",
          [r.message for r in late], ["two", "three", "four"])
    log.add_sink(lambda r: (_ for _ in ()).throw(RuntimeError("boom")))
    log.info("t", "five")
    check("a throwing sink does not take the service down",
          seen[-1].message, "five")

    print("startup lines the log must carry")
    startup = [r.message for r in service.log.tail() if r.category == "start"]
    check("says the port", any(":%d" % port in m for m in startup), True)
    check("says the ban list path",
          any("ban list" in m for m in startup), True)
    check("says the retention window",
          any("keeping %d days" % filelog.RETAIN_DAYS in m for m in startup), True)

    print("log files - a directory that rotates on a 31-day window")
    logdir = os.path.join(workdir, "logs")
    today = time.strftime("%Y-%m-%d")
    check("today's file exists",
          os.path.exists(os.path.join(logdir, "master-%s.log" % today)), True)
    with open(os.path.join(logdir, "master-%s.log" % today), encoding="utf-8") as fh:
        check("and carries the startup line",
              "lsw2 master server starting" in fh.read(), True)

    sink = filelog.FileSink(logdir)
    old = time.strftime("%Y-%m-%d", time.localtime(time.time() - 40 * 86400))
    edge = time.strftime("%Y-%m-%d", time.localtime(time.time() - 30 * 86400))
    for day in (old, edge):
        with open(sink.path_for(day), "w", encoding="utf-8") as fh:
            fh.write("x\n")
    with open(os.path.join(logdir, "notes.txt"), "w", encoding="utf-8") as fh:
        fh.write("not mine\n")
    removed = sink.prune()
    check("a 40-day-old file is pruned", "master-%s.log" % old in removed, True)
    check("a 30-day-old file is kept", "master-%s.log" % edge in removed, False)
    check("a stranger's file is left alone",
          os.path.exists(os.path.join(logdir, "notes.txt")), True)
    sink.close()

    print("appending, not truncating - a restart must not erase the day")
    marker = os.path.join(logdir, "master-%s.log" % today)
    size_before = os.path.getsize(marker)
    second = filelog.FileSink(logdir)
    second(eventlog.Record(time.time(), eventlog.INFO, "t", "after restart"))
    second.close()
    check("the file grew rather than restarted",
          os.path.getsize(marker) > size_before, True)

    # ------------------------------------------------------------------------
    # One lobby per host: dedup on listing, replace on re-register, TTL expiry.
    # The two bugs found in play - a 2-player lobby drawn as two rows, and ghost
    # lobbies that linger and pile up when a host closes and re-hosts - are one
    # root cause: register used to add a fresh row every time and the listing
    # never collapsed rows by host. The table is empty here (the ban above took
    # the last lobby down and 127.0.0.1 has since been unbanned).
    # ------------------------------------------------------------------------

    print("a lobby is one row, keyed by the host endpoint")
    _, first = call("POST", base + "/v1/servers",
                    {"name": "Rehost", "port": 28650, "players": 1})
    _, body = call("GET", base + "/v1/servers")
    check("the host appears once",
          len([s for s in body["servers"] if s["port"] == 28650]), 1)

    print("a re-register REPLACES the prior entry, it does not accumulate")
    # The host crashed or closed without unregistering, then re-hosts on the
    # same listen port. The old advertisement must be gone, not left as a ghost.
    _, second = call("POST", base + "/v1/servers",
                     {"name": "Rehost", "port": 28650, "players": 1})
    check("the re-host mints a new id", first["id"] != second["id"], True)
    _, body = call("GET", base + "/v1/servers")
    rows = [s for s in body["servers"] if s["port"] == 28650]
    check("still one row for that host, not two", len(rows), 1)
    check("and it is the new registration", rows[0]["id"], second["id"])
    status, _ = call("PUT", base + "/v1/servers/" + first["id"],
                     {"token": first["token"], "players": 1})
    check("the replaced lobby's id is gone (its heartbeat is refused)",
          status, 403)

    print("two hosts on one machine (different ports) stay distinct")
    _, host_a = call("POST", base + "/v1/servers", {"name": "A", "port": 28651})
    _, host_b = call("POST", base + "/v1/servers", {"name": "B", "port": 28652})
    _, body = call("GET", base + "/v1/servers")
    check("a different listen port is a different lobby",
          sorted(s["port"] for s in body["servers"]
                 if s["port"] in (28650, 28651, 28652)),
          [28650, 28651, 28652])

    print("the listing collapses a duplicate host row it did not create")
    # Belt and braces for a database written by an older directory that did not
    # replace on register: two rows for one endpoint must still list once, and
    # the survivor is the most recently updated.
    now = int(time.time())
    service.store.db.execute(
        "INSERT INTO servers (id, token_hash, code, kind, name, address, port,"
        " players, max_players, roster_json, config_json, created_at,"
        " updated_at, expires_at)"
        " VALUES ('dupe','h','DUP1','bounty','Dupe','127.0.0.1',28652,1,4,'[]',"
        " '{}',?,?,?)",
        (now, now - 5, now + ms.TTL_SECONDS))
    service.store.db.commit()
    _, body = call("GET", base + "/v1/servers")
    rows = [s for s in body["servers"] if s["port"] == 28652]
    check("a duplicate host row is collapsed to one", len(rows), 1)
    check("keeping the most recently updated row", rows[0]["id"], host_b["id"])

    print("a lobby with no heartbeat drops off on its TTL")
    service.store.db.execute(
        "UPDATE servers SET expires_at = ? WHERE port = ?",
        (int(time.time()) - 1, 28651))
    service.store.db.commit()
    _, body = call("GET", base + "/v1/servers")
    check("the un-heartbeated lobby expired out of the list",
          [s for s in body["servers"] if s["port"] == 28651], [])
    check("the still-beating lobbies remain",
          sorted(set(s["port"] for s in body["servers"]
                     if s["port"] in (28650, 28652))),
          [28650, 28652])

    for sid in (second["id"], host_b["id"], "dupe"):
        service.store.remove(sid, force=True)

    service.shutdown()
    shutil.rmtree(workdir, ignore_errors=True)
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
