# Bringing a DS game to PC: the process, start to finish

Written for whoever does this next — LEGO Battles, or any DS title. It is the
route that worked for LEGO Star Wars II in one session, in order, with the
decisions that actually mattered and the traps that cost the most time.

Read `08-standing-a-ds-game-up-on-a-recompiler.md` alongside this: 08 is the
mechanics, this is the plan.

## The shape of the thing you are building

Copy the split a mature port uses (Ship of Harkinian for Ocarina of Time). It is
**three separable pieces**, and conflating them is the single biggest risk:

| piece | SoH | yours |
|---|---|---|
| the game's behaviour | a hand decompilation | **lifted banks from a recompiler** |
| the shell | libultraship | your own frontend over the recompiler's core |
| content | OTR archives | the ROM's archive, rebuilt with overrides |

**Most of what people mean by "modular" is the shell and the content, not the
decompilation.** That matters enormously, because the decompilation is the one
piece you cannot produce quickly — OoT's took a large community years. A port
that waits on it never ships.

So: **do not gate the port on decompiling the game.** Get behaviour from a
recompiler, modularity from the shell and the assets, and make code readable
incrementally where it earns its place.

## The order that works

### 1. Extract, and check what you extracted

ROM header, ARM9/ARM7 images, overlay tables, NitroFS. Then the game's own
archive format, if it has one.

Two things to establish immediately because everything downstream depends on
them:

- **Is the ARM9 compressed?** Usually yes (BLZ). The header `size` field is the
  *packed* size and the image that runs is bigger. If a lot of known-good
  functions later report as "outside the image", this is why.
- **How many programs are there really?** Not "ARM9 and ARM7". LSW2 needed
  **four**: main code, ITCM, the cartridge ARM7, and a copy the ARM7 makes of
  itself in shared WRAM. The last two are not in the header, and until they were
  banked, every instruction of them ran interpreted — the difference between 21
  FPS and 60.

### 2. Get a function corpus, with modes

Ghidra, headless. What you need out of it is not the decompiled C — it is
**addresses plus ARM/Thumb mode**, and the mode must come from Ghidra's `TMode`
context register, never from guessing at prologue bytes. See 08 §3 for the
measurement: a reasonable heuristic was wrong 4 times in 5,860, and every error
was a Thumb function called ARM, which lifts to something entirely wrong.

**Export the function list and commit it before you mutate anything.** It is a
few hundred KB and it is the only undo you have.

### 3. Tell the lifter where the data is

Subtract instruction coverage from initialised memory; what remains is data by
construction. Undeclared data gets decoded as code, and arbitrary bytes decode
into legal-looking ARM — 82,946 `swi` instructions in a game that issues almost
none.

### 4. Build, run, and measure honestly

Then close the loop with **runtime coverage**: play the game, harvest the
addresses that fell through to the interpreter, feed them back to Ghidra as
seeds, take back what it confirms as function starts. That took ARM9 interpreter
dispatches from 79,360 to 808.

Two rules or the numbers will lie to you:

- **A coverage address is not necessarily a function entry.** It is where
  interpretation *started*, often mid-function. Feed the list to Ghidra and let
  its analysis decide; do not declare them entry points directly.
- **Measure like for like.** Same cycle budget, same route, one variable. Two
  play sessions of different length produced an apparent 15,000× regression that
  was nothing at all.

### 5. Assets before code

This is the step most likely to be deferred and should not be. Find the game's
archive, learn its index format, and build a tool that **rebuilds it with
replacements**.

LSW2's was a `u32` offset table of N+1 entries followed by payloads. The N+1
shape is common and it is the enabling detail: an asset's extent is derived from
the table, so **replacements need not be the same size**.

Validate it with an **identity round-trip** — rebuild with one asset written
back unchanged and confirm the output is byte-identical to the original ROM.
Any error in the table, the concatenation, the FAT or the header CRC changes the
hash, so a matching hash proves all four at once.

Watch for: the cartridge address mask requires a **power-of-two ROM size**, so a
grown image must be padded and the header capacity field updated; and a modded
ROM's changed SHA-1 will stop your banks registering unless the runner is
reconfigured.

### 6. Your own shell

Exclude the framework's `main.cpp` and `frontend.cpp`, link the rest as a
library, and write your own window management, imgui, input binding and config.
This is where a port stops being an emulator with a fixed ROM and starts being
a customizable thing.

### 7. Readable C, only where it earns its place

Not a blanket rewrite. Replace a lifted function when you want to **change**
its behaviour or when it is central enough to be worth owning — and verify each
one against the lifted original, because the lifted build is an executable
specification and that is a luxury a from-scratch decompilation does not have.

## The traps, ranked by what they cost here

1. **Applying a requirement at the wrong layer.** "No save system" is a content
   statement; it was implemented as `save_type = "none"`, which is a hardware
   fact, and the game refused to boot with *"could not detect DS data"*. Write
   down which layer owns what, early, and check against it.
2. **Bulk-creating functions in Ghidra.** 2,027 coverage addresses through
   `CreateEntryPoints -Analyze` destroyed 275 existing functions and 8% of
   disassembly coverage — while the function *count* rose, which reads as
   success. Snapshot first.
3. **An instrument whose failure looks like a result.** A verification harness
   printed 32 confident divergences while the code under test never executed.
   What exposed it was a relink that touched nothing relevant and changed the
   answer. **Before building an instrument, decide what its failure would look
   like and whether you could tell it from a real finding.**
4. **Chasing initialisation.** Three iterations went into reconstructing a booted
   machine outside the boot path. Each "one more piece" felt like the last. If
   you need a running machine, run one.
5. **Pipes eating exit codes.** `cmd | head` then `$?` reports *head's* status.
   This turned two failed builds into apparent successes in one session.
6. **The wrong compiler.** A `g++` with no C++ standard library still passes
   CMake's compiler check, because that check compiles a bare `main()`.

## What to automate first

In the order they pay off:

- **config generation from the corpus** — addresses, modes, data ranges, all
  emitted from the Ghidra database rather than hand-maintained;
- **one-command build** — ROM to running executable, every stage logging and
  checking its own exit code;
- **coverage harvest** — runtime misses back into Ghidra seeds;
- **asset rebuild** — with an identity round-trip as its self-test.

Each of those was written once here and then used many times. The build script
in particular: every stage in it was got wrong by hand at least once, and a
script that fails loudly is the only version that stays true a month later.
