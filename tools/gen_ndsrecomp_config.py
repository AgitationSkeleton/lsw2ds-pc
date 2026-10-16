"""Emit ndsrecomp program configs for LSW2 from this project's function corpus.

WHY. ndsrecomp lifts guest ARM to C ahead of time and needs, per program, a
TOML describing the image and every function entry point:

    [program]
    cpu = "arm9"  isa = "armv5te"  load_address = ...  size = ...  entry_pc = ...
    [identity]
    sha1 = ...
    [[entry_point]]
    addr = 0x...   mode = "arm"   name = "..."

That entry-point list is the expensive part of bringing a title up, and this
project already has it: 5,860 ARM9 and 930 ARM7 functions located by Ghidra.
So the corpus transfers directly even though the execution core would not.

THE ONE THING THE CORPUS DOES NOT RECORD IS THE INSTRUCTION MODE. Ghidra knows
whether each function is ARM or Thumb; the exports here kept only addresses.
Emitting `mode = "arm"` for everything would be a silent lie for every Thumb
function, and a wrongly-lifted function is exactly the class of defect this
whole exercise is trying to escape.

So the mode is DERIVED FROM THE BYTES, and the derivation is reported rather
than assumed:

  * a 4-byte-aligned word whose top nibble is a valid ARM condition and which
    decodes as a recognisable prologue - STMFD sp!{...}, SUB sp, MOV rX, rY -
    is evidence for ARM;
  * a halfword of the form 0xB4xx/0xB5xx is Thumb PUSH, which is how nearly
    every Thumb function begins;
  * anything else is reported as UNKNOWN and written with no mode, so the
    framework rejects it rather than lifting it wrongly.

This is a heuristic and is labelled as one. It is a starting point for a
bring-up, not a substitute for Ghidra's own mode information - re-exporting
that from the Ghidra database would be strictly better and is the right fix if
this path is pursued.
"""

from __future__ import annotations

import bisect
import hashlib
import json
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "out/work/ndsrecomp"

# The image a function actually lives in is NOT the one the ROM header points
# at. The ARM9 is BLZ-compressed: the header's 0xA3F24 expands to 0x107238 and
# unpacks into three autoload sections (0x02000000 code, 0x01FF8000 ITCM,
# 0x027E0000 DTCM). Using the header extents put 2,600 of 5,860 functions
# "outside the image" - they are past 0x020A3F24 but well inside the
# decompressed 0x1019C0-byte section 0. The load address and entry still come
# from the header; only the bytes come from the unpacked section.
# THE THIRD PROGRAM IS NOT IN THE ROM HEADER AT ALL. The ARM7 copies a block of
# itself into shared WRAM at 0x037F8000 and runs it there. Nothing in the header
# describes that region, so it has no header offset and its load address is a
# measured fact rather than a read field - established in playbook/01 by sliding
# arm7.bin against a console savestate until the bytes matched, which they do
# exactly from arm7.bin+0x1B0 over the first 48KB.
#
# IT IS ALSO WHERE ALL THE TIME GOES. The runtime coverage manifest recorded
# 62,284,837 interpreted ARM7 instructions against 78,855 on the ARM9 - 790x -
# with 3,715,036 of the hits landing in 0x037F0000. Our lsw2_arm7 bank is keyed
# to 0x02380000, so none of the WRAM copy matched it and every instruction of it
# ran in the interpreter.
#
# Because this code is copied into RAM at runtime rather than being immutable
# cartridge content, its bank MUST be emitted with --validate-live-bytes: the
# runner then checks the live bytes still match before dispatching, instead of
# trusting the address alone.
CORPORA = [
    # (label, decomp dir, cpu, isa, header offset | (base, entry), image)
    ("lsw2_arm9", "out/ghidra/decomp/c", "arm9", "armv5te", 0x20,
     "out/raw/arm9_section_0_02000000.bin"),
    ("lsw2_arm7", "out/ghidra/decomp_arm7/c", "arm7", "armv4t", 0x30,
     "out/raw/arm7.bin"),
    ("lsw2_arm7_wram", "out/ghidra/decomp_arm7_wram/c", "arm7", "armv4t",
     (0x037F8000, 0x037F8000), "out/raw/arm7_wram_037f8000.bin"),
    # ITCM - autoload section 1. The ARM9 puts its hottest routines in tightly
    # coupled memory, and the runtime coverage manifest (which lists what fell
    # through to the INTERPRETER, not what ran) showed 187 of the 253 remaining
    # ARM9 gaps sitting at 0x01FF0000. Same story as the WRAM copy: real code at
    # an address no bank claimed.
    ("lsw2_arm9_itcm", "out/ghidra/decomp_itcm/c", "arm9", "armv5te",
     (0x01FF8000, 0x01FF8000), "out/raw/arm9_section_1_01ff8000.bin"),
]


