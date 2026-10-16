# The literal-pool cross-reference, and why it beats a decompiler

Three findings in a row - the cheat-code system, the Free Play page table,
and the whole wireless module - came out of the same four-step move. It is
cheap, it needs no decompiler, and it transfers to any ARM game. Write it down
once and stop rediscovering it.

## The move

**1. Find a known plaintext in the image.** A string, a table, a value the game
must contain.

**2. Search the image for 32-bit words equal to its address.** On ARM, code
reaches constants through a PC-relative load from a literal pool, so the address
appears as a plain word near the code that uses it. `struct.unpack` over the
whole image at 4-byte alignment. Ten lines of Python.

**3. Count the hits.** This is the step that decides how much work is left:

- **One hit** is the jackpot. One function uses this thing, and you have found
  it. The cheat-code table at `0x020FED9C` had exactly one, at `0x02079778`,
  and that named the validator immediately.
- **A handful** usually means a small module, and their spread is its extent.
- **Hundreds** means a global everything touches, and the reference count itself
  is the finding - `0x02102900` is referenced from about 106 sites, which is how
  it was known to be the game's central context rather than an incidental copy.

**4. Find the instruction that loads the pool word.** Scan for `LDR Rd,[pc,#imm]`
- encoding `(word & 0x0F7F0000) == 0x051F0000` - and check whether
`addr + 8 ± imm` equals the pool slot. Remember PC is instruction + 8; getting
that wrong sends you to the neighbouring word and a confusing disassembly.

## Why this beats reading decompiled C

`playbook/01` records what Ghidra silently drops. This method never asks it
anything. It reads the bytes the CPU reads, so there is nothing to be dropped,
and it works identically on code Ghidra failed to decompile at all.

It is also **directional in the useful direction**. A decompiler answers "what
does this function do". This answers "who uses this fact", which is the question
you actually have when you know a value and want the behaviour around it.

## The variants that paid off

**Strings bound a module.** Every pool word pointing into the WH/WM debug-string
block landed in `0x020AD3AC`-`0x020AF204` and none outside it. A module that
logs from 83 sites and nowhere else is a module those 83 sites delimit - no
function-boundary analysis required.

**A log line names its function.** `"Ending scan in WH_Finalize()"` is
referenced from exactly one place, so that place is `WH_Finalize`. Sixteen entry
points were named this way in one pass. Debug strings surviving into a release
build are common on DS and are worth checking for first, before anything harder.

**Tables abut, and that is a proof.** Two tables that end and begin at the same
address fix each other's length. `0x020EEDAC` + 100 rows of 0x14 lands exactly
on `0x020EF57C`, where the character descriptors start. The music slot table's
29 rows were fixed the same way (`config/music_slots.json`). When you need a
row count and the code has no bound check, look at what comes next.

**A gap bounds an array.** Two bitfield bases 0x10 apart means the first is at
most four words - there is no room for a fifth before the second begins. That
is how the unlock array's size was fixed without any bound check existing
anywhere.

**A field offset is a search key.** Readers of a struct field are many; writers
are few, and the writers are the behaviour. Searching for `STRB` with imm12 ==
0x479 gave 2 writers out of 17 accesses and handed over the whole level-entry
chain.

## Pitfalls, each of which cost time here

**Your plaintext may not be stored as text.** Four cheat codes searched as ASCII
returned nothing. They were keypad indices - 0-9 then A-Z as 0..35 - and the
screen's own decoded description said so. **A failed search is evidence the
value is transformed, not evidence it is absent.** Try both plausible encodings
and let the data pick: here the letters-first alphabet matched nothing and
digits-first matched all four, which is what makes the answer a measurement.

**Match the access width, not just the offset.** Recorded already in
`playbook/06` and it keeps biting. The Free Play page restore uses `ldrsb`, and
that is the whole reason `0xFF means unset` works: as a signed byte it is -1,
fails the range test, and falls to the default. Read unsigned it is 255 and
takes the other branch.

**PC is +8.** Said above; repeated because a mis-resolved pool word disassembles
into something plausible and wrong.

**A pool word inside a function is not code.** Disassemblers happily render
literal pools as instructions - `andeq lr, pc, #156, #26` and friends. Nonsense
arithmetic on `pc` or `lr` in the middle of a sensible function is a pool, not a
branch you failed to follow.

## The script

Small enough to retype, which is the point.

```python
import struct
data = open('arm9_section_0_02000000.bin','rb').read()
BASE = 0x02000000

def pool_refs(target):
    w = struct.pack('<I', target)
    return [BASE+i for i in range(0, len(data)-4)
            if i % 4 == 0 and data[i:i+4] == w]

def loaders(pool_slot, lo, hi):
    out = []
    for a in range(lo, hi, 4):
        word = struct.unpack_from('<I', data, a-BASE)[0]
        if (word & 0x0F7F0000) != 0x051F0000:
            continue
        imm = word & 0xFFF
        up = (word >> 23) & 1
        if a + 8 + (imm if up else -imm) == pool_slot:
            out.append(a)
    return out
```

## When to reach for it

Whenever you know a **value** and want the **code**. That is most of the
questions worth asking about a game you did not write: where is this table used,
what sets this flag, which function owns this screen, how big is this array.

Reach for a decompiler when you know the code and want its meaning. The two are
complements, and this project spent a long time using only the second.
