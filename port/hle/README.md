# `port/hle/` — where our own C goes

This is the modifiable layer. The lifted C is 29,890 machine-generated ARM9
functions at roughly 6.5 KB each; it is correct enough to run the game, and it
is not something anyone will read or edit. This directory is the readable
version, written a function at a time, and **verified against the lifted code
rather than trusted.**

## The rule that makes this safe

The framework states it plainly, and it is this project's own rule in someone
else's words:

> The recompiled/LLE implementation is the accuracy floor and remains linked,
> runnable, and forceable. Performance HLE is a set of measured replacements
> above that floor. A replacement may become the default path after promotion;
> it never deletes its LLE body or changes a miss into a Tier-3 interpreter
> call.
>
> — `HLE_ARCHITECTURE.md`

So a hand-written routine never *replaces* the lifted one in the build. Both
stay linked, and a policy chooses between them. The framework's
`HLE_ARCHITECTURE.md` defines the vocabulary:

| policy | behaviour |
|---|---|
| `off` | the lifted C only — the accuracy floor |
| `on` | our routine, falling back to lifted for unsupported input |
| `force` | our routine; a miss or fallback is **fatal** — proves it is hit |
| `verify` | **run both from the same input, keep the LLE result, report divergence** |

**At the pinned framework version these are design intent, not a working
switch.** `HLE_ARCHITECTURE.md` names `NDS_HLE=off|on|verify` as the intended
master control, but the runner does not read `NDS_HLE` anywhere, and the
recompiler's `--hle-manifest` path builds profiling descriptors rather than
substituting implementations. `verify.c` in this directory is the stand-in: it
drives the lifted routine and ours from identical machine state and compares
them register by register (see below).

`verify` is the important one either way. It is a differential oracle for our
own code: write the readable version, run both, and the comparison tells you
where the two disagree. This project has found that "the C looks right" is
worthless as evidence — six classes of silent mistranslation were found here,
three of which produce plausible wrong answers rather than crashes.

## Declaring a routine

A manifest passed to the recompiler with `--hle-manifest` (none is checked in
yet; `port/hle_manifest.toml` is the intended name):

```toml
version = 1

[program]
bank = "lsw2_arm9"
sha1 = "7a337bd2d2f5c42d771f5c865abf0c8a425d270e"   # of the UNPACKED image

[[routine]]
id          = "some_readable_name"
address     = 0x020XXXXX
end_address = 0x020XXXXX
mode        = "arm"
```

The parser is strict on purpose and rejects anything else: exactly the three
top-level keys, exactly `bank` and `sha1` under `[program]`, and exactly
`id`, `address`, `end_address`, `mode` per routine. The range must be aligned to
the mode (4 for ARM, 2 for Thumb) and non-empty, ids must be unique, and no two
routines may claim the same address range. A malformed manifest aborts rather
than being partially applied.

Note the `sha1` is the **image** hash, not the ROM's — for `lsw2_arm9` that is
the unpacked autoload section, because the ARM9 is BLZ-compressed and the packed
bytes are not what runs.

## The order of work

1. **Pick a function that matters.** Not an arbitrary one — something on the
   Bounty Hunter path, or something the profile says is hot.
2. **Read the ARM.** `tools/disasm_arm.py <image> <base> <lo> <hi>`. The first
   rule of this work is that the instructions contain the answer and the
   decompiled C frequently does not.
3. **Write the readable C** here, from the Ghidra output plus the
   disassembly — not from the Ghidra output alone.
4. **Verify it** and fix what diverges. The lifted result is authoritative; if
   they disagree, ours is wrong until proven otherwise.
5. **Only then** consider promoting it.

## What does not belong here

Behaviour the ROM does not specify. Host concerns — window layout, input
mapping, netplay, the unlocked save — live in the host layer and are flagged
where they are implemented. This directory is for *the game's own logic, made
readable*, and a divergence here is a defect rather than a feature.

---

## Status: nothing here is verified yet

`irq.c` holds two readable routines, `irq_disable` (`0x020CE804`) and
`irq_restore` (`0x020CE818`). Neither is proven against the lifted code. The
standalone harness that was meant to prove them turned out not to be
trustworthy, and the plan now is to verify inside a running game instead. The
sections below record how that was established, newest first.

### Standalone verification abandoned; verify inside a running game

The harness tried to reuse the runner's own start-up. Progressively:

| added | result |
|---|---|
| set `g_cpu.R[15]` to the guest entry address | needed - a lifted function switches on PC and `dispatch_miss`es otherwise |
| `runtime_call_push_return()` | needed - `bx lr` is only a C return when it matches the top of the call stack |
| `nds_io_reset()`, `runtime_init()`, `runtime_trace_reset()`, `nds_register_dispatch()` | still yields |
| `scheduler_init()` | still yields |

