# Game internals

How LEGO Star Wars II: The Original Trilogy for the Nintendo DS is put together:
its two programs and memory map, the frame loop, the state and screen manager,
levels, saves and unlocks, the random number generator, the string bank, and
short tours of the asset formats, the actor and customization systems, the
Bounty Hunter match rules and the HUD.

Every address in this document is a guest address in the **USA cartridge**
(game code `AL7E`, SHA-1 `7c1afa070797945ba5a2577c7ddfded5cf2fc443`). Function
names such as `FUN_0205ECC4` are the decompiler's address labels, not the
game's names.

Confidence is stated where it matters. **Read from the code** means the
behaviour comes from the disassembly or decompilation at the cited address.
**Measured** means it was observed in a running game, either on an emulator or
in the port. **Inferred** means it fits the evidence but has not been proved.

For the wireless stack, data sharing and the match message protocol, see
[multiplayer.md](multiplayer.md). For how the port lifts and runs this code, see
[the-port.md](the-port.md).

---

## 1. The programs and the memory map

### The cartridge

The NitroFS holds only five files:

| file | size | what it is |
|---|---:|---|
| `rom.bin` | 18,334,192 | the game's own indexed asset archive (section 9) |
| `Sound/SoundData.rom` | 3,626,656 | the custom audio container (section 9) |
| `Multiboot/child.bin` | 2,781,896 | a separate download-play child program |
| `Banner/GameBanner.nbfc`, `.nbfp` | 512, 32 | banner data |

There are **no overlays**: both overlay tables in the header are empty. Every
line of ARM9 code is resident from boot.

### The ARM9 program and its BLZ compression

The header places the ARM9 at ROM offset `0x4000`, loading at `0x02000000`
with entry point `0x02000800`. The size the header gives, `0xA3F24`, is the
**packed** size. The ARM9 is BLZ-compressed (the standard backward-LZ scheme of
Nintendo's SDK) and unpacks itself in place at boot. It expands to `0x107238`
bytes, so the running image extends about `0x63000` bytes past the end of the
packed one.

At runtime the decompressor's back-copy loop was seen at `0x020009C4`, with the
CP15 cache flush at `0x020009EC` after it, and execution later continued past
`0x020A3F24`, the end of the packed image. A tool that decompiles the header's
extents as they are gets almost half the program wrong: 2,600 of 5,860 known
functions fell outside the image when the port's configuration was first built
that way.

The unpacked image carries the SDK's module-parameters block (magic words
`0xDEC00621` / `0x2106C0DE`). It gives the static BSS as
`0x021019C0..0x0211EFA0`, the SDK version word as `0x03017531`, and a
two-entry autoload list at `0x02107220`, which copies two blocks out of the
image:

| section | destination | size | contents |
|---|---|---:|---|
| 0 (static) | `0x02000000` | `0x1019C0` | the main program and its initialised data |
| 1 | `0x01FF8000` (ITCM) | `0x55C0` | hot routines, including the archive's type-6 decompressor and the interrupt handler |
| 2 | `0x027E0000` (DTCM) | `0x2A0` + `0xC00` BSS | a handful of very hot globals; the stack also lives in DTCM |

The sections never move after boot, which makes the ARM9 an easy target for
static recompilation, but a lifter must read ITCM code at `0x01FF8000`, not at
its position in the file.

### The ARM7 program and its WRAM copy

The ARM7 binary sits at ROM offset `0xA8000`, is `0x286A0` bytes, and loads
and enters at `0x02380000`. It serves the ARM9 over the IPC FIFO; the touch
screen, the real-time clock and the wireless driver are among its confirmed
duties. It does not run entirely where it loads:

- **A 64 KB block runs from WRAM at `0x037F8000`**, starting at `arm7.bin`
  offset `0x1B0`. This was measured against a real console's savestate (48 KB
  byte-identical). The code that performs the copy has not been found, so the
  mapping rests on that measurement. The FIFO channel-registration function is
  in this block, at `0x037FEAB8`.
- **The tail of `arm7.bin` runs `0x4FC18` bytes higher in main RAM**
  (decompiled at `0x023A6000`, running at `0x023F5C18`).
  `tools/resolve_arm7_relocated_calls.py` applies the delta. About 60 call
  targets remain unresolved and probably need a second delta (inferred).

### Memory map

| region | what lives there |
|---|---|
| `0x01FF8000..0x01FFD5C0` | ITCM: ARM9 autoload section 1 |
| `0x02000000..0x021019C0` | ARM9 static code and data |
| `0x021019C0..0x0211EFA0` | ARM9 static BSS: the game's globals (table below) |
| `0x0211EFA0..` | the heap: state objects, level data, decoded assets |
| `0x02380000..` | ARM7 program, with a relocated tail near `0x023F5C18` |
| `0x027E0000..0x027E4000` | DTCM (16 KB): hot globals, the stack, and the IRQ handler vector at `+0x3FFC` |
| `0x027FF800`, `0x027FFC00` | the firmware's two copies of the cartridge chip ID |
| `0x027FFFA8`, `0x027FFFAA` | button and touch words published by the ARM7 |
| `0x037F8000..` | the ARM7's WRAM copy |

The interrupt vector at DTCM `+0x3FFC` holds `0x01FFD2C8`, an ITCM address
(measured on hardware). The VBlank handler the game installs is
`FUN_0205D1A0`.

The globals a reader meets most often:

