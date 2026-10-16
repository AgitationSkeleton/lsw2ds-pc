"""Re-run the earlier marker interventions against a symptom that is on screen.

WHY THIS EXISTS. An earlier investigation tried three ways to turn the co-op
marker off - clearing the marker's on/off flag, nulling the arrow model pointers,
and nulling the ForceGlow model pointers - and all three appeared to fail. Every
poke was read back and confirmed to hold, and the state was checked before and
after, so the work was careful. But it was later established that no peer had
ever been ADMITTED in those runs, which means the marker was not being drawn at all.
Nulling a pointer for something that is not on screen changes nothing, and looks
exactly like a failed intervention.

So the eliminations have to be redone now that the arrow can be reproduced.

WHAT THIS DOES. It finds the marker instances the way the material-bind trace did -
from the material carrying TEXIMAGE_PARAM 0x0803682C, whose neighbourhood in RAM
holds the ASCII string "MultArrow" - then finds every pointer into that asset,
derives the owning instance, and applies one intervention at a time, reading
back each write and reporting the green-pixel count before and after.

IT MEASURES THE PICTURE, NOT THE POKE. A write that holds proves only that the
write held; whether the marker went away is a question about pixels.

Usage:
    python tools/arrow_intervene.py --port 19843 --what flag|model|none
"""

import json
import socket
import sys

START = 0x02000000
END = 0x02400000
CHUNK = 0x10000
NEEDLE = 0x0803682C

# The earlier investigation's own numbers: the instance's on/off flag is bit 0 of +0xB2, which
# lives in the word at +0xB0 as bit 16; the model pointer is at +0x04.
FLAG_WORD = 0xB0
FLAG_BIT = 0x00010000
MODEL_PTR = 0x04

# THE SECOND ASSET POINTER, and it has never been touched. A live marker reads
#   +00 020FFBB8 (vtable)  +04 022072F4 (model)  +08 0220730C (another asset)
# and the earlier attempt nulled only +04. Nulling a pointer frees nothing, so unlike the
# cartridge's off switch this is safe to try.
MODEL_PTR2 = 0x08

# THE MARKER CLASS'S VTABLE. Its two destructors 0209F494 and 0209F4E4 sit at
# +00 and +04 of it, and its asset setter 0209F640 at +08. Requiring word 0 to
# match keeps every poke off the impostor object that merely holds the same
# model pointer - measured at 021211C4, whose word 0 points into itself.
MARKER_VTABLE = 0x020FFBB8

# THE BYTE THE CARTRIDGE ITSELF CLEARS. FUN_020acba8, which the off switch calls
# after the freeing step, is only two lines:
#
#     FUN_020ab1a8(marker + 4);
#     *(u8 *)(marker + 0x1C) = 0;
#
# Clearing that byte is the whole of its visible state change, and unlike
# FUN_0209f5d0 it frees nothing. Live markers read 02120001 and 00000001 there,
# so the low byte is 1. write_mem only does 16 and 32 bit widths, so the word is
# read and rewritten with its low byte cleared, which is the same result.
ACTIVE_BYTE = 0x1C

# THE MATERIAL'S OWN ALPHA, and the game's own reason for skipping a draw.
# The ITCM bind reads the material's polygon attribute and refuses to bind at
# all when its alpha bits are zero:
#
#     01FFBC60  LDR r4, [r8, #+0xB0]      ; the material
#     01FFBC64  LDR r1, [r4, #+0xC]       ; its polygon attribute
#     01FFBC68  TST r1, #0x1F0000         ; the alpha field
#     01FFBC6C  BEQ 0x01FFBCF4            ; zero alpha -> skip
#
# So this is not an invented lever; it is the branch the cartridge already
# takes. The arrow's material reads 001F78FF, alpha 0x1F.
MAT_ATTR = 0x0C
MAT_ALPHA_MASK = 0x001F0000


