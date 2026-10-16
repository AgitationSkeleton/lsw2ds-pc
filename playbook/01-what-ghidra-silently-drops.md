# What Ghidra silently drops, and how to catch each one

Ghidra's decompiler is excellent at producing C that *reads* like the function.
It is not trying to produce C that *is* the function, and for a recompilation
that difference is the whole problem. What follows is every class of loss this
project hit, with the bug it caused, how it presented, and the automated check
that catches it.

The presentations matter as much as the causes. Almost none of these looked
like "the decompiler was wrong". They looked like memory corruption, timing
bugs, or a regression in whatever file was edited most recently.

---

## 1. Dropped call arguments

**What happens.** Ghidra decides a function takes N parameters. At a call site
where the caller sets up more registers than that, the extra ones are simply
not written. `FUN_020ce818(r0)` becomes `FUN_020ce818()`.

**Why it is fatal for a recomp.** Built with `-mregparm=3` to reproduce the ARM
r0-r3 convention, a call written with no arguments passes whatever happened to
be in EAX, EDX and ECX. The callee reads garbage that changes depending on
unrelated code.

**How it presented.** A crash inside a BIOS `CpuSet` that appeared and vanished
as other files were edited, and was twice misdiagnosed as a regression in the
change under test.

**Worst case.** 169 of 170 call sites for one function dropped its argument.

**The check.** Disassemble every call site and do register dataflow backwards
from the branch: which of r0-r3 are written before the call and never consumed?
If a register is set from a literal or an immediate you can restore it
mechanically and safely. If it came from memory or another call, *do not guess*
- flag it and leave it. See `tools/gen_call_patches.py` and
`tools/gen_irq_patches.py` in this repo; the latter repaired 67 of 169 sites
and correctly refused the other 102.

**The rule.** A repair tool that guesses is worse than no tool. Make it prove
the value or decline, and make it *report* how many it declined, so the
remaining risk is a number you can see rather than an assumption.

---

## 1b. Dropped arguments on INDIRECT calls - the same defect, much bigger

Section 1 is about direct calls. The same thing happens to calls through a
function pointer, and in an object-oriented game engine that is where most of
the calls are:

```c
(**(code **)(*(int *)(param_1 + (uint)(1 < param_4) * 0x38) + 8))();
```

**Scale on this project: 891 indirect calls written with no arguments, 456 of
them through a vtable, across 702 files.** The direct-call case was 170 sites
for one function; this is an order of magnitude larger and it is where a C++
style codebase spends its time.

**What it costs.** The callee gets whatever is in EAX/EDX/ECX. Here that meant
a `this` pointer of garbage, which became a garbage pool pointer, which four
calls later had a free-list allocator dereferencing `0xEB000311` - *an ARM
instruction word*. Nothing in the crash report connected back to the dispatch
site.

**A detail the C hides completely.** Read the instructions:

```
0x02062840  E7B02002  LDR r2, [r0, +r2]!    ; WRITEBACK: r0 = param_1 + offset
0x02062844  E5922008  LDR r2, [r2, #+0x8]   ; r2 = vtable[2]
0x02062848  E12FFF32  BLX r2
```

The `!` is the whole point: the callee's first argument is the *subobject*
address, not `param_1`. No amount of reading the decompiled C recovers that.

Also worth knowing: by the time the BLX executes, r2 holds the function pointer
itself, so a callee taking three register arguments would receive its own
address as the third. In practice that tells you these vtable entries take at
most two - a useful constraint when deciding how many arguments to restore.

**The approach.** Same as section 1 and worth building early: dataflow at the
BLX, restore what can be proved, refuse the rest, and report the refusals.

---

## 2. Dropped return values

**What happens.** A function whose result is used only through a register the
decompiler did not track gets typed `void`.

**Consequence.** Every caller silently reads garbage in place of the result.

**The check.** Scan for functions typed `void` whose ARM ends by setting r0
in a way that reaches the return. Retyping to `undefined4` and re-decompiling
fixes it wholesale - `tools/ghidra_scripts/FixVoidReturns.java` retyped 107
functions with zero failures.

**The lesson.** This one is cheap and high-yield, and it is *scriptable inside
Ghidra*. Do it before you export anything.