def rom_bytes():
    roms = sorted(ROOT.glob("rom/*.nds"))
    if not roms:
        return None
    return roms[0].read_bytes()


def classify(image: bytes, base: int, addr: int):
    """('arm' | 'thumb' | None, why) for a function entry."""
    off = addr - base
    if off < 0 or off + 4 > len(image):
        return None, "outside the image"
    if addr & 1:
        return "thumb", "odd address"
    half = struct.unpack_from("<H", image, off)[0]
    word = struct.unpack_from("<I", image, off)[0]

    # Thumb PUSH {..., lr} / PUSH {...} - how nearly every Thumb function opens.
    if (half & 0xFE00) == 0xB400:
        return "thumb", "PUSH"
    if (addr & 3) != 0:
        return "thumb", "unaligned for ARM"

    cond = word >> 28
    if cond == 0xF:
        return None, "0x%08X has an invalid condition" % word

    # NOT CODE AT ALL. Some corpus entries sit on data - Ghidra created a
    # function where a string table or a pointer array happens to live. These
    # must not be handed to a lifter as entry points, and they decode as
    # perfectly valid (if absurd) instructions, so they have to be caught
    # before the opcode tests rather than after.
    if all(0x20 <= b <= 0x7E for b in image[off:off + 4]):
        return None, "reads as ASCII text %r" % image[off:off + 4].decode()
    if base <= word < base + len(image) and cond == 0:
        return None, ("0x%08X is an EQ-conditional word that is also a "
                      "valid pointer into this image - probably a pointer "
                      "table" % word)
    # STMFD sp!, {...}  - by far the commonest ARM prologue.
    if (word & 0x0FFF0000) == 0x092D0000:
        return "arm", "STMFD sp!"
    # SUB sp, sp, #imm
    if (word & 0x0FFF0000) == 0x024DD000:
        return "arm", "SUB sp"
    # MOV rX, rY / MOV rX, #imm as a leaf opening
    if (word & 0x0DEF0000) in (0x01A00000, 0x03A00000):
        return "arm", "MOV"
    # Data processing with an immediate - CMP rX, #0 and MVN r0, #0 open a
    # great many leaf functions here. Omitting this shape was what left 572
    # real ARM functions unclassified on the first run.
    if (word & 0x0E000000) == 0x02000000:
        return "arm", "data processing, immediate"
    # A plain conditional branch or load also decodes; accept as weak evidence.
    if (word & 0x0E000000) in (0x0A000000, 0x04000000, 0x00000000):
        return "arm", "decodes as ARM"
    return None, "0x%08X matches no known prologue" % word