def cmd(port, obj):
    s = socket.create_connection(("127.0.0.1", port), timeout=30.0)
    f = s.makefile("rwb")
    f.write((json.dumps(obj) + chr(10)).encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    return json.loads(line) if line.strip() else {}


def rd(port, addr, ln):
    h = cmd(port, {"cmd": "read_mem", "cpu": 9, "addr": addr, "len": ln}).get("hex", "")
    return bytes.fromhex(h) if h else b""


def wr(port, addr, value):
    # THE PARAMETER IS `value`, A NUMBER - NOT `hex`.
    #
    # The first version of this sent {"hex": ...}, which write_mem ignores, so
    # `value` defaulted to ZERO and every intervention wrote 0 no matter what it
    # meant to write. It then reported "wrote 00002000 now 00000000" and the
    # run read as "the poke reverted". It had not reverted; the poke was a
    # zeroing, and doing that to ten loosely-chosen objects faulted the guest.
    # Nothing about that run said anything about the marker.
    r = cmd(port, {"cmd": "write_mem", "cpu": 9, "addr": addr,
                   "value": value & 0xFFFFFFFF, "width": 32})
    if "error" in r:
        print("    write refused: %s" % r["error"])
    return r


def snapshot(port):
    blob = bytearray()
    a = START
    while a < END:
        n = min(CHUNK, END - a)
        d = rd(port, a, n)
        if len(d) != n:
            d = d + bytes(n - len(d))
        blob += d
        a += n
    return bytes(blob)


def w32(blob, addr):
    o = addr - START
    return None if o < 0 or o + 4 > len(blob) else int.from_bytes(blob[o:o + 4], "little")


def holders(blob, value):
    need = value.to_bytes(4, "little")
    out, off = [], 0
    while True:
        off = blob.find(need, off)
        if off < 0:
            break
        if off % 4 == 0:
            out.append(START + off)
        off += 4
    return out


def main():
    port = 19843
    what = "none"
    a = sys.argv[1:]
    i = 0
    while i < len(a):
        if a[i] == "--port":
            port = int(a[i + 1], 0); i += 2
        elif a[i] == "--what":
            what = a[i + 1]; i += 2
        else:
            i += 1

    blob = snapshot(port)
    mats = [h - 0x10 for h in holders(blob, NEEDLE)]
    if not mats:
        print("the marker's material is not in RAM - it is not on screen, so")
        print("nothing here would mean anything. No pokes were made.")
        return
    mat = mats[0]
    print("material %08X" % mat)

    # Confirm identity out loud: the asset name sits just before the material.
    win = blob[mat - START - 0x40: mat - START + 0x20]
    txt = "".join(chr(c) if 32 <= c < 127 else "." for c in win)
    print("asset name nearby: %s" % txt.strip("."))

    # ONLY THE INSTANCES THAT SHARE ONE MODEL POINTER.
    #
    # Taking every pointer into a 0x200 window around the material gave TEN
    # candidates, including objects at 0224xxxx that have nothing to do with the
    # marker, and poking all ten faulted the guest. The marker instances are the
    # ones whose +0x04 holds the SAME model pointer as each other - the earlier
    # investigation found exactly that shape, two instances both holding 0220B4F0 - so group
    # by the pointer value and take the largest group.
    groups = {}
    for cand in range(mat - 0x200, mat + 0x40, 4):
        for x in holders(blob, cand):
            groups.setdefault(cand, []).append(x - MODEL_PTR)
    if not groups:
        print("nothing points into the asset region.")
        return
    model_ptr = max(groups, key=lambda k: len(groups[k]))
    inst = sorted(set(groups[model_ptr]))
    # ONLY REAL MARKERS GET POKED. One of the three objects sharing the model
    # pointer is not a marker at all - its word 0 points into itself - and
    # writing to its +0xB0 or +0x04 would be corrupting an unrelated struct.
    real = [b for b in inst if w32(blob, b) == MARKER_VTABLE]
    if what != "none" and real:
        skipped = [b for b in inst if b not in real]
        for b in skipped:
            print("  skipping %08X - vtable %08X is not the marker's"
                  % (b, w32(blob, b)))
        inst = real
    print("model pointer %08X (material%+d) is shared by %d instances"
          % (model_ptr, model_ptr - mat, len(inst)))
    for k in sorted(groups):
        if k != model_ptr:
            print("  (ignoring %08X, %d holder(s))" % (k, len(groups[k])))
    print("instances referencing it: %s" % ", ".join("%08X" % b for b in inst))

    if what == "none":
        # THE VTABLE IS PRINTED BECAUSE THE BASE IS AN ASSUMPTION.
        #
        # "instance base = pointer address - 4" came from the earlier investigation and has been
        # carried ever since without being checked. The marker class's vtable is
        # 020FFBB8 (its two destructors are 0209F494 and 0209F4E4), so word 0 of
        # a real instance should read that. If it does not, these objects are
        # something else that merely holds the same model pointer, and every
        # conclusion about +0xB2 and +0x04 is about the wrong struct.
        for b in inst:
            vt = w32(blob, b)
            fw = w32(blob, b + FLAG_WORD)
            mp = w32(blob, b + MODEL_PTR)
            mark = "  <- marker vtable" if vt == 0x020FFBB8 else ""
            print("  %08X  vtable=%08X  flagword(+B0)=%08X  model(+04)=%08X%s"
                  % (b, vt if vt is not None else 0,
                     fw if fw is not None else 0,
                     mp if mp is not None else 0, mark))
            head = []
            for o in range(0, 0x20, 4):
                v = w32(blob, b + o)
                head.append("%08X" % (v if v is not None else 0))
            print("        +00..+1C  %s" % " ".join(head))
        return

    if what == "readback":
        # JUST LOOK, ON THIS PORT. Used to check whether an earlier write is
        # still in place AFTER a capture, which is the check an earlier experiment omitted.
        # It reads the same process the poke went to, which tools/read_guest_mem
        # does not - that prints both ports, host first.
        addr = mat + MAT_ATTR
        v = w32(blob, addr)
        print("  material attr %08X reads %08X now (alpha %d)"
              % (addr, v, (v & MAT_ALPHA_MASK) >> 16))
        return

    if what == "alpha":
        addr = mat + MAT_ATTR
        before = w32(blob, addr)
        want = before & ~MAT_ALPHA_MASK
        print("  material attr %08X: was %08X -> %08X (alpha %d -> 0)"
              % (addr, before, want, (before & MAT_ALPHA_MASK) >> 16))
        wr(port, addr, want)
        raw = rd(port, addr, 4)
        if len(raw) != 4:
            print("  READ-BACK FAILED, so nothing is known")
            return
        back = int.from_bytes(raw, "little")
        print("  now %08X %s" % (back, "HELD" if back == want else "REVERTED"))
        return

    for b in inst:
        if what == "flag":
            addr = b + FLAG_WORD
            before = w32(blob, addr)
            if before is None:
                continue
            want = before & ~FLAG_BIT
            if want == before:
                print("  %08X flag: already clear (%08X) - not written"
                      % (addr, before))
                continue
            wr(port, addr, want)
            raw = rd(port, addr, 4)
            if len(raw) != 4:
                print("  %08X flag: READ-BACK FAILED, so nothing is known"
                      % addr)
                continue
            back = int.from_bytes(raw, "little")
            print("  %08X flag: was %08X wrote %08X now %08X %s"
                  % (addr, before, want, back,
                     "HELD" if back == want else "REVERTED"))
        elif what == "byte1c":
            addr = b + ACTIVE_BYTE
            before = w32(blob, addr)
            if before is None:
                continue
            want = before & 0xFFFFFF00
            if want == before:
                print("  %08X +1C: low byte already 0 (%08X)" % (addr, before))
                continue
            wr(port, addr, want)
            raw = rd(port, addr, 4)
            if len(raw) != 4:
                print("  %08X +1C: READ-BACK FAILED" % addr)
                continue
            back = int.from_bytes(raw, "little")
            print("  %08X +1C: was %08X wrote %08X now %08X %s"
                  % (addr, before, want, back,
                     "HELD" if back == want else "REVERTED"))
        elif what == "model2":
            addr = b + MODEL_PTR2
            before = w32(blob, addr)
            if before is None or before == 0:
                print("  %08X model2: already null or unreadable" % addr)
                continue
            wr(port, addr, 0)
            raw = rd(port, addr, 4)
            if len(raw) != 4:
                print("  %08X model2: READ-BACK FAILED" % addr)
                continue
            back = int.from_bytes(raw, "little")
            print("  %08X model2: was %08X wrote 0 now %08X %s"
                  % (addr, before, back, "HELD" if back == 0 else "REVERTED"))
        elif what == "model":
            addr = b + MODEL_PTR
            before = w32(blob, addr)
            if before is None:
                continue
            if before == 0:
                print("  %08X model: already null - not written" % addr)
                continue
            wr(port, addr, 0)
            raw = rd(port, addr, 4)
            if len(raw) != 4:
                print("  %08X model: READ-BACK FAILED, so nothing is known"
                      % addr)
                continue
            back = int.from_bytes(raw, "little")
            print("  %08X model: was %08X wrote 0 now %08X %s"
                  % (addr, before, back, "HELD" if back == 0 else "REVERTED"))


main()
