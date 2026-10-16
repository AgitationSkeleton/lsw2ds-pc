# Standing a DS game up on a recompiler

Written from doing it: LEGO Star Wars II went from "a corpus of Ghidra C that
never booted" to "the game running at 60fps with the arena playable" in one
session, on ndsrecomp. Most of the time went on four or five specific mistakes,
all of which are avoidable next time.

This is the order that worked and the traps in it. Some of it is
ndsrecomp-specific; the parts about *your own data* apply to any recompiler.

## 0. What the recompiler actually wants

Per program: an image, its load address, its entry PC, an identity hash, and a
list of function entry points with **ARM/Thumb mode**. That is all. The
entry-point list is the expensive part of a bring-up, and if you have done a
Ghidra pass you already have it.

## 1. A "program" is not the same thing as a file in the ROM

This is where the first day goes if you let it. LSW2 needed **four** programs and
only two of them are described by the ROM header:

| program | where | how it was found |
|---|---|---|
| ARM9 main | `0x02000000` | header — but see §2 |
| ARM7 | `0x02380000` | header |
| ARM9 ITCM | `0x01FF8000` | autoload section 1 |
| ARM7 WRAM copy | `0x037F8000` | savestate correlation |

The last two are **code at addresses nothing in the header mentions**, and until
they were banked, every instruction of them ran in the interpreter. The ARM7's
WRAM copy alone was 62 million interpreted instructions and was the entire
reason one area ran at 21 FPS while another ran at 60.

**Look for them before you profile, not after.** A DS game will copy code into
ITCM, into shared WRAM, and into the ARM7's own WRAM. Anything you find at a
RAM address must be emitted with live-byte validation, because dispatching on
address alone would trust bytes that may not be there yet.

## 2. The ARM9 image in the header is compressed

Covered in playbook/06 and repeated here because it bit this bring-up
immediately: header `size` is the **packed** size. Build the config from it and a
large fraction of your functions report as *outside the image* — 2,600 of 5,860
here. The instinct is to doubt the function list. Doubt the extent first.

## 3. Get the mode from the database, never from the bytes

Your corpus probably records addresses and not ARM/Thumb mode. It is tempting to
recover mode by matching prologues. Measured against Ghidra's `TMode` context
register over 5,860 functions, a reasonable prologue heuristic:

- agreed 5,400 times,
- said "unknown" 456 times (safe — those get omitted),
- and was **wrong 4 times**, every one a Thumb function called ARM.

A function lifted in the wrong instruction set is not subtly wrong, it is
entirely wrong. And the four errors all came from the rule added to *improve*
coverage from 4,800 to 5,400. **Widening a heuristic buys coverage and sells
accuracy, and you cannot see the price until you have something to check
against.** Ghidra knows exactly; export it (`ExportFunctionModes.java`).

The same applied to the WRAM program: 9 Thumb functions, of which the heuristic
found 4.

## 4. Tell it where the data is

A lifter discovers functions by following branches, and arbitrary bytes decode
into legal-looking ARM. Undeclared data therefore becomes "code":

```
swi  82,946 times   (a game issues almost none)
cdp  12,630 times   (ARM9 game code has essentially none)
and 350,554 times   ('and' is opcode 0000 - what random data decodes to)
```

Ghidra already knows. `ExportDataRanges.java` subtracts instruction coverage
from initialised memory; what is left is data by construction. Declaring it took
functions-containing-undefined-encodings from 853 to 242.

**Report "never disassembled" separately from "defined data".** Bytes Ghidra
never looked at are indistinguishable from data here, and fencing the lifter out
of real code reached only indirectly would be a silent, expensive mistake.

## 5. Expect your corpus to contain things that are not functions

417 entries in this ARM9 corpus sit on string tables and pointer arrays. Ghidra
flagged every one itself — `size=4` plus *"Control flow encountered bad
instruction data"* — and the corpus carried them as functions anyway. A byte
test (four printable bytes; an EQ-conditional word that is also a valid pointer)
agreed with Ghidra 417 times out of 417.

A stricter framework will *refuse to run* on the contradiction — ours did, on 26
entry points that fell inside declared data ranges. That is a feature. Fail
closed beats picking a side.

## 6. Then close the loop with coverage, carefully

Static discovery cannot follow a computed call, so it misses real functions.
Playing the game finds them: the runtime records every address that fell through
to the interpreter. Here that surfaced **8,816 executed addresses absent from
the Ghidra corpus entirely**.