def main() -> int:
    rom = rom_bytes()
    if rom is None:
        print("no ROM in rom/ - cannot read the header")
        return 1
    OUT.mkdir(parents=True, exist_ok=True)

    for label, cdir, cpu, isa, hdr, imgpath in CORPORA:
        d = ROOT / cdir
        if not d.exists():
            print("%s: no corpus at %s" % (label, cdir))
            continue
        if isinstance(hdr, tuple):
            # Not described by the ROM header - see the CORPORA comment.
            ram, entry = hdr
            _packed = 0
        else:
            _rom_off, entry, ram, _packed = struct.unpack_from("<4I", rom, hdr)
        img = ROOT / imgpath
        if not img.exists():
            print("%s: no unpacked image at %s" % (label, imgpath))
            continue
        image = img.read_bytes()
        size = len(image)
        sha = hashlib.sha1(image).hexdigest()

        # THE GHIDRA DATABASE IS THE AUTHORITY ON WHAT FUNCTIONS EXIST, not the
        # directory of decompiled .c files. Those two drift apart the moment new
        # functions are created: feeding 2,027 coverage-discovered addresses
        # through CreateEntryPoints.java took the ARM9 program from 5,860
        # functions to 7,692, while the decomp directory still held 5,860 files
        # because the decompiler had not been re-run.
        #
        # Taking addresses from the modes export instead means the config tracks
        # the database immediately, and decompiling to readable C becomes an
        # independent step rather than a prerequisite for a better bank.
        addrs = sorted({int(p.stem[4:], 16) for p in d.glob("FUN_*.c")})

        # HOW BIG EACH FUNCTION IS, where the corpus records it. The header
        # line of every decompiled file carries it, e.g.
        #     // 0x020ae388  FUN_020ae388  size=256
        #
        # This is used by the data-range reconciliation below to tell a real
        # function that a stale range happens to cover from the junk entries
        # that reconciliation exists to remove. Without a size the two are
        # indistinguishable, and the whole set gets dropped together.
        fsizes = {}
        for p in d.glob("FUN_*.c"):
            try:
                with p.open(encoding="utf-8", errors="replace") as fh:
                    m = re.search(r"size=(\d+)", fh.readline())
            except OSError:
                continue
            if m:
                fsizes[int(p.stem[4:], 16)] = int(m.group(1))
        mpath = OUT / ("modes_%s.json" % label)
        if not mpath.exists():
            mpath = OUT / ("modes_%s.json" % cpu)
        if mpath.exists():
            db = sorted({int(r["addr"], 16)
                         for r in json.loads(mpath.read_text(encoding="utf-8"))})
            if len(db) > len(addrs):
                print("%s: Ghidra has %d functions, the decomp directory has "
                      "%d - using the database" % (label, len(db), len(addrs)))
                addrs = db

        # PREFER THE TRUTH WHEN IT IS AVAILABLE. classify() reads prologue
        # bytes and is a guess; Ghidra's TMode context register is the fact,
        # exported by tools/ghidra_scripts/ExportFunctionModes.java. Measured
        # against it on the ARM9, the heuristic agreed 5,400 times, said
        # UNKNOWN 456 times - and got 4 wrong, every one a Thumb function it
        # called ARM, which would have been lifted in the wrong instruction
        # set and been silently, totally incorrect.
        #
        # All 4 came from the "data processing, immediate" rule, which was
        # added to rescue 572 otherwise-unclassified functions. Widening a
        # heuristic to gain coverage cost accuracy, and the cost was invisible
        # until there was something to check it against.
        # Keyed by LABEL first, then cpu. Both ARM7 programs are "arm7", but
        # they occupy different address ranges - the WRAM copy lives at
        # 0x037F8000 and the cartridge ARM7 at 0x02380000 - so sharing one
        # modes file silently matched nothing while still printing "using
        # Ghidra TMode", which is worse than saying nothing. The overlap is
        # now measured and reported rather than assumed.
        truth = {}
        tpath = OUT / ("modes_%s.json" % label)
        if not tpath.exists():
            tpath = OUT / ("modes_%s.json" % cpu)
        if tpath.exists():
            truth = {int(r["addr"], 16): r["mode"]
                     for r in json.loads(tpath.read_text(encoding="utf-8"))}
            covered = sum(1 for a in addrs if a in truth)
            if covered == 0:
                print("%s: %s covers NONE of this program's addresses - "
                      "ignoring it and using the prologue heuristic, which is "
                      "a guess. Export modes for this program."
                      % (label, tpath.name))
                truth = {}
            else:
                print("%s: using Ghidra TMode from %s (%d of %d addresses "
                      "covered)" % (label, tpath.name, covered, len(addrs)))
        else:
            print("%s: NO modes file - falling back to the prologue heuristic, "
                  "which is a guess" % label)

        # RECONCILE AGAINST THE DATA RANGES. ExportDataRanges.java emits every
        # byte range Ghidra did not turn into instructions. A function entry
        # inside one of those is Ghidra contradicting itself - it created a
        # Function object at an address whose bytes it typed as data, or never
        # disassembled at all.
        #
        # ndsrecomp refuses to run on that contradiction rather than picking a
        # side, which is the right behaviour and is how these were found:
        #   CONFIG ERROR [[extra_func]] at 0x020E5CA4 falls inside
        #   [[data_range]] [0x020E5CA4, 0x020E5CC0) - defined data, 28 bytes
        #
        # There are 26 on the ARM9, and they are not a judgement call: every
        # one is size<=8 AND carries "bad instruction data". They are the same
        # junk-entry class as the 417, found by a different route. Dropped.
        dranges = []
        dpath = OUT / ("data_ranges_%s.toml" % cpu)
        if dpath.exists():
            dranges = sorted(
                (int(m.group(1), 16), int(m.group(2), 16))
                for m in re.finditer(
                    r"start = (0x[0-9A-F]+)\nend   = (0x[0-9A-F]+)",
                    dpath.read_text(encoding="utf-8")))
        dstarts = [r[0] for r in dranges]

        def in_data(addr):
            i = bisect.bisect_right(dstarts, addr) - 1
            return i >= 0 and dranges[i][0] <= addr < dranges[i][1]

        arm = thumb = unknown = 0
        in_data_n = 0
        rescued_n = 0
        lines = []
        reasons = {}
        suspect = []
        for a in addrs:
            mode, why = classify(image, ram, a)
            # The heuristic still runs, because it is the only thing that
            # detects entries sitting on data: Ghidra reports mode "arm"
            # perfectly happily for a function it created on a string table,
            # so the JSON cannot be used alone.
            #
            # But "not code" rests on the ASCII and pointer-table tests
            # SPECIFICALLY, not on the heuristic merely failing to recognise a
            # prologue. Those are different claims, and conflating them dropped
            # a genuine Thumb function at 0x020005D4 whose opening simply is
            # not in the prologue list. Where the bytes are not positively
            # data, Ghidra's mode wins.
            not_code = why and ("ASCII" in why or "pointer table" in why)

            # A STALE DATA RANGE MUST NOT DELETE A REAL FUNCTION.
            #
            # The goal this serves, in plain english: the wireless match-start
            # transition runs through nine WM callbacks, and every one of them
            # was being thrown away here. The build declared
            # 0x020AE0F0..0x020AE3B8 "never disassembled, 712 bytes" while that
            # range in fact holds FUN_020ae128, ae170, ae1d0, ae2f8 and ae388 -
            # code this port's own WH step signal shows executing hundreds of
            # times in every healthy lobby. A second range hid four more. The
            # consequence was not just missing symbols: a probe on
            # FUN_020ae388 could not link, because dropping the entry point is
            # what stops the recompiler emitting the function at all.
            #
            # The ranges come from ExportDataRanges.java, which subtracts
            # Ghidra's instruction coverage from initialised memory. Its note
            # says "never disassembled" - so it ran against a Ghidra state in
            # which this code had not been turned into instructions, while the
            # corpus above came from one in which it had. When two exports of
            # the same database disagree, the one that says a function EXISTS
            # is the later evidence.
            #
            # The reconciliation is still worth having, and the note above it
            # says why: most entries inside a range are junk Ghidra created on
            # string tables. That claim was checked rather than assumed. Of the
            # 100 entries now falling inside a range, 82 are size<=8 - exactly
            # the class described - and 18 are substantial, nine of them the
            # WM callbacks. So size is the discriminator, and it separates the
            # two groups cleanly rather than by judgement.
            #
            # Kept only when the bytes are not POSITIVELY data either: the
            # ASCII and pointer-table tests still veto, so a large entry
            # sitting on a string table is dropped exactly as before.
            in_range = bool(dranges) and in_data(a)
            rescue = in_range and fsizes.get(a, 0) > 8 and not not_code
            if in_range and not rescue:
                not_code = True
                mode, why = None, "inside a Ghidra data range"
                in_data_n += 1
            elif a in truth and not not_code:
                mode, why = truth[a], "Ghidra TMode"
            if rescue:
                rescued_n += 1
            if mode == "arm":
                arm += 1
            elif mode == "thumb":
                thumb += 1
            else:
                unknown += 1
                suspect.append("FUN_%08x  %s" % (a, why))
                reasons[why.split()[-1] if why else "?"] = \
                    reasons.get(why.split()[-1] if why else "?", 0) + 1
                # OMIT it. An entry_point with no mode is not a safer version
                # of a wrong one - it is still an assertion that a function
                # begins here, and for the 417 that read as strings or pointer
                # tables that assertion is simply false. They go to the report
                # instead, where they can be looked at.
                continue
            lines.append("[[entry_point]]")
            lines.append("addr = 0x%08X" % a)
            lines.append('mode = "%s"' % mode)
            lines.append('name = "FUN_%08x"' % a)
            lines.append("")

        head = [
            "# %s.toml - generated by tools/gen_ndsrecomp_config.py" % label,
            "#",
            "# Entry points come from this project's Ghidra corpus. The MODE is",
            "# derived from the instruction bytes, not from Ghidra, because the",
            "# exports here kept only addresses - see the module docstring.",
            "#",
            "# Entries whose mode could not be established are OMITTED and",
            "# listed in %s_not_code.txt instead. Most of them are not" % label,
            "# functions at all: Ghidra marks them size=4 with 'Control flow",
            "# encountered bad instruction data', and they sit on string",
            "# tables and pointer arrays.",
            "",
            "[program]",
            'name         = "LEGO Star Wars II (NDS) %s"' % cpu.upper(),
            'id           = "%s"' % label,
            'cpu          = "%s"' % cpu,
            'isa          = "%s"' % isa,
            "load_address = 0x%08X" % ram,
            "size         = 0x%08X" % size,
            "entry_pc     = 0x%08X" % entry,
            "",
            "[identity]",
            'sha1 = "%s"   # of the UNPACKED %s image' % (sha, cpu),
            "# packed size in the ROM header is 0x%X" % _packed,
            "",
        ]
        p = OUT / (label + ".toml")
        p.write_text("\n".join(head + lines), encoding="utf-8")

        print("%s -> %s" % (label, p))
        print("   load 0x%08X  size 0x%X  entry 0x%08X" % (ram, size, entry))
        print("   sha1 %s" % sha)
        print("   entries %d:  arm %d   thumb %d   UNKNOWN %d"
              % (len(addrs), arm, thumb, unknown))
        if dranges:
            print("   %d data ranges declared; %d entries dropped as inside one"
                  % (len(dranges), in_data_n))
            if rescued_n:
                print("   %d KEPT despite a range - substantial and not data; "
                      "see the reconciliation comment" % rescued_n)
        if reasons:
            for why, n in sorted(reasons.items(), key=lambda kv: -kv[1])[:4]:
                print("     unknown because ...%s  x%d" % (why, n))
        if suspect:
            sp = OUT / (label + "_not_code.txt")
            sp.write_text("\n".join(suspect) + "\n", encoding="utf-8")
            print("   %d entries look like DATA, not code -> %s"
                  % (len(suspect), sp.name))
        print()

    print("These are a STARTING POINT for a bring-up, not a validated config.")
    print("Re-exporting the mode from the Ghidra database would be strictly")
    print("better than deriving it here, and is the right fix if this is taken")
    print("further.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
