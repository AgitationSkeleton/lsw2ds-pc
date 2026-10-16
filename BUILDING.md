# Building from source

The build turns your ROM into the two executables. It lifts the game's code to
C with ndsrecomp, compiles about 44,000 functions, and links them with this
repository's shell and networking. The first build takes a while (on the order
of half an hour). Later builds recompile only what changed.

## What you need

- **Windows 10/11, 64-bit**, with [MSYS2](https://www.msys2.org) installed at
  `C:\msys64`. From an MSYS2 shell, install:

      pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 \
          mingw-w64-ucrt-x86_64-python patch

  Use the **UCRT64** toolchain, not MINGW64 and not devkitPro's g++. If you
  installed MSYS2 elsewhere, set `UCRT64_BIN` to its `ucrt64/bin`.
- **Git for Windows** or an MSYS2 shell to run the scripts, and **Python 3.10+**.
- **Your ROM:** LEGO Star Wars II: The Original Trilogy (USA),
  SHA-1 `7c1afa070797945ba5a2577c7ddfded5cf2fc443`. Put it in `rom/`.
- **Your BIOS dumps:** `biosnds9.rom` and `biosnds7.rom` in `port/bios/`. The
  build lifts them and verifies each against its known hash. *Playing* needs
  no BIOS, because the executables fall back to the built-in FreeBIOS. The
  build needs the dumps because the runner links a retail-BIOS bank, and that
  bank cannot honestly be produced from anything else.

## Build

    bash tools/fetch_ndsrecomp.sh
    bash port/build.sh

`fetch_ndsrecomp.sh` downloads the framework,
[RetroPortingToolKit/ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp),
at the one commit `port/patches/` is written against, into `third_party/`. To
use a checkout elsewhere, set `NDSRECOMP_ROOT`.

`port/build.sh` then runs these stages. It stops loudly at the first failure and
names the log under `out/work/logs/`:

1. **preflight:** checks the toolchain, the ROM's SHA-1 and the BIOS dumps.
2. **extract:** unpacks the ROM into `out/raw/` on the first build: the ARM9
   and ARM7 programs, the sections they load at runtime, and the game's archive.
   Nothing extracted is ever committed (`.gitignore`).
3. **patch the framework:** applies `port/patches/` to it. This is safe to
   repeat, because a patch that is already applied is detected and skipped.
4. **build the recompiler**, then **lift:** with the entry-point lists in
   `port/lift/`, emits C for the four programs: ARM9 main, ARM9 ITCM, ARM7, and
   the ARM7's WRAM copy.
5. **build the runner and the shell:** compiles the lifted banks, the framework
   runtime, `port/frontend/` and `port/net/`, and links `build/lsw2.exe`. It
   copies the exe as `build/lsw2_bountyhunter.exe`, and copies the DLLs and the
   game's fonts (exported from your ROM) beside them.
6. **unit tests:** `tools/run_unit_tests.sh`. Set `LSW2_SKIP_TESTS=1` to skip
   them while iterating. The build then says it is unverified.

Run `build/lsw2_bountyhunter.exe` or `build/lsw2.exe` with no arguments.

### Options

| variable | effect |
|---|---|
| `NDSRECOMP_ROOT` | use this ndsrecomp checkout instead of `third_party/ndsrecomp` (or write its path, one line, in an untracked `.ndsrecomp_root` file at the repo root) |
| `UCRT64_BIN` | where the UCRT64 `g++` lives (default `/c/msys64/ucrt64/bin`) |
| `LSW2_SKIP_TESTS=1` | skip the unit suite at the end |
| `LSW2_ALLOW_MODDED_ROM=1` | build for an asset-modded ROM (`tools/build_asset_rom.py`) whose code is unchanged |

`bash port/build.sh clean` wipes the generated banks and objects first.

## Tests

    bash tools/run_unit_tests.sh

The suite has about 70 entries: C++ unit tests of the shell and networking
logic, which need no ROM, and Python sweeps of the source (unreached functions,
settings with no consumer, menu options that do nothing, and so on). Entries
that need the extracted ROM or a built executable report **skipped** in a
checkout that has neither. CI runs the suite on every push.

## Packaging

    py -3.13 tools/package_build.py --version my-test

This makes `dist/LSW2DS-PC-my-test.zip`, the layout a player unzips. See
[RELEASING.md](RELEASING.md) for real releases.

## The master server

The public lobby directory is a Cloudflare Worker, documented in
[services/master-server/README.md](services/master-server/README.md). You don't
need it to build or play: the game's default address points at the hosted one,
and `services/master-server-selfhost/` runs a private directory on your own
machine.

## Layout

| path | what is in it |
|---|---|
| `port/frontend/` | the shell: windows, presentation, the imgui menu, input, audio; the fork's menus and game-function overrides |
| `port/net/` | lockstep over TCP, the host-level wireless layer, master-server client, LAN, UPnP |
| `port/patches/` | changes to ndsrecomp, applied in order by the build |
| `port/lift/` | the lifter's entry points (address, mode, name) |
| `port/hle/` | readable C reimplementations of a few hot routines (experimental) |
| `port/game.toml` | the title config: ROM identity, memory layout, host settings |
| `tools/` | build helpers, extraction, reverse-engineering tools, the sweeps |
| `config/` | facts measured from the ROM that tools and tests use: tables, layouts, ids |
| `docs/`, `playbook/` | the research notes and the method |
| `services/` | the master server (Worker and self-host) and a TURN relay config |
