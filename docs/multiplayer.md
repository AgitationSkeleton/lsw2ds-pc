# Multiplayer: the game's wireless play, and the port's replacement

LEGO Star Wars II on the DS supports up to four consoles over local wireless:
co-op through the story and Free Play, and the Bounty Hunter arena
(deathmatch and capture the Carbonite). This document explains how the
cartridge's multiplayer works, from the radio layer up to the lobby and the
match, and then how the PC port replaces the radio with TCP/IP networking
while leaving the game's own multiplayer code in charge.

Every address is a guest address in the USA cartridge
(SHA-1 `7c1afa070797945ba5a2577c7ddfded5cf2fc443`). Claims are marked
by how they are known: *read from the code* (disassembly), *measured* (a live
run of the port), or *inferred*. The game's state machine, the RNG and the
actors are covered in [game-internals.md](game-internals.md), and the
recompiler, overrides and guest calls in [the-port.md](the-port.md).

## The shape in one paragraph

The game is a **deterministic lockstep** design. Every frame, each console
contributes one fixed 124-byte block, and no console advances until it holds
every console's block for that frame. On top of that exchange sits a small
message protocol (70 message types) that carries every decision: a pick, a
round start, a pause, a seed. Every console runs the full simulation, and each
block carries a checksum of the shared state. When two consoles' checksums
disagree, the game declares the session broken and shows **Communication
Error**. The port keeps all of this. It replaces only the bottom layer, the
part that talks to the radio.

## The layers

```
 game: lobby manager, netgame tick, message pump/dispatcher, match rules
   |      (LSW2's own code)
 WH   : Nintendo's wireless helper, a C++ object with a 26-slot vtable
   |      context 0x0210B320, object *(0x0210B348), vtable 0x020FFD50
 WM   : the NitroSDK wireless manager, 0x020D6xxx-0x020D8xxx
   |
 ARM7 wireless firmware and the radio
```

### WH is a C++ object

The game reaches the wireless helper almost entirely through one object. Its
constructor (`0x020AD354`) stores vtable `0x020FFD50` at offset 0 and the object
pointer at `WHCTX+0x28` = `0x0210B348`. Every method then ignores `this` and
loads `WHCTX` (`0x0210B320`) itself. All 26 methods are called **virtually**, so
a scan for direct `BL` callers finds none. That once made the module look like
dead code. The only direct call from game code into WH is
`WH_GetSystemState` (`0x020AD310`), which returns `WHCTX+0x20`. *Read from the
code; the call sequence was then confirmed with a live trace.*

| slot | address | role |
|---:|---|---|
| 0 | `0x020AD438` | WH_Initialize: buffer sizes (send 0x220, receive 0xC80), starts WM |
| 3 | `0x020AD970` | parent connect (mode 4 = DataSharing parent) |
| 4 | `0x020ADBCC` | child connect (accepts only modes 1, 3, 5; LSW2 joins as 5) |
| 6 | `0x020AD7F0` | WH_StepDataSharing(send), the per-frame exchange |
| 9 | `0x020AD884` | GetSharedDataAddress(aid), a tail call to `0x020D860C` |
| 10 / 11 | `0x020AD8CC` / `0x020ADC78` | connected bitmap (+0x0E) / own AID (+0x08) |
| 19 | `0x020ADC88` | WM_SetEntry(flag): opens or closes the room to joiners |
| 20 | `0x020ADD78` | WH_End |
| 22 / 23 | | host table at `0x0210BF40` (16 records of 0xC4: WMBssDesc + TTL) / host count |

The rest are finalize, reset, kick (WM_Disconnect), end data sharing, the
last-error and GGID accessors, channel measurement, scan start/stop, and the
beacon's user info.

The state setter `0x020AD7A4` (not a vtable slot) stores to `WHCTX+0x20` and
logs names from the table at `0x020FFD20`: 0 STOP, 1 IDLE, 2 SCANNING, 3 BUSY,
4 CONNECTED, 5 DATASHARING, 7 MEASURECHANNEL, 9 ERROR. The error recorder
`0x020AD8B4` writes the WM code to `+0x30` unless the state is already ERROR,
and does nothing else.

### The step signal

`WHCTX+0x48` records which WH operation last completed. Every WM completion
callback stamps its own constant there (about thirty of them), and WH's wait
helper `0x020ADDF0` spins until the expected id appears or an error is
recorded, for up to `WHCTX+0x14` iterations (the game sets 20000 through slot
18). So a console that sits in BUSY can be read as a sequence: which operation
was issued, and which completion never came back. On the radio route this
field showed that a host issuing "end parent" (step 0x10) never received the
completion (step 0x11). *Read from the code, used live.*

