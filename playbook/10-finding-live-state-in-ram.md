# Finding live game state in RAM

For LEGO Battles or any other DS title, this is the problem you hit the moment
you want a cheat menu, a warp, or a mod hook: you know what a value MEANS, you may
even know its layout in the save file, and you have no idea where it lives while
the game runs. The ROM does not say.

## The order to try things

**1. Search for it as-is.** If the value exists in a file you control - a save,
a config, an asset - load a copy whose contents you know and look for those bytes
in memory. `tools/ram_find.py` does this through the debug surface's
`read_region`, across mainram / wramshared / wram7 / itcm / dtcm.

This is cheap and it either succeeds or tells you something: **a failed search
means the value is transformed on load.** That is worth knowing before you spend
an afternoon on the wrong theory.

**2. Look for a PARTIAL match.** This is the step people skip. When LSW2's save
did not match, a diff still found five bytes of it at a fixed address - and
comparing the surrounding memory to the file showed the RAM struct drops the
file's magic and inserts a pointer. So it is not a copy, it is a different
structure sharing some fields. A fragment is a lead, not a coincidence.

**3. Differential search.** `tools/ram_diff.py`: capture several snapshots in each
of two states, keep addresses that are identical across every snapshot within a
state AND different between states. Frame counters and random allocations fail
the stability test; what survives is constant-while-running and depends on the
change.

## The trap in step 3, which cost real time

**The filter assumes the two states differ ONLY in the thing you are looking
for. Two separate boots do not satisfy that.** Different runs diverge in
allocation order, so large stretches of heap are stable within each run and
different between runs while having nothing to do with your change. Every
candidate the LSW2 unlock search produced was explicable that way, including a
20-byte run of 0xFF that looked exactly like an unlock bitfield and was not.

Practical consequences:

- **Prefer changing the value in a LIVE run** - in-game, over the debug
  surface, through a menu - so everything else keeps its identity. Restarting is
  the last resort, not the first.
- **If you must restart, reach the same point the same way**, and take the
  snapshots at the same moment in the same scene.
- **Narrow the region first** if you know the value is static rather than
  heap-allocated. Static data and heap look identical to the filter.
- **Treat a promising-looking hit as a hypothesis**, and check whether the
  surrounding memory is simply zero in one state - that is the signature of heap
  that was never touched, not of a flag that is clear.

## Read the existing notes before disassembling anything

The LSW2 level index was already recorded in the project's own docs, derived from
the disassembly, and the header that was supposed to expose it had it wrong -
storing a struct base under a name that claimed it was the value. Verifying the
documented chain against the running game took one script and two minutes:

    index = *(u8*)( *(u32*)(0x0210930C + 0x68) + 0x479 )

The general lesson is that **a name in a generated header is not evidence.** Read
it back from the live game before you build on it. Three wrong answers in this
project have come from trusting a recorded address or field width without
checking it against the running program - see also the struct-field-width trap.

## Reading is not writing

Knowing where a value lives lets you READ it. It does not mean writing it does
anything: the LSW2 level index is what the code reads when it wants to know where
you are, not the switch that takes you somewhere. Anything that should cause an
action needs the code path that performs it, which is a separate hunt.

Say which of the two you have. "We can read the current level" and "we can warp"
are very different claims.
