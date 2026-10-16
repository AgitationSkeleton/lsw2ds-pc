"""The three ARM9 scans this project keeps re-typing, with the bug taken out.

WHY THIS EXISTS. Every investigation that reads the ROM writes the same three
helpers inline - "which function contains this address", "who calls it", "is it
in a vtable" - and one of them was wrong for months in a way that produced
confident, plausible, false answers.

`entry_of` stepped back one instruction BEFORE testing for a prologue. So when
the address handed to it was itself a function entry, it silently returned the
function before it. That is not a rare input: it is exactly what happens when
you feed it the result of a previous scan.

It caused two published mistakes in quick succession:

  - One research note named the player-count derivation `0x020719E0`. It is
    `0x02071B9C`, which is a prologue, so the walk skipped it.
  - Another concluded the customize builder "begins at `0x02050B48`" and
    reasoned from its first instructions. `0x02050C44` is its own entry;
    `0x02050B48` is a DIFFERENT entry in the same dispatch table (index 3, not
    index 0), and reading one as the head of the other inverted the
    customization work's premise until it was caught.

Both were readings of real code that could not be checked by any of the four
test gates, because the code was fine and the question was wrong.

    python tools/armscan.py                 run the self-test
    python tools/armscan.py 0x02050C44      report on one address

WHAT IT DELIBERATELY DOES NOT DO. It does not disassemble - `tools/disasm_arm.py`
does that. It answers structural questions only, and every answer it gives is
one a human still has to read the code to believe.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
BASE = 0x02000000


def load(path: Path = IMAGE) -> bytes:
    if not path.is_file():
        # EXIT 2 IS "SKIPPED", not "failed" - the suite's own three-outcome
        # rule. A checkout without the extracted ROM has not broken anything.
        print(f"no ARM9 image at {path} - run the extraction first")
        raise SystemExit(2)
    return path.read_bytes()


def word(data: bytes, addr: int) -> int:
    off = addr - BASE
    if off < 0 or off + 4 > len(data):
        raise ValueError(f"0x{addr:08X} is outside the image")
    return struct.unpack_from("<I", data, off)[0]


def is_prologue(w: int) -> bool:
    """`STM sp!, {..., lr}` - a pushing STM that saves lr.

    Not every function starts this way; leaf functions that touch no lr push
    nothing at all. So a MISS from `entry_of` means "no pushing prologue above
    this address", never "this address is not in a function".
    """
    return (w & 0xFFFF0000) == 0xE92D0000 and bool(w & 0x4000)


def entry_of(data: bytes, addr: int, limit: int = 0x4000) -> int | None:
    """The function entry at or above `addr`.

    THE FIX IS THE FIRST LINE OF THE LOOP: test `addr` itself before stepping
    back. An address that is already an entry is its own answer, and returning
    the previous function for it is the failure this module was written for.

    `limit` bounds the walk so a bad address scans a page rather than the image.
    """
    a = addr & ~3
    stop = max(BASE, a - limit)
    while a >= stop:
        if is_prologue(word(data, a)):
            return a
        a -= 4
    return None


def callers(data: bytes, target: int) -> list[int]:
    """Every `BL` site whose destination is `target`. Sites, not functions."""
    out = []
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if (w >> 24) & 0x0F != 0x0B:
            continue
        imm = w & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        if BASE + off + 8 + (imm << 2) == target:
            out.append(BASE + off)
    return out


def word_refs(data: bytes, value: int) -> list[int]:
    """Every word in the image equal to `value` - vtable slots and literals.

    A hit is NOT proof of a vtable: literal pools hold the same shape. The
    caller has to look. This returns candidates.
    """
    return [
        BASE + off
        for off in range(0, len(data) - 3, 4)
        if struct.unpack_from("<I", data, off)[0] == value
    ]


def literal_loaders(data: bytes, value: int) -> list[tuple[int, int]]:
    """`LDR rD, [pc, #imm]` sites whose literal is `value`, as (site, rD)."""
    out = []
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if (w & 0x0F7F0000) != 0x051F0000:
            continue
        imm = w & 0xFFF
        lit = BASE + off + 8 + (imm if (w >> 23) & 1 else -imm)
        if BASE <= lit < BASE + len(data) and word(data, lit) == value:
            out.append((BASE + off, (w >> 12) & 0xF))
    return out


def field_access(data: bytes, offset: int) -> list[tuple[int, str]]:
    """Every load/store of `[rN, #offset]` where rN is NOT the PC.

    WHY THE PC EXCLUSION IS THE WHOLE POINT. `LDR rD, [pc, #0x48]` is a literal
    pool load and encodes almost identically to `LDR rD, [rN, #0x48]`. An
    ad-hoc pattern that forgets base != 15 reports literal loads as field
    accesses, and one did - it "found" a vtable accessor reading +0x48 that was
    really loading a constant, and a false conclusion very nearly got written
    down.

    Returns (address, kind) with kind in {"LDR","STR","LDRB","STRB"}.
    Halfword forms use a different encoding and are not covered here.
    """
    out = []
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if ((w >> 16) & 0xF) == 15:
            continue
        if (w & 0x0FFF) != (offset & 0xFFF):
            continue
        kind = w & 0x0FF00000
        name = {0x05900000: "LDR", 0x05800000: "STR",
                0x05D00000: "LDRB", 0x05C00000: "STRB"}.get(kind)
        if name:
            out.append((BASE + off, name))
    return out


def _self_test() -> int:
    data = load()
    checks = failed = 0

    def check(ok: bool, what: str) -> None:
        nonlocal checks, failed
        checks += 1
        if ok:
            print(f"  ok: {what}")
        else:
            failed += 1
            print(f"  FAIL: {what}")

    # THE CASE THAT BROKE, both halves of it.
    check(is_prologue(word(data, 0x02071B9C)),
          "0x02071B9C is a prologue (the address that exposed the bug)")
    check(entry_of(data, 0x02071B9C) == 0x02071B9C,
          "an address that IS an entry returns itself, not the one before")
    check(entry_of(data, 0x020720D0) == 0x02071B9C,
          "the count derivation resolves to 0x02071B9C, not 0x020719E0")

    check(is_prologue(word(data, 0x02050C44)),
          "0x02050C44 is its own entry")
    check(entry_of(data, 0x02050C44) == 0x02050C44,
          "the character builder is not read as starting at 0x02050B48")
    check(entry_of(data, 0x02050B48) == 0x02050B48,
          "...and 0x02050B48 is a separate entry, as the table says")

    # The two chains that were checked and came back unchanged - pinned so a
    # future "correction" cannot quietly move them.
    check(entry_of(data, 0x020B0C1C) == 0x020B0BDC,
          "the mask refresh is 0x020B0BDC")
    check(callers(data, 0x020B0BDC) == [0x020B0994],
          "...with exactly one caller")
    check(entry_of(data, 0x0206F894) == 0x0206F848,
          "the count propagator is 0x0206F848")

    # The dispatch table entries these addresses belong to.
    check(word(data, 0x020ED5CC + 0 * 4) == 0x02050C44,
          "appearance table index 0 is 0x02050C44")
    check(word(data, 0x020ED5CC + 3 * 4) == 0x02050B48,
          "appearance table index 3 is 0x02050B48 - a DIFFERENT builder")

    # The session authority's accessor, since viewmask.h rests on it.
    check(word(data, 0x020FFD50 + 10 * 4) == 0x020AD8CC,
          "authority vtable slot 10 is the mask accessor")
    check(word(data, 0x020FFD50 + 11 * 4) == 0x020ADC78,
          "slot 11 is the active-view accessor")

    # THE PART TABLES, checked against the ROM rather than against the port's
    # copy of them. port/frontend/partslot_policy.cpp transcribes the ranges and
    # strides from 0x0208DA74/0x0208DAD8; these assert the bytes those addresses
    # actually land on, so a transcription error cannot pass by agreeing with
    # itself. All eight are headwear, and config/customization_modes.json
    # recorded slot_index 3 for them by an unrelated method.
    for pid, name, desc_addr in (
        (178, "BaseballCap", 0x020F2B48),
        (187, "CowboyHat", 0x020F2CD4),
        (188, "Crown", 0x020F2D00),
        (202, "KnightHelmet", 0x020F2F68),
        (212, "Moustache", 0x020F3120),
        (214, "PirateHat", 0x020F3178),
        (228, "TopHat", 0x020F33E0),
        (229, "WizardHat", 0x020F340C),
    ):
        slot = data[desc_addr + 8 - BASE]
        check(slot == 3, f"part {pid} ({name}) descriptor +8 reads slot 3")

    # THE TWO CUSTOMIZATION STORES, which are different subsystems and were
    # conflated across two research notes before being corrected.
    # Pinned here so the next reader cannot re-merge them.
    #
    #   Customize panel  = Mix N Match, id 0x48, six HALFWORDS at
    #                      0x02107890 + 0xA6..0xB0
    #   Character Creator = Custom 1..6, ids 0x49.., six BYTES at
    #                      0x02104DE2 + (id - 0x49) * 6
    check(word(data, 0x02002E34) == 0xE2481049,
          "the creator's index is (id - 0x49), not (id - 0x48)")
    check(word(data, 0x02002E38) == 0xE3A00006,
          "...times six, one byte per part")
    creator_lit = 0x02002E40 + 8 + (word(data, 0x02002E40) & 0xFFF)
    check(word(data, creator_lit) == 0x02104DE2,
          "the creator's parts live at 0x02104DE2")
    check(0x02104DE2 - 0x02104D90 == 0x52,
          "...which is +0x52 into the save object, just past the page memory")

    # THE ATTACH/DETACH PAIR, and the claim that nothing drops a hat on damage.
    # If a future reading finds a fourth caller of the detach, this breaks and
    # the claim has to be re-argued rather than quietly inherited.
    check(len(callers(data, 0x02006A50)) == 3,
          "the detach 0x02006A50 has exactly three callers")
    check(entry_of(data, 0x0207B39C) == 0x0207B370,
          "one of them is the general slot setter 0x0207B370")
    check(word(data, 0x0207B38C) == 0xE35400FA,
          "...which detaches only when the requested part is 0xFA")
    check(word(data, 0x02006A64) == 0xE35000FA,
          "the detach returns early on an already-empty slot")
    check(word(data, 0x02006AA0) == 0xE3A010FA,
          "...and writes 0xFA back when it clears one")

    # 0x02004800 clears slots at +0xE8 and +0xEC - that is slots 0 and 2.
    # Headwear is slot 3, so this path cannot be what removes a hat.
    check(word(data, 0x02004854) == 0xE1C01EB8 and
          word(data, 0x02004858) == 0xE1C01EBC,
          "0x02004800 clears slots 0 and 2 only, never slot 3")

    # The cartridge's own per-station appearance handler, called from inside
    # the receive pump - so peer customization has a native protocol.
    check(word(data, 0x020B04A8) == 0xEB000366,
          "the pump calls the appearance handler 0x020B1248 at 0x020B04A8")
    check(entry_of(data, 0x020B04A8) == 0x020B0410,
          "...and that call site is inside the receive pump")

    # THE HELMET CLASS. Applying a customize hat to Boba or Jango Fett swaps
    # the helmet for the hat and leaves an invisible head.
    # Byte 4 of the 0x18-byte character descriptor at 0x020EF57C picks out
    # exactly those two and nobody else - a data-driven confirmation of a
    # report, arrived at without knowing which characters to look for.
    tbl, stride = 0x020EF57C, 0x18
    helmet = [c for c in range(79) if data[tbl + c * stride + 4 - BASE] == 9]
    check(helmet == [7, 29],
          "descriptor byte 4 == 9 is exactly Boba Fett and Jango Fett")

    # The same byte marks the seven customizable characters with 0x20, which is
    # a bit rather than one of the small class numbers 1..9.
    custom = [c for c in range(79) if data[tbl + c * stride + 4 - BASE] == 0x20]
    check(custom == [72, 73, 74, 75, 76, 77, 78],
          "descriptor byte 4 == 0x20 is Mix N Match plus Custom 1-6")

    # DESCRIPTOR +8 IS THE PORTRAIT, not a head model. An earlier note guessed
    # "head/torso/legs" without reading free_play_pages.json's own source
    # block, which already recorded portrait_field +0x08. Checked against a
    # handful of characters whose portrait ids are known independently.
    for cid, portrait in ((20, 2530), (7, 606), (29, 3058), (48, 3656)):
        a = tbl + cid * stride + 8 - BASE
        got = data[a] | (data[a + 1] << 8)
        check(got == portrait,
              f"character {cid} descriptor+8 is portrait asset {portrait}")

    # THE PART ID RANGES ARE THE SLOTS. config/part_slots.json rests on this:
    # every part in a category reports the same slot, so an id alone decides where
    # it goes. Checked at each range's two ends, where an off-by-one would put a
    # head in the torso table.
    for pid, want in ((0, 0), (44, 0), (45, 1), (106, 1), (107, 2),
                      (176, 2), (177, 3), (231, 3), (232, 4), (249, 4)):
        for lo, hi, tb, st in ((0x00, 0x2C, 0x020F2414, 0x28),
                               (0x2D, 0x6A, 0x020F3E68, 0x2C),
                               (0x6B, 0xB0, 0x020F3490, 0x24),
                               (0xB1, 0xE7, 0x020F2B1C, 0x2C),
                               (0xE8, 0xF9, 0x020F2144, 0x28)):
            if lo <= pid <= hi:
                got = data[tb + (pid - lo) * st + 8 - BASE]
                break
        check(got == want, f"part {pid} reports slot {want}")

    # THE PAUSE MENU HAS EXACTLY SIX ROWS, dispatched positionally. It was
    # once feared that the row SET might vary by state; the selection handler's own
    # jump table settles it - CMP #5 and six branches. Only the LABEL varies
    # (0x03C/0x03D/0x03E), which is what the id-based hiding handles.
    check(word(data, 0x0208EA64) == 0xE59900EC,
          "the selection handler reads the cursor at menu+0xEC")
    check(word(data, 0x0208EA68) == 0xE3500005,
          "...bounds it at 5, so there are exactly six rows")
    check(word(data, 0x0208EA6C) == 0x908FF100,
          "...and jump-tables on it, positionally")
    check(word(data, 0x0208EA78) == 0xEA00001A,
          "row 1 dispatches to 0x0208EAE8 - the wireless row")
    check(word(data, 0x0208EB04) == 0xE3110040,
          "which branches on the multi-view bit: host, or exit")
    # NOT A SEAM. 0x020B111C has EIGHT callers in four functions, so
    # it is a shared commit routine that the host path calls, not the host
    # start. Pinned so the "single-function override" idea cannot come back.
    check(word(data, 0x0208EB28) == 0xEB00897B,
          "row 1 calls 0x020B111C on the host path")
    check(len(callers(data, 0x020B111C)) == 8,
          "...which has eight callers, so overriding it would hijack seven")

    # EVERY FUNCTION ON ROW 1'S PATH IS SHARED. There is no cheap
    # seam: the row tears down, unpauses, then requests a state, and all three
    # are general-purpose. The only thing unique to row 1 is a branch target
    # inside the handler, which is not a function and cannot be overridden.
    check(len(callers(data, 0x02093C6C)) == 14,
          "the unpause 0x02093C6C is shared by 14 callers")
    check(len(callers(data, 0x0205ECC4)) == 176,
          "the state request 0x0205ECC4 is shared by 176 - the whole game")
    check(word(data, 0x0208EB50) == 0xEBFF405B,
          "row 1's real action is that state request, at 0x0208EB50")

    # THE TWO OVERRIDES WITH MANY CALLERS both reimplement their target
    # exactly, which is why a high caller count is a question and not a
    # failure. WH_GetSystemState is three instructions:
    check(word(data, 0x020AD314) == 0xE5900020,
          "0x020AD310 is LDR r0, [authority + 0x20] - what the port's override does")
    check(word(data, 0x020AD318) == 0xE12FFF1E,
          "...then BX lr, with nothing else in it")
    # And its object is the same session authority mapped earlier, whose +0xE
    # is the connection mask and +0x8 the active view.
    check(word(data, 0x020AD31C) == 0x0210B320,
          "...on 0x0210B320, the session authority")

    # THE ENHANCED CAMERA'S CHAIN, verified end to end. The movement direction
    # function reads the view index and refuses to rotate at the sentinel.
    check(word(data, 0x0204A5C0) == 0xE5D1400C,
          "0x0204A5BC reads the index at [r1 + 0x0C]")
    check(word(data, 0x0204A5C8) == 0xE3540004,
          "...compares it against 4, the sentinel")
    check(word(data, 0x0204A5CC) == 0x03A00000 and
          word(data, 0x0204A5D0) == 0x01C500B0,
          "...and on a match writes yaw 0 - which is tank movement")
    # And [r1 + 0x0C] IS actor + 0x5DA: every caller builds the pointer in two
    # steps because 0x5CE is not a single ARM immediate.
    check(word(data, 0x02047600) == 0xE28410CE,
          "a caller does ADD r1, actor, #0xCE")
    check(word(data, 0x02047608) == 0xE2811C05,
          "...then ADD r1, r1, #0x500 - so +0x0C lands on actor + 0x5DA")

    # THE ENHANCED CAMERA'S YAW SOURCE. 0x0204A5BC hands a real index to
    # 0x020AAE4C, which in MULTI-VIEW resolves the record and reads +0x6A -
    # so the right stick's implementation is a write to that halfword.
    check(word(data, 0x020AAE5C) == 0xE3100040,
          "0x020AAE4C branches on the multi-view bit")
    check(word(data, 0x020AAE78) == 0xE12FFF32,
          "...calls the manager's slot 0 to resolve the record")
    check(word(data, 0x020AAE7C) == 0xE1D006BA,
          "...and reads the yaw at record + 0x6A")
    # Single view takes a different source entirely - +0x42 of another object.
    check(word(data, 0x020AAE8C) == 0x01D104B2,
          "single-view index 0 reads +0x42 instead, a different field")

    # THE NETWORK PROTOCOL. 0x020B1248 is reached from the receive
    # pump for every pending slot and jump-tables on byte 1 of the message.
    check(word(data, 0x020B1254) == 0xE5D43001,
          "the dispatcher reads the message type at byte 1")
    check(word(data, 0x020B125C) == 0xE3530045,
          "...bounds it at 0x45, so there are 0x46 message types")
    check(word(data, 0x020B1260) == 0x908FF103,
          "...and jump-tables on it")

    # TWO MESSAGE HANDLERS worth pinning by what they call.
    def _branch_target(addr):
        w = word(data, addr)
        if (w >> 25) & 7 != 5:
            return None
        imm = w & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        return addr + 8 + (imm << 2)

    def _calls(start, limit=0x100):
        out = []
        for a in range(start, start + limit, 4):
            w = word(data, a)
            if (w >> 24) & 0x0F == 0x0B:
                imm = w & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 0x1000000
                out.append(a + 8 + (imm << 2))
            if (w & 0xFFFF0000) == 0xE8BD0000 and (w & 0x8000):
                break
        return out

    # 0x13 reaches the player-count setter, so a RECEIVED message can set
    # [0x020FEB88] - the other end of the view-mask work.
    h13 = _branch_target(0x020B1268 + 0x13 * 4)
    check(h13 is not None and 0x02073054 in _calls(h13),
          "message type 0x13's handler calls the player-count setter")

    # And four types dress an actor - the native peer-appearance protocol.
    dressers = [t for t in (0x1C, 0x1D, 0x1E, 0x34)
                if (lambda h: h is not None and 0x020029C4 in _calls(h))(
                    _branch_target(0x020B1268 + t * 4))]
    check(dressers == [0x1C, 0x1D, 0x1E, 0x34],
          "types 0x1C, 0x1D, 0x1E and 0x34 all call the dresser")

    # 0x15's handler unpauses first, which is what a session-start message does.
    check(word(data, 0x020B1CF8) == 0xEBFF87DB,
          "type 0x15's handler calls the unpause 0x02093C6C first")

    # THE GAME'S OWN COUNT BOOTSTRAP. Two sites set ALL THREE count
    # words directly, to a hardcoded 2 - so writing them is a mode the game
    # itself uses, not only a conclusion it derives. Which mechanism is live
    # depends on the state.
    check(word(data, 0x020700E0) == 0xE3A01002 and
          word(data, 0x020700E8) == 0xE3A00002 and
          word(data, 0x020700F0) == 0xE3A00002,
          "0x0207009C sets all three counts to a literal 2")
    check(word(data, 0x020727AC) == 0xE3A01002 and
          word(data, 0x020727B4) == 0xE3A00002 and
          word(data, 0x020727BC) == 0xE3A00002,
          "...and 0x02072700 does the same, from a state vtable slot")

    # THE CARTRIDGE'S OWN ARENA MENU. Four rows, ids computed as
    # 0x2A5 + row - which is why no id table exists to find - centred at X=128,
    # first row Y=0x3A, pitch 0x1C. Different metrics from the PAUSE menu's
    # 0x28/0x19, and the fork's menus currently use the pause menu's.
    check(word(data, 0x0209CB4C) == 0xE28900A5,
          "the arena menu computes its string id as row + 0xA5")
    check(word(data, 0x0209CB50) == 0xE2800C02,
          "...plus 0x200, so the ids are 0x2A5..0x2A8")
    check(word(data, 0x0209CB80) == 0xE3590004,
          "...over four rows")
    check(word(data, 0x0209CB00) == 0xE3A0401C,
          "...with a row pitch of 0x1C")
    check(word(data, 0x0209CB6C) == 0xE283303A,
          "...and a first row at Y = 0x3A")

    # THE PAUSE HANDLER'S ARMS ARE NOT LANDING PADS. An override of
    # 0x0208EA58 cannot dispatch back into an arm it does not want to change,
    # so it must reproduce them - which is what makes the entry point cost what
    # it costs. If the recompiler ever emits landing pads here, this breaks and
    # the cost estimate for that override should be redone rather than inherited.
    check(word(data, 0x0208EA74) == 0xEA000004,
          "row 0's arm is a plain branch, not a call")
    check(word(data, 0x0208EA88) == 0xEA00011C,
          "row 5's arm likewise - the six are interior blocks")

    # THE CO-OP SCREENS ARE STATES, not entities. 0x02069514 sends
    # message 0x15 and tears down - the pause row's pair - and is reached only
    # through a vtable, whose class the STATE FACTORY builds. So the Cantina
    # console requests a state; it does not open a menu.
    check(word(data, 0x020695CC) == 0xE3A01015,
          "0x02069514 sends message 0x15, the host message")
    check(word(data, 0x020695E0) == 0xE3A01001,
          "...and calls the teardown with 1, as the pause row does")
    check(len(callers(data, 0x02069514)) == 0,
          "...and has no BL callers - it is reached through a vtable")

    # THE STATE FACTORY. 0x0205F574 dispatches 58 states through a
    # jump table, and STATE 20 builds the co-op screen class - the trail
    # the co-op-screen finding above followed. config/game_states.json rests on these three.
    check(word(data, 0x0205F58C) == 0xE3590039,
          "the state factory bounds the state id at 0x39 - 58 states")
    check(word(data, 0x0205F594) == 0x908FF109,
          "...and jump-tables on it")
    check(word(data, 0x0205F5AC) == 0xEA000090,
          "...with per-state arms, not a single body")

    # THE CO-OP HOST TABLE (the master server's co-op host listing). coophosts.h transcribes
    # 16 slots of 0xC4 with the TTL at +0xC0; 0x020AD8EC walks exactly that to
    # DERIVE the count, which is why the port can publish without an override.
    check(word(data, 0x020AD8F4) == 0xE59310C0,
          "the count walk reads the TTL at record +0xC0")
    check(word(data, 0x020AD904) == 0xE3520010,
          "...over 16 slots")
    check(word(data, 0x020AD908) == 0xE28330C4,
          "...striding 0xC4 per record")

    # THE CUSTOMIZE PANEL IS FIVE PARTS AND A WEAPON. The store holds six
    # halfwords at 0x02107890 + 0xA6..0xB0 and there are only five slots. The
    # first five go to the gear applier; the SIXTH goes to 0x02004404, which
    # writes actor+0x6C3 - the action row, i.e. the weapon. So "attachments and
    # weapon changes" is literally what the panel stores.
    check(word(data, 0x02002C20) >> 24 == 0xEB,
          "the fifth customize value goes to a BL")
    check(word(data, 0x02002C2C) == 0xE1D11BB0,
          "the sixth value is read from +0xB0")
    check(word(data, 0x02002C30) >> 24 == 0xEB,
          "...and goes to a different call than the other five")
    check(word(data, 0x0200445C) == 0xE5C656C3,
          "0x02004404 writes the action row to actor+0x6C3")
    check(word(data, 0x02004414) == 0xE3A0100F,
          "...after consulting flag 15, the same flag the gear applier uses")

    # THE CUSTOM-CHARACTER BLOCK IS CUSTOMS ONLY. Inside it the dresser reads a
    # 12-byte per-character record - five attachment halfwords plus a weapon
    # byte - and the inner tests at 0x02002D64/0x02002D6C compare against 0x49
    # and 0x4B, which reads exactly like "any ordinary character". It is not:
    # the ENCLOSING guard has already restricted r8 to 0x49..0x4E, so the inner
    # "id < 0x49" arm is unreachable. Read alone, the inner test says the
    # opposite of the truth, and that is why the outer one is pinned here.
    check(word(data, 0x02002D44) == 0xE3580049,
          "the custom block is entered only for id >= 0x49")
    check(word(data, 0x02002D4C) == 0xE358004E,
          "...and only for id <= 0x4E - Custom 1 through 6")
    check(word(data, 0x02002D5C) == 0xE3100040,
          "inside it, the multi-view bit 0x40 picks between two custom stores")

    # The weapon. Descriptor byte +5 is the action row, copied to actor+0x6C3;
    # the action row's +0x00 is the weapon asset (config/bounty_hunter_weapons).
    check(word(data, 0x020062A0) == 0xE5C816C3,
          "the action row is written to actor+0x6C3 at 0x020062A0")
    check(word(data, 0x02002DC8) == 0xE5C916C3 and
          word(data, 0x02002E28) == 0xE5C916C3,
          "...and the two custom paths write the same field")

    # THE PC EXCLUSION, pinned. 0x020AD3E4 is `LDR r1, [pc, #0x48]` - a literal
    # load that an ad-hoc "+0x48 field access" pattern reported as an accessor,
    # nearly producing a false conclusion about the session authority.
    check(all(a != 0x020AD3E4 for a, _ in field_access(data, 0x48)),
          "field_access does not report the literal load at 0x020AD3E4")
    check(any(a == 0x020ADE14 for a, _ in field_access(data, 0x48)),
          "...but does report the real read at 0x020ADE14")

    print(f"\n{checks} checks, {failed} failed")
    return 1 if failed else 0


def _report(addr: int) -> int:
    data = load()
    entry = entry_of(data, addr)
    print(f"0x{addr:08X}")
    print(f"  entry      : {'0x%08X' % entry if entry else 'none within range'}")
    if entry is None:
        return 0
    cs = callers(data, entry)
    print(f"  BL callers : {[hex(x) for x in cs] if cs else 'none'}")
    refs = word_refs(data, entry)
    print(f"  word refs  : {[hex(x) for x in refs] if refs else 'none'}")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) > 1:
        return _report(int(argv[1], 0))
    return _self_test()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
