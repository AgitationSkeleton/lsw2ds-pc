# LSW2 DS → PC port, on ndsrecomp

The port targets the ndsrecomp framework rather than compiling Ghidra's C
directly. The short version of why: ndsrecomp falls back to an ARM interpreter
running the real instructions for anything not compiled, so a mistranslated
function degrades to *slow* instead of *silently wrong*, and this project has
found six distinct classes of silent mistranslation.

## What works right now

**The baseline is complete and reproducible.** `port/build.sh` goes from the ROM
to a running executable in one command; `port/launch.sh` runs it.

### Running it

```sh
port/build.sh                        # ROM -> banks -> build/lsw2.exe
port/launch.sh                       # baseline: the game as shipped
port/launch.sh --mode bountyhunter   # the fork: unlocked save
port/launch.sh --extract-assets      # unpack the archive for modding
```

**The executables land in `build/`** — `build/lsw2.exe` (the baseline) and
`build/lsw2_bountyhunter.exe` (the same binary under a second name; the
executable picks its build from its own filename), with the runtime DLLs beside
them and a `PlayLSW2.bat` launcher so they also run when double-clicked. The
build system's own output tree (`out/work/port-build/`) is where objects live
and is a silly place to go looking for a game.

**Esc and F1 both open the settings menu**, and neither quits. Close the window
to quit. Escape used to quit outright, which is a hostile default: the first
thing anyone does with a new build is press Escape expecting a menu.

**Running it needs only a ROM.** Drop a valid cartridge dump in `rom/` — no BIOS
dumps, no firmware, no extraction step. FreeBIOS is embedded and was verified
equivalent to retail dumps on an identical boot path (same ARM9 PC, dispatch
counts within ~1%). Your own dumps in `port/bios/` are used automatically if
present; that is the oracle-diffed path and is strictly preferred, but for
running it is an upgrade rather than a requirement.

**Building it needs the retail BIOS dumps.** The framework links banks
recompiled from them unconditionally (see "Why the build needs BIOS dumps"
below), so `port/build.sh` stops in preflight unless `port/bios/biosnds9.rom`
and `port/bios/biosnds7.rom` are present.

### The shell

Our own frontend over the framework's core — the same division Ship of Harkinian
makes with libultraship. We link every runner object except `frontend.cpp` and
supply our own, which keeps all of the setup and gives us the present loop.

| | |
|---|---|
| screens | top gameplay, bottom touch, both correct |
| frame rate | 60 FPS, clocked by audio consumption |
| audio | 32728 Hz stereo, the exact mixer rate |
| windows | side by side, resizable, aspect-preserving, letterboxed |
| settings UI | imgui, Esc or F1 — Video / Input / Touch / Debug / Cheats (baseline only) / Multiplayer |
| renderer | OpenGL compute renderer presented directly by the shell (default), software rasteriser as fallback |
| input | keyboard + gamepad, **rebindable**, persisted by action name |
| resolution | 1x–6x window presets and a scale slider; 3D rendered at 1x–4x; xBR texture upscaling 2x/4x |
| floating touch | the bottom screen inside the main window, clickable |
| instances | `port/launch.sh` refuses a second ordinary launch; `--instance N` runs another copy with its own save, settings and ports |
| remote control | screenshots and input injection on 127.0.0.1:19842 (instance N: 19842 + N - 1) |

The Cheats tab exists only in the baseline, and nothing in it is applied at
startup or unless you press it. The baseline runs the game as shipped — that is
its entire value, because it is the reference the fork is measured against. The
fork has no Cheats tab at all: its unlocks are granted wholesale at startup and
its match rules come from the host.

### Four programs, not two

Only the first two are described by the ROM header. The other two are code at
addresses nothing mentions, and until they were banked every instruction of them
ran in the interpreter.

| bank | region | functions |
|---|---|---|
| `lsw2_arm9` | unpacked autoload section 0 | 32,915 |
| `lsw2_arm9_itcm` | ITCM `0x01FF8000` | 610 |
| `lsw2_arm7` | cartridge `0x02380000` | 5,947 |
| `lsw2_arm7_wram` | ARM7's WRAM copy `0x037F8000` | 4,859 |

