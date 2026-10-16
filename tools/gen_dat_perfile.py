"""Shape every DAT_ symbol by the usage IN THE FILE THAT USES IT.

WHY PER-FILE IS NOT A HACK. Ghidra infers types per function. The same global is
therefore an integer in one body and a pointer in another - 149 symbols in this
corpus are used both ways, and a single global header has to pick one and break
the other. Each translation unit is independent, so each one gets the shape its
own body implies. That is not a compromise between two guesses; it is what
Ghidra actually inferred, applied where it was inferred.

TELLING `*x` FROM `a * x`, WHICH IS THE WHOLE DIFFICULTY.

A leading `*` is a dereference; an infix `*` is a multiply. A fixed-width
lookbehind cannot separate them, because `a * DAT_x` puts a SPACE immediately
before the star - which is exactly how `param_5 * DAT_02055b60` came to be read
as a dereference and a plainly-integer symbol came to be typed as a pointer. So
the preceding TOKEN is captured and inspected, not the preceding character. A
cast is the one parenthesised token that still means dereference.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTDIR = ROOT / "recomp" / "decomp" / "gen" / "dat"
DIRS = [ROOT / "out" / "ghidra" / "decomp" / "c",
        ROOT / "out" / "ghidra" / "decomp_itcm" / "c"]

# Ghidra also names a pointer after WHAT IT POINTS AT: PTR_FUN_020e515c is
# the function pointer stored at 0x020E515C, and PTR_DAT_/PTR_LAB_ follow
# the same shape. The address is always the trailing eight hex digits.
# Missing this form left 41 symbols undeclared across 16 files, and each
# of those files became an aborting stub the port died on.
# Ghidra also puts a HUNGARIAN TYPE PREFIX on the name and sometimes a leading
# underscore, and both were being missed:
#
#     puRam020ecf58      pointer-to-unsigned at 0x020ECF58
#     _FUN_01ff8a98      the data at a function address, not the function
#
# The prefix letters are lowercase and the body is not, which is what keeps this
# from matching a plain call. A BARE FUN_xxxxxxxx MUST NOT MATCH - that is an
# ordinary function call, and defining it as data would break every call site in
# the corpus - so the FUN spelling is only accepted with a leading underscore.
SYM = (r"(?:"
       r"_{1,2}[a-z]{0,3}(?:DAT|PTR|UNK|FUN|LAB|Ram)_?"
       r"(?:FUN_|DAT_|LAB_)?[0-9a-fA-F]{8}"
       r"|[a-z]{0,3}(?:DAT|PTR|UNK)_?(?:FUN_|DAT_|LAB_)?[0-9a-fA-F]{8}"
       r"|[a-z]{1,3}Ram[0-9a-fA-F]{8}"
       r")")
ANY = re.compile(r"\b(%s)\b" % SYM)
INDEX = re.compile(r"\b(%s)\s*\[" % SYM)
CALL = re.compile(r"\(\s*\*\s*(%s)\s*\)\s*\(" % SYM)
STAR = re.compile(r"(\([^()]*\)|[A-Za-z0-9_\]]+|\S)\s*\*\s*(%s)\b" % SYM)
HEX = re.compile(r"[0-9a-fA-F]{8}$")

TYPE_WORDS = {
    "int", "uint", "char", "byte", "sbyte", "short", "ushort", "long", "ulong",
    "void", "code", "bool", "float", "double", "undefined", "undefined1",
    "undefined2", "undefined4", "undefined8", "longlong", "ulonglong",
    "uint8_t", "uint16_t", "uint32_t", "int8_t", "int16_t", "int32_t",
    "u8", "u16", "u32", "unsigned", "signed",
}


# A KEYWORD IS NOT AN OPERAND, AND TREATING IT AS ONE COSTS A FUNCTION.
#
#     return *DAT_01ffd5b8;
#
# `return` ends in a letter, so the "an operand precedes the star, therefore
# multiplication" rule fired and the symbol was typed as a plain value. The file
# then failed to compile on `*DAT_...`, became an aborting stub, and the port
# stopped at FUN_01FFD55C claiming Ghidra had produced no C for it - which was
# not true. Nothing can be multiplied BY a keyword, so a keyword to the left of
# a star always means dereference.
KEYWORDS = {"return", "case", "sizeof", "else", "do", "typedef", "static",
            "const", "volatile", "extern", "register", "goto"}


def is_cast(token: str) -> bool:
    """`(int *)*DAT_x` dereferences; `(a + b) * DAT_x` multiplies."""
    if not (token.startswith("(") and token.endswith(")")):
        return False
    inner = token[1:-1].replace("*", " ").split()
    return bool(inner) and all(w in TYPE_WORDS for w in inner)


# TAKING THE ADDRESS MEANS BYTE ARITHMETIC, AND GETTING THAT WRONG MOVED A
# HARDWARE REGISTER.
#
#     *(undefined4 *)(&DAT_040000b0 + iVar1) = param_2;   /* DMAxSAD */
#     *(undefined4 *)(iVar1 + 0x40000b4) = param_3;       /* DMAxDAD */
#
# The two lines are the same address computed two ways, so `&DAT_040000b0 +
# iVar1` has to be a BYTE offset - which is what Ghidra means, because it
# declares these as bytes. Typing the symbol as undefined4 makes `&` yield an
# undefined4 * and C scales the addition by four: with iVar1 = channel * 12,
# channel 3's source register was written at 0x04000140 instead of 0x040000D4.
# It was never written at all, so the DMA ran from address 0 and the port
# faulted inside its own I/O layer.
#
# So a symbol whose ADDRESS is taken is a byte. Every use of `&SYM` in this
# corpus is "the address of", with the surrounding code casting to the width it
# wants, so this loses nothing.
# AND `&` IS THE SAME TRAP AS `*`, WHICH I WALKED STRAIGHT INTO.
#
#     uVar2 = _DAT_04000210 & _DAT_04000214;
#
# That is a bitwise AND of two hardware registers - IE and IF - and reading the
# ampersand as address-of typed IF as a single byte. The game's interrupt
# handler then acknowledged it with
#
#     _DAT_04000214 = 0x80000000 >> iVar1;
#
# truncated to one byte of zero. Writing zero to a write-one-to-clear register
# clears nothing, so the interrupt stayed pending and the handler ran 327,608
# times in ten seconds without making any progress.
#
# The fix is the one the star already uses: inspect the PRECEDING TOKEN. An
# operand before the ampersand means bitwise AND; anything else - an operator,
# a comma, an open bracket, a keyword - means address-of.
ADDRESSED_RAW = re.compile(
    r"(\([^()]*\)|[A-Za-z0-9_\]]+|\S)?\s*&\s*(%s)\b" % SYM)


def addressed_syms(text: str) -> set:
    """The symbols whose ADDRESS is taken, as opposed to being AND-ed."""
    out = set()
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("&"):
            m = re.match(r"&\s*(%s)\b" % SYM, stripped)
            if m:
                out.add(m.group(1))
    for m in ADDRESSED_RAW.finditer(text):
        token, name = m.group(1), m.group(2)
        if token is None or is_cast(token) or token in KEYWORDS:
            out.add(name)
        elif token[-1].isalnum() or token[-1] in "_)]":
            continue            # an operand precedes it: bitwise AND
        else:
            out.add(name)
    return out


def dereferenced(text: str) -> set:
    """The symbols this text actually dereferences or indexes."""
    out = set(INDEX.findall(text))
    # A star with nothing before it on the line is unambiguously a dereference.
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("*"):
            m = re.match(r"\*+\s*(%s)\b" % SYM, stripped)
            if m:
                out.add(m.group(1))
    for m in STAR.finditer(text):
        token, name = m.group(1), m.group(2)
        if is_cast(token):
            out.add(name)
        elif token in KEYWORDS:
            out.add(name)  # `return *x`, `case *x`: never multiplication
        elif token and (token[-1].isalnum() or token[-1] in "_)]"):
            continue  # an operand precedes the star: multiplication
        else:
            out.add(name)  # =*x  (*x  ,*x  +*x  **x ...
    return out


# NAMES GHIDRA USES FOR TWO DIFFERENT THINGS. Its ARM spec spells the processor
# modes and the exception vectors with the same words, and the values differ -
# FIQ is mode 0x11 and vector 0x1C. ghidra_intrinsics.h defines the modes,
# because that is the commoner reading; a body that takes the ADDRESS means the
# vector, and gets an override here. Taking the address of an enum constant is
# not possible anyway, so these files did not compile at all.
VECTORS = {
    "UndefinedInstruction": 0x04,
    "NotUsed": 0x14,
    "IRQ": 0x18,
    "FIQ": 0x1C,
}
ADDR_OF = {n: re.compile(r"&\s*%s\b" % n) for n in VECTORS}


# THE ELEMENT TYPE OF A POINTER SYMBOL, WHICH DECIDES WHAT INDEXING MEANS.
#
#     undefined2 *puVar1;
#     puVar1 = DAT_020cd660;
#     if (DAT_020cd660[1] != 0) { ... early exit ... }
#
# Ghidra typed that symbol `undefined2 *`, so [1] is the halfword at +2. Shaping
# every pointer symbol as `undefined4 *` makes [1] the word at +4 instead - a
# different address entirely. At that address sat a non-zero pointer, so the
# thread scheduler took its early exit on every call, never switched threads,
# and the port spun in a file-system wait that only another thread could end.
#
# The type is recoverable: Ghidra declares a local of the right pointer type and
# assigns the symbol to it. So the declarations are read and the assignment
# followed, and only when neither exists does the shape fall back to undefined4.
DECL = re.compile(r"^\s{2,}([A-Za-z_]\w*)\s*\*(\w+)\s*;", re.M)
ASSIGN_TMPL = r"\b%s\s*=\s*(?:\([^)]*\)\s*)?%s\s*;"

ELEMENT_TYPES = {
    "undefined1", "undefined2", "undefined4", "undefined8",
    "char", "byte", "sbyte", "short", "ushort", "int", "uint",
    "long", "ulong", "code", "bool", "float", "double",
}


def load_overrides() -> dict:
    """Symbols whose element type the C does not reveal. See the config file."""
    out = {}
    f = ROOT / "config/dat_element_types.txt"
    if not f.exists():
        return out
    for line in f.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) == 2:
            out[parts[0]] = parts[1]
    return out


OVERRIDES = load_overrides()


def element_type(text: str, name: str) -> str:
    """The element type Ghidra gave this symbol in THIS file, or undefined4."""
    if name in OVERRIDES:
        return OVERRIDES[name]
    for m in DECL.finditer(text):
        kind, var = m.group(1), m.group(2)
        if kind not in ELEMENT_TYPES:
            continue
        if re.search(ASSIGN_TMPL % (re.escape(var), re.escape(name)), text):
            return kind
    return "undefined4"


# WHERE THE ARITHMETIC IS ON THE LOCAL, NOT ON THE SYMBOL.
#
# The rule above applies a recovered element type only where the SYMBOL is
# indexed - `DAT_x[1]`. That narrowing was right to make and too narrow to
# leave, because the common shape in this corpus puts the arithmetic on the
# local instead:
#
#     undefined1 *puVar1;
#     puVar1 = DAT_020d57cc;          /* 0x040001A1, the card interface */
#     *DAT_020d57cc = 0xc0;
#     puVar1[7]  = (char)(param_1 >> 0x18);   /* ROMCMD[0] at 0x040001A8 */
#     ...
#     puVar1[0xe] = (char)param_2;            /* ROMCMD[7] at 0x040001AF */
#
# Shaped `undefined4 *` those eight byte writes land 28 to 56 bytes past where
# they belong and the cartridge command is never issued. The port spun in this
# function. VRAMCNT_A..G had the same problem, which is why no VRAM bank was
# ever enabled, and so did DIVCNT, IME and IPCFIFOCNT - 148 symbols in all,
# measured by tools/audit_indirect_element_types.py.
#
# THIS IS DELIBERATELY LIMITED TO SYMBOLS WHOSE TARGET HAS A KNOWN LAYOUT. A
# bulk retype once moved 381 symbols at once and broke the port somewhere new,
# so the criterion is one that can be checked rather than trusted, and the value
# the symbol holds is read out of the extracted image rather than assumed.
#
# TWO REGIONS QUALIFY, for the same reason:
#
#   0x04xxxxxx  the registers. VRAMCNT_A..I are nine consecutive bytes, IME and
#               DIVCNT are halfwords - the width is a fact about the hardware.
#
#   0x027FFxxx  the DS system region. The firmware's layout here is equally
#               fixed: chip IDs at 0x027FF800, the boot indicator at
#               0x027FFC40, and the ROM header copied to 0x027FFE00 with its
#               documented field offsets.
#
# The system region was left out of the first pass and that was wrong. The file
# system initialiser FUN_020D2E28 has
#
#     short *psVar2;
#     psVar2 = DAT_020d2f54;              /* 0x027FFC40 */
#     iVar4 = *(int *)(psVar2 + 0x100);   /* 0x027FFE40 */
#     iVar3 = *(int *)(psVar2 + 0x104);   /* 0x027FFE48 */
#
# and 0x027FFE40 is header+0x40 - the FNT offset - followed by FNT size, FAT
# offset and FAT size. Shaped undefined4 those offsets become 0x02800040, past
# the end of main RAM. iVar4 read as zero, the guard `iVar4 == 0` sent the
# function down its do-nothing path, FUN_020D2638 was never called, and the file
# system never learned where its own directory table was. Several hundred
# thousand instructions later the port called a callback on a device that had
# never finished initialising, and died on a null pointer with nothing to
# connect it back to here.
HW_RANGES = ((0x04000000, 0x05000000),      # hardware registers
             (0x027FF000, 0x02800000))      # the DS system region

# STAGE 2: THE REGION TEST IS DROPPED, because it kept being wrong in the same
# direction. Stage 1 was registers, stage 1b added the system region after the
# file system read its own directory table from 0x02800040. Then FUN_020D43A4
# turned up doing
#
#     short *psVar1;
#     psVar1 = DAT_020d4408;      /* 0x021174F4 - ordinary heap */
#     psVar1[2] = 0;              /* meant +4, became +8 */
#     psVar1[0x10] = 0;           /* meant +0x20, became +0x40 */
#
# which is the same defect a fourth time, in memory no region test was ever
# going to whitelist. Three stagings, three regions, one cause.
#
# WHAT THE CRITERION ACTUALLY IS, once the region test is gone: Ghidra declared
# a local of a specific pointer type, assigned THIS symbol to it, and then did
# arithmetic on that local. That is not an inference about the target - it is
# Ghidra's own recorded type, applied where Ghidra recorded it. The region test
# was never what made it trustworthy; it was a way of taking small bites.
#
# This is NOT the bulk retype that once moved 381 symbols and broke the port.
# That applied types to every DEREFERENCED symbol, whether or not Ghidra had
# said anything about them. This applies only where Ghidra spelled the type out
# and only where the width changes an address. The counter below reports how
# many, so the change is measured rather than assumed.
STAGE2_ALL_REGIONS = True


def load_images() -> dict:
    out = {}
    raw = ROOT / "out" / "raw"
    if not raw.exists():
        return out
    for p in sorted(raw.glob("arm9_section_*.bin")) + \
             sorted(raw.glob("arm7_wram_*.bin")):
        m = re.search(r"_([0-9a-f]{8})\.bin$", p.name)
        if m:
            out[int(m.group(1), 16)] = p.read_bytes()
    return out


IMAGES = load_images()


def in_known_layout(a: int) -> bool:
    return any(lo <= a < hi for lo, hi in HW_RANGES)


def points_at_hardware(name: str) -> bool:
    """Whether this symbol is eligible for the recovered element type.

    With STAGE2_ALL_REGIONS the answer is always yes and the evidence test
    moves entirely to local_element_type() below - which is where it belonged.
    """
    if STAGE2_ALL_REGIONS:
        return True
    m = HEX.search(name)
    if not m:
        return False
    addr = int(m.group(0), 16)
    if in_known_layout(addr):          # named for the target itself
        return True
    for base, blob in IMAGES.items():
        off = addr - base
        if 0 <= off <= len(blob) - 4:
            return in_known_layout(
                int.from_bytes(blob[off:off + 4], "little"))
    return False


LOCAL_ARITH = re.compile(r"\s*(?:\[|\+|-(?!>))")


def local_element_type(text: str, name: str) -> str:
    """Type of a local this symbol is assigned to, when the LOCAL is offset.

    Returns undefined4 when there is no such local, so the caller's behaviour
    is unchanged wherever the evidence is absent.
    """
    for m in DECL.finditer(text):
        kind, var = m.group(1), m.group(2)
        if kind not in ELEMENT_TYPES or kind == "undefined4":
            continue
        if not re.search(ASSIGN_TMPL % (re.escape(var), re.escape(name)), text):
            continue
        # The width only changes an address if the local is actually offset.
        if re.search(r"\b%s\b%s" % (re.escape(var), LOCAL_ARITH.pattern), text):
            return kind
    return "undefined4"


def main() -> int:
    OUTDIR.mkdir(parents=True, exist_ok=True)
    files = 0
    ptr = val = fn = vec = addr_taken = typed = from_local = 0
    for d in DIRS:
        if not d.exists():
            continue
        for p in sorted(d.glob("*.c")):
            text = p.read_text(encoding="utf-8", errors="replace")
            used = set(ANY.findall(text))
            vecs = [n for n, rx in ADDR_OF.items() if rx.search(text)]
            files += 1
            if not used and not vecs:
                (OUTDIR / (p.stem + ".h")).write_text("", encoding="utf-8")
                continue
            deref = dereferenced(text)
            called = set(CALL.findall(text))
            addressed = addressed_syms(text)
            indexed = set(INDEX.findall(text))
            lines = ["/* Generated by tools/gen_dat_perfile.py - do not edit. */"]
            for name in sorted(used):
                addr = int(HEX.search(name).group(0), 16)
                if name in addressed:
                    # A byte, so &name is a byte pointer and offsets are bytes.
                    shape = "(*(undefined1 *)(uintptr_t)0x%08Xu)" % addr
                    addr_taken += 1
                elif name in called:
                    shape = "(*(undefined4 (**)())(uintptr_t)0x%08Xu)" % addr
                    fn += 1
                elif name in deref:
                    # THE ELEMENT TYPE IS APPLIED ONLY WHERE IT CHANGES AN
                    # ADDRESS - that is, where the symbol is INDEXED. For a
                    # plain `*DAT_x` the width would change how many bytes are
                    # read, which is a much wider behavioural change than the
                    # defect being fixed, and applying it everywhere at once
                    # moved 381 symbols and broke the port in a new place. The
                    # scheduler bug was `DAT_020cd660[1]`, and indexing is where
                    # the evidence is.
                    if name in OVERRIDES or name in indexed:
                        kind = element_type(text, name)
                    elif points_at_hardware(name):
                        # The arithmetic is on a local Ghidra typed, and the
                        # symbol is a register whose width the layout proves.
                        kind = local_element_type(text, name)
                        if kind != "undefined4":
                            from_local += 1
                    else:
                        kind = "undefined4"
                    shape = "(*(%s **)(uintptr_t)0x%08Xu)" % (kind, addr)
                    ptr += 1
                    if kind != "undefined4":
                        typed += 1
                else:
                    shape = "(*(undefined4 *)(uintptr_t)0x%08Xu)" % addr
                    val += 1
                lines.append("#define %s %s" % (name, shape))
            for name in sorted(vecs):
                lines.append("#undef %s" % name)
                lines.append("#define %s (*(undefined1 *)(uintptr_t)0x%02Xu)"
                             % (name, VECTORS[name]))
                vec += 1
            (OUTDIR / (p.stem + ".h")).write_text("\n".join(lines) + "\n",
                                                  encoding="utf-8")
    print("per-file DAT headers   %d" % files)
    print("  function pointers    %d" % fn)
    print("  data pointers        %d" % ptr)
    print("    with a recovered element type %d" % typed)
    print("      of those, from a typed local on a register %d"
          % from_local)
    print("  plain data           %d" % val)
    print("  address-taken (bytes) %d" % addr_taken)
    print("  vector-name overrides %d" % vec)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