### Parent, child and entry

The parent and child connect methods each store the connect mode at
`WHCTX+0x44` and a `doubleMode` byte at `WHCTX+0x00`. The parent callback
handler starts data sharing on mode 4, and the child handler on mode 5. The
child method rejects any mode except 1, 3 and 5. **LSW2 hosts as mode 4 and joins
as mode 5.** The defaults method (slot 21) sets GGID `0x00400141`, the entry
flag, and a beacon period of 200.

The game installs no accept judge: `WH_Initialize` stores NULL at `+0x10`, and
nothing else writes it, so a parent accepts every child that associates. Who
actually gets *seated* is decided higher up, by the lobby manager's Yes/No
prompt (below). On the radio route a live read of the host's parent
parameters found room for seven children, yet a third and fourth console still
could not associate after the first child joined. That is a radio-route
problem, and the host-level WH described later does not have it.

## Data sharing: the per-frame exchange

Both `WM_StartDataSharing` call sites (`0x020AE41C`, `0x020AEC3C`) pass the same
arguments:

```
WM_StartDataSharing(info 0x0210B720, port 13, aidBitmap 0x0F, dataLength 124, doubleMode)
```

Every station contributes **124 bytes per exchange**, and up to four stations
take part. The send buffer the caller flushes is 544 bytes (`WHCTX+0x3C`), but
only the first 124 are transmitted. The receive `WMDataSet` at `0x0210B520` is
referenced from exactly two places in the whole image: slot 6 (step) and slot
9 (get address). The game never reads it by address. It asks slot 9 for a
pointer to each station's block. *Read from the code.*

### The netgame's exchange loop

`FUN_020AF594` is the game's side of the exchange. The send block is at
netgame object +0x40, the receive slots at +0x240 + aid*0x70, and the
per-station present flags at +0x6B0 + aid:

```
if WH_GetSystemState() != DATASHARING: mark every station absent; return
if !StepDS(send):
    loop: if state != DATASHARING -> return 0
          if last error == ILLEGAL_STATE (3) -> return 0
          wait one VBlank
          if StepDS(send) -> break
for aid 0..3: p = GetSharedDataAddress(aid)
    p ? copy 0x70 bytes into the station's record, present = 1 : present = 0
```

"No dataset yet" (WM code 5) is not an error to the game. It waits a VBlank and
tries again, which makes the exchange itself the lockstep barrier. WM's own
contract couples a console's steps: the first call primes one send and returns
NO_DATASET, and after that each dataset received releases exactly one more
send. Every console receives an identical dataset.

The game's frame loop (`0x020013A4`) waits for two VBlanks and then runs the
netgame tick, so the simulation and the exchange both run at 30 Hz.

### What a block carries

The packer `0x020B0BDC` / `0x020B0C00` fills the local block each frame:

| offset | size | contents | how known |
|---|---|---|---|
| `+0x60` | 4 | sum of `actor+0xB0`, `+0xB4`, `+0xB8` (position X, Y, Z) over every party actor | read from the code (`0x020AD21C`) |
| `+0x66` | 2 | connected-station bitmap by AID, from WH slot 10 | read from the code |
| `+0x68` | 2 | the RNG byte at `0x02104344`, widened | read from the code |
| first 0x70 bytes | | copied into the station's view record after each exchange (`0x020AF688`) | read from the code |
| the rest of `0x00`-`0x5F` | | not fully mapped | the input sampler reads each station's pad word from that station's record, so input travels here; the message traffic must also cross in the blocks, since the host-level WH carries nothing else and messages arrive (inferred) |

The station **count** follows the bitmap, not per-frame presence. A block
missing for one frame does not change how many players the game believes
there are.