The two RAM banks are emitted `--validate-live-bytes`: that code is copied into
RAM at runtime, so dispatching on address alone would trust bytes that may not
be there yet.

### Rendering

The shell presents the framework's **OpenGL compute renderer** itself
(`frontend/glpresent.cpp`), and that is the default. It needs OpenGL 4.3; when
the compute renderer cannot start, the shell falls back to the software
rasteriser — the framework's accuracy floor — and the startup log and the Video
tab both say so. Under OpenGL the Video tab offers a 3D resolution multiplier
(1x–4x) and xBR texture upscaling (2x or 4x); both need a restart.

**Known defect:** above 1x on either setting, the compute renderer's HD
compositing path draws the Arena Lobby's Cantina hologram as a solid white shape
instead of a translucent one. 1x/1x is correct, and the software renderer is
correct at any setting. The Video tab warns about this when it applies.

**Adaptive widescreen** is a Video-tab option, off by default. It is a title
patch rather than a presentation setting — the game culls actors against the
original 256-pixel screen, so widening the view needs a guest-side change — and
is described in `frontend/widescreen.cpp`. `game.toml` keeps the framework's own
`adaptive_widescreen` off; see "Configuration" below.

### Known gaps

- Layer-1 readable C is unverified — see `port/hle/README.md`.

### Interpreter dispatches, same 400M-cycle boot path each time

| | ARM9 | ARM7 |
|---|---|---|
| 2 banks | 79,360 | 4,494,691 |
| + ITCM & WRAM banks | 17,337 | 814,449 |
| + 2,084 coverage-found entry points | **808** | 814,449 |

ARM9 is down **99%** and effectively fully compiled. Each row changes one
variable against a fixed budget and route — an earlier comparison that let the
play route move as well produced an apparent 15,000× regression that turned out
to be nothing at all.

The corpus itself grew from runtime evidence: addresses that fell through to the
interpreter were fed back to Ghidra, which confirmed most of them as real
function starts (ARM9 5,860 → 7,935; ARM7 WRAM 475 → 3,358). **Snapshot the mode
export before doing that** — creating functions in bulk destroyed 275 ARM9 and
21 WRAM functions, and the committed snapshot was the only way back.

## BIOS and firmware

With retail dumps, both BIOS images must match the SHA-1 the framework demands:

```
port/bios/biosnds9.rom    4096  bfaac75f101c135e32e2aaf541de6b1be4c8c62d
port/bios/biosnds7.rom   16384  24f67bdea115a2c847c8813a262502ee1607b7df
```

Then the port takes the **retail, oracle-diffed path**, not the BSD-2-Clause
replacement. All four BIOS banks generate:

```
arm9_bios     167 functions      freebios_arm9   70 functions
arm7_bios     637 functions      freebios_arm7   72 functions
```

The FreeBIOS images are not shipped as files — the framework's
`third_party/freebios` is an empty submodule — but they are embedded in
`vendor/freebios/freebios_images.cpp` as byte arrays, and extracting them
(`tools/extract_freebios.py`, run by `port/build.sh`) reproduces the pinned
hashes exactly (`b85d6afd…`, `eaab9b21…`). They are always linked;
`port/launch.sh` selects them at runtime only when no retail dumps are present.

**A firmware dump is not used.** The framework's firmware RAM banks are captured
against one specific image (`ae22de59…`), and a dump from any other console
(for example `87dae250…`) does not match it. That is not a defect in the dump,
but pairing it with banks captured against a different image would be unsound.
`NDS_BOOTSTRAP_FIRMWARE=ON` drops the firmware-bank requirement entirely, the
runner synthesizes firmware (`--generated-firmware`), and `boot = "direct"` —
which the port uses anyway to skip the boot sequence — skips the menu.

**The dumps are copyrighted and are gitignored.** `port/bios/` is excluded; the
hashes above are recorded, the bytes never are.

<details>
<summary>Why the build needs BIOS dumps</summary>

The runner links a `nds_banks` target whose sources are **unconditional**
(`runner/CMakeLists.txt:140`):