| address | what it is |
|---|---|
| `0x027E02A4` | the state manager's current slot (section 3) |
| `0x027E02A8` | the game-state bits word, read at about 194 sites |
| `0x021027F8` | per-player input: four records of stride `0x0A`, held buttons at `+0x04`; player count at `+0x29`, local station index at `+0x2A` |
| `0x02102824` | VBlank bookkeeping; `+0x14` (`0x02102838`) is the VBlank counter |
| `0x02102900` | the state manager |
| `0x02104344` | the RNG state (section 7) |
| `0x02104364` | the save file's working header buffer |
| `0x021044A4` | the touch manager's object |
| `0x021046FC` | the `@N` text-substitution slots, 64 bytes each |
| `0x02104D90` | the biased base pointer for the progress flag arrays (section 6) |
| `0x02106FD8` | per-player appearance records, stride `0x40` |
| `0x02107860` | pointer to the Bounty Hunter match settings object (section 11) |
| `0x0210794C` | the netgame message object (see [multiplayer.md](multiplayer.md)) |
| `0x021092F4` | the multi-view camera array, present only in a wireless game |
| `0x0210930C` | the level root: the party array at `+0x4C`, the installed level object at `+0x68` |

Bits of `0x027E02A8` that are understood, all read from the code:

| bit | meaning |
|---|---|
| `0x03` | paused; the two bits are always set and cleared together |
| `0x04 << slot` | that state slot is being rebuilt |
| `0x10 << slot` | that slot's fade is in progress |
| `0x40` | the multi-view camera array exists. It is set when a wireless game allocates the array and cleared when it is freed, and it also routes many setters into network broadcasts |
| `0x100` | makes the lid-close handler return early |
| `0x200` | the frame loop skips its VBlank wait and the manager takes a single-slot path |
| `0x800` | enables the chip-ID check after a lid close; set in every capture examined |

---

## 2. The main loop and the 30 Hz simulation

### Boot

`main()` is `FUN_02000BC0`: about forty initialisation calls, then

```c
do { FUN_020013A4(); FUN_02001480(); } while (true);
```

The initialisation includes the file system. `FUN_0205AA8C` mounts the
cartridge, caches the FAT and FNT in RAM and opens `rom.bin`. It also seeds the
RNG from the clock (the call at `0x02000D00`, section 7). The first states
requested are the boot splash and a long-lived UI state (section 3).

### One iteration of the frame loop

`FUN_020013A4`, read from the code:

1. **Run the state manager's tick**, `FUN_0205ED08(0x02102900)`. This one call
   does a whole frame's work: input, touch, the level update, state
   transitions, per-state update and draw, and finally the 3D buffer swap,
   written as `(flag << 1) | 1` to `SWAP_BUFFERS` (`0x04000540`).
2. **Every 20th frame** of the current slot's frame counter
   (`manager + 0x88 + slot*4`), call a routine on the progress block at
   `+0x44AC`. From its arithmetic it converts a calendar date and time into
   seconds, so it is probably a clock refresh (inferred).
3. **Wait for VBlank**, unless bit `0x200` of `0x027E02A8` is set. The loop
   calls the SDK's VBlank-interrupt wait (`FUN_020CE878`) **until the VBlank
   counter at `0x02102838` has advanced by at least 2** since the last frame,
   then records the new count.
4. **Run the netgame tick**, `FUN_020B07E8(0x0210794C)`. It runs after the
   wait, which matters for wireless timing.

`FUN_02001480`, the loop's second call, returns at once unless bit 15 of the
ARM7-published button word `0x027FFFA8` is set (by convention the lid;
inferred). Otherwise it ends any wireless game, sleeps, and on waking re-reads
the cartridge chip ID. A different ID makes the game halt on purpose
(`FUN_020CEA74`), because it concludes the card was removed.

### Why the game runs at 30 Hz

**Each pass of the game loop waits for two VBlanks**, so the game simulates,
renders and swaps at 30 Hz while the display refreshes at 60. This was also
measured before the loop was read: every traced per-actor routine fires once
per two emulator frames. The physics agrees. Gravity (`471/4096`) and the jump
impulse (`6349/4096`) give a 0.87 s jump at 30 Hz against about 0.9 s
measured, and Greedo's table speed of 1.2 units per tick matches his measured
36 units per second.

Every "per frame" constant in the game is therefore per 30 Hz tick, including
the fire cooldown, the 20-tick respawn lock and the 83-tick spawn immunity.
Timers count ticks or VBlanks, not wall-clock time, so the match clock runs
slow whenever the emulator does.

---

## 3. The state and screen manager

Every screen and every level is a **state object** built by one factory and
owned by one manager. The game requests screen changes from 176 call sites,
all through one function.

### The manager and its two slots

The manager is the static object at `0x02102900`. It runs **two slots**, and
they are two layers rather than a stack:

- **slot 0, the world**: a level (kind 4), the front end (kind 0x11), the boot
  splash (kind 6) and similar states;
- **slot 1, the screen**: the touch-screen and menu layer. Examples are the
  in-game UI (kind 48), Game Settings (54), the character select (41), the
  match (42) and the score screen (44).

Both are live at once. While Game Settings is open the screen slot holds 54
and the world slot still holds level 9, the arena lobby.

Manager layout, read from the code:

| offset | per slot | contents |
|---|---|---|
| `+0x00` | `+slot*4` | the slot's current state object |
| `+0x08` | `+slot*0x20` | the previous state's arguments, copied as it is destroyed |
| `+0x48` | `+slot*0x20` | the request record: 8 words |
| `+0x88` | `+slot*4` | a frame counter |
| `+0x90` | `+slot` | the status byte: 1 requested, 2 fading out, 3 built |
| `+0x94` | `+slot*0x20` | the command queue: 8 words |
| `+0xD4` | `+slot*4` | the command count |
| `+0xDC` | `+slot` | a byte that, when non-zero, skips the slot's update and draw |

`0x027E02A4` is the **current slot**. `0x02001688` pushes the old value onto a
small stack at `0x027E02AC` and stores a new one, and `0x020016C0` pops it.
The frame loop sets the current slot before working on each slot, and code
that needs to address "my" layer reads it.

### Requesting a state

```
FUN_0205ECC4(manager, kind, slot, mode, level, arg)
```

This function pushes `slot` as the current slot, calls `FUN_0205EC4C` and pops.
`FUN_0205EC4C` writes the request record at `manager + slot*0x20 + 0x48`:

```
+0x48 kind   +0x4C mode   +0x50 level   +0x54 arg   +0x58..+0x64 zeroed
```

**A request is eight words.** A writer that changes only the first word leaves
stale arguments from the last request behind. The eighth word (`+0x64`) is a
no-fade flag, and the requester always clears it.

Requesting does not touch the status byte. The manager's tick moves the
request through three steps, all read from the code:

1. **Transition (`FUN_0205EF84`).** A non-zero kind in the request starts a
   fade-out (unless `+0x64` is set), sets status 2 and sends the old state
   message 7 through its handler at vtable `+0x14`. When the fade completes, it
   calls the old state's **exit** (vtable `+0x10`), copies the old state's
   arguments to `manager + slot*0x20 + 0x08`, calls its **destructor**
   (vtable `+0x04`), clears the slot and sets status 1.
2. **Pump (`FUN_0205F45C`).** On status 1 it sets status 3, builds the new
   object through the factory and stores it. It then **zeroes the request's
   kind word**, calls the new state's **enter** (vtable `+0x08`), drains the
   command queue and starts the fade-in.
3. **Update and draw.** Each frame `FUN_0205F168` drains the queue and calls
   the state's **update** (vtable `+0x0C`) unless bit 1 of the object's `+0x20`
   is set, followed by an optional child object's update. `FUN_0205F520` draws
   the slot.

So a non-zero kind word at `+0x48` is an **accepted request not yet built**,
and it tells you where a transition is going while the old screen is still
current. Setting the status byte to 1 by hand also builds the new state, but
skips the old one's exit and destructor and leaks its registrations; the port
writes the eight words exactly as `FUN_0205EC4C` does instead
(`lsw2_state_request_in_slot()` in `port/frontend/statereq.cpp`).

### The command queue

Each slot also has a small command queue, written by `FUN_02060084(manager,
slot, opcode, argc, args)` and by its varargs wrapper `FUN_02060158`. A command
is one word, `opcode | argc << 16`, followed by `argc` argument words. The
pump and the update drain the queue. Each command goes first to the current
state's handler at vtable `+0x14`; if that returns 0, the manager's default
handler `FUN_0205FE58` takes it:

| opcode | default action |
|---|---|
| 2 | go back: request again the previous state saved at `+0x08` |
| 4, 6 | request a state, with 1, 3 or 4 argument words (kind, mode, level, arg) |
| 9 | forward the rest of the command to another slot |
| 13 | request a level: kind 4 with the given level |

Menus use this path to change screens. The pause menu's rows, for example,
post commands to slot 1 rather than calling the requester themselves. The
level state's own handler (`0x0208CFEC`, 27 cases) adds commands of its own;
opcode 26 shows a message popup whose first argument is a string id.

### The factory and the state kinds

`FUN_0205F574(manager, kind, mode, level, arg)` checks `kind <= 0x39` and
jumps through a table of 58 arms at `0x0205F59C + kind*4`. Each real arm
allocates (`0x0205E80C`), calls a constructor, and stores the kind at
`+0x04`, the mode at `+0x08`, the level at `+0x0C` and the extra argument at
`+0x10`. **Twenty-two kinds share the default arm at `0x0205FE30`, which
returns null**: those kinds are not implemented. Kinds 11 and 12 share one arm.

> `config/game_states.json` records the same table, but its numbering is
> one higher than the factory's (its "state N" is kind N−1) and it omits kind
> 57. The table below is read directly from the jump table.

