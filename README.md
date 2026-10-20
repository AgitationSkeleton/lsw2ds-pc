# LEGO Star Wars II: The Original Trilogy (DS) — PC port

A native Windows port of the Nintendo DS game, built by **static
recompilation**: the cartridge's ARM9 and ARM7 machine code is translated to C
ahead of time and compiled into a PC program. It is not an emulator. The port
draws both screens in their own windows at 60 fps, adds rebindable keyboard and
gamepad controls, higher internal resolution and a settings menu, and replaces
the DS wireless link with **online multiplayer for up to four players**.

The game itself is not included. You need your own dump of the cartridge.

## Two programs, one install

| executable | what it is |
|---|---|
| `lsw2_bountyhunter.exe` | **Bounty Hunter.** The arena mode as a game of its own: everything unlocked, an online lobby you walk around, a host settings console (roster, customization, controls, music, lobby timer, Extras), room codes, join/leave in progress, and 2–4 players over the internet. |
| `lsw2.exe` | **The game as shipped**: story, Free Play and the rest. It also gains online co-op through the cartridge's own wireless screens. It keeps its own separate save. |

Both are the same binary under two names: the name picks the mode.

## Playing

1. Download a release zip and unzip it somewhere you can write to.
2. Run `build\lsw2_bountyhunter.exe` (or `build\lsw2.exe`).
3. On the first start, the game asks for your ROM: LEGO Star Wars II: The
   Original Trilogy (USA), SHA-1 `7c1afa070797945ba5a2577c7ddfded5cf2fc443`.
   It checks the file and copies it into `rom\`.

- **Menu:** Esc or F1 opens the port's menu (video, input, online and more).
  Close the window to quit.
- **Online:** to host, start a lobby; it appears in the public lobby list with a
  four-letter room code. To join, pick a lobby from the list or type its code.
  No port forwarding is needed: when a player can't reach the host directly,
  the game connects them through the master server's relay automatically.
- **BIOS:** no BIOS or firmware files are needed. An open-source replacement
  BIOS (FreeBIOS) and a generated firmware image are built in.

## How it works

```
 your ROM ──► extract ──► ndsrecomp ──► C for ~44,000 functions ──► compiled into lsw2.exe
                          (lifter)      (4 programs: ARM9, ITCM,
                                         ARM7, ARM7 WRAM copy)
                                                   │
             anything the lifter did not reach ────┴──► runs on the built-in
                                                        ARM interpreter instead
```

The port is built on
[ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp), a DS static
recompilation framework. On top of it this repository adds:

- **`port/frontend/`, the shell:** windows, presentation, the imgui settings
  menu, input, audio, and the Bounty Hunter fork's in-game menus, drawn in the
  game's own style with its own fonts.
- **`port/frontend/dispatch_overrides.cpp`:** where the port replaces or wraps
  individual game functions by address. This is how the fork changes the game's
  behaviour without touching the ROM.
- **`port/net/`, the networking:** a deterministic lockstep transport over TCP,
  a host-level stand-in for the DS wireless hardware layer, the master-server
  client, LAN discovery and UPnP.
- **`port/patches/`:** the changes this port makes to the framework, applied by
  the build.
- **`port/lift/`:** the function entry points (address, ARM/Thumb mode, name)
  that the lifter translates.
- **`services/master-server/`:** the public lobby directory, a Cloudflare
  Worker. `services/master-server-selfhost/` is the same protocol as a program
  you can run yourself.
- **`tools/`:** the build's helpers, the reverse-engineering tools, and the
  checks the test suite runs.

## Documentation

- **[docs/](docs/README.md):** how the game's code works, by subject:
  - the program structure, main loop and state machine
  - the asset formats
  - characters, customization and the Bounty Hunter rules
  - rendering and the HUD
  - the wireless multiplayer and its data-sharing protocol
  - how the recompiled port runs all of it
- **[playbook/](playbook/README.md):** the method, written for anyone porting a
  DS game: bring-up order, finding live state in RAM, verifying a port.
- **[port/ARCHITECTURE.md](port/ARCHITECTURE.md):** baseline vs. fork, and which
  layer owns what.
- **[BUILDING.md](BUILDING.md):** building from source.
- **[RELEASING.md](RELEASING.md):** versions, tags and the release script.

## Building

See [BUILDING.md](BUILDING.md). In short, on Windows with MSYS2:

```sh
bash tools/fetch_ndsrecomp.sh   # the framework, at the pinned commit
# put your ROM in rom/ and your BIOS dumps in port/bios/
bash port/build.sh              # -> build/lsw2.exe and build/lsw2_bountyhunter.exe
```

## Status

**Early pre-release.**

- **Tested on one machine.** Every build so far has been tested on a single
  Windows PC. Expect problems on other hardware, GPUs and drivers.
- **Online play is not thoroughly playtested.** Two to four players have
  reached matches at 60 fps, with several game instances running on that one
  machine. Real internet play between different PCs has had little testing,
  and the same goes for NAT and routers, join-in-progress and disconnects.
  Desyncs and dropped sessions are likely.

The [CHANGELOG](CHANGELOG.md) lists what each release changed. Bug reports
are very welcome. Please include the newest file from `build\logs\`.

## Legal

This is an unofficial fan project. It is not affiliated with or endorsed by
the LEGO Group, Lucasfilm, TT Games, Traveller's Tales, Amaze Entertainment or
Nintendo. All trademarks belong to their owners.

**No game code or data is in this repository.** You supply your own legally
obtained dump of the cartridge. The build reads it to produce the executable.
The decompiled output, extracted assets and BIOS images are never committed.

The port's own code is licensed under the **GNU General Public License v3.0 or
later** ([LICENSE](LICENSE)). The executables link GPL-3.0-or-later code from
melonDS through ndsrecomp, which is MIT-licensed, so the executables are
GPL-3.0-or-later as a whole. [THIRD_PARTY.md](THIRD_PARTY.md) lists every
component and its license.