Two small helpers read the bitmap: `0x020AD1C8` (lowest set bit, "which
station") and `0x020AD1F0` (popcount, "how many").

## The desync check and the Communication Error

**Communication Error is a consistency check, not a transport failure.** This
is the most important fact about the game's netplay. It took a long time to
see, because the message names the radio.

The view manager's step (`FUN_020AF330`, view-manager vtable slot 3) has three
outcomes:

```c
FUN_020af42c(vm);
if (FUN_020af594(vm, ...)) {                 // the exchange ran
    if (check_armed && FUN_020af460(...) == 0)
        return 2;                            // the cross-player check failed
    return 0;                                // healthy
}
return 1;                                    // the exchange did not run
```

The result is stored at **`0x021092FC`**. The netgame tick `FUN_020B07E8` reads
it, and when it is non-zero the tick tears the netgame down (`0x02072CD0`),
requests world state 15 and then screen state 38, the Communication Error
screen. *Measured:* in every comm-error failure examined, the word read 2 on
both consoles. Each console had independently decided the other disagreed.

`FUN_020AF460` takes each member's reported `{frame number, checksum}` and
looks up its **own** checksum for that frame in an **eight-entry history**.
`FUN_020AD2A8` then compares the position sums for every player and the RNG
half-word. Two consequences matter for any replacement transport:

- Two consoles may drift at most seven frames apart before a perfectly
  consistent game reads as a mismatch, because the history no longer holds
  the frame being compared.
- Anything that changes the simulation on one console at a different step from
  another (a position, a character swap, an extra RNG draw) is caught within
  a frame or two.

The check is armed and disarmed around transitions. Loading a level arms it
(`FUN_0208C888`). The sites that seed the RNG arm it in the same breath, and
the sites that draw for a transition disarm it. A seed message in flight is
therefore not a false alarm.

### Other state-38 sites

The cartridge has nine sites that request state 38. Two besides the netgame
tick matter in practice:

| function | what it compares |
|---|---|
| `FUN_0206EF0C` | a mask it stored at `+0x92` against the live view mask (record `+0x66`). It reads the view manager pointer without a null check, so after a session teardown it calls through a null vtable. The port guards it with the same `if (view manager == 0) return` that the netgame tick uses. |
| the match's player-set guard, `FUN_0209B88C` on the match object `[0x02107860]` | host only. On the first match frame it samples view record 0's player mask into `+0xB2`, then ends the session if the mask ever differs. Only a session Finalize resets it (`0x0209B7CC` via `0x02072BD4`). |

When a session breaks, read `0x021092FC` first (1: the exchange stopped, 2: a
desync), then the WH state and step signal.

## The RNG's role

The game's generator is **MT19937** (`0x02062D10`, with the tempering
constants `0x9D2C5680` and `0xEFC60000`). Its state block is at `0x02104344`.
`0x02062AE4` is `rand_range(n)`, with 45 callers. `0x02062A40` re-seeds the
generator. At boot `0x020629C4` seeds it from the real-time clock. Two real
consoles are therefore guaranteed to start with different streams. *Read from
the code.*

The game's own fix is a **seed broadcast**. When a round starts, one console
draws a number and sends it as message 0x0A. Every console, the sender
included once its own message comes back through the pump, calls
`0x02062A40(seed)` and then acts on the tag in the message
(`FUN_02072CF4`). From that instant all generators are identical. The draw
itself puts the consoles apart for a moment, which is why the check is
disarmed until the message is handled.

A port therefore must not seed the RNG itself during a session, and must not
add draws on one console. The RNG byte is part of every block's checksum, so
an extra draw is a desync.

## The message protocol

All game-level traffic goes through two functions:

- **Send**: `FUN_020B04F0(owner 0x0210794C, type, payload, len)`, called from 73
  sites.
- **Receive**: the pump `FUN_020B0410`, called each frame from the state
  manager's tick at `0x0205EE1C`. It runs only while the byte
  `[0x021027F8 + 0x29]` (the station popcount) is above 1. For each station
  it walks a list of 4-byte-aligned entries at
  `0x0210794C + 0xA3C + station*0x344`:

  | byte | meaning |
  |---|---|
  | 0 | size in words, and the step to the next entry; 0 ends the list |
  | 1 | message type; 0 means skip |
  | 2-3 | sequence number |
  | 4.. | payload |

  An entry is dispatched only when its sequence differs from the last one
  handled for that slot (`owner + station*10 + 0x40 + slot`). The sequence is
  compared as a halfword but stored as a byte, so a sequence above 255 would
  be re-dispatched every frame. This has not been seen to happen.

- **Dispatch**: `FUN_020B1248` reads the type at `msg+1`, compares it with 0x45,
  and jumps through a table at `0x020B1268`. There are 70 types. Six of them
  (0x00, 0x01, 0x26-0x29) land on the do-nothing arm `0x020B2F58`, which
  leaves 64 real ones. Most arms first check which state is running, through
  `[[0x02102900] + 4] + 4`.

`config/net_messages.json` (from `tools/export_net_messages.py`) lists every
type with its handler and the functions its first block calls. The ones whose
meaning is established:

| type | meaning | sender / arm |
|---|---|---|
| 0x03 | the statue select is complete: `{u32 counter, u16 random}`; fills each player's character record, then asks for the arena (world: kind 4 with the level; screen: 42) | `0x0207372C`, `0x020998C4` |
| 0x07 | a player's Free Play character switch | `0x02068A18` |
| 0x09 | a statue pick `{u8 slot, s8 index}` | `0x020996C4` |
| 0x0A | seed + selector: re-seed the RNG, then `FUN_02072CF4(tag)`. Tag 0 = everyone reloads the lobby after a join; 1 = Game Settings; 3 = the statue select | six senders, including `0x020B0E8C` (the lobby manager) |
| 0x0D | requests the arena pair directly (world kind 4 mode 0xF, screen 42) | `0x0209CBCC` |
| 0x12 | the match clock has run out | `0x0209BABC` |
| 0x13 | the new station mask and player count (calls the count setter `0x02073054`). One run also saw its arm request the score screen (44) and its world (43); the conditions for that have not been read | four senders, including the lobby manager |
| 0x15 / 0x2D | the pause menu's wireless row (host / exit) | `0x0208EB10` / `0x0208EB6C` |
| 0x1C, 0x1D, 0x1E, 0x34 | dress a whole actor (calls `0x020029C4`) | |
| 0x2B / 0x2C / 0x42 | pause opened / resumed / quit confirmed | `0x0208CEF8` / `0x0208EADC` / `0x0208EF70` |
| 0x2E / 0x2F | set one customize slot (hat) / the weapon | `0x0207B240` / `0x0207B2C8` |
| 0x35 | the 60 s and 30 s time-limit warnings | `0x0209BA68` |
| 0x36 | toggle one Extra (the arm runs the full toggle `0x02086810`) | `0x02085270` |
| 0x39 | set a per-station byte in a table, for example the camera mode at `0x02109C88 + 0x208` | `0x020A99BC` |
| 0x44 / 0x45 | save / restore that table entry | `0x0204A938` / `0x0204B29C` |

The meaning of the remaining types is not established. Their arms are known,
but their purpose is not.

### The multi-view bit

Bit `0x40` of the session word at `0x027E02A8` is set once a lobby has seated a
second player. While it is set, the game's per-player actions go out as
messages instead of acting locally: customize, Extras, pause, camera mode, and
leaving. The setter `0x020A99BC`, for example, broadcasts message 0x39 when the
bit is up, and writes the byte directly when it is not. Every console then
applies the change at the same exchange. This one branch is what makes the
game deterministic under lockstep.

## The lobby and match flow

Screens are states in the game's two-slot state manager (`0x02102900`; slot 0
is the *world*, slot 1 the *screen*; see [game-internals.md](game-internals.md)).
The numbers below are screen-slot states, as measured on the radio route and
on the host-level WH:

| state | screen |
|---:|---|
| 15 | front end (world kind 0x11 is Quit Game's request) |
| 34 | the joiner's "Browsing Wireless Games" list |
| 35 | the host's join prompt: "*name* is trying to connect. Do you want to let them join your game?" |
| 38 | Communication Error |
| 39 | the joiner's "Waiting for Host" |
| 40 | the loading screen a join passes through (everyone reloads the lobby) |
| 41 | the hunter statue select (about 30 s) |
| 42 | the match itself |
| 44 | the score screen ("Continue", "Select Character", "Settings", "Quit"); its world is 43 |
| 48 | the arena lobby's screen, over level 9 (the Mos Eisley Cantina) |
| 53 | the pause menu |
| 54 | Game Settings (map, game type, time) |

The world slot carries the level and a load mode. CoopEnter (0x0E) re-enters
the lobby keeping the party records. Walkaround (0x10) is the plain lobby.

### Hosting and joining

**Host** (traced live): `FUN_020B10A8` ("start a wireless session") calls
`FUN_0207281C`. That function creates the WH manager and, at its tail, allocates
the 0x704-byte view manager (constructor `0x020AF214`), whose pointer goes to
`0x021092F4`. It then runs WH slots 18, 12 (GGID), 15 (measure channel) and
19 (`SetEntry(1)`), spins until the measurement finishes, then runs slot 25
(beacon info), slot 14, and slot 3 (parent connect, mode 4). The console is in
DATASHARING by the time the lobby (48) has loaded. Each frame after that it
calls slots 6, 9 ×4, 10, 11 and 24.

**Joiner**: `FUN_02072AD8` tears its own WH down (slots 8, 1, 2, 20) and opens
the browse list (34). That runs slot 16 (scan), polls slots 22/23 until a host
appears, then slot 17 and slot 4 (child connect, mode 5, with the host's table
record). The screens go 39, then 40, then 48.

### Seating: the lobby manager

The host's lobby manager is `FUN_020B0CD4`, whose state block is
`*(0x02106D90 + 0xBB8)`. It acts only on the host. *Read from the code,
confirmed live:*

```
bitmap changed, +0x10 == 0  -> +0x10 = the new AID; recount; message 0x13
bitmap changed, +0x10 != 0  -> RESET: reload, end the session, start a new one
+0x10 set, lobby idle       -> request state 35 (the Yes/No prompt), SetEntry(0)
answer YES (+0x0D = 1)      -> draw a seed, message 0x0A tag 0, +0x08 = 1:
                               every console reloads the lobby with the new count
```

Before the reload, the game snapshots each party member into a per-station
record (`FUN_02088B84` → `FUN_02089388`). The CoopEnter load then rebuilds
each actor from its record. In the retail game the manager takes **one**
joiner per session. A join always ends in a reload that rebuilds the manager,
so `+0x10` is never cleared, and a second change to the bitmap would take the
reset path.

**AIDs are stations.** The host is AID 0. The bitmap at block `+0x66` is
indexed by AID. The party array `0x0210930C + 0x4C` and the per-station records
are indexed by the same number. This console's own station is the byte at
`0x021027F8 + 0x2A`. The player count lives at `0x020FEB88`, written by
`0x02073054`, which also reconfigures the view manager. A consequence that
caught early port code: "party slot 0" is the **host** on every console. It is
not the local player.

### From the lobby to the match and back

1. The host opens Game Settings (54) and confirms. That broadcasts message
   0x0A. `FUN_02072CF4` mode 3 asks for the world `(4, slot 0, Walkaround,
   level 9)` at `0x02072DC4` and the screen 41 at `0x02072DE0`.
2. In the statue select, each pick is message 9. When everyone has chosen,
   message 3 stores each player's character into the per-station records
   (u16 at `0x02106FD8 + i*0x40`, gate byte at `+0x1B`, through the hunter
   table at `0x020FFA00`) and asks for the arena. The arena build
   (`0x020897F8`) reads those records once.
3. During the match (42), the clock warnings are message 0x35, and time up is
   message 0x12. A respawn does not consult the records: it rebuilds the dead
   actor's own character through `0x02091B88`.
4. The score screen (44) waits for a button indefinitely. The retail game has
   no timed return to the lobby.

## The port's replacement: a host-level WH

The port can run the cartridge's own radio code: the framework emulates the
DS wireless hardware, and a relay can carry its frames between machines (below).
That route proved the game's flow end to end. It also showed why it cannot be
the default:

- every multiplayer exchange blocks the host's frame on each child's reply,
  so the host is bound by the round trip and internet play is slow;
- the third and fourth children did not associate.

So the Bounty Hunter build answers the WH object itself. `port/frontend/vwh.cpp`
overrides the WH methods that reach into WM: slots 0, 1, 2, 3, 4, 5, 6, 8, 9, 15,
16, 17, 19, 20 and 24. It keeps the fields the rest of the cartridge reads
coherent in guest memory: state `+0x20`, error `+0x30`, bitmap `+0x0E`, AID
`+0x08`, channel `+0x02`, and the host table. The pure accessors (7, 10, 11, 12,
13, 14, 18, 21, 22, 23, 25) still run as the cartridge's own code. Above this
layer nothing is replaced: the join prompt, the lobby manager, the netgame,
Game Settings, the statue select and the match all run on datasets exactly as
they do over the radio.

It is on by default in `lsw2_bountyhunter.exe` (`LSW2_VWH=0` turns it off) and
never in the base build, whose co-op keeps the emulated radio. Details of the
WH contract matter, and each of these was found by breaking it: `WH_Initialize`
while WM is up must fail and leave the state alone; returning 0 with error 5
is how a console waits; and a console gets at most one dataset per VBlank
(WM's one cycle per frame), keyed to the VBlank rather than scanline 0 because
the game exchanges right after its VBlank wait.

### Deterministic lockstep over TCP

`port/net/lsw2_vwh.*` is the session, and it has no guest code at all. It
reproduces WM data sharing with a pipeline of depth **L**:

- Each console sends its block for slot `cursor + L`. Its first step primes L+1
  slots, as WM's first call primes one.
- The **host commits** dataset *r* once every member that has started sharing
  has its block for *r* in. It sends `COMMIT(r, member bitmap)` and consumes
  the dataset itself. A joiner counts from its first block. It is in the
  bitmap from admission, so the game's join prompt sees it, but nobody waits on
  it until it shares.
- The host relays every block as it arrives. TCP keeps one connection's order,
  so a client always holds a dataset's blocks before its COMMIT.
- A client consumes dataset *r* only when `COMMIT(r)` is in. Until then it
  returns "no dataset" and the game waits a frame.
- A member silent for 600 game calls (about 10 s) is dropped, as WM's timeout
  would drop it.

**Why the host waits.** An earlier design committed "whatever had arrived" and
never waited. It failed the game's own check at every match start: a member
late by a few datasets was compared against frames the eight-entry history no
longer held. *Measured.* WM's data sharing is a lockstep, and the replacement
has to be one too.

When a block is missing, the port also holds emulation in real time (up to
250 ms) rather than answering "no dataset" straight away. Emulated time then
stays identical on every console, which is what real hardware's VBlank sync
gives. A client that falls behind runs frames unthrottled until its backlog of
committed datasets drops back.

**The depth.** L = 2 (about 33 ms) by default. The host pings every connected
station four times a second, takes the median of the last eight echoes, and
derives the depth a link needs: one dataset per 33.3 ms of round trip plus one
for jitter, clamped to 2..6. Every console **must run the same depth**: a
joiner at depth 6 beside a host at 2 still reported its frame 0 after the host
began checking, and the match ended in state 38. So before seating a joiner
whose link needs more, the host raises the whole session's depth through an
effect (below), and seats the joiner only after the raise has landed. The cap
of 6 comes from the game, not the network: depth must stay under the
eight-entry history. *Measured:* two and three consoles at 102-148 ms round
trips held 59-61 fps at depth 5-6, where depth 2 gave about 48 fps.

### The lockstep rule

> While a session with two or more members is sharing datasets, the port must
> not change simulation state from its between-frames tick.

Each console's tick runs at a different point in the game's frame, so a write
made there lands at a different simulation step on each machine, and the
checksum catches it a second later. This was measured, not assumed. A roster
sync that rewrote a peer's pending-character byte from the tick swapped actors
two steps apart on host and joiner, and the RNG streams then parted because
different behaviour handlers ran.

`lsw2_vwh_lockstep()` is the gate. Fifteen mutators return early under it. A
change the port must make during a session goes one of two ways:

1. **The game's own replicated path.** Send the message the game itself would
   send, and let every console apply it at the same exchange: a hat is
   message 0x2E, an Extra 0x36, the camera mode 0x39.
2. **An effect at the dataset boundary.** The COMMIT record has 124 spare
   bytes. The host stamps effect records on it (`[type][len][bytes]`, whole
   records, in post order). Every console, the host included, gets the same
   bytes back with that dataset and applies them in `vwh_step_ds` before the
   game sees the dataset.

| effect | what every console does at that dataset |
|---|---|
| `kLsw2EffectLobbyReturn` | write the lobby pair (world CoopEnter level 9, screen 48) into both state-manager slots exactly as `0x0205EC4C` does |
| `kLsw2EffectDepth` | raise the pipeline depth |
| `kLsw2EffectRoster` | the round's roster rule, salt and characters, written into the per-station records before the arena build reads them |
| `kLsw2EffectPickEnd` | end the Free Play pre-game pick and ask for the round start it replaced |
| test-only kill / teleport / hit | let an automated run reach a death, a spot or a hit without steering |

Visual-only changes (HUD, portraits, popups) are allowed from the tick,
because each console's picture is its own anyway.

### What the TCP transport carries

`port/net/lsw2_tcp.*` is a star: clients connect to the host, and the host
relays. A mesh of four would need six connections, each its own NAT problem. Every
record is exactly **132 bytes**: `u32 frame | u8 station | u8 flags | u16 |
124-byte block`. The size never varies, so there is no length field to get
wrong, and a per-peer send queue means a full socket buffer never splits a
record. Bits of the flags byte mark out-of-band records with no block: seat
assignment, host settings, player names (the host stamps the true station
when it relays), customization parts, and the host-level WH's control records
START (first slot, AID, depth), COMMIT (index, bitmap, effects) and PING. A
few other record kinds (seed, match start, relayed messages) served earlier
routes. On the host-level WH the game's own seed and messages travel in the
shared blocks. `port/net/test_vwh.cpp`, `test_lockstep.cpp` and `test_tcp.cpp`
run four stations in one process or over loopback, with no ROM.

### Seating more than one joiner

Seating goes **one joiner at a time, and only in the lobby**, because the
retail lobby manager handles exactly one join per session. A connected station
waits in the host's queue until the host has been in state 48 for a second
with no join under way and the station's round trip has been measured. Then
it is seated (START). The game's own sequence runs from there: the bitmap
grows, the Yes/No prompt appears, message 0x0A tag 0 goes out, and every
console reloads. The port answers the prompt with Yes. For the second and later
joins the prompt cannot open while the multi-view bit is set, so the port
writes the answer byte (`+0x0D = 1`) and the manager runs its own Yes path.

At the dataset boundary where the bitmap grows, the manager's pending fields
(`+0x08`, `+0x0D`, `+0x10`) are cleared, so the next join is the first join
over again. When a seating completes, `+0x10` is cleared again. Without that,
any later departure would take the manager's reset path and end everyone's
session. *Measured:* with this, four consoles reach the arena at 59-60 fps.

The cartridge's own join screens (34, 35, 39) are still part of the handshake.
The Bounty Hunter build leaves their text layer out of the touch-screen
picture (presentation only), so a player sees the loading backdrop instead.

### Join in progress and disconnects

- **Joining mid-match**: the station connects, waits, and is seated at the
  next lobby return. Nobody is seated during a match or during the Free Play
  pre-game pick, which *is* the round start. The lobby countdown treats a
  seating as a hold rather than a reset, so a late joiner never stalls the next
  round.
- **The player-set guard**: its sampled mask outlives the round, so a round
  with a different set of players would raise state 38. When the host's block
  carries a mask different from view record 0's, every console clears the
  guard's `+0xB2` at the dataset boundary and the guard samples the new set
  (state updates run before the netgame tick, `0x02001430`, so this is safe).
- **A player who leaves** drops out of the next commit. Message 0x13 carries
  the new mask and the round goes on. Every console sees the departure in the
  consumed membership at the same step and shows a "has disconnected" popup,
  which the port draws itself: in a networked arena the game's own message box
  reports busy for the whole match.
- **Pause and Disconnect**: in the retail game one player's Start (message
  0x2B) pauses every console, and a confirmed quit (0x42) ends the session for
  everyone. The port overrides the message dispatcher `0x020B1248` (one caller,
  the pump) so the pause opens only on the sender's console, nothing freezes,
  and the paused player's input is zeroed identically everywhere. A client's
  quit counts as that player leaving; the host's Shut Down Lobby still ends
  the lobby for all. A match has no Start pause at all: `FUN_02093BD8` allows
  one only in states 48 and 53.
- **A host that vanishes**: the client's netgame tick takes the game's own
  reaction (world 15, screen 38). From that screen, with the wireless down,
  the port returns the client to its offline lobby.
- **A console that crashes**: a dispatch miss halts a CPU while the window and
  sockets live on. A halted console closes its sockets at once, and a halted
  host also withdraws its directory listing, LAN beacon and UPnP mapping.

### Rounds, the score screen and the lobby return

The score screen's clock is per console, so only the host's 15-second timer
counts. It posts the lobby-return effect, and every console writes the game's
own request pair at one dataset, into each slot's request block and with no
status byte, so the manager fades the slot and runs the old state's exit and
destructor before building the lobby. (Forcing the status byte skipped the
arena's teardown and left freed sprite sources behind.) The retail game
snapshots the party before every CoopEnter; the port cannot, because the
snapshot would store arena positions, so it refills each gated record's
hearts byte to 4, the value the player constructor uses. Without that the
lobby restores 255 hearts and never draws them.

## The other path: the radio over a network

The base build's co-op keeps the cartridge's wireless unmodified. The
framework's emulated radio exchanges frames as loopback UDP;
`port/net/lsw2_wireless_relay.*` forwards them over one TCP link (game port
+ 1), and `lsw2_relay_session.*` pumps that relay on its own thread, because a
child's admission blocks the frame loop while it waits for the host. Framework
patches make this work: bridge frames skip the RF channel match, waits are
event-driven, and `LSW2_ITERCAP=128` shifts the CPU interleave so a joiner's
WM-enable race goes its way. The base build's lobby list is the cartridge's
own browse screen: the port writes directory and LAN lobbies into the host
table at `0x0210BF40` and replaces the child connect (`0x020ADBCC`). The chosen
row is `(record - 0x0210BF40) / 0xC4`.

## Finding a game

### The master server

`services/master-server/` is the public directory, a Cloudflare Worker over
D1. `services/master-server-selfhost/` is the same protocol as one Python
process with a command-line and a GUI front end (default port 28553). The
client, `port/net/lsw2_master.cpp`, speaks plain HTTP/1.1 with no TLS, so the
host name must keep answering plain HTTP. Game traffic never passes through
the directory.

| request | purpose |
|---|---|
| `GET /v1/health` | `{ok, service, ttl}` |
| `GET /v1/servers?kind=coop\|bounty` | live lobbies of one kind |
| `POST /v1/servers` | register `{name, port, players, kind, roster, state, config}` → `{id, token, code, expires_in}` |
| `PUT /v1/servers/<id>` | heartbeat with `X-Token`: players, roster, `state` (`lobby`/`match`), `config` |
| `DELETE /v1/servers/<id>` | disband |
| `GET /v1/codes/<code>` | resolve a room code |

The token is returned once and stored only as a SHA-256 hash. Hosts heartbeat
every 5 s, and a lobby expires 20 s after its last heartbeat. The directory
publishes player names, never player addresses. An operator's kick rides the
host's next heartbeat reply, since the directory has no connection to any
player. Optional extras: Discord event posts (webhooks held as Worker secrets)
and short-lived TURN credentials (`services/turn-relay`).

### Room codes

A room code is four characters of Crockford base 32 without I, L, O or U
(about a million codes), drawn at random and checked against the live
lobbies. On lookup, input is upper-cased, I and L fold to 1, and O folds to 0,
so a misread code still resolves. The Worker and the self-host server must
normalise identically. A code is a convenience for friends, not access
control. Only the token authorises changes to a lobby.

### LAN

`port/net/lsw2_lan.*` broadcasts one small fixed-size UDP datagram a second
on port 28554 (magic `LSW2` plus a format byte) while the host menu's
"Visible to LAN" row is on. Clients only listen. Nothing is authenticated: an
announcement is an invitation to try a connection. Every field is
bounds-checked, and the name passes through the printable-ASCII filter
(`lsw2_name.*`). LAN lobbies are merged into the same list as directory
lobbies. The base build's list shows them first, in green.

### UPnP and the listen port

A host listens on **TCP 28550** by default. `port/net/lsw2_upnp.*` makes three
requests: an SSDP M-SEARCH to find the gateway, a GET of its description XML,
and one SOAP `AddPortMapping`. It needs no library. Every failure is soft. A
host behind carrier-grade NAT, or with UPnP off, still hosts for the LAN and
for anyone who can reach it directly, and the log says why the mapping
failed. `lsw2_ping.*` measures a lobby's ping as a TCP connect to its listen
port, so the number is the distance to the host, not to the directory. A
lobby only reachable through the relay shows no ping.

### The relay

Many hosts can't accept connections at all: a VPN, a router without UPnP,
carrier-grade NAT. For those, the master server carries the connection
(`services/master-server/src/relay.js`): a Cloudflare Durable Object room
per lobby, reached by WebSocket over the same plain HTTP the lobby list uses.

- **Host.** A listed host keeps one connection to its room, authenticated with
  its registration token (`Lsw2RelayHost` in `port/net/lsw2_wsrelay.*`). Each
  joiner the room announces becomes a loopback connection to the host's own
  listen port, which the transport accepts like any other.
- **Joiner.** `join_address` tries the host directly (1.5 s). For a lobby from
  the list or a room code, it then opens the lobby's room (`Lsw2RelayJoin`)
  and joins a loopback port the bridge carries.
- **The wire.** Host messages are `[kind][peer][bytes]` (0 data, 1 open,
  2 close); a joiner's are the bytes alone. The room never parses the game's
  stream, so the lockstep and every record are what a direct connection runs.

`LSW2_TEST_FORCE_RELAY=1` skips the direct attempt, so the relay can be
exercised on one machine.

## Where to look in the source

| file | what it holds |
|---|---|
| `port/frontend/vwh.cpp`, `vwh.h` | the WH overrides, the dataset boundary, seating, effects, the lockstep gate |
| `port/net/lsw2_vwh.*`, `test_vwh.cpp` | the lockstep session: commits, depth, stall kick, effects channel |
| `port/net/lsw2_tcp.*`, `lsw2_lockstep.*` | the 132-byte star transport and the original lockstep session |
| `port/net/lsw2_net_bind.*` | the one place the guest binding and the sockets meet |
| `port/frontend/forksession.cpp` | hosting, joining, the directory, LAN, UPnP, heartbeats, departures |
| `port/frontend/whsession.cpp`, `whsession.h` | bringing the cartridge's session up, the join prompt, session end, the state numbers |
| `port/frontend/sessionpause.cpp`, `sessionroster.cpp`, `scorescreen.cpp` | per-player pause, roster rules, the lobby return |
| `port/frontend/netmsg.h`, `netmsglog.cpp` | the message ring layout and a read-only walker that names each message |
| `port/frontend/commguard.h` | the null-manager guard on `FUN_0206EF0C` |
| `port/net/lsw2_wireless_relay.*`, `lsw2_relay_session.*` | the radio-over-TCP route |
| `port/net/lsw2_master.*`, `lsw2_lan.*`, `lsw2_upnp*.cpp`, `lsw2_ping.*` | discovery and reachability |
| `services/master-server/`, `services/master-server-selfhost/` | the directory, hosted and self-hosted |
| `config/net_messages.json`, `tools/export_net_messages.py` | the 70 message types |
| `config/game_states.json` | the state factory's arms |