**Two traps, both easy to fall into:**

1. **A coverage address is not necessarily a function entry.** Tier 3 records
   where interpretation *started*, often mid-function. Declaring an interior
   address as an entry point asserts a function begins there, and that is
   frequently false. Feed the list to Ghidra as *seeds*, let its analysis decide
   what is a function start, and take back only what it confirms.
2. **Coverage is not a scope set.** A tier-3 manifest lists what the banks
   *missed*, which is a tiny and unrepresentative slice of what ran. Using it to
   decide what a game mode needs would understate it enormously.

### Creating functions in bulk can destroy analysis. Snapshot first.

Running 2,027 coverage addresses through `CreateEntryPoints -Analyze` here:

```
functions      5,860  ->  7,692      (+1,832)
BUT 275 previously-known functions were GONE
instruction coverage  88.1%  ->  80.0%     (85 KB lost its disassembly)
```

Creating a function at an address that is really mid-instruction or data makes
Ghidra clear the conflicting disassembly around it, and the damage is silent —
the function *count* goes up, which reads like success.

Recovery was possible only because the previous mode export was **committed**:
diff old against new, get the 275 addresses, re-create them. That restored the
functions and part of the coverage (81.1%), but 29 were unrecoverable and the
"never disassembled" region stayed larger than before (15.2% vs 7.9%).

So:

- **Export and commit the function/mode list before any bulk mutation.** It is a
  few hundred KB and it is the only undo you have; the `.rep` is gitignored and
  there is no other snapshot.
- **Prefer a filtered list.** Addresses whose bytes do not look like a plausible
  function opening, or that fall inside a known data range, should be held back
  rather than fed in — the same tests used to find non-functions in the corpus
  apply here in reverse.
- **Re-export data ranges after, and check the "never disassembled" figure.** If
  it grew, analysis got *worse* even though function count grew. A larger
  never-disassembled region is not itself a correctness bug — the finder simply
  refuses to walk in, so real code there interprets instead of breaking — but it
  is lost ground and should be seen, not discovered later as mystery slowness.

The general shape of the mistake is worth naming, because it is not specific to
Ghidra: **an operation that improves the metric you are watching while quietly
degrading one you are not.** Function count went up 31%. Disassembly coverage
went down 8%. Only one of those was on screen.

## 7. Measure like for like or do not measure

Two interactive sessions of different lengths, following different routes,
produced an apparent 15,000× regression in interpreted ARM9 instructions.
Nothing had regressed; the second run reached gameplay, where the game leans on
code that was unbanked in *both* runs.

The same comparison done properly — same headless cycle budget, same boot path,
one variable changed — gave the real answer: **−78% ARM9 and −82% ARM7
interpreter dispatches.**

> A number that moves between two runs is not a result if anything else moved
> too.

Holding the route and the budget fixed costs almost nothing and converts an
alarming ratio into a trustworthy one.

## 8. Build-environment traps that cost real time

- **Not every `g++` on PATH has a C++ standard library.** devkitPro's compiles a
  bare `int main(){}` — so CMake's compiler check *passes* — then dies on the
  first real file with `fatal error: array: No such file or directory`.
- **Do not mix path flavours.** A POSIX cmake driving a Windows ninja fails with
  a missing-source error that reads like a broken compiler.
- **Shard the generated C.** Unsharded, this ARM9 bank was a single 242 MB
  translation unit. Frameworks also tend to *glob* body shards by name
  (`*_[0-9][0-9].c`), so an unsharded bank may be silently not picked up at all.
- **Never put a pipe between a command and its exit code** (playbook/02). This
  turned two failed builds into apparent successes in one session.
- **A running game holds its own executable open.** `Permission denied` at link
  time is not a build problem.

## 9. The payoff worth designing for

The single most valuable property of a tiered recompiler is that **the failure
mode of an uncompiled or mistrusted function is "slow", not "silently wrong"**.
This project spent most of its previous effort on six classes of silent
mistranslation, three of which produce plausible wrong answers rather than
crashes.

Once the game runs, the lifted build becomes an **oracle**: hand-written,
readable C can be checked against it automatically (`NDS_HLE=verify` runs both
and reports divergence). That is the way to get a maintainable C codebase that
is actually correct — write it second, against something that already works,
rather than first and hope.