---

## 3. Wrong element width on a data symbol - the expensive one

**What happens.** Ghidra types a `DAT_` symbol as a pointer to something, and
pointer arithmetic in C scales by the element size. Get the size wrong and
every offset is wrong by that factor - silently, with no warning, and with C
that looks entirely reasonable.

**This class alone caused three separate multi-hour bugs on this project:**

| symbol | Ghidra's type | truth | what broke |
| --- | --- | --- | --- |
| `&DAT_x + n` generally | `undefined4` | byte-addressed | DMA channel 3's source register went to 0x04000140 instead of 0x040000D4; **276 symbols affected** |
| `DAT_020d1334` (IPCFIFOCNT) | `undefined4 *` | halfword | `+2` became +8 bytes, so IPC messages went to 0x0400018C instead of 0x04000188 |
| `DAT_020d6174` (card check) | 2-byte element | word address, halfword read | offsets `-0x208`/`-8` were really `-0x410`/`-0x10`; the port compared a chip ID against unwritten memory and shut itself down |

**How it presents.** As an address that is *nearly* right. Registers in the
same block, RAM in the same structure. The value read is plausible, which is
exactly why it survives review.

**The check.** For any `DAT_` symbol used with arithmetic, get the access width
from the instruction, not from the C:

```
LDRB / STRB   -> 1 byte
LDRH / STRH   -> 2 bytes      (E1D210B0 is LDRH, not LDR)
LDR  / STR    -> 4 bytes
```

Then confirm the byte offset arithmetically. In the card-check case the
instruction encodes the offset directly - `02421E41` is `SUBEQ r1,r2,#0x410`
(imm8 0x41 rotated right by 28) - so the true offset is readable with no
inference at all.

**The automation.** Keep an override file (`config/dat_element_types.txt` here)
mapping symbol to true element type, applied by the generator that shapes the
exported C. Every entry gets the disassembly that justifies it recorded
alongside. That way the claim is checkable by the next reader instead of
trusted.

**Detection heuristic worth building early:** flag every `DAT_` symbol that is
both (a) used in arithmetic and (b) resolves into the MMIO range. Those are the
ones where a wrong width silently retargets a hardware register.

---

## 4. Address-of versus bitwise-and

**What happens.** Naive pattern matching on `& DAT_x` to find address-taken
symbols also matches `a & DAT_x`, which is a bitwise AND of a *value*.

**How it presented.** The interrupt-flags register was typed as one byte, so
the IRQ handler's acknowledgement wrote a single byte of zero, failed to clear
the flag, and **the handler ran 327,608 times**.

**The fix.** Inspect the preceding token. This one change took a symbol list
from 276 entries to 125 - meaning more than half of what the naive rule
"found" was noise, and each piece of noise was a potential width bug.

**The general lesson.** When a text-level heuristic over decompiled C produces
a suspiciously large result set, the heuristic is wrong, not the codebase.
Measure the false-positive rate before you act on the list.

---

## 5. Reordered reads that have side effects

**What happens.** The decompiler hoists a memory read above the branch that
guards it, because in its model a read is pure.

**On DS hardware, reads are frequently not pure.** Reading `IPCFIFORECV` *pops
a word off the FIFO*.

**How it presented.** With one word queued, the decompiled order popped the
word, then tested "is the FIFO empty?", saw yes, and discarded the word it had
just taken. The ARM7's reply was received and thrown away, and the ARM9 waited
forever on a flag that could never clear. This looked exactly like the ARM7
never answering.

**The check.** For every read of an MMIO address with read side effects
(FIFOs, some status registers, anything the hardware docs describe as
"reading clears" or "reading pops"), verify the read's position against the
instructions. The ARM will show the load *after* the conditional branch:

```
BL   0x020CE804        ; disable interrupts
LDRH r1,[r9,#0x0]      ; IPCFIFOCNT
TST  r1, #0x100        ; receive FIFO empty?
BEQ  +2                ; not empty -> fall through
...
LDR  r6,[r7,#0x0]      ; only here is IPCFIFORECV read
```

**The rule.** Build a list of side-effecting read addresses for your console
*before* you start, and grep the exported corpus for every one of them. This
is a five-minute job that would have saved a day here.

