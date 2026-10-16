# A playbook for decompiling and recompiling a Nintendo DS game

Written while doing it to LEGO Star Wars II: The Original Trilogy (NDS), and
written to be picked up by the next one - LEGO Battles is the intended next
target. Nothing here is theory. Every rule in it is here because breaking it
cost real time on this project, and the cost is named so you can judge whether
the rule applies to your case.

## Read these in order

| file | what it saves you from |
| --- | --- |
| [01-what-ghidra-silently-drops.md](01-what-ghidra-silently-drops.md) | the single largest source of bugs, by a wide margin |
| [02-automation-and-the-no-hand-edits-rule.md](02-automation-and-the-no-hand-edits-rule.md) | losing your work every time you re-export |
| [04-bring-up-order.md](04-bring-up-order.md) | months of debugging in the wrong order |
| [05-the-oracle.md](05-the-oracle.md) | arguing with yourself about what the hardware does |
| [06-ds-specific-gotchas.md](06-ds-specific-gotchas.md) | DS hardware details that differ from the GBA and from intuition |
| [07-starting-a-new-ds-project.md](07-starting-a-new-ds-project.md) | what to copy, and the order to do day one in |
| [08-standing-a-ds-game-up-on-a-recompiler.md](08-standing-a-ds-game-up-on-a-recompiler.md) | reimplementing an engine when you could recompile one |
| [09-the-process-end-to-end.md](09-the-process-end-to-end.md) | not knowing what the whole road looks like before you start |
| [10-finding-live-state-in-ram.md](10-finding-live-state-in-ram.md) | searching 4 MB with a filter that cannot work |
| [11-verifying-a-port-and-its-tools.md](11-verifying-a-port-and-its-tools.md) | trusting a tool that has never been checked against reality |
| [12-the-literal-pool-cross-reference.md](12-the-literal-pool-cross-reference.md) | reading decompiled C when you could search for who uses a value |

## The one-paragraph version

Ghidra's decompiled C is a *lossy* rendering of the instructions, and the
losses are silent, systematic, and concentrated exactly where a recompilation
breaks. Treat the C as a starting draft and the disassembly as the authority.
Never hand-edit the exported C - drive every correction from a config file
through a generator, so the corpus can be re-exported at any time without
losing a single fix. Build a differential against an emulator early, because
"it compiles and runs" tells you almost nothing. And instrument the port so a
stuck run *names the function it is stuck in*, because otherwise you will spend
your time reading code instead of reading evidence.

## The meta-lesson, stated once

> Nearly every blocker on this project was Ghidra silently losing information
> that the instructions still contained.

Dropped arguments. Dropped return values. Wrong symbol widths. Reordered reads
that have side effects. Each one produced a bug that looked like something
else entirely - a timing problem, a memory corruption, a regression in whatever
was edited last. The fix in every case was to go and read the ARM.

If you internalise one habit from this document, make it this: **when the C
does not explain the behaviour, disassemble the function before forming a
theory.** It is faster than the theory.
