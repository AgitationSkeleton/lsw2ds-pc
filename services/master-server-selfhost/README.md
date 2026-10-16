# The master server, self-hosted

The discovery service for player-hosted games - **Bounty Hunter arena** lobbies
from the fork, and **co-op** lobbies from the base recomp. It stores short
advertisements and hands out addresses; **no game traffic passes through it**,
so it cannot make an unreachable host reachable.

## Two flavours

The server comes as two executables: a command-prompt instance and a GUI. Both
run the same service; they differ only in where the log goes.

```
python master_server.py          # command prompt
python master_server_gui.py      # window
```

Frozen binaries, if you want them:

```
pip install pyinstaller
python build.py                  # dist/lsw2-master.exe, dist/lsw2-master-gui.exe
```

PyInstaller is needed to **build**, never to run. The service still starts from
source on any machine with Python and no build step - that fallback is
deliberate.

Both take the same flags:

```
lsw2-master --port 28553 --db master.sqlite3 --bans bans.txt --logs logs
```

`--quiet` drops the DEBUG lines. Paths are relative to where the binary is
**run**, not where it was built.

## The window

- **Sidebar**, two scrolling lists stacked one above the other: Bounty Hunter
  lobbies and Co-op lobbies. Expand a lobby to see who is in it.
- **Right-click** any lobby or player for **Kick** and **Ban IP**.
- **The larger right-hand part**: the console log, with a `debug` filter, a
  `follow` toggle and a button to the log folder.
- **Status bar**: lobby and player counts, the ban count, and where the ban file
  is.

## What Kick and Ban actually do

**The directory has no connection to a player.** Players connect to the host;
the host connects here. So:

- **Kick a player** queues a kick order that rides the host's **next
  heartbeat** - at most a third of the TTL away. The directory asks; the host acts.
- **Kick a lobby** removes it from the directory. Anyone already in it stays
  connected to the host. It stops being findable, that is all.
- **Ban IP** appends the address to the ban file. Every request from it is
  refused, and anything that address is hosting is dropped from the list at
  once - a lobby nobody may heartbeat would otherwise sit there for the full TTL
  looking joinable.
- Banning a **player** queues a kick as well, because the ban alone would leave
  them playing in the lobby they are being banned for.

## Bans

One address per line, in a plain text file:

```
# griefers
203.0.113.9
198.51.100.4    # spam, 2026-08-27
```

Blank lines and `#` comments are ignored. **Bans are permanent** and survive
restarts. To lift one, **edit the file** - the service notices the change and
re-reads it without a restart, so unbanning someone never means dropping every
lobby on the directory.

Only real IP addresses are written. An address is parsed, not scrubbed: a first
version stripped characters outside `[0-9A-Fa-f:.]`, which let
`10.0.0.5; rm -rf` through as `10.0.0.5` plus the `f` from `rf`.

## Logs

Written to `logs/` as one file per day:

```
logs/master-2026-08-27.log
```

Kept for **31 days**, on a rolling window - the sweep runs when the sink rolls
over to a new day, so there is no timer thread. Files are appended to, never
truncated: several runs in a day belong to the same day's file, and the reason
to read these is usually "the service died", which is exactly when truncating on
start would destroy the evidence.

The GUI reads the same records the files do. A window opened an hour after
startup still shows the startup and port lines, because the log keeps a ring
buffer and replays it to a sink that attaches late.

## The contract

| method | path | purpose |
| --- | --- | --- |
| `GET` | `/v1/health` | liveness, and the TTL it is using |
| `POST` | `/v1/servers` | advertise a lobby; returns `id`, `token`, `code` |
| `PUT` | `/v1/servers/{id}` | heartbeat; may carry `roster`; may return `directives` |
| `DELETE` | `/v1/servers/{id}` | shut the lobby down (`X-Token` header) |
| `GET` | `/v1/servers` | the list; `?kind=bounty` or `?kind=coop` to filter |
| `GET` | `/v1/codes/{code}` | resolve a room code to an address and port |

`kind` is `bounty` or `coop`; anything else, or absent, means `bounty` - which
is what the fork sends and what predates the column.

`roster` is a list of `{slot, name, address}`. It is how the GUI can expand a
lobby, and how Ban IP has an address to act on.

