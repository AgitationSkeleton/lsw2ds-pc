"""The master server, self-hosted.

WHY THIS EXISTS ALONGSIDE services/master-server. That one is a Cloudflare
Worker written during the Godot era: it needs an account, wrangler, and a D1
database, and it runs on Cloudflare's edge. This one is hosted by whoever runs
the lobby list, on their own machine, so a Worker is the wrong shape.

This is the same contract in a form you can run with one command and no
dependencies - Python's standard library only, SQLite for storage. The endpoint
paths and the expiry model are deliberately identical to the Worker's, so a
client written against one works against the other and the Worker stays a valid
deployment option.

    python master_server.py --port 28553

WHAT IT DOES AND DOES NOT DO. It is a directory, not a relay: it stores short
advertisements and hands out addresses. No game traffic passes through it, so it
cannot make an unreachable host reachable - that is what the UPnP mapping on the
host is for, and failing that services/turn-relay.

THE PORT DEFAULT IS 28553, matching the client. 553 is the ROM's own release
number lifted into the dynamic range, so it is traceable to this project rather
than a number somebody picked, and it stays clear of the ranges Steam and the
common game servers occupy.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import socket
import sqlite3
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import eventlog
from banlist import BanList
from eventlog import EventLog
import filelog
from filelog import FileSink

# How long an advertisement survives without a heartbeat - the Worker's value
# too, so the two behave identically. The game heartbeats about once a second
# (forksession.cpp, every 60 fork ticks), so twenty seconds survives a long run
# of dropped requests, and a host that crashed to the desktop leaves the list in
# twenty seconds rather than ninety; the longer TTL kept zombie hosts listed.
# A host whose game crashed with its window still open now
# withdraws itself at once (forksession.cpp, abandon_session).
TTL_SECONDS = 20

MAX_PLAYERS = 4          # the arena's own width - four player slots, four palettes
MAX_NAME = 10            # the waiting list has fixed column width

# CROCKFORD BASE 32, minus I, L, O and U. Digits and letters that cannot be
# confused when read aloud or typed from a photograph: I and L fold to 1, O to
# 0. U is excluded rather than folded, which is Crockford's own reason - it
# keeps a randomly generated code from spelling something unfortunate.
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
CODE_LENGTH = 4          # room codes are four letters/digits

# THE TWO KINDS OF LOBBY. The GUI sidebar stacks two scrolling lists one above
# the other, Bounty Hunter lobbies and co-op lobbies, and the co-op ones
# are games broadcast by the base recomp. That
# makes the kind a first-class column, not a flag buried in config_json: the
# lists are built by filtering on it, and a client asking for one kind should
# not have to download and sift the other.
KIND_BOUNTY = "bounty"
KIND_COOP = "coop"
KINDS = (KIND_BOUNTY, KIND_COOP)

SCHEMA = """
CREATE TABLE IF NOT EXISTS servers (
  id          TEXT PRIMARY KEY,
  token_hash  TEXT NOT NULL,
  code        TEXT UNIQUE,
  kind        TEXT NOT NULL DEFAULT 'bounty',
  name        TEXT NOT NULL,
  address     TEXT NOT NULL,
  port        INTEGER NOT NULL,
  players     INTEGER NOT NULL DEFAULT 1,
  max_players INTEGER NOT NULL DEFAULT 4,
  roster_json TEXT NOT NULL DEFAULT '[]',
  config_json TEXT NOT NULL DEFAULT '{}',
  created_at  INTEGER NOT NULL DEFAULT 0,
  updated_at  INTEGER NOT NULL,
  expires_at  INTEGER NOT NULL
);
"""

# SEPARATE FROM THE TABLE, AND RUN AFTER THE MIGRATIONS. Indexing a column that
# an older database has not been given yet fails the whole script, and
# CREATE TABLE IF NOT EXISTS does nothing to add it - so a service upgraded in
# place died on startup with "no such column: kind" while every test passed,
# because the tests use a fresh :memory: database. Order is the fix: create,
# migrate, then index.
INDEXES = """
CREATE INDEX IF NOT EXISTS servers_expires_at ON servers(expires_at);
CREATE INDEX IF NOT EXISTS servers_code ON servers(code);
CREATE INDEX IF NOT EXISTS servers_kind ON servers(kind);
"""

# Columns added after the first release. SQLite has no "ADD COLUMN IF NOT
# EXISTS", so an existing master.sqlite3 is upgraded by trying each and ignoring
# the duplicate-column error. A directory is disposable state - every row expires
# in ninety seconds - but throwing away someone's running service to add a column
# would be a rude way to ship an update.
MIGRATIONS = (
    "ALTER TABLE servers ADD COLUMN kind TEXT NOT NULL DEFAULT 'bounty'",
    "ALTER TABLE servers ADD COLUMN roster_json TEXT NOT NULL DEFAULT '[]'",
    "ALTER TABLE servers ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0",
)


def normalise_kind(raw) -> str:
    """Anything unrecognised is a bounty lobby - that is what the fork sends and
    what predates the column, so it is the safe default for an old client."""
    kind = str(raw or "").strip().lower()
    return kind if kind in KINDS else KIND_BOUNTY


def sanitize_name(raw: str) -> str:
    """Player names are sanitized to avoid injections and other security
    problems.

    ALLOW-LIST, NOT DENY-LIST. Stripping the characters you can think of is a
    losing game; permitting the ones the game can actually draw is not. The DS
    font here covers ASCII letters, digits, space and a few marks, so anything
    outside that could not be rendered even if it were safe.

    This is not the only defence - every value reaching SQLite goes through a
    bound parameter, and the JSON encoder escapes on output - but a name is the
    one field that travels from a stranger to every other player's screen, so it
    is worth narrowing at the door as well.
    """
    cleaned = re.sub(r"[^A-Za-z0-9 _\-.]", "", raw or "")
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    # STRIP AGAIN AFTER THE CUT. The strip above runs before the truncation, so
    # it cannot see whitespace the truncation itself exposes - and it did:
    # "Maclunkey - Arena" is clean, and its first ten characters are
    # "Maclunkey " with a trailing space, which is what the directory logged and
    # what the in-game list drew. Slicing a string can create trailing
    # whitespace that was interior a moment earlier.
    return cleaned[:MAX_NAME].strip() or "PLAYER"


def valid_address(raw) -> str:
    """An IP address, or the empty string.

    PARSE IT, DO NOT SCRUB IT. The first version of this stripped characters
    outside `[0-9A-Fa-f:.]`, which reads like an allow-list and is not one:
    "10.0.0.5; rm -rf" came through as "10.0.0.5f", because the f in "rf" is a
    hex digit. Scrubbing produces a string that LOOKS like an address; parsing
    tells you whether it is one. This value ends up in a ban file that an
    operator edits by hand, so a plausible-looking near-miss is worse than
    nothing at all.
    """
    text = str(raw or "").strip()
    if not text or len(text) > 45:
        return ""
    for family in (socket.AF_INET, socket.AF_INET6):
        try:
            socket.inet_pton(family, text)
            return text
        except (OSError, ValueError):
            continue
    return ""


def sanitize_roster(raw) -> list:
    """The players in a lobby, as the host reports them.

    The GUI's lists can be expanded to view the connected players in each
    lobby, and the right-click menu offers Kick and Ban IP for any server or
    player. Neither is possible from a player COUNT,
    so the host now reports who is in the room.

    THE ADDRESSES HERE ARE THE HOST'S CLAIM, not something this service
    observed - the players connect to the host, not to the directory, so the
    directory has no other way to learn them. That is fine for the purpose (an
    operator banning someone who is griefing a lobby) and would not be fine as
    an authorisation input, which is why nothing here is used for one. A host
    that lies about its roster can only get its own players banned.

    Every field is clamped: a roster arrives from the network and lands in a
    GUI list, so the width the window was drawn for is the width it may be.
    """
    if not isinstance(raw, list):
        return []
    out = []
    for entry in raw[:MAX_PLAYERS]:
        if isinstance(entry, str):
            entry = {"name": entry}
        if not isinstance(entry, dict):
            continue
        address = valid_address(entry.get("address"))
        slot = entry.get("slot")
        try:
            slot = max(0, min(MAX_PLAYERS - 1, int(slot)))
        except (TypeError, ValueError):
            slot = len(out)
        out.append({"slot": slot,
                    "name": sanitize_name(entry.get("name")),
                    "address": address})
    return out


def make_code(db: sqlite3.Connection) -> str:
    """A room code that is free right now.

    Four Crockford characters is about a million codes. That is plenty for
    concurrent lobbies and far too few to be unguessable, which is fine and
    deliberate: a room code is a convenience for friends, not an access control.
    Anything that must not be guessed uses the token instead.
    """
    for _ in range(64):
        code = "".join(secrets.choice(CROCKFORD) for _ in range(CODE_LENGTH))
        if not db.execute("SELECT 1 FROM servers WHERE code = ?", (code,)).fetchone():
            return code
    # Every attempt collided, which means the space is saturated. Say so rather
    # than loop forever or hand out a duplicate.
    raise RuntimeError("no free room code")


def normalise_code(raw: str) -> str:
    """Fold the substitutions people actually make when typing a code they read
    somewhere: I and L are 1, O is 0, case does not matter."""
    s = (raw or "").upper().strip()
    return s.replace("I", "1").replace("L", "1").replace("O", "0")


class Store:
    def __init__(self, path: str):
        # check_same_thread=False because ThreadingHTTPServer answers on worker
        # threads; every access below goes through this one lock-free connection
        # and SQLite serialises writes itself.
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.executescript(SCHEMA)
        for statement in MIGRATIONS:
            try:
                self.db.execute(statement)
            except sqlite3.OperationalError:
                pass          # already present - see MIGRATIONS
        self.db.executescript(INDEXES)
        self.db.commit()

        # Orders (kicks) waiting to be handed to a host on its next heartbeat.
        # id -> list of dicts. IN MEMORY ON PURPOSE: a kick is about a player
        # who is connected right now, and one that survived a restart of the
        # directory would be delivered to a session that had long since ended.
        # Bans are the durable half, and those live in the text file.
        self.pending: dict = {}

        # SWEEP ON START, NOT ONLY ON REQUEST. The store is a file on purpose -
        # a lobby is disposable state but the service is not - so a directory
        # restarted while hosts were up reopens a database that still holds
        # their rows. Any whose TTL lapsed while the directory was down are
        # ghosts; clearing them here means a reopened server does not hand out
        # dead lobbies until the first client happens to call in and trigger the
        # per-request sweep. Live hosts re-heartbeat or re-register and reclaim
        # their row, and a re-register replaces its own prior entry (see
        # register), so old lobbies are not "preserved" - they are replaced or
        # expired.
        self.sweep()

    def sweep(self) -> list:
        """Drop expired rows. Called on every request rather than on a timer:
        a directory nobody is reading does not need tidying, and this way there
        is no background thread to leak.

        Returns what went, so the caller can log it. A lobby vanishing because
        its host stopped heartbeating is exactly the "lobby state" line the
        log pane exists to show, and it is the one event with no request
        behind it to report it."""
        now = int(time.time())
        gone = self.db.execute(
            "SELECT id, kind, name, address FROM servers WHERE expires_at < ?",
            (now,)).fetchall()
        if not gone:
            return []
        self.db.execute("DELETE FROM servers WHERE expires_at < ?", (now,))
        self.db.commit()
        for row in gone:
            self.pending.pop(row[0], None)
        return [{"id": r[0], "kind": r[1], "name": r[2], "address": r[3]}
                for r in gone]

    def register(self, name, address, port, players, config, kind=KIND_BOUNTY,
                 roster=None):
        now = int(time.time())
        port = int(port)

        # ONE LOBBY PER HOST, REPLACED ON RE-REGISTER - not accumulated.
        #
        # A host identifies itself by the endpoint it advertises: the socket
        # address it connected from (which it cannot spoof - see
        # _client_address) and the game port it listens on. A second
        # registration from that same endpoint is the same host hosting again -
        # after a crash or a window-close that skipped its unregister, after
        # "Shut Down Lobby" then a re-host, or after the directory itself was
        # restarted while the host kept running. Left alone, the previous
        # advertisement would sit in the list beside the new one: a ghost row
        # that refuses connections for the rest of its TTL, and one more of them
        # every time the host comes back. So the prior entry for this endpoint
        # is deleted before the new one is written - a re-register REPLACES
        # rather than duplicates. TTL expiry (see sweep) still covers a host
        # that crashes and never returns.
        stale = self.db.execute(
            "SELECT id FROM servers WHERE address = ? AND port = ?",
            (address, port)).fetchall()
        if stale:
            self.db.execute(
                "DELETE FROM servers WHERE address = ? AND port = ?",
                (address, port))
            for (old_id,) in stale:
                self.pending.pop(old_id, None)

        sid = secrets.token_hex(8)
        token = secrets.token_urlsafe(24)
        code = make_code(self.db)
        self.db.execute(
            "INSERT INTO servers (id, token_hash, code, kind, name, address,"
            " port, players, max_players, roster_json, config_json,"
            " created_at, updated_at, expires_at)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (sid, _hash(token), code, normalise_kind(kind), sanitize_name(name),
             address, port, _clamp_players(players), MAX_PLAYERS,
             json.dumps(sanitize_roster(roster)), json.dumps(config or {}),
             now, now, now + TTL_SECONDS))
        self.db.commit()
        # THE TOKEN IS RETURNED ONCE AND STORED ONLY AS A HASH. A leaked
        # database should not let anyone update or delete other people's
        # lobbies.
        return {"id": sid, "token": token, "code": code,
                "expires_in": TTL_SECONDS}

    def heartbeat(self, sid, token, players, config, roster=None):
        row = self.db.execute(
            "SELECT token_hash FROM servers WHERE id = ?", (sid,)).fetchone()
        if not row or not secrets.compare_digest(row[0], _hash(token)):
            return None
        now = int(time.time())
        fields = ["players = ?", "updated_at = ?", "expires_at = ?"]
        args = [_clamp_players(players), now, now + TTL_SECONDS]
        if config is not None:
            fields.insert(1, "config_json = ?")
            args.insert(1, json.dumps(config))
        if roster is not None:
            fields.insert(1, "roster_json = ?")
            args.insert(1, json.dumps(sanitize_roster(roster)))
        args.append(sid)
        self.db.execute("UPDATE servers SET " + ", ".join(fields) + " WHERE id = ?",
                        args)
        self.db.commit()

        # THE HEARTBEAT IS THE ONLY CHANNEL BACK TO A HOST. The directory never
        # dials out: hosts are behind the NAT that UPnP just punched a single
        # game port through, so nothing here can open a connection to them. A
        # kick therefore waits in `pending` and rides the next heartbeat, which
        # is at most TTL/3 away.
        out = {"ok": True, "expires_in": TTL_SECONDS}
        directives = self.pending.pop(sid, None)
        if directives:
            out["directives"] = directives
        return out

    def enqueue_directive(self, sid, directive) -> bool:
        """Queue one instruction for a host. False if there is no such lobby."""
        row = self.db.execute("SELECT 1 FROM servers WHERE id = ?", (sid,)).fetchone()
        if not row:
            return False
        queue = self.pending.setdefault(sid, [])
        if len(queue) >= 16:
            # An operator clicking Kick at a host that is not heartbeating
            # should not build an unbounded list. Sixteen is four times the
            # arena's width - more than a real operator queues before noticing.
            queue.pop(0)
        queue.append(directive)
        return True

    def remove(self, sid, token=None, force=False):
        row = self.db.execute(
            "SELECT token_hash, kind, name, address FROM servers WHERE id = ?",
            (sid,)).fetchone()
        if not row:
            return None
        # `force` is the operator's path from the GUI, where there is no token -
        # the machine running the directory is allowed to close a lobby on it.
        if not force and (token is None or
                          not secrets.compare_digest(row[0], _hash(token))):
            return None
        self.db.execute("DELETE FROM servers WHERE id = ?", (sid,))
        self.db.commit()
        self.pending.pop(sid, None)
        return {"ok": True, "kind": row[1], "name": row[2], "address": row[3]}

    def listing(self, kind=None, operator=False):
        """The lobby list.

        `operator` is the GUI's view, called in-process, and is the ONLY view
        that carries player IP addresses.

        THE PUBLIC LIST MUST NOT CARRY PLAYER ADDRESSES. /v1/servers is
        unauthenticated by design - it is how every client finds a game - so
        anything in it is public to anyone who can reach the directory. A host's
        address belongs there, because that is the point of a directory and the
        host chose to advertise it. The addresses of the people who JOINED that
        host did not choose anything, and publishing them would turn the lobby
        browser into an IP harvester for everyone playing. The roster's names
        stay; the addresses are for the operator's Ban IP menu and go no
        further than this machine.
        """
        sql = ("SELECT name, address, port, players, max_players, code,"
               " config_json, kind, id, roster_json, created_at FROM servers")
        args = ()
        if kind is not None:
            sql += " WHERE kind = ?"
            args = (normalise_kind(kind),)
        sql += " ORDER BY updated_at DESC"
        rows = self.db.execute(sql, args).fetchall()

        # ONE ROW PER HOST. The rows arrive newest-first, so the first time an
        # endpoint (address + game port) is seen is its freshest advertisement;
        # any later row for the same host is an older duplicate and is dropped.
        # register() already replaces on re-register, so in normal running there
        # are none to drop - this is the guard for a database written by an
        # older directory that did not dedup, and for the brief window between a
        # host crashing and the sweep clearing it. A 2-player lobby is one lobby,
        # identified by its host, and it appears exactly once.
        out = []
        seen = set()
        for r in rows:
            key = (r[1], r[2])           # host endpoint: address, listen port
            if key in seen:
                continue
            seen.add(key)
            roster = json.loads(r[9] or "[]")
            if not operator:
                roster = [{"slot": p.get("slot", 0), "name": p.get("name", "")}
                          for p in roster]
            # NO PING FIELD, deliberately. The game's browser row shows one -
            # "Maclunkey 1/4 96 <green_ds_icon>" - but a ping is between the
            # CLIENT and the host, and a number measured here would be the
            # distance to this machine instead. The client measures it after
            # resolving the address.
            out.append({"name": r[0], "address": r[1], "port": r[2],
                        "players": r[3], "max_players": r[4], "code": r[5],
                        "config": json.loads(r[6] or "{}"), "kind": r[7],
                        "id": r[8], "roster": roster, "created_at": r[10]})
        return out

    def find(self, sid, operator=True):
        for entry in self.listing(operator=operator):
            if entry["id"] == sid:
                return entry
        return None

    def resolve(self, code):
        row = self.db.execute(
            "SELECT name, address, port, players, max_players FROM servers"
            " WHERE code = ?", (normalise_code(code),)).fetchone()
        if not row:
            return None
        return {"name": row[0], "address": row[1], "port": row[2],
                "players": row[3], "max_players": row[4]}


def _hash(token: str) -> str:
    import hashlib
    return hashlib.sha256(token.encode()).hexdigest()


def _split_query(path: str):
    """`/v1/servers?kind=coop` -> ("/v1/servers", {"kind": "coop"}).

    Hand-rolled rather than urllib.parse because the whole query surface is one
    optional key and the answer wanted is a plain dict of last-wins strings.
    """
    if "?" not in path:
        return path, {}
    head, _, tail = path.partition("?")
    out = {}
    for pair in tail.split("&"):
        if not pair:
            continue
        key, _, value = pair.partition("=")
        out[key] = value
    return head, out


def _clamp_players(n) -> int:
    try:
        n = int(n)
    except (TypeError, ValueError):
        n = 1
    return max(0, min(MAX_PLAYERS, n))


class Handler(BaseHTTPRequestHandler):
    server_version = "lsw2-master/1"
    # HTTP/1.1 because every response carries an accurate Content-Length, which
    # is the requirement for it. The default is 1.0, and a client that matched
    # the version string rather than parsing the status code read that as a
    # failure - both ends are fixed, since the client should not have cared and
    # the server should have said 1.1.
    protocol_version = "HTTP/1.1"
    store: Store = None            # set by serve()
    bans: BanList = None           # set by serve()
    log: EventLog = None           # set by serve()

    def log_message(self, fmt, *args):
        # The default logs every request to stderr, which on a long-running
        # service is noise that hides the lines worth reading. Everything worth
        # saying goes through self.log with a category instead.
        pass

    def log_error(self, fmt, *args):
        self.log.debug("http", "%s %s" % (self.client_address[0], fmt % args))

    def _send(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return {}
        if n <= 0 or n > 64 * 1024:
            return {}
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            return {}

    def _client_address(self, body):
        """THE HOST DOES NOT GET TO CHOOSE ITS OWN PUBLIC ADDRESS.

        Taking it from the request body would let anyone advertise a lobby
        pointing at someone else's machine - a small but real reflection
        vector, and the sort of thing that is free to prevent now and awkward
        later. The socket knows where the request came from.

        The PORT is still the host's to state: the socket's source port is
        ephemeral and unrelated to the port it is listening on.
        """
        return self.client_address[0]

    # ONE DOOR. Every method funnels through here so the ban check and the sweep
    # cannot be forgotten when an endpoint is added - which is exactly how a ban
    # list stops being a ban list. It also puts the expiry log line in one place.
    def _drain_body(self):
        """Read and discard the request body.

        REFUSING A REQUEST IS NOT A REASON TO LEAVE ITS BODY ON THE SOCKET.
        This is HTTP/1.1 with keep-alive: a POST whose body is never read leaves
        those bytes in the stream, and the next request on that connection
        either parses them as a request line or gets an RST when the server
        closes with data pending. The client sees `ConnectionAbortedError` on a
        LATER request than the one that was refused, which is why this looked
        like an intermittent network flake rather than a bug in the refusal.

        Found exactly that way: the suite failed once in four runs, always just
        after the ban took effect, and always on the request AFTER the refused
        POST.
        """
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return
        if n <= 0:
            return
        remaining = min(n, 64 * 1024)
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 8192))
            if not chunk:
                break
            remaining -= len(chunk)

    def _dispatch(self, handler):
        peer = self.client_address[0]
        if self.bans.is_banned(peer):
            self._drain_body()
            self.log.warn("ban", "refused %s %s from banned %s"
                          % (self.command, self.path, peer))
            return self._send(403, {"error": "banned"})
        for gone in self.store.sweep():
            self.log.info("lobby", "expired %s lobby '%s' (%s) - host stopped "
                                   "heartbeating" % (gone["kind"], gone["name"],
                                                     gone["address"]))
        handler()

    def do_GET(self):
        self._dispatch(self._get)

    def do_POST(self):
        self._dispatch(self._post)

    def do_PUT(self):
        self._dispatch(self._put)

    def do_DELETE(self):
        self._dispatch(self._delete)

    def _get(self):
        path, query = _split_query(self.path)
        if path == "/v1/servers":
            kind = query.get("kind")
            listing = self.store.listing(normalise_kind(kind) if kind else None)
            self.log.debug("list", "%s asked for %s lobbies, %d returned"
                           % (self.client_address[0], kind or "all", len(listing)))
            return self._send(200, {"servers": listing})
        if path.startswith("/v1/codes/"):
            wanted = path[len("/v1/codes/"):]
            got = self.store.resolve(wanted)
            self.log.info("code", "%s looked up room code %s - %s"
                          % (self.client_address[0], normalise_code(wanted),
                             "found" if got else "no match"))
            if not got:
                return self._send(404, {"error": "no such room code"})
            return self._send(200, got)
        if path in ("/", "/v1/health"):
            return self._send(200, {"ok": True, "service": "lsw2-master",
                                    "ttl": TTL_SECONDS})
        self._send(404, {"error": "not found"})

    def _post(self):
        if self.path != "/v1/servers":
            return self._send(404, {"error": "not found"})
        body = self._body()
        port = body.get("port")
        try:
            port = int(port)
        except (TypeError, ValueError):
            return self._send(400, {"error": "port required"})
        if not (1 <= port <= 65535):
            return self._send(400, {"error": "port out of range"})
        kind = normalise_kind(body.get("kind"))
        address = self._client_address(body)
        try:
            got = self.store.register(body.get("name"), address,
                                      port, body.get("players", 1),
                                      body.get("config"), kind,
                                      body.get("roster"))
        except RuntimeError as exc:
            self.log.error("lobby", "could not register a lobby for %s: %s"
                           % (address, exc))
            return self._send(503, {"error": str(exc)})
        # The line this log is modelled on: someone at an IP address connects
        # and opens a new Bounty Hunter or co-op game.
        self.log.info("lobby", "%s started a new %s game as '%s' on %s:%d - "
                               "room code %s"
                      % (address, kind, sanitize_name(body.get("name")),
                         address, port, got["code"]))
        self._send(201, got)

    def _put(self):
        if not self.path.startswith("/v1/servers/"):
            return self._send(404, {"error": "not found"})
        sid = self.path[len("/v1/servers/"):]
        body = self._body()
        before = self.store.find(sid)
        got = self.store.heartbeat(sid, body.get("token", ""),
                                   body.get("players", 1), body.get("config"),
                                   body.get("roster"))
        if not got:
            self.log.warn("lobby", "rejected a heartbeat for %s from %s - "
                                   "unknown lobby or bad token"
                          % (sid, self.client_address[0]))
            return self._send(403, {"error": "unknown server or bad token"})
        self._log_roster_change(before, self.store.find(sid))
        if got.get("directives"):
            self.log.info("admin", "handed %d directive(s) to lobby '%s'"
                          % (len(got["directives"]),
                             (before or {}).get("name", sid)))
        self._send(200, got)

    def _delete(self):
        if not self.path.startswith("/v1/servers/"):
            return self._send(404, {"error": "not found"})
        token = self.headers.get("X-Token", "")
        got = self.store.remove(self.path[len("/v1/servers/"):], token)
        if not got:
            return self._send(403, {"error": "unknown server or bad token"})
        self.log.info("lobby", "%s closed its %s lobby '%s'"
                      % (got["address"], got["kind"], got["name"]))
        self._send(200, got)

    def _log_roster_change(self, before, after):
        """The log's player join/leave lines.

        THE DIRECTORY NEVER SEES A JOIN. Players connect to the host, so the
        only evidence a join happened is the host's roster changing between two
        heartbeats - which means these lines are up to one heartbeat late and
        are derived, not observed. Saying so here because a log that reads like
        a live feed but is a diff will mislead whoever reads it at 2am.
        """
        if not before or not after:
            return
        was = {(p.get("slot"), p.get("name")) for p in before.get("roster", [])}
        now = {(p.get("slot"), p.get("name")) for p in after.get("roster", [])}
        name = after.get("name", "?")
        for slot, who in sorted(now - was):
            self.log.info("player", "%s joined %s lobby '%s' in slot %s"
                          % (who, after.get("kind"), name, slot))
        for slot, who in sorted(was - now):
            self.log.info("player", "%s left %s lobby '%s'"
                          % (who, after.get("kind"), name))
        if before.get("players") != after.get("players"):
            self.log.debug("lobby", "'%s' now %d/%d"
                           % (name, after.get("players", 0),
                              after.get("max_players", MAX_PLAYERS)))


class Directory(ThreadingHTTPServer):
    """The listener, with one behaviour changed on Windows.

    TWO COPIES MUST NOT SHARE A PORT. http.server sets allow_reuse_address, and
    on Windows SO_REUSEADDR does not mean what it means on Unix: a SECOND live
    listener may bind a port that is already being served, and incoming
    connections are then split between the two at random. An operator who
    double-clicks the executable twice gets a directory that answers from two
    different databases depending on the packet - a lobby registered on one is
    invisible to the client that lists from the other, intermittently.

    That is not hypothetical; it happened while testing this build, and it cost
    twenty minutes because it looks exactly like "the log is broken" - the
    second window shows startup lines and then nothing, because it is serving
    nobody.

    SO_EXCLUSIVEADDRUSE is the Windows way to say what Unix's SO_REUSEADDR
    already says: this port is mine while I hold it, but do not make the next
    start wait for TIME_WAIT.
    """

    daemon_threads = True
    allow_reuse_address = os.name != "nt"

    def server_bind(self):
        if os.name == "nt" and hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            try:
                self.socket.setsockopt(socket.SOL_SOCKET,
                                       socket.SO_EXCLUSIVEADDRUSE, 1)
            except OSError:
                pass
        super().server_bind()


def build_parser():
    """Shared by both flavours so the two executables take the same flags."""
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=28553)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--db", default="master.sqlite3")
    ap.add_argument("--bans", default="bans.txt",
                    help="newline-separated IP ban list; edit it by hand to "
                         "lift a ban, the service re-reads it on change")
    ap.add_argument("--logs", default="logs",
                    help="directory for daily log files, kept %d days"
                         % filelog.RETAIN_DAYS)
    ap.add_argument("--quiet", action="store_true",
                    help="log only INFO and above")
    return ap


def local_addresses():
    """Every address this machine is likely reachable on.

    The log carries ports and IP info, and the first
    thing an operator needs is the address to give people - which is not
    0.0.0.0. The UDP trick finds the interface that would carry traffic to the
    outside without sending anything; the hostname lookup catches the rest.
    """
    found = []
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("203.0.113.1", 9))       # TEST-NET-3, never routed
        found.append(probe.getsockname()[0])
    except OSError:
        pass
    finally:
        probe.close()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None,
                                       socket.AF_INET):
            addr = info[4][0]
            if addr not in found:
                found.append(addr)
    except OSError:
        pass
    return found


class Service:
    """The directory, assembled but not yet serving.

    BOTH EXECUTABLES BUILD ONE OF THESE. The command-prompt flavour attaches a
    stdout sink and calls serve_forever on this thread; the GUI attaches its
    text pane and runs the same thing on a worker. Nothing about the service
    knows which it is, which is the only way the two stay identical.
    """

    def __init__(self, args, log=None):
        self.args = args
        self.log = log or EventLog()
        self.store = Store(args.db)
        self.bans = BanList(args.bans)
        self.file_sink = None
        self.httpd = None

    def attach_file_log(self):
        if self.args.logs and self.file_sink is None:
            self.file_sink = FileSink(
                self.args.logs,
                eventlog.INFO if self.args.quiet else eventlog.DEBUG)
            self.log.add_sink(self.file_sink)
        return self.file_sink

    def announce(self):
        a = self.args
        self.log.info("start", "lsw2 master server starting")
        self.log.info("start", "listening on %s:%d" % (a.host, a.port))
        for addr in local_addresses():
            self.log.info("start", "local address %s:%d"
                          % (addr, a.port))
        self.log.info("start", "database %s" % os.path.abspath(a.db))
        self.log.info("start", "advertisement ttl %ds" % TTL_SECONDS)
        self.log.info("start", "ban list %s (%d entr%s)"
                      % (os.path.abspath(a.bans), len(self.bans.entries()),
                         "y" if len(self.bans.entries()) == 1 else "ies"))
        if self.file_sink is not None:
            self.log.info("start", "logging to %s, keeping %d days"
                          % (os.path.abspath(self.args.logs),
                             self.file_sink.retain_days))
            if self.file_sink.last_error:
                self.log.warn("start", "log directory problem: %s"
                              % self.file_sink.last_error)
        self.log.debug("start", "hosts must forward %d/tcp for internet "
                                "joins; the client attempts UPnP first"
                       % a.port)

    def build(self):
        # Class attributes because BaseHTTPRequestHandler is instantiated per
        # request by the server; there is one Service per process.
        Handler.store = self.store
        Handler.bans = self.bans
        Handler.log = self.log
        try:
            self.httpd = Directory((self.args.host, self.args.port), Handler)
        except OSError as exc:
            # SAY WHICH PORT AND WHY. "OSError: [WinError 10048]" in a window
            # that then closes is the least useful thing this could do, and the
            # cause is almost always the copy already running.
            self.log.error("start", "cannot listen on %s:%d - %s"
                           % (self.args.host, self.args.port, exc))
            self.log.error("start", "another copy of the master server is "
                                    "probably already running on that port")
            raise
        return self.httpd

    def serve_forever(self):
        self.build()
        self.httpd.serve_forever()

    def shutdown(self):
        if self.httpd is not None:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None
        if self.file_sink is not None:
            self.file_sink.close()

    # ------------------------------------------------------- operator actions

    def kick(self, sid, slot, who="", address=""):
        """The GUI's "Kick", for a player inside a lobby."""
        ok = self.store.enqueue_directive(sid, {"action": "kick", "slot": slot,
                                                "address": address})
        lobby = self.store.find(sid)
        name = (lobby or {}).get("name", sid)
        if ok:
            self.log.warn("admin", "kick queued for %s in lobby '%s' - the host "
                                   "acts on its next heartbeat"
                          % (who or "slot %s" % slot, name))
        else:
            self.log.warn("admin", "kick ignored: no lobby %s" % sid)
        return ok

    def close_lobby(self, sid):
        """The GUI's "Kick", for a whole server row."""
        got = self.store.remove(sid, force=True)
        if got:
            self.log.warn("admin", "closed %s lobby '%s' (%s) from the console"
                          % (got["kind"], got["name"], got["address"]))
        else:
            self.log.warn("admin", "close ignored: no lobby %s" % sid)
        return bool(got)

    def ban(self, address, note=""):
        """The GUI's "Ban IP". Permanent, and the file is the truth.

        Banning also removes what that address is hosting - leaving a banned
        host's lobby in the list would advertise a game nobody can heartbeat,
        and it would sit there for the full TTL looking joinable.
        """
        address = valid_address(address)
        if not address:
            self.log.warn("ban", "refused to ban something that is not an "
                                 "IP address")
            return False
        added = self.bans.add(address, note)
        if added:
            self.log.warn("ban", "banned %s permanently (%s)"
                          % (address, os.path.abspath(self.bans.path)))
        else:
            self.log.info("ban", "%s was already banned" % address)
        for lobby in list(self.store.listing(operator=True)):
            if lobby["address"] == address:
                self.store.remove(lobby["id"], force=True)
                self.log.warn("ban", "removed banned host's %s lobby '%s'"
                              % (lobby["kind"], lobby["name"]))
        return added


def main(argv=None):
    args = build_parser().parse_args(argv)
    service = Service(args)
    service.log.add_sink(eventlog.stdout_sink(
        eventlog.INFO if args.quiet else eventlog.DEBUG))
    service.attach_file_log()
    service.announce()
    try:
        service.serve_forever()
    except KeyboardInterrupt:
        service.log.info("stop", "stopping on keyboard interrupt")
    except OSError:
        # build() has already logged which port and why. A traceback after that
        # tells the operator nothing they were not just told, and in a frozen
        # binary it is followed by PyInstaller's own error line, which reads
        # like a second, unrelated failure.
        service.shutdown()
        return 1
    finally:
        service.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
