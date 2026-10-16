# DS hardware details that will catch you out

Collected while implementing them. Several of these are places where DS
differs from GBA in a way that is easy to miss because so much else carries
over, and where the wrong answer produces plausible-looking output rather than
an obvious failure.

## 2D graphics

**Character base is four bits on the DS, two on the GBA.**

```c
unsigned char_base = (cnt >> 2) & 15u;   /* DS  */
unsigned char_base = (cnt >> 2) & 3u;    /* GBA - wrong here */
```

A char base of 4 read as 0 points tile fetches at the wrong VRAM entirely. The
output is not blank - it is *other real data*, rendered convincingly. This
project looked at the resulting cyan stripes, found that tile 0 at the wrong
base genuinely was a stripe pattern and that palette index 4 genuinely was
cyan, and concluded the stripes were correct. They were not. Bear this in mind
generally: **a wrong base address produces output that can be justified**, so
"I can explain what I am seeing" is not evidence that it is right.

**VRAM banks appear in the LCDC region only while their MST is 0.** Get the
bank mapping exact and derive texture and texture-palette slots from it rather
than assuming a fixed layout.

**Count what you do not implement.** Windows, blending, mosaic and extended
palettes were decoded and counted rather than drawn here. When something looks
wrong, the report says whether an unimplemented feature was even requested,
which converts "the image is wrong somehow" into "the game enabled a window on
BG2 four times".

## 3D graphics

Measure the feature set before implementing. This game uses 4x4 compressed
textures heavily, always W-buffering, and modulation plus shadow blending; it
never touches fog, edge marking, alpha test, A3I5 or direct colour. Measuring
first removed a large amount of work.

## Interrupts

- `IF` is **write-one-to-clear**. Writing zero does nothing. Writing the wrong
  *width* also does nothing useful - a one-byte write to a register you needed
  to clear as a word leaves the flag set, and the handler re-enters
  immediately. That produced 327,608 handler entries here.
- Keep a wrapping ring of the last N interrupt events and print it on exit.
  Bounded cost, permanently affordable, and it is almost always the last 40
  events that matter.

## DMA

- Trigger on the **0 -> 1 edge** of the enable bit. Never on level. Code
  commonly does `*(ushort*)0x040000BA &= 0xc5ff`, which *preserves* bit 15 - on
  a level trigger that re-runs the channel, typically from addresses that have
  since been zeroed.
- Range-check source and destination and **report** refusals. A refused
  transfer named in the output is a lead; a fault is a debugging session.

## IPC

- Two 16-word queues, and **reading `IPCFIFORECV` pops a word**. This is the
  canonical side-effecting read: see section 5 of
  [01](01-what-ghidra-silently-drops.md) for how a decompiler reordering it
  above its guard silently ate the ARM7's reply.
- `IPCFIFOCNT` is a halfword register. A wrongly-typed symbol sent messages to
  0x0400018C instead of 0x04000188.

## The system region in main RAM

The firmware leaves data the game depends on. Publish all of it:

```
0x027FF800 / 0x027FF804    chip ID, twice
0x027FFC00 / 0x027FFC04    chip ID again, the mirrored copy
0x027FFC40                 boot indicator
0x027FFE00                 ROM header, 0x160 bytes
```

Both chip ID copies matter. This game's card-removal check picks *which* copy
to compare against based on a halfword elsewhere in the region, so shipping one
of the two is enough to boot and not enough to keep running - the game halts
exactly as it would if you swapped the cartridge mid-play.

More generally: **publish the region from a savestate readout, not from the
parts you think the game needs.** The cost of publishing a value nothing reads
is zero. The cost of missing one is a shutdown that looks like a crash.

## The ARM7

Do not treat it as optional. The ARM9 waits on it constantly, and the sound
system lives there. Extract its relocated WRAM block and decompile it at its
runtime address, or a large part of the audio path simply is not in your corpus.

Measured values beat derived ones: the sound channel mask here (`0x00037FF0`)
was read out of savestates rather than reasoned about.

## The cartridge

Command `0xB7` reads data at a 32-bit address, 512 bytes per command; `0xB8`
returns the chip ID. The command bytes are big-endian in the register because
the game writes them a byte at a time.

Implement the commands the game issues and **report the ones it issues that you
have not implemented**, by name, rather than returning zero and letting it
proceed with a wrong answer.

## The ARM9 binary is compressed, and the header lies about its size

Nearly every commercial DS ARM9 is BLZ-compressed. **The `size` field at header
+0x2C is the PACKED size**, and the image it describes is not the image the
game runs. For LSW2:

```
header:    ram=0x02000000  size=0xA3F24   -> 0x02000000..0x020A3F24
unpacked:  0x107238 bytes, in three autoload sections
    0x02000000  0x1019C0   code
    0x01FF8000  0x55C0     ITCM
    0x027E0000  0x2A0      DTCM
```

The code section alone is 0x5E000 bytes larger than the header's extent. Any
tool that maps a function address back to its bytes — a mode classifier, a
prologue check, a disassembler, a lifter config — **must read the unpacked
autoload sections, not `rom[rom_off : rom_off+size]`.**

The symptom is loud and easy to misread: a large fraction of known-good
functions report as *outside the image* (2,600 of 5,860 here). The instinct is
to doubt the function list. Doubt the extent first — check whether the highest
function address exceeds `ram + size`, because if it does the list is fine and
the window is wrong.

Note that the DTCM section lands at 0x027E0000, which is where DS games keep
system-region structures. If a "static data" address in that range seems to be
initialised by nobody, this is why: it arrives with the binary.

## Ghidra creates functions on data, and it tells you so

Treating a Ghidra function list as an entry-point corpus imports its mistakes.
In this ARM9, **1,040 of 5,860 (17.8%)** carry

```c
/* WARNING: Control flow encountered bad instruction data */
```

and 731 of those are `size=4` — string tables and pointer arrays that got a
function created on them. They are not functions.

Two cheap filters, which agreed with each other 417 times out of 417 here:

1. **Ghidra's own warning plus a tiny size.** `size <= 8` together with the
   bad-instruction warning means the entry itself is junk.
2. **Read the first word.** Four printable bytes is a string. A word that is
   simultaneously a valid pointer into the image *and* has condition `0000`
   (EQ) is a pointer table — no compiler opens a function with an
   EQ-conditional instruction, because nothing has been compared yet.

The residue matters more than the junk: the same warning on a **large**
function means the body was translated up to the bad data and then stopped,
usually ending in `halt_baddata()`. That is a silently truncated function, and
it is the one case here where the C compiles, runs, and is simply missing its
back half. Enumerate those separately and check every one is either fixed or
flagged — 32 large ones here, all accounted for, none silently compiled.