The diagnostic rules out four of the five yield conditions:

```
yield state: halted=0 dma_stalled=0 insn_stop=0 terminal=0
```

which leaves `nds_event_break_hit()` — the scheduler's event queue reading as
perpetually due because nothing is advancing time. Reproducing a booted
machine's timing state from outside the boot path is a rabbit hole, and each
step down it was a guess dressed up as a plan.

**The replacement: verify inside a running game instead of simulating one.**
The runner already brings up a fully initialised machine and runs the game to
the arena. A verification pass hooked into that — at a known point, snapshot
`g_cpu`, run the readable routine, compare against what the lifted one did —
needs no reconstruction, because the machine is genuinely running. That also
matches how a routine will actually be used, which a synthetic input sweep does
not.

**The lesson.** Three iterations went into an instrument rather than the work,
and each "one more piece" felt like the last. The check that would have stopped
it early: **before building an instrument, decide what its failure looks like
and whether you could tell that from a real result.** Here the failure printed a
clean, confident table of divergences. It took a relink that touched nothing
relevant, changing the answer, to expose it.

### The harness was not trustworthy — an earlier result RETRACTED

An earlier run reported `irq_disable ok 16/16` and `irq_restore diverged 6/16`
as a verified finding. **It is not verified**, and the harness that produced it
is not sound.

The evidence that it is unsound: the identical binary logic gives different
answers across runs. `irq_disable` passed 16/16 once, then later faulted on its
first input; re-ordering the two cases made *both* fault. A pre-call probe shows
the runtime is **not** unwinding before the call, so `runtime_should_yield()` is
returning true *during* the first guest instruction — and which of its five
conditions fires depends on globals that no one initialised:

```c
if (g_nds_insn_stop || nds_event_break_hit())              { unwind; return true; }
if (nds_cpu_halted(...) || nds_dma_cpu_stalled(...))       { unwind; return true; }
if (active_static_code_changed())                          { unwind; return true; }
```

Outside the runner, those read whatever memory happens to contain, and the
answer moves with link layout. **A result that changes when you reorder two
independent test cases is not a result.**

**What survives, and why.** The *conclusion* about `irq_restore` still stands,
because it does not rest on the harness. It rests on the ROM:

```
ORR r2, r2, r0      ; ALL 32 bits of r0, not just the I bit
MSR CPSR_c, r2      ; commits the low byte - and MSR cannot write T
```

The first readable version masked `R0` to the I-bit; the instruction does not.
That is readable straight off the disassembly and the corrected `irq.c`
implements it. What cannot currently be claimed is that it was *proven* against
the lifted code.

The harness had excluded `main.cpp` precisely to avoid the runner's start-up
path — which is exactly the initialisation it needs (scheduler, IO, memory map
and bank registry, all before any guest instruction executes). That is what led
to the start-up reuse described above.

### The first harness result, and the bug it pointed at

The very first version of the harness printed 32 confident divergences that were
pure artifact: the lifted routine never executed, so "lifted" equalled the input
in every row. A lifted function needs `g_cpu.R[15]` set to its guest entry
address (its first act is `if (g_cpu.R[15] != 0x...) switch (...) default:
runtime_dispatch_miss`), and the harness now **proves execution before it is
allowed to conclude anything**: PC must have advanced and the runtime must not
be unwinding, otherwise it reports `HARNESS FAULT` and refuses to compare.

With that fixed, it reported:

```
irq_disable 0x020CE804   ok          16/16 inputs
irq_restore 0x020CE818   diverged    6/16 inputs
```

(the result later retracted above as a verification, though its pointer to the
bug held up). The readable `irq_restore` masked `R0` to the I-bit, because that
is what the function is *for*. The guest does not:

```
ORR r2, r2, r0      ; ALL 32 bits of r0
MSR CPSR_c, r2      ; commits the low byte - but not T, which MSR ignores
```

With `R0=0xFFFFFFFF` the F bit (`0x40`) also gets set; with `R0=0x12345678` the
difference was the T bit. Both showed up as exact bit differences in the table,
and reading the disassembly again with those two facts in hand made it obvious.

That is the argument for this whole layer in one function: **the readable
version was plausible, confidently written, and wrong** — and nothing short of
executing both would have shown it.

After the fix, `irq_restore` reported `HARNESS FAULT ... unwinding=1` — the
lifted side stopped executing once `irq_disable`'s 16 runs had gone before it,
so runtime state was leaking between cases. The instrument said so rather than
guessing, which is the point.