| kind | hex | constructor | size | what it is | evidence |
|---:|---|---|---|---|---|
| 4 | 0x04 | `0x0208A938` | `0xC0` | **the level state** (world slot), vtable `0x020FF478` | read and measured |
| 6 | 0x06 | `0x02069DB8` | `0x90` | the boot splash (world slot at power-on) | measured |
| 9, 10 | | `0x0207FCD4`, `0x020801D8` | | requested by level-script command 42; not named | |
| 11, 12 | | `0x0207B71C` | `0xE4` | shared arm; not named | |
| 15 | 0x0F | inline, vtable `0x020FE6F0` | `0x28` | an empty placeholder used in either slot: slot 1 from boot, slot 0 behind the cartridge's own Game Settings route, and with the comm-error screen | measured |
| 17 | 0x11 | `0x02073A00` | `0xAC` | **the front end** (title and menus), world slot | measured |
| 19 | 0x13 | `0x0206D13C` | `0x90` | a co-op / wireless screen; its class sends a host message and tears down | read |
| 28, 30-33 | | | | seen in captures of the arena flow; not named | measured |
| 34 | 0x22 | `0x0206FD30` | `0xA4` | the cartridge's wireless game list | read |
| 35 | 0x23 | `0x0206ECBC` | `0x94` | the host's "join?" Yes/No prompt | measured |
| 38 | 0x26 | `0x0206E8C0` | `0x9C` | the Communication Error screen | measured |
| 40 | 0x28 | `0x020730E0` | `0xE0B4` | the lobby (re)load at the start of a wireless game | read |
| 41 | 0x29 | `0x02098790` | `0x2B20` | the Bounty Hunter character (statue) select | measured |
| 42 | 0x2A | `0x02099DB4` | `0xF0` | **the arena match** | measured |
| 43 | 0x2B | `0x0209BD78` | `0xB8` | raised as the match ends; also seen in the world slot during the score screen | measured |
| 44 | 0x2C | `0x0209C720` | `0x90` | the score screen: Continue, Select Character, Settings, Quit | measured |
| 48 | 0x30 | `0x020872B4` | `0x100` | the in-game touch-screen UI (slot 1 in a level) | measured |
| 53 | 0x35 | `0x0208E260` | `0xF4` | the pause menu, vtable `0x020FF524` | measured |
| 54 | 0x36 | `0x0209D948` | `0xBC` | Game Settings: the map, mode and time chooser | measured |

The kinds left out of the table (36, 37, 39, 46, 49 to 52, 55 to 57) have
constructors but have not been identified.

A full arena round seen in recordings runs `48` (lobby) → `54` → `41` → `42` →
`44`, then `54` → `41` → `42` again. The netgame message handler
(`FUN_020B1248`) requests states 39, 42, 43 and 44: the shared match sequence
is driven by network messages. States 40, 41 and 54 are all requested from one
function, `FUN_02072CF4`, which picks the next screen from a selector byte:

```
selector 0   (0x28, slot 1)                     restart the lobby
selector 3   (4, slot 0, mode 0x10, level 9)    reload the lobby as the world
             (0x29, slot 1)                     and open the character select
otherwise    (0x0F, slot 0) + (0x36, slot 1)    Game Settings
```

Quit Game posts `0x11` to slot 0 and `0x0F` to slot 1.

### Reading where the game is

- **Current screen:** `[[0x02102900 + 4] + 4]`, the kind of the screen slot's
  object (`lsw2_state_current()` in `port/frontend/statelog.cpp`).
- **Current world:** `[[0x02102900] + 4]`. A level reads 4. The front end
  reads 0x11, and **its level field holds 0, a valid level id**, so "level < 0"
  is not a test for the front end. During a reload the slot is briefly empty.
- **Current level:** `[[0x02102900] + 0x0C]`, the level argument of the world
  state. A second copy of the level id lives in the installed level object, as
  the byte at `[[0x0210930C + 0x68] + 0x479]`. The ordinary entry path writes
  it, and a manager-request warp leaves it unchanged, so after a warp the two
  can disagree.

**The level and the screen are separate things.** Requesting a level does not
dismiss the current screen. A level request made while the score screen is up
loads the new level underneath it.

---

## 4. Levels

### The level table

| table | address | shape |
|---|---|---|
| level records | `0x020F55C4` | 74 records, stride 28 |
| level id → record | `0x020FF674` | 73 pointers |
| lighting rig per level | `0x020FF54C` | 73 entries |

Byte 0 of a record is the **mode class**: `0x10` for a hub (the Mos Eisley
Cantina, 14, and the arena lobby, 9) and `0x11` for an arena (4 and 5).
`0x0203814C` uses that class to decide whether a respawn relocates the player.
Byte 1 is the level's **music slot**, read by `0x02093F6C`. The other fields
include asset ids for the level's BSP, script and textures.

Only a few ids are named with confidence:

| id | level | how it is known |
|---:|---|---|
| 4 | Jabba's Tower (arena) | named, and reached in matches |
| 5 | Mos Eisley Streets (arena) | named, and reached in matches |
| 9 | Cantina Arena Lobby | named, and reached |
| 14 | Mos Eisley Cantina, the main hub | observed: the game reads 14 there |
| 15 | the level a new game enters directly | hard-coded at `0x0207FD94` |

`tools/extract_level_table.py` generates `port/frontend/levels.h` with a name
for every id, but the others are **derived from asset names and unreliable**
(id 14 derived as "Arena (2)").

### The level entry chain

The level state (kind 4) has a six-entry vtable at `0x020FF478`:
`0x0208D1E0`, `0x0208D1C0` (destructor), `0x0208C888` (enter),
`0x0208CDB0` (update), `0x0208CF08` (exit) and `0x0208CFEC` (command
handler).

The enter method `0x0208C888` loads the level through
`0x02090AE8(0x0210930C, level, mode)` → `0x02090F3C`. That function allocates
the level object, runs the constructor `0x0208F760`, which stores the index at
`+0x479` and the mode at `+0x47B`, and stores the object at
`0x0210930C + 0x68`.

The enter method then branches on the **mode** (`+0x08`) seven times. The mode
is fixed for the object's lifetime, and it decides whether the players are
spawned:

