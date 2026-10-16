# Baseline and fork: what belongs where

The goal is a **baseline port** that is faithful and boring, an **enhanced port**
built on top of it, and a **Bounty Hunter section** carved out of it — without
those three ever getting tangled together. This document is the rule for which
change goes where, because that is the decision that will be made hundreds of
times, and getting it wrong once is enough to blur the layers.

## Layer 0 — the baseline

**Definition: the game, unchanged, running on a PC.**

Built by `port/build.sh` from the ROM in one command:

```
ROM ─► Ghidra corpus ─► program configs ─► 4 title banks ┐
                                                          ├─► nds_runner.exe
your BIOS dumps ──────► 4 BIOS banks ────────────────────┘
```

| bank | what | functions |
|---|---|---|
| `lsw2_arm9` | main code, unpacked autoload section 0 | 29,890 |
| `lsw2_arm9_itcm` | ITCM hot routines @ `0x01FF8000` | 610 |
| `lsw2_arm7` | cartridge ARM7 @ `0x02380000` | 5,947 |
| `lsw2_arm7_wram` | the copy the ARM7 makes into WRAM @ `0x037F8000` | 2,343 |

The two RAM banks are emitted `--validate-live-bytes`: that code is copied into
RAM at runtime, so dispatching on address alone would be trusting bytes that may
not be there yet.

**The rule for layer 0: nothing original.** Every value comes out of the ROM or
out of a verified dump. This layer's only job is to be *correct*, and its worth
is that it is the oracle everything above it is measured against. When something
higher up misbehaves, layer 0 answers "is that what the game does?"

Anything the ROM does not specify — window layout, input mapping — is host
plumbing and is flagged where it is implemented.

## Layer 1 — readable C (`port/hle/`)

**Definition: the same behaviour, written so a human can work with it.**

The lifted C runs the game and nobody will ever edit it — 29,890 machine
functions at ~6.5 KB each. Layer 1 replaces them one at a time with C written
from the Ghidra corpus *plus the disassembly*, declared in an HLE manifest
passed to the recompiler.

**The rule for layer 1: the lifted version stays linked and stays authoritative.**
Verification runs both from the same input and reports divergence, keeping the
lifted result. The framework's intended `NDS_HLE=verify` policy is not
implemented at the pinned version, so `port/hle/verify.c` does the comparison
itself. A hand-written routine is not "done" because it looks right — it is
done when verification is quiet. See `port/hle/README.md`.

This layer is *behaviour-preserving by construction*. A divergence here is a
bug, never a feature.

## Layer 2 — the shell (`port/frontend/`)

**Definition: things the DS could not do — and the UI to configure them.**

The baseline needs what a Shipwright-style port is expected to have:

- **imgui** for enhancement configuration and input binding
- **configurable resolution**, with the viewport auto-resizing to the window
  while the HUD keeps its own aspect ratio
- **a floating touch-screen panel inside the main window**, the way Shipwright
  pops out its item tracker — as well as the current separate window
- **debug and cheat tabs**

**None of that can live in the framework's frontend.** `runner/src/frontend.cpp`
owns the SDL windows and the present loop, there is no imgui anywhere in the
framework, and the framework is a pinned dependency rather than our tree: the
few changes the port needs in it are kept as patches in `port/patches/`, which
`port/build.sh` applies.

### The decision: our own shell over the framework's core

This is already proven possible. The layer-1 harness linked **every runner
object except `main.cpp`** and supplied its own `main()`, and it built and ran.
The framework's core — banks, runtime, scheduler, IO, GPU, audio, net — is
usable as a library.

So the split is:

```
ndsrecomp core (banks, runtime, scheduler, IO, GPU)   <- our "libultraship"
        +
port/frontend/  our window management, imgui, input binding, config
        =
the modular port
```

Excluding `frontend.cpp` and `main.cpp` and writing our own is the same move SoH
made: *libultraship* is their shell over the decompiled game, not the N64's.

**The rule for layer 2 is unchanged and matters more here**: it may change what
the player experiences, and it must not change what the game computes. Scaling
changes how a frame is presented; it must not change the projection the game
produced. Where an enhancement must reach into guest state, it is a title patch
at a named address, gated on the ROM SHA-1, and written down.

PC netplay, the touch screen as its own window, aspect-preserving scaling with
a tiled starfield, and 60fps while preserving the guest tick also live at this
layer.

`adaptive_widescreen` is off in `game.toml` today for exactly this reason: the
framework's own notes record offscreen geometry wrapping into the widened
region on another title, and LSW2's projection has not been audited.

## Layer 3 — the Bounty Hunter fork (`--mode bountyhunter`)

**Definition: a separate build, not a mode of the baseline.**

**CORRECTED.** Unlock seeding was originally put in `port/run.sh`. That was
wrong: the baseline does not strip currency/studs or force a 100% save and
unlocks. Those are reserved for the Bounty Hunter fork only.

It is not pedantry. **The baseline's entire value is being the reference every
fork is measured against.** The moment it quietly ships a modified save, the
question "does the fork differ from stock?" stops having an answer — and that is
the question this whole project exists to keep answerable.

So:

| | `--mode baseline` | `--mode bountyhunter` |
|---|---|---|
| save | whatever the game creates | seeded from the unlocked template |
| studs / currency | **untouched** | fork's decision |
| roster | as shipped | restriction / unrestriction |
| entry point | normal | arena-focused |
| host Extras (Old West, Medieval…) | — | host options |
| camera on right stick / mouse | — | fork binding |
| netplay | — | modern networking |

The fork owns content; the baseline owns fidelity.

**One directory, one launcher.** Both builds share `port/` — the same settings,
the same assets, the same banks and host layer — and differ only in save file
and content policy. `port/launch.sh --mode baseline|bountyhunter`. A separate
`fork/` tree was tried and removed: duplicating the directory duplicates the
settings and the assets, which is exactly what the shared layout exists to
avoid.

## Running it: only a ROM

Drop a valid cartridge dump in `rom/` and run `port/launch.sh`. Nothing else is
required — no BIOS dumps, no firmware, no extraction step.

That works because the runner embeds **FreeBIOS** (BSD-2-Clause) and can
synthesize firmware. **Verified rather than assumed:** on an identical
400M-cycle boot path FreeBIOS reaches the same ARM9 PC (`0x020CEA8C`) as retail
dumps, with dispatch counts within ~1%:

| | ARM9 hit/absent | ARM7 hit/absent |
|---|---|---|
| retail dumps | 1,666,678 / 808 | 7,705,233 / 396,682 |
| FreeBIOS | 1,650,821 / 778 | 7,708,154 / 397,717 |

If you *do* have your own dumps, drop them in `port/bios/` and they are used
automatically — that is the framework's oracle-diffed path and is strictly
preferred. An upgrade, never a requirement — for *running*. *Building* is
different: the framework's `nds_banks` target unconditionally links banks
recompiled from the retail BIOS, so `port/build.sh` needs `biosnds9.rom` and
`biosnds7.rom` in `port/bios/` and stops in preflight without them.

Asset extraction is only needed for **modding**, because the guest reads its own
content out of the cartridge natively — unlike SoH, which must convert N64
assets before it can render them. `port/launch.sh --extract-assets` when you
want to mod.

