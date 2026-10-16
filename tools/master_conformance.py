#!/usr/bin/env python3
"""Does a master server speak the protocol the GAME CLIENT actually uses?

WHY THIS EXISTS. It was found that the directory the fork ships as its default
- the Cloudflare worker - rejected every registration the client made, because
`validConfig` demanded `map` and `game_mode`, two fields `lsw2_master.cpp` has
never sent. Four more calls diverged behind it. It went unnoticed for as long as
it did because every test used the SELF-HOST server, and the two were assumed
equivalent for implementing "the same routes".

Two implementations of one protocol drift silently. This is the thing that
notices.

THE ONLY SOURCE OF TRUTH IS THE CLIENT. Every expectation below is taken from
port/net/lsw2_master.cpp, quoted where it matters:

    reachable()       GET  /v1/health          body must contain "ok"
    register_lobby()  POST /v1/servers         body {name, port, players, kind,
                                               roster}; reads id, token, code
    heartbeat()       PUT  /v1/servers/<id>    X-Token header; body {token,
                                               players, roster}; reads directives
    unregister()      DELETE /v1/servers/<id>  X-Token header
    refresh()         GET  /v1/servers         reads name, address, port,
                                               players, max_players, code, kind,
                                               roster
    resolve()         GET  /v1/codes/<code>    reads name, address, port

Run it against anything that claims to be a directory:

    python tools/master_conformance.py http://127.0.0.1:28553

It is deliberately picky about the things that actually broke: the exact method,
the exact path, the header name, and the presence of each field the client
parses. A server can pass every route and still fail here by omitting one field,
which is the failure mode that shipped.
"""

from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print("  %-52s %s%s" % (name, "PASS" if ok else "FAIL",
                            ("  " + detail) if detail and not ok else ""))
    return ok