| mode | effect |
|---|---|
| `0x04` | calls the slot 0..3 spawn dispatcher `0x02089E60` directly |
| `0x05` | none of the arms: the lobby is entered this way and nobody is respawned |
| `0x0E`, `0x12` | spawn through `0x02089A6C` + `0x02089C88`; `0x0E` is the co-op entry |
| `0x0F` | places the players only (`0x020897F8`): how the networked arena match enters |
| `0x10` | the walkaround: broadcasts behaviour 6 and sets a walkaround byte |

`0x0208C888` has also been read as a per-frame update. The manager calls
vtable `+0x08` once, when the state is built, and `+0x0C` every frame, so this
document treats it as the enter method.

The arena request in the match flow (`0x02099D50..0x02099D6C`) reads the map
from the match settings object (`[0x02107860] + 0x40`, section 11) and asks for
kind 4, mode `0x0F`, with that level.

---

## 5. Script and command systems

The game has three dispatch tables that are easy to confuse. Their index
spaces are unrelated:

| dispatcher | table | entries | what it dispatches |
|---|---|---:|---|
| `0x0208CFEC` | inline | 27 | the level state's command handler (manager queue opcodes; 26 = message popup) |
| `0x02055E98` | `0x020ED928` | 192 | **level-script commands** |
| `0x02067838` | `0x020ED5CC` | 47 | builders for a level's actors, chosen by a descriptor's kind byte (section 10) |

**The level-script interpreter** is `FUN_02067A98`. A script is a count byte
followed by a stream of one-byte command indices. A per-command argument-size
table at `0x020F716C` steps an argument pointer forward after each command,
and each command goes through `0x02055E98`, which looks up the handler and
calls it. Every console runs its own copy of a level's script. Commands
identified so far:

| command | handler | does |
|---:|---|---|
| 42 | `0x02056FAC` | requests a state |
| 44 | `0x02057084` | "if flag N then A else B" on a progress flag; the Cantina console's two refusals branch here |
| 65 | `0x02057A28` | makes the local player playable |
| 78 | `0x02057BC8` | sets the local player's behaviour byte from its 4-byte argument |

`config/script_commands.json` lists all 192 handlers. Command 78 is how an
arena turns on its control scheme. When the arena loads, its script runs
command 78 with operand 9 (measured). In a wireless game the setter broadcasts
the value instead of writing it locally, so every console's behaviour table
reads 9. Behaviour 9 selects the world-relative "tank" movement handler; it
does not freeze the player.

---

## 6. Saves and unlocks

### The save device and file

The cartridge's backup chip is a **512-byte EEPROM**. The game probes for it
at boot and stops with "could not detect DS data" if it is absent. The file
begins with the magic `LEGO000D` (in the image at `0x020EDC58`).
`0x0206353C` copies the 32-byte header into the RAM buffer at `0x02104364` and
checksums it; the slot summaries follow. In the file, the region
`0x7E..0xA5` (40 bytes) is all `0xFF` in a fully unlocked save and all zero in
a fresh one. **How that region reaches the runtime flag arrays below is not
established.** Loading either save left the arrays identical in the runs that
checked, so those runs may simply not have reached the point where a save's
unlocks are applied.

### The progress flag arrays

The game's progress lives in nine bit arrays reached through one accessor
idiom: `base + 0x4000 + k`, word `n >> 5`, bit `n & 31`, with base
`0x02104D90`. That base is **biased**: there is no `0x4600`-byte structure
behind it, and the arrays sit among unrelated globals at `0x02109218` and up.
The initialiser at `0x02085718..0x020857DC` clears them all, and its loop
bounds give each array's size. The sizes tile exactly, with no gaps:

| offset | absolute | bits | test / set | holds |
|---|---|---:|---|---|
| `+0x4488` | `0x02109218` | 64 | `0x02086204` / `0x020861BC` | mode bits: **10** = Free Play party mode; **11** = a wireless game is active |
| `+0x44C8` | | (one word) | | not an array: a flag word initialised to `0x80000000` |
| `+0x44CC` | `0x0210925C` | 128 | `0x02086534` / `0x02086458` | characters OWNED, by character id |
| `+0x44DC` | `0x0210926C` | 128 | `0x02086588` / `0x02086558` | characters SEEN; without it the roster draws `????` |
| `+0x44EC` | `0x0210927C` | 256 | `0x020865DC` | customization parts owned, by part id |
| `+0x450C` | `0x0210929C` | 64 | `0x02086630` | a second customization array, ids 7..33 |
| `+0x4514` | `0x021092A4` | 32 | `0x0208671C` | Cantina terminal purchases: Extras at 0..21, Hints at 22..28 |
| `+0x4518` | `0x021092A8` | 32 | `0x020867EC` / `0x02086810` | Extras **enabled** |
| `+0x451C` | `0x021092AC` | 32 | `0x02086E4C` / `0x02086E1C` | not identified; the **test remaps** 22→0, 23→5 and 24→10, the setter does not |
| `+0x4520` | `0x021092B0` | 32 | `0x02086F20` | not identified |

Notes on individual bits:

- The Cantina terminal's Extras toggle `0x02086810(base, i, value)` does more
  than set a bit. It starts theme music, switches letterboxing, re-dresses the
  party and keeps the ten theme Extras mutually exclusive. Writing the
  `+0x4518` bit directly skips all of that.
