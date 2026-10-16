# The netplay layer

The PC networking under the port, and all of it provable without the ROM: every
file here is guest-free except the binding, and each has a test that runs with
no game and no window.

| file | what it is |
| --- | --- |
| `lsw2_lockstep.*` | the session: frame-aligned exchange of fixed blocks, delay buffer, join and loss |
| `lsw2_tcp.*` | a transport for it: star topology, fixed-size records, plus the lobby's control records (roster, seed, Extras, customization, relayed netgame messages, host-level WH control) |
| `lsw2_vwh.*` | the host-level WH: the game's WM data sharing supplied by PC networking, with a measured pipeline depth |
| `lsw2_net_bind.*` | where the guest meets a session — the one place the overrides and the sockets see each other |
| `lsw2_wireless_relay.*` | the DS radio carried across a network: the framework's loopback-UDP radio forwarded over a TCP link |
| `lsw2_relay_session.*` | that relay stood up inside the fork, so no external bridge process is needed |
| `lsw2_master.*` | the client for `services/master-server-selfhost` (lobby list, heartbeats, operator kicks) |
| `lsw2_lan.*` | LAN discovery by UDP broadcast, behind the host menu's "Visible to LAN" row |
| `lsw2_upnp.*`, `lsw2_upnp_parse.cpp` | automatic port forwarding (SSDP, the gateway's description, one SOAP AddPortMapping) |
| `lsw2_ping.*` | round-trip time to a lobby host, measured as a TCP connect to its listen port |
| `lsw2_name.*` | player names made safe to draw and safe to send (a printable-ASCII allow-list) |
| `test_*.cpp` | one test per piece — e.g. `test_lockstep.cpp` runs four stations in one process over an in-memory bus, `test_tcp.cpp` a host and three joiners over real loopback sockets |

## Why the shape is not a choice

LSW2's own netplay is this one call:

```
WM_StartDataSharing(info, port=13, aidBitmap=0xF, dataLength=124, ...)
```

Every station contributes **124 bytes** each frame and receives every station's
block before the frame advances. That is deterministic lockstep, in the shipped
game. The port does not get to pick a netcode model - it has to provide this
one, and `kLsw2ShareBytes` and `kLsw2MaxStations` are the game's numbers rather
than decisions made here.

## The three rules worth knowing

**Send before checking whether you can advance.** A station stalled waiting for
a peer must still be feeding that peer, or two stations each waiting for the
other deadlock. That is the classic way a lockstep session dies, and the test
reproduced it by accident when it stepped only one station.

**Stall rather than advance.** A missing block is not something to paper over:
running the guest without it produces exactly the divergence the game's own
checksum exists to catch. `step()` returns false and the caller must not
advance.

**Fixed-size records mean no framing bugs.** 132 bytes, always - 8 header plus
124 payload - because DataSharing's length genuinely never varies. No partial
header parsing, no length field that can disagree with reality, no
attacker-chosen allocation. Read 132 bytes, you have a message.

## Star, not mesh

The host relays. Four stations meshed would be six connections and six NAT
problems; a star is three, and multiplayer here is host-and-joiners anyway -
people join someone's lobby.

The cost, stated plainly: the host's upstream carries three copies of every
frame - about 30 KB/s, which is nothing - and a client's latency to another
client is two hops. That second hop is why the lockstep delay is measured
rather than assumed.

## Two network paths

The fork reaches other machines two ways, and `lsw2_net_bind.h` says which one
is live:

- **The host-level WH** (`lsw2_vwh.*`): the game's wireless manager is answered
  by the PC session instead of the emulated radio. On in the fork
  (`LSW2_FORK=1`), off in the baseline; `LSW2_VWH=0/1` overrides either.
- **The radio relay** (`lsw2_wireless_relay.*`, `lsw2_relay_session.*`): the
  cartridge's own wireless, unmodified, with the framework's loopback radio
  frames carried over TCP. Environment-driven (`LSW2_RELAY=host|connect` and
  friends, which `port/launch.sh --host` / `--join` set); absent the
  environment it binds nothing.

## Build and run the tests

`tools/run_unit_tests.sh` builds and runs every test here (and `port/build.sh`
runs that suite at the end of a build). By hand, for example:

```
g++ -std=c++20 -I. test_lockstep.cpp lsw2_lockstep.cpp lsw2_tcp.cpp -o test_lockstep -lws2_32
g++ -std=c++20 -I. test_tcp.cpp lsw2_tcp.cpp lsw2_lockstep.cpp -o test_tcp -lws2_32
```

The in-memory test proves the **algorithm**; the loopback test proves the
**wire**. Different things break in each - framing, partial reads, seat
assignment and a peer closing are all invisible to a bus that hands whole
messages around.

## Keeping the guest out

Nothing here except `lsw2_net_bind.*` knows about the guest, the ARM9, or
`0x0210B520`. The dispatch overrides in `port/frontend/` that connect the game's
wireless calls to a session know nothing about sockets, and the binding between
them is a few functions wide on purpose.

Keeping the two apart is deliberate: it is what lets this half be finished and
tested with no ROM and no window, independently of the guest-side work.