def call(base, method, path, body=None, token=None, timeout=8):
    """One request, exactly as lsw2_master.cpp would form it."""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    # THE CLIENT AUTHENTICATES WITH X-Token. request() adds it to every call
    # once it holds one, and never sets an Authorization header at all. A server
    # expecting Bearer will 401 here, which is the point.
    if token:
        req.add_header("X-Token", token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            return r.status, (json.loads(raw) if raw.strip() else {})
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(raw) if raw.strip() else {}
        except json.JSONDecodeError:
            return e.code, {"_raw": raw}
    except Exception as e:            # noqa: BLE001 - report, do not raise
        return 0, {"_error": str(e)}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    base = sys.argv[1].rstrip("/")
    print("conformance against %s\n" % base)

    # --- reachable() ------------------------------------------------------
    print("reachable()  GET /v1/health")
    st, body = call(base, "GET", "/v1/health")
    check("/v1/health answers 200", st == 200, "got %s" % st)
    check('body carries "ok"', "ok" in body)

    # --- register_lobby() -------------------------------------------------
    print("\nregister_lobby()  POST /v1/servers")
    reg_body = {"name": "ConformanceBot", "port": 28550, "players": 1,
                "kind": "bounty",
                "roster": [{"slot": 0, "name": "ConformanceBot"}]}
    st, body = call(base, "POST", "/v1/servers", reg_body)
    ok_reg = check("registration accepted", st in (200, 201),
                   "got %s %s" % (st, body))
    sid = body.get("id", "")
    token = body.get("token", "")
    code = body.get("code", "")
    check("reply carries id", bool(sid))
    check("reply carries token", bool(token))
    # The room code is the four-character one players type to join. Its ABSENCE is what
    # made join-by-code impossible against the hosted directory.
    check("reply carries a 4-character room code",
          isinstance(code, str) and len(code) == 4, "got %r" % (code,))

    if not ok_reg or not sid:
        print("\ncannot continue without a registration")
        return 1

    # --- refresh() --------------------------------------------------------
    print("\nrefresh()  GET /v1/servers")
    st, body = call(base, "GET", "/v1/servers")
    check("listing answers 200", st == 200, "got %s" % st)
    rows = body.get("servers", [])
    check("listing has a 'servers' array", isinstance(rows, list))
    mine = next((r for r in rows if r.get("id") == sid), None)
    check("our lobby is listed", mine is not None)
    if mine:
        for field in ("name", "address", "port", "players", "max_players",
                      "code", "kind", "roster"):
            check("listing row carries %r" % field, field in mine)
        check("roster entries carry slot and name",
              isinstance(mine.get("roster"), list) and
              all("slot" in e and "name" in e for e in mine.get("roster", [])))
        # /v1/servers is unauthenticated by design, so anything in it is public.
        # A host advertising its own address chose to; the people who joined it
        # did not.
        check("roster does NOT leak player addresses",
              all("address" not in e for e in mine.get("roster", [])))

    # --- resolve() --------------------------------------------------------
    print("\nresolve()  GET /v1/codes/<code>")
    if code:
        st, body = call(base, "GET", "/v1/codes/" + code)
        check("the issued code resolves", st == 200, "got %s" % st)
        for field in ("name", "address", "port"):
            check("resolve carries %r" % field, field in body)
        # Codes are read off a screen, so the alphabet excludes I, L, O and U
        # and lookups fold the substitutions people actually type.
        st2, _ = call(base, "GET", "/v1/codes/" + code.lower())
        check("lookup is case-insensitive", st2 == 200, "got %s" % st2)
    st, _ = call(base, "GET", "/v1/codes/ZZZZ")
    check("an unknown code 404s", st == 404, "got %s" % st)

    # --- heartbeat() ------------------------------------------------------
    print("\nheartbeat()  PUT /v1/servers/<id>")
    hb = {"token": token, "players": 2,
          "roster": [{"slot": 0, "name": "ConformanceBot"},
                     {"slot": 1, "name": "Guest"}]}
    st, body = call(base, "PUT", "/v1/servers/" + sid, hb, token=token)
    check("heartbeat accepted on PUT with X-Token", st == 200,
          "got %s %s" % (st, body))
    # NOT "the field is present". The self-host server adds `directives` only
    # when some are PENDING, and the client drains it with
    # lsw2_json_objects(reply, "directives"), which finds none and appends
    # nothing. Absence is the correct quiet case; a non-list would be the bug.
    check("'directives', if present, is a list",
          isinstance(body.get("directives", []), list))
    st, body = call(base, "GET", "/v1/servers")
    mine = next((r for r in body.get("servers", []) if r.get("id") == sid), None)
    check("heartbeat updated the player count", bool(mine) and mine.get("players") == 2,
          "got %s" % (mine.get("players") if mine else None))

    print("\nheartbeat rejects a bad token")
    # THE TOKEN TRAVELS TWICE and the two servers read different copies: the
    # self-host server authenticates the heartbeat from the BODY's "token", the
    # worker from the X-Token header. The client sends both, so either is valid
    # - which means a conformance check has to corrupt BOTH to test anything at
    # all. The first version of this changed only the header, left the good
    # token in the body, and reported a security hole that was not there.
    bad = dict(hb, token="not-the-token")
    st, _ = call(base, "PUT", "/v1/servers/" + sid, bad, token="not-the-token")
    check("a wrong token is refused", st in (401, 403), "got %s" % st)

    # --- unregister() -----------------------------------------------------
    print("\nunregister()  DELETE /v1/servers/<id>")
    st, _ = call(base, "DELETE", "/v1/servers/" + sid, token=token)
    check("unregister accepted", st in (200, 204), "got %s" % st)
    st, body = call(base, "GET", "/v1/servers")
    gone = not any(r.get("id") == sid for r in body.get("servers", []))
    check("the lobby is delisted", gone)

    print("\n%d passed, %d failed" % (len(PASS), len(FAIL)))
    if FAIL:
        print("\nFAILED:")
        for f in FAIL:
            print("   ", f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