- Extras flag **15** is a global "classic" swap. The gear applier uses it to
  substitute two part ids, and the weapon setter uses it to collapse action
  rows `0x11..0x19` onto `0x15`. It is almost certainly the "Classic LEGO
  Parts" Extra (inferred from string `0x256`). Flags 7, 8 and 9 are Old Timey,
  Old West and Medieval.
- `+0x4488` bit 11 is the one the Cantina console's script tests. Set, it
  refuses with "You can't do that during a Wireless game!"; clear, with "You
  must Save before doing that!". Both branches refuse, so no value of the flag
  opens the console. The multi-view teardown clears the bit.
- Other fields near the base: `+0x51` remembers the last Free Play page
  (`0xFF` means unset), and bit 0 of `0x02104D90` refuses party swaps.

### Cheat codes

The Cantina terminal's code entry checks against a **108-entry table at
`0x020FED9C`**, six bytes per code, each byte a symbol index in the alphabet
`0-9` then `A-Z`. The first four rows are `ABACAB`, `UDLRAB`, `867539` and
`BIGGUN`: Extras, characters, editor parts and editor weapons. The validator
`0x020787B0` compares the entered symbols, stored at `screen + 0x145`, with
every row, then jumps through a 109-entry table on `match + 1` (entry 0 is no
match). Each code's body sets bits through the accessors above, choosing the
OWNED or SEEN array from a byte in the 100-row roster table at `0x020EEDAC`.

---

## 7. The random number generator

The game uses **MT19937**. The generator at `0x02062D10` is identified by its
tempering masks `0x9D2C5680` and `0xEFC60000`. Its state record is at
`0x02104344`:

| offset | |
|---|---|
| `+0x08` | numbers left before a refill; decremented on every draw |
| `+0x0C` | the walking pointer into the state, advanced by 4 per draw |
| `+0x1C` | the seed |

| function | does |
|---|---|
| `0x020629C4` | seeds once at boot, from the real-time clock (called at `0x02000D00`) |
| `0x02062A40(seed)` | re-seeds: stores `+0x1C` and re-initialises |
| `0x02062BB4` | refill. **Only a count below −1 re-seeds from `+0x1C`**; −1 itself just regenerates from the current state |
| `0x02062AE4(n)` | `rand_range`: 0..n, from `(raw & 0x7FFF) * (n+1)`, 45 callers |
| `0x02062A7C(lo, hi)` | `lo + (((raw & 0x7FFF) * (hi-lo+1)) >> 15)`; returns `lo` without drawing when `lo == hi` |

One example draw: idle animations start on a random frame (`0x0201088C`), which
is why a room of characters never breathes in step.

Because the boot seed comes from the clock, **two consoles are guaranteed to
hold different generator states**. The game's own wireless code handles this.
Pressing Start Game draws a number and broadcasts it in message `0x0A`, and
every console re-seeds with `0x02062A40` when it handles the message. The
generator's value also feeds the netgame consistency check. See
[multiplayer.md](multiplayer.md).

---

## 8. The string bank

All UI text, 1,333 strings, comes from one Huffman-compressed bank. The
decoder is `0x0205CD20`. Its control block at `0x020FE5C8` is filled at load
and reads zero in the static image:

```
[+0]  offset table, indexed by id*4
[+4]  compressed data
[+8]  Huffman tree: u16 pairs; node n's children at tree + (n-0x100)*4 and +2
```

The root is node `0x100`, and any value below `0x100` is a literal byte. Bits
are consumed **low bit first**, with a fresh byte every 8 bits. A decoded 0
ends the string, except that bytes `0xF0` and above lead a two-byte glyph
whose trail byte may be zero. No string in this cartridge contains one.

The text path, read from the code:

```
0x0205CD20  decode
0x02066708  get_string(id): decodes into a shared 0x400-byte buffer at 0x02104918
0x02066470  get_string_substituted(id): splices "@N" tokens in place
0x0206B934  set_message(widget, _, id, id2): a widget carries two ids at +0x88C/+0x88E, 0xFFFF = none
```

`@1`, `@2` and so on are replaced from **64-byte slots at `0x021046FC`**, so
`@1` is slot 0. Player names reach popups this way. Useful ids: character
names are `0x1CB + character id` (`0x1DF` Greedo, `0x1D2` Boba Fett); the
Bounty Hunter match-type labels are `0x29F + mode`; Free Play page titles
start at `0x231`. `tools/export_strings.py` writes the whole bank to
`config/strings.json`, and the port's decoder is `port/frontend/strings.cpp`.

---

## 9. Asset formats, briefly

**`rom.bin`** is a flat table of 32-bit offsets followed by data: 5,549
offsets, the last being an end sentinel, so 5,548 assets numbered 0..5547. A
wrapper byte selects compression. **Type 6** is decoded by an ARM9 routine
copied into ITCM at `0x01FF8000..0x01FF84A4`. The archive dispatch is
`0x02059EFC`, the type lookup `0x0205ABB4` and the loader `0x0205ACA0`.
`tools/lsw2_archive.py` runs the cartridge's own ITCM decoder in a sandboxed
ARM emulator rather than reimplementing it, so it needs a copy of the ROM. 29
uncompressed entries begin with a byte in the wrapper range; the recovery and
audit tools handle them.

The unwrapped contents are mostly standard Nitro 3D containers: about 930
NSBMD models, 1,690 NSBCA joint animations, and NSBTX, NSBTA, NSBVA and NSBMA
files. Most of the remaining 2,474 entries are game-specific binaries, among
them the version-7 BSP collision containers (Jabba's Tower is BSP 319 with
models 322 to 324; Mos Eisley Streets is BSP 325 with models 327 to 329).