`directives` appear on a heartbeat reply when the operator has queued something,
and are delivered exactly once. Today the only one is
`{"action": "kick", "slot": N, "address": "..."}`.

## Decisions worth knowing

**A host does not choose its own address.** The advertised address comes from
the socket, not the request body. Taking it from the body would let anyone
advertise a lobby pointing at someone else's machine - a small but real
reflection vector, free to prevent now and awkward later. The **port** is still
the host's to state, because the socket's source port is ephemeral and unrelated
to what it is listening on.

**The public list hides player IP addresses.** `/v1/servers` is
unauthenticated - it is how every client finds a game - so everything in it is
public. A host's address belongs there; it chose to advertise. The addresses of
the people who **joined** chose nothing, and publishing them would turn the
lobby browser into an IP harvester. Roster **names** are public; addresses stay
on the machine running the directory, for the operator's Ban IP menu.

**A roster is the host's claim, not an observation.** Players connect to the
host, so there is no other way to learn them, and nothing here uses a roster for
authorisation. A host that lies can only get its own players banned.

**Joins and leaves are derived from a diff**, not seen. The directory never
witnesses a connection, so those log lines are up to one heartbeat late.

**The token is returned once and stored only as a SHA-256 hash.** A leaked
database should not let anyone update or delete other people's lobbies.

**Names are allow-listed, not deny-listed.** Stripping the characters you can
think of is a losing game; permitting the ones the game can draw is not. Cut to
10 characters because the waiting list has fixed column width, and empty becomes
`PLAYER`.

**Four players, clamped server-side.** Four is the arena's own width - the
handler-0 constructor counts exactly four slots and the ROM carries four slot
palettes.

**Room codes are four Crockford characters**, minus I, L, O and U. That folds
the substitutions people actually make reading a code aloud, and excluding U
keeps a generated code from spelling something unfortunate. About a million
codes: plenty for concurrent lobbies and far too few to be unguessable, which is
deliberate - a room code is a convenience for friends, not access control.
Anything that must not be guessed uses the token.

**No ping in the listing.** The game's browser row shows one -
`Maclunkey 1/4 96 <green_ds_icon>` - but a ping is between the **client** and
the host, and a number measured here would be the distance to this machine
instead. The client measures it after resolving the address.

**Expired rows are swept on request, not on a timer.** A directory nobody is
reading does not need tidying, and this way there is no background thread to
leak.

**Two copies cannot share a port.** `http.server` sets `SO_REUSEADDR`, and on
Windows that lets a *second live listener* bind a port already being served -
connections then split between the two at random, so a lobby registered on one
is intermittently invisible to a client listing from the other. The listener
asks for `SO_EXCLUSIVEADDRUSE` on Windows instead, and a second start fails with
a message naming the port. This is not hypothetical; it happened in testing,
and it looks exactly like "the log is broken".

## Upgrading an existing install

Columns added after the first release (`kind`, `roster_json`, `created_at`) are
applied to an existing `master.sqlite3` on startup. The order is create,
migrate, **then** index - indexing a column an older database has not been given
yet fails the whole script, and `CREATE TABLE IF NOT EXISTS` will not add it.

## Test it without the game

```
python test_master_server.py     # the service
python test_master_gui.py        # the window's list logic, no window shown
```

Both run from `tools/run_unit_tests.sh` alongside the C++ tests.

The service tests start it in-process on a free port and exercise every
endpoint, concentrating on the parts that are easy to get wrong rather than easy
to test: the token check, the sanitisers, the player clamp, code normalisation,
that a host cannot advertise someone else's address, that the public list hides
player IPs, that a database written by the previous release still opens, and
that **requests actually reach the log** - the last one exists because every
earlier check asserted on the HTTP reply, so a service that answered correctly
and logged nothing would have passed the whole suite.

The GUI tests cover the part of a window that can be wrong without looking
wrong: which lobby a right-click acts on after the sidebar has been rebuilt and
reordered underneath it. The root is withdrawn before it can map, so nothing
appears and no focus is taken; a machine without Tk exits 2 for "skipped".

## NAT

The host attempts a UPnP mapping first. This service only publishes the
resulting address and port. For symmetric NAT and CGNAT, `services/turn-relay`
is the fallback.