```cmake
add_library(nds_banks STATIC
    ${GEN}/arm9_bios.c      ${GEN}/arm9_bios_dispatch.c
    ${GEN}/arm7_bios.c      ${GEN}/arm7_bios_dispatch.c
    ${GEN}/freebios_arm9.c  ${GEN}/freebios_arm9_dispatch.c
    ${GEN}/freebios_arm7.c  ${GEN}/freebios_arm7_dispatch.c
    ${FW_BANK_BODIES} ${FW_BANK_DISPATCH} ...)
```

`arm9_bios.c` and `arm7_bios.c` are recompiled from **retail DS BIOS dumps**,
and there is no CMake option that omits them — `NDS_BOOTSTRAP_FIRMWARE=ON` does
not, and neither does the title-bank path. So selecting FreeBIOS at *runtime*
does not remove the retail banks from the *link*.

| file | what it is | identity |
|---|---|---|
| `bios/biosnds9.rom` | retail ARM9 BIOS, 4 KB | sha1 `bfaac75f101c135e32e2aaf541de6b1be4c8c62d` |
| `bios/biosnds7.rom` | retail ARM7 BIOS, 16 KB | sha1 checked by the config |
| `bios/firmware_banks/*` | 23 firmware RAM-bank captures | from a live capture setup; not needed with `NDS_BOOTSTRAP_FIRMWARE=ON` |

These have to be dumped from your own DS hardware. They are not something this
project can synthesize, and the SHA-1 checks mean a substitute will be rejected
rather than silently accepted — which is the correct behaviour.

</details>

## Build recipe

`port/build.sh` does all of this; the runner step on its own is:

```sh
# MSYS2's UCRT64 toolchain first on PATH - NOT devkitPro's g++, which has no
# C++ standard library headers
export PATH="<msys2>/ucrt64/bin:<msys2>/usr/bin:$PATH"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNDS_GENERATED_DIR=<repo>/out/work/ndsrecomp/gen_bios \
  -DNDS_BOOTSTRAP_FIRMWARE=ON \
  -DNDS_TITLE_BANK_DIR=<repo>/out/work/ndsrecomp/generated \
  -DNDS_TITLE_ROM_SHA1=7c1afa070797945ba5a2577c7ddfded5cf2fc443 \
  -B <build> <ndsrecomp>/runner
cmake --build <build> -j 6
```

`NDS_GENERATED_DIR` matters: without it the runner writes and reads banks inside
the framework checkout. Ours go in this repo, so the framework tree stays
pristine apart from the patches in `port/patches/`.

The framework itself is fetched by `tools/fetch_ndsrecomp.sh` into
`third_party/ndsrecomp`, or found through `NDSRECOMP_ROOT`; `UCRT64_BIN` points
the build at a UCRT64 `bin` directory other than the default.

## What is still worth doing

- reduce the 29,890 discovered functions toward the real count — the finder
  reports **624 data-range collisions**, each a function whose body runs off the
  end of the code Ghidra found;
- `swi` still decodes 28,169 times against an expected ~57, so data-in-code
  remains undeclared;
- export ARM7 modes with `ExportFunctionModes.java` (ARM9 is done).

## Configuration

`game.toml` carries the ROM identity and the host settings, with every
divergence from the cartridge marked. Two are deliberate: the save device is a
512-byte EEPROM (`save_type = "eeprom-tiny"`) — present because the game probes
it at boot and refuses to continue without it, even though the design persists
nothing meaningful through it — and `boot = "direct"`, which skips the boot
sequence.

The framework's `adaptive_widescreen` is **off on purpose**. Scaling has to be
delicate and aspect-preserving; the framework's widescreen path widens the 3D
viewport, and the Prime Hunters notes record offscreen geometry wrapping into
the side regions because of it. Native first, widen after the projection is
audited — which is what the shell's own off-by-default widescreen patch does.

## Licence note

ndsrecomp vendors melonDS's GPU3D and Wi-Fi under the GPL. Building on it means
inheriting those terms. This project previously wrote its own rasteriser from
the hardware specification specifically to avoid that; on this path the choice
is already made. Nothing is copied from ndsrecomp or from the Prime Hunters
port — both are references for the shape of the problem and carry their own
licences.
