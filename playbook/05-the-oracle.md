# The oracle: settling disagreements with the real machine

## The principle

Two sources, and neither alone settles a claim:

- **The ROM** supplies the code and the data behind it.
- **The running console** - an emulator with a savestate - supplies addresses,
  pointers, and live memory.

A claim founded in both is solid. A claim founded in one is a hypothesis.

## Savestates are the highest-value tool in the project

A savestate is a complete, byte-exact snapshot of the machine at a moment you
chose. It answers questions no amount of static analysis will:

- **What is actually at this address at runtime?** The card-check bug here was
  solved by reading four words out of a savestate and seeing that hardware had
  the chip ID in two places while the port wrote one.
- **Where does this relocated block really live?** Slide the binary against the
  savestate's RAM looking for a byte-exact run. A 100% match over 48KB is not a
  coincidence, and the point where the match collapses tells you where code
  ends and data begins.
- **What hardware configuration does the game actually use?** Read the real
  register values and implement what is there instead of what the manual
  describes.

**Practical setup.** Vendor the emulator's source for its savestate *layout*
only, and read the layout from that source rather than reverse-engineering the
container. Watch for details that bite: this format's section header is
magic(4) + length(4) + **8 reserved bytes**, and assuming 8 bytes instead of 16
silently misaligns everything after the first section.

**Locating a savestate against the ROM.** Take a distinctive run of bytes from
an extracted section, find it in the savestate, and you have the base offset
for every subsequent address. A twenty-line helper that turns
"read 0x027FF800" into a number is worth writing on day one; this project used
one constantly.

### The correlation trap: matching a binary against a copy of itself

The most dangerous failure of this method, because the wrong answer looks
exactly like the right one.

A savestate contains **main RAM**, and main RAM contains **the binaries the
loader put there**. So searching a savestate for a signature from a ROM binary
can find the binary's own load image rather than the runtime region you meant
to test - and then every byte matches, because you are comparing the file with
a copy of the file.

On this project that produced a confident, wrong conclusion: a search for the
ARM7 WRAM block's first bytes returned 100% matches for 96KB and "proved" the
block was half again as large as recorded. It had actually found the ARM7
binary at 0x02380000 in main RAM. The signature occurred twice; at the second
occurrence - the real WRAM - the match collapsed to 1.6% exactly where the
existing extractor said it should.

Three habits that catch it:

- **A correct correlation stops.** Test one region past the end of the region
  you believe in. A match that continues past 0x0380FFFF is not ARM7 WRAM,
  because ARM7 WRAM ends there.
- **Enumerate every occurrence of the signature**, never just the first.
  `find()` returns whichever copy is earliest in the file, and main RAM is
  usually earlier than the peripheral memories.
- **Know where main RAM starts in the state file** and check whether your hit
  is inside it. Locate it independently - the ARM9 image at 0x02000000 is a
  reliable anchor - and then a hit inside `[main_base, main_base + 4MB)` is
  suspect by construction.

More generally: a correlation result that is *too* good deserves the same
suspicion as a text-level heuristic that returns too many hits (see
[01](01-what-ghidra-silently-drops.md) section 4). 100% over a large range is
evidence of an identity, and the identity might be "this file equals itself".

## What "verified" has to mean

**Translated is not verified.** Translated means it compiled and linked.
Verified means it agreed with the real machine on real input. A port can be
97% translated and 9% verified, and reporting the first number as progress
without the second is how a project convinces itself it is nearly finished.

The differential to build: run a function in the port and in the emulator from
the same inputs and the same memory state, and compare outputs and writes. Leaf
functions first - they need the least state set up - then work upward. This
project has 544 leaf functions through the differential against a closure of
1,555, and the report says so plainly rather than rounding it into the
translated figure.

## When the port disagrees with the console

The order of suspicion, learned the hard way:

1. **The translation.** Something Ghidra dropped. Go read the ARM.
2. **The host layer.** A register that is not implemented, an interrupt not
   raised, a piece of the system region not published.
3. **The game.** Essentially never.

The corresponding discipline: **translated code is not edited by preference.**
If recompiled behaviour looks wrong, the fault lies in the translation or in
the harness. Improving on the game's logic is not the job. Every time this rule
was tested here it held, and the temptation to "fix" the game was always the
symptom of a host-layer bug not yet found.

## Flag divergences at the code that causes them

Some divergences are unavoidable. A port that cannot preempt has to deliver
interrupts at a polling point. A host renderer will not be cycle-exact.

Write the flag **in the function that implements the divergence**, not in a
document. Documents are not read at the moment someone is debugging the thing
the document is about; the comment above the code is.

## Distinguish "measured" from "plausible" in your own comments

A comment reading "any plausible mask ROM answers" next to a hardcoded chip ID
is a note that the value was *chosen*. A comment reading "read from a savestate
of the real console at 0x027FF800" is a note that it was *measured*. These look
similar in the source and mean entirely different things when someone is
deciding whether to trust the value six weeks later.

Get in the habit of writing which one it is, every time. A number with no
source does more harm than a gap that is admitted, because a gap gets filled and a wrong
number gets built on.