---

## 6. BIOS / SWI stubs lose everything

An `SWI` decompiles to something like `software_interrupt(0xb)` with no
parameters, because on ARM the arguments are simply sitting in r0-r2. Your host
implementation of `CpuSet` necessarily takes three. Every such site needs the
arguments recovered from the instructions by hand, and there are few enough of
them that a small table of transcriptions - each with its disassembly attached
- is the right tool. See `tools/gen_swi_patches.py`.

---

## 6b. Adjacent stack locals that were one structure

**What happens.** The decompiler names a stack local after its frame offset -
`local_20`, `local_1c`, `iStack_b8`, `iStack_b4`. A function that builds a small
structure in its own frame and passes its address comes out as the address of
the FIRST local, with the callee indexing past it:

```c
int local_20;                      /* the device */
int local_1c;                      /* the cursor */
local_1c = base + index * 8;
local_20 = device;
read(&local_20, buffer, 8);        /* the callee reads param[0] and param[1] */
```

**Why it is fatal.** C guarantees nothing about where two separate locals go.
They may be in either order, or apart, and `-O0` does not help. `param[1]` reads
whatever the host compiler happened to put beside `local_20`.

**How it presented here.** The cursor into the File Name Table read as **zero**,
so the archive read copied from a null pointer. No crash at the point of the
bug: the path walk simply failed to find the file, the open returned failure,
the caller did not check it, and the port died much later in an asset loader
reading a handle that had never been opened.

**How to recognise it.** The names carry the evidence:

- Two names four apart - `local_20` and `local_1c` - were one word apart on the
  original stack.
- The **lower-numbered name is at the higher address**, so `local_20` is `[0]`
  and `local_1c` is `[1]`.
- The giveaway is `&local_N` passed to something that indexes its pointer
  argument. Check the callee's body for `param_1[1]`.
- It scales: one site here was six declarations - five words and a 128-byte
  buffer - forming a 148-byte directory entry, all of which had to be
  contiguous.

**The fix.** Give them one array so the language guarantees adjacency, and alias
the original names onto it so every other line is untouched:

```c
int lsw2_cursor[2];
#define local_20 (lsw2_cursor[0])
#define local_1c (lsw2_cursor[1])
...
read(lsw2_cursor, buffer, 8);
```

An `int` array also fixes alignment, which a `char` array would not.

**Related but different: `stack0x` names.** When the decompiler cannot place a
slot in the frame at all it emits `stack0xffffffe8`, and a naive port declares
those as globals - so two functions can share one. That is a separate class with
its own hazards. Both come from the same root: **stack layout is
information the instructions carry and the decompiled C does not.**

---

## 7. Relocated code decompiled at the wrong address

Not a decompiler defect so much as a setup one, but it costs the same. A DS
binary can contain blocks that are copied elsewhere at load time - the ARM9's
ITCM and DTCM, and on this cartridge a 64KB ARM7 block that ends up at
0x037F8000. Load the binary flat and Ghidra decompiles those blocks at the
wrong addresses, so every call into them becomes an unresolved
`thunk_EXT_FUN_0380xxxx` with no body. Here that was 178 distinct addresses and
475 functions that did not exist in the corpus at all.

**Finding the mapping when there is no autoload table.** This binary had no
module-params structure, and no literal in it referenced either end of the
copy, so the copying code was never located. The mapping was established by
*measurement* instead: take a savestate of the real console, and slide the
binary against the savestate's WRAM looking for a byte-exact run. The answer
here was arm7.bin+0x1B0 -> 0x037F8000, 100% identical over the first 48KB,
collapsing to 0-13% past that - which is exactly where code ends and runtime
data begins, and is itself the confirmation that the offset is right.

**The general technique is worth keeping:** when you cannot read the loader,
correlate the binary against real memory and let the match length tell you
where you are. Extract the block *whole* - split it in two and every call
between the halves becomes an unresolved external again.

---

## The habit that catches all seven

Before forming any theory about why translated code misbehaves, **disassemble
the function**. In every case above, the instructions contained the answer
plainly and the C did not contain it at all. Reading the ARM is not the slow
path; it is the fast path taken too late.