**Fonts** are five custom 4bpp assets: 2300 and 2298 (8 px), 2299 (13 px),
2374 (14 px) and 3442 (18 px). The nibbles are palette indices 0 to 3
(transparent, fill, edge blend, outline), not coverage levels. The menu font
is the 18 px one. Its advances reproduce the measured widths of all six pause
menu labels exactly. `tools/export_custom_fonts.py` exports them.

**Audio** is not Nitro SDAT. `Sound/SoundData.rom` opens with ten section
offsets, holding 1,908 records between them: 29 tracker modules (Impulse
Tracker style), 683 instruments, 360 sample descriptors, 243 patterns, 344
sample payloads, and envelope and note-map tables. **Music is addressed by
slot, not by module.** The 29-row slot table at `0x02100A00` (u16 module, u8
mode, u8 parameter) is read by the selector `0x020B90E8`. Out-of-range slots
clamp to 0, and requesting the slot already playing does nothing unless a flag
bit forces it. Each level's record names its slot. The arenas (4, 5) and the
lobby (9) all use slot 2, so the arena has one track. `config/music_slots.json`
and `config/music_levels.json` hold both tables.

---

## 10. Actors, characters and customization, briefly

**The party** is four actor pointers at `0x0210930C + 0x4C + i*4`. A slot
counts as present when `actor+0x08 & 0x803 == 1`. The local slot index is
`0x021027F8 + 0x2A`.

Actor fields a reader will meet:

| offset | field |
|---|---|
| `+0x08` | flags (bit `0x20` gates gravity) |
| `+0x60..+0x68` | velocity, 20.12 fixed point |
| `+0x8C` | 4×3 world matrix |
| `+0x180` / `+0x182` | animation descriptor set / state within it |
| `+0x5DA` | view index: 4 makes movement world-relative |
| `+0x6B6` / `+0x6B8` | hearts / maximum (players are built with 4) |
| `+0x6BB` | character id |
| `+0x6BE` | spawn immunity (83 ticks) |
| `+0x6C1` | fire cooldown, counted in 30 Hz ticks |
| `+0x6C3` | action row; its `+0x00` is the weapon asset |

**Character ids** index the 100-row roster table at `0x020EEDAC` and the
descriptor table at `0x020EF57C` (stride `0x18`). The value `0x64` (100) means
"no character" wherever an id is expected. Greedo is 20; 4-LOM is 0. The ten
Bounty Hunters, from the Free Play pages' own id table at `0x020EDDB2`, are
20, 7, 29, 27, 0, 13, 8, 34, 36 and 71: Greedo, Boba Fett, Jango Fett, IG-88,
4-LOM, Dengar, Bossk, Lando (Jabba's Palace), Leia (Bounty Hunter) and Zuckuss.
Run speed is an exact per-character constant from the motion table at
`0x020EE04C`. It is reached through the animation, not indexed by character id.

**The Free Play select** is a paging widget over 14 pages: 12 character pages
(the Bounty Hunters are pages 6 and 7) and 2 vehicle pages. Its low and high
page bounds are fields on the widget (`+0x24`, `+0x28`). L and R page through
it, and paging wraps. A first touch highlights a portrait and a second touch
on the same portrait commits. A party swap goes through `0x020922F4`.

**Parts** fall into five id ranges, and the range is the slot:

| ids | slot | count |
|---|---|---:|
| `0x00..0x2C` | head | 45 |
| `0x2D..0x6A` | torso | 62 |
| `0x6B..0xB0` | legs | 70 |
| `0xB1..0xE7` | headgear | 55 |
| `0xE8..0xF9` | the slot used by the theme Extras | 18 |

The categoriser is `0x0208DA74`, and each range has its own descriptor table
(`config/part_slots.json`). The gear applier `0x020063D4(actor, part, 0)`
attaches a part. The customize store at `0x02107890 + 0xA6..0xB0` holds six
halfwords: five parts for the gear applier and one action row for the weapon
setter `0x02004404`.

**Appearance is applied when an actor is built.** A level's actors come from
the 47-entry builder table. Builder 0 (`0x02050C44`) makes the playable
character and ends in the apply `0x02091B88`, which refuses when the party
holds two or more members. The per-player appearance records at `0x02106FD8 +
slot*0x40` are restored on the next spawn only when their gate byte (`+0x1B`)
is set; the byte below it carries hearts.

---

## 11. The Bounty Hunter match rules object

`[0x02107860]` points to a `0xB4`-byte object (vtable `0x020FFA78`) built by
`0x0209B61C`. Its fields, read from the code and checked against recorded
matches:

| offset | field |
|---|---|
| `+0x14` | captures, four signed halfwords |
| `+0x1C` | kills, four signed halfwords |
| `+0x24` | deaths, four signed halfwords |
| `+0x30..+0x33` | slot order, reset to 0,1,2,3 and never permuted during play |
| `+0x34` | match type, 0..4 |
| `+0x3C` | time limit in seconds; default 300, range 15..1200, no unlimited setting |
| `+0x40` | arena level id; default 5 |
| `+0x50`, `+0x68`, `+0x80`, `+0x98` | four 24-byte player records |

The five match types are labelled by string `0x29F + type`. Their scores come
from a jump table at `0x0209BC10`:

| type | label | score |
|---:|---|---|
| 0 | Kills - Deaths | kills − deaths |
| 1 | Capture | captures |
| 2 | Deathmatch | kills |
| 3 | DM Points | captures + kills − deaths |
| 4 | Points - Deaths | captures − deaths |

`0x0209BCA4` counts a kill and `0x0209BCBC` a death. **The arena constrains
the type**: on Mos Eisley Streets (5) the cycler admits only types 0 and 2, so
Capture is a Jabba's Tower mode. All 27 reads of the object are in menu,
lobby and broadcast code; none happens during play, so mode-dependent
behaviour is configured at setup.

The same object runs the match clock. `FUN_0209B88C` counts down the time
limit, announces with message `0x35` at 60 and 30 seconds remaining, and
broadcasts `0x12` at zero, which requests the score screen (state 44). The
character select has its own 30-second countdown, seen as 29.77 s before it
advances on its own.

---

## 12. HUD, menus and the touch screen, briefly

**Input** is sampled every frame at the top of the manager's tick. The button
update `FUN_0205CE64(0x021027F8)` runs first, then the touch manager
`FUN_0206468C(0x021044A4)`. The touch object holds a Nitro `TPData` at
`+0x2A` (x, y, touch, validity), with sampling flags at `+0x00`. Touch is
sampled unconditionally, and 37 sites hit-test the point.

**The in-match HUD**, as captured from live matches:

- **Top screen.** The player's ring and portrait are two **3D textured quads**,
  the last two polygons submitted each frame. The hearts are a BG1 layer. The
  HUD setup `FUN_0208AAF0` builds player one's portrait only while its ring
  slot (`+0x54`) is empty, so the portrait is built once and not rebuilt on a
  character switch. The partner portrait has a full rebuild path
  (`FUN_0208B54C`).
- **Touch screen.** Four BG layers (minifig heads and radar bezel; radar
  sweep, score digits and buttons; a scrolling starfield and its sparkle
  overlay) under 17 to 19 sprites written straight to OAM, including a column
  of four player slots at a 48-pixel pitch. The radar is player-relative,
  rotated by facing and scaled by 1/4. **Whether the slot column re-sorts by
  score is uncertain**: one set of captures was read that way, but the
  settings object's order bytes never permute.

**Menus** are 2D: tiles, sprites and the 18 px font, not polygons. The pause
menu (state 53) builds its rows at runtime: Resume, Host Wireless, Settings,
Extras, Save Game and Quit Game. The wireless row's label changes to "Shut Off
Wireless" while a wireless game is up. Popups go through `0x0206B934`'s
two-id widget (section 8), raised either from the level state's command 26 or
directly through the dialog path.

---

## 13. The game's tables and the port's dispatch

The game leans on tables of code addresses: state vtables, the script and
builder tables, the factory's jump table and the netgame message handlers. In
the port every lifted function is registered by guest address, and an indirect
call (a `BLX` through one of those tables) is resolved through that registry,
while a direct `BL` within the same generated shard becomes a plain C call.
So **a function the game reaches only through its own tables is always
dispatched**, and an override registered at its address always fires: script
commands, state methods and message handlers are the natural places to observe
or replace behaviour. A function with direct same-shard callers cannot be
reliably intercepted (`tools/sweep_override_reachable.py` counts those call
sites). See [the-port.md](the-port.md).

---

## Where to look in the source

| subject | files |
|---|---|
| ROM sections, header and NitroFS | `tools/nds_extract.py`, `tools/extract_rom.py`, `tools/extract_arm7_sections.py`, `tools/resolve_arm7_relocated_calls.py`, `port/game.toml` |
| state manager, requests and the state log | `port/frontend/statelog.h`, `port/frontend/statelog.cpp`, `port/frontend/statereq.h`, `port/frontend/statereq.cpp`, `config/game_states.json` (numbered one above the factory's kinds), `tools/export_game_states.py` |
| levels, warps and the party | `port/frontend/levels.h` (generated), `tools/extract_level_table.py`, `port/frontend/cheats.h`, `port/frontend/cheats.cpp` |
| command queue and script commands | `port/frontend/scriptqueue.h`, `port/frontend/scriptqueue.cpp`, `config/script_commands.json`, `port/frontend/behaviourhook.cpp` |
| flag arrays, Extras and unlocks | `port/frontend/cheats.cpp` (`kLsw2FlagArrays`), `config/cantina_terminal.json`, `config/customization_modes.json` |
| RNG | `port/frontend/rngsync.h`, `port/frontend/rngsync.cpp` |
| strings | `port/frontend/strings.h`, `port/frontend/strings.cpp`, `tools/export_strings.py`, `config/strings.json` |
| archive, fonts and audio | `tools/lsw2_archive.py`, `tools/audit_archive_catalog.py`, `tools/export_custom_fonts.py`, `tools/analyze_sounddata.py`, `config/music_slots.json`, `config/music_levels.json` |
| actors, roster and parts | `config/free_play_pages.json`, `config/playable_roster.json`, `config/part_slots.json`, `config/physics.json` |
| match rules and HUD | `port/frontend/scorescreen.cpp`, `port/frontend/arenahud.cpp`, `config/bounty_hud_layout.json`, `config/menu_screens.json` |
| override registration | `port/frontend/dispatch_overrides.cpp`, `tools/sweep_override_reachable.py` |
