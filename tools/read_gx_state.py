"""Read the captured 3D state out of a melonDS savestate, and say what it uses.

WHY. The top screen of this game is entirely 3D - every capture shows engine A
with BG0 taken by the 3D layer on all 192 scanlines - so nothing of the actual
game world appears until the geometry engine and its rasteriser exist. Both are
large, and building either against the whole DS 3D specification would mean
implementing a lot of hardware this cartridge may never use.

The savestate settles that by measurement. It carries VertexRAM and PolygonRAM
as melonDS left them, which is the geometry ALREADY TRANSFORMED into screen
space: final positions, final colours, per-polygon texture parameters, and the
render list. So this reports what LSW2 actually asks for - which texture
formats, which blending modes, how many polygons, whether it uses shadows,
translucency, the W buffer - and the rasteriser gets written for that.

It also means the two halves can be validated separately, each against the ROM:
the rasteriser against this captured polygon list, and later the geometry
pipeline by comparing the PolygonRAM it produces against this one.

THE LAYOUT IS NOT GUESSED. It is transcribed from GPU3D::DoSavestate in the
melonDS tree vendored at tools/vendor/melonds-src, whose SAVESTATE_MAJOR/MINOR
match the version in the file. The transcription checks itself: every field is
sized and the running total must land exactly on the end of the section. If it
does not, the layout is wrong somewhere and the tool says so instead of
reporting numbers read from the wrong offsets.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import struct

SECTION_HEADER = 16          # magic(4) + length(4) + 8 reserved

# Sizes that recur.
VERTEX = 4*4 + 3*4 + 2*2 + 4 + 2*4 + 3*4 + 2*4      # 64
POLYGON = (10*4 + 4 + 10*4 + 10*4 + 4 + 4 + 4 + 4
           + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4)


def fifo(entry_size: int, entries: int) -> int:
    return 4 + 4 + 4 + entry_size * entries


# GPU3D::DoSavestate, in order. (name, bytes)
LAYOUT = [
    ("CmdFIFO", fifo(8, 256)),
    ("CmdPIPE", fifo(8, 4)),
    ("NumCommands", 4), ("CurCommand", 4), ("ParamCount", 4),
    ("TotalParams", 4),
    ("NumPushPopCommands", 4), ("NumTestCommands", 4),
    ("DispCnt", 4), ("AlphaRefVal", 1), ("AlphaRef", 1),
    ("ToonTable", 64), ("EdgeTable", 16),
    ("FogColor", 4), ("FogOffset", 4), ("FogDensityTable", 32),
    ("ClearAttr1", 4), ("ClearAttr2", 4),
    ("RenderDispCnt", 4), ("RenderAlphaRef", 1),
    ("RenderToonTable", 64), ("RenderEdgeTable", 16),
    ("RenderFogColor", 4), ("RenderFogOffset", 4), ("RenderFogShift", 4),
    ("RenderFogDensityTable", 34),
    ("RenderClearAttr1", 4), ("RenderClearAttr2", 4),
    ("RenderXPos", 2),
    ("ZeroDotWLimit", 4),
    ("GXStat", 4),
    ("ExecParams", 128), ("ExecParamCount", 4), ("CycleCount", 4),
    ("Timestamp", 8),
    ("MatrixMode", 4),
    ("ProjMatrix", 64), ("PosMatrix", 64), ("VecMatrix", 64), ("TexMatrix", 64),
    ("ProjMatrixStack", 64), ("PosMatrixStack", 32*64),
    ("VecMatrixStack", 32*64), ("TexMatrixStack", 64),
    ("ProjMatrixStackPointer", 4), ("PosMatrixStackPointer", 4),
    ("TexMatrixStackPointer", 4),
    ("Viewport", 24),
    ("PosTestResult", 16), ("VecTestResult", 6),
    ("VertexNum", 4), ("VertexNumInPoly", 4), ("NumConsecutivePolygons", 4),
    ("TempVertexBuffer", 4 * VERTEX),
    ("LastStripPolygonIndex", 4),
    ("CurRAMBank", 4), ("NumVertices", 4), ("NumPolygons", 4),
    ("NumOpaquePolygons", 4),
    ("FlushRequest", 4), ("FlushAttributes", 4),
    ("VertexRAM", 6144 * 2 * VERTEX),
    ("PolygonRAM", 2048 * 2 * POLYGON),
    ("CmdStallQueue", fifo(8, 64)),
    ("VertexPipeline", 4), ("NormalPipeline", 4), ("PolygonPipeline", 4),
    ("VertexSlotCounter", 4), ("VertexSlotsFree", 4),
    ("RenderNumPolygons", 4),
    ("RenderPolygonRAM", 2048 * 4),
    ("CurVertex", 6), ("VertexColor", 3), ("TexCoords", 4),
    ("RawTexCoords", 4), ("Normal", 6),
    ("LightDirection", 2*4*3), ("LightColor", 4*3),
    ("MatDiffuse", 3), ("MatAmbient", 3), ("MatSpecular", 3),
    ("MatEmission", 3),
    ("UseShininessTable", 4), ("ShininessTable", 128),
    ("AbortFrame", 4), ("GeometryEnabled", 4), ("RenderingEnabled", 4),
    ("PolygonMode", 4), ("PolygonAttr", 4), ("CurPolygonAttr", 4),
    ("TexParam", 4), ("TexPalette", 4),
]

TEX_FORMAT = {
    0: "none", 1: "A3I5", 2: "4 colour", 3: "16 colour", 4: "256 colour",
    5: "4x4 compressed", 6: "A5I3", 7: "direct 16-bit",
}
BLEND_MODE = {0: "modulation", 1: "decal", 2: "toon/highlight", 3: "shadow"}


def sections(data: bytes) -> dict:
    out, off = {}, 16
    while off + SECTION_HEADER <= len(data):
        tag = data[off:off+4].decode("latin1", "replace")
        (ln,) = struct.unpack_from("<I", data, off + 4)
        if ln < SECTION_HEADER or off + ln > len(data):
            break
        out[tag] = (off + SECTION_HEADER, ln - SECTION_HEADER)
        off += ln
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("state")
    ap.add_argument("--export",
                    help="write the render list as a flat file the C "
                         "rasteriser can read")
    a = ap.parse_args()

    data = pathlib.Path(a.state).read_bytes()
    if data[:4] != b"MELN":
        raise SystemExit("not a melonDS savestate")
    sec = sections(data)
    if "GP3D" not in sec:
        raise SystemExit("no GP3D section")
    base, length = sec["GP3D"]

    at, off = {}, 0
    for name, size in LAYOUT:
        at[name] = off
        off += size

    print("GP3D payload   %d bytes" % length)
    print("layout total   %d bytes" % off)
    if off != length:
        print("MISMATCH of %d bytes - the layout does not describe this file, "
              "so nothing below would be read from the right place." % (off - length))
        return 1
    print("the transcribed layout lands exactly on the end of the section.")
    print()

    def u32(name, i=0):
        return struct.unpack_from("<I", data, base + at[name] + i*4)[0]

    disp = u32("RenderDispCnt")
    print("RenderDispCnt 0x%08X" % disp)
    print("  texture mapping      %s" % ("on" if disp & 1 else "off"))
    print("  shading              %s" % ("toon/highlight" if disp & 2 else "none"))
    print("  alpha test           %s" % ("on" if disp & 4 else "off"))
    print("  alpha blending       %s" % ("on" if disp & 8 else "off"))
    print("  anti-aliasing        %s" % ("on" if disp & 0x10 else "off"))
    print("  edge marking         %s" % ("on" if disp & 0x20 else "off"))
    print("  fog                  %s" % ("on" if disp & 0x80 else "off"))
    vp = [struct.unpack_from("<I", data, base + at["Viewport"] + i*4)[0]
          for i in range(6)]
    print("  viewport             %s" % vp)

    n = u32("RenderNumPolygons")
    total = u32("NumPolygons")
    print()
    print("polygons in the render list %d   (PolygonRAM holds %d)" % (n, total))

    fmt = collections.Counter()
    blend = collections.Counter()
    sides = collections.Counter()
    flags = collections.Counter()
    tmode = collections.Counter()
    tsize = collections.Counter()
    poly_base = base + at["PolygonRAM"]
    listed = base + at["RenderPolygonRAM"]
    for i in range(n):
        (idx,) = struct.unpack_from("<I", data, listed + i*4)
        if idx == 0xFFFFFFFF or idx >= 4096:
            flags["unresolved list entry"] += 1
            continue
        p = poly_base + idx * POLYGON
        nv = struct.unpack_from("<I", data, p + 40)[0]
        attr = struct.unpack_from("<I", data, p + 124 + 4)[0]
        texp = struct.unpack_from("<I", data, p + 124 + 8)[0]
        translucent = struct.unpack_from("<I", data, p + 124 + 20)[0]
        shadowmask = struct.unpack_from("<I", data, p + 124 + 24)[0]
        shadow = struct.unpack_from("<I", data, p + 124 + 28)[0]
        wbuf = struct.unpack_from("<I", data, p + 124)[0]
        sides[nv] += 1
        fmt[TEX_FORMAT.get((texp >> 26) & 7, "?")] += 1
        tsize["%dx%d" % (8 << ((texp >> 20) & 7), 8 << ((texp >> 23) & 7))] += 1
        blend[BLEND_MODE.get((attr >> 4) & 3, "?")] += 1
        if translucent: flags["translucent"] += 1
        if shadowmask: flags["shadow mask"] += 1
        if shadow: flags["shadow"] += 1
        if wbuf: flags["W buffer"] += 1
        if (attr >> 6) & 1: flags["render back face"] += 1
        if (attr >> 7) & 1: flags["render front face"] += 1
        # TEXIMAGE_PARAM: repeat is bits 16/17 and flip is 18/19. Bits 30-31
        # are the texcoord transform mode, which an earlier version of this
        # read as the repeat flags and reported wrongly.
        if (texp >> 16) & 1: flags["texture repeats in S"] += 1
        if (texp >> 17) & 1: flags["texture repeats in T"] += 1
        if (texp >> 18) & 1: flags["texture flips in S"] += 1
        if (texp >> 19) & 1: flags["texture flips in T"] += 1
        if (texp >> 29) & 1: flags["colour 0 transparent"] += 1
        tmode[(texp >> 30) & 3] += 1

    def census(title, c):
        print("\n%s" % title)
        for k, v in c.most_common():
            print("  %-24s %6d" % (k, v))

    census("vertices per polygon", sides)
    census("texture format", fmt)
    census("blending mode", blend)
    census("texture size", tsize)
    census("texcoord transform mode", collections.Counter(
        {"none" if k == 0 else "mode %d" % k: v for k, v in tmode.items()}))
    census("attributes", flags)

    if a.export:
        export(data, base, at, n, pathlib.Path(a.export))
    return 0


# Vertex and Polygon field offsets, from the struct definitions the savestate
# writes in order. Both totals are checked by the section-length assertion
# above, so these are not independent guesses.
V_TEXCOORDS, V_FINALPOS, V_FINALCOL = 28, 36, 44
P_NUMVERTS, P_FINALZ, P_FINALW = 40, 44, 84
P_WBUFFER, P_ATTR, P_TEXPARAM, P_TEXPAL = 124, 128, 132, 136


def export(data, base, at, n, path):
    """Flatten the render list into one record per polygon.

    The point is that the rasteriser then needs no knowledge of melonDS at all -
    it takes polygons in screen space, which is also the shape the geometry
    engine will hand it once that exists.
    """
    out = bytearray(b"LSW2GX01")
    vram_base = base + at["VertexRAM"]
    poly_base = base + at["PolygonRAM"]
    listed = base + at["RenderPolygonRAM"]
    recs, written = bytearray(), 0

    for i in range(n):
        (idx,) = struct.unpack_from("<I", data, listed + i*4)
        if idx == 0xFFFFFFFF or idx >= 4096:
            continue
        p = poly_base + idx * POLYGON
        nv = struct.unpack_from("<I", data, p + P_NUMVERTS)[0]
        if not 3 <= nv <= 10:
            continue
        rec = struct.pack("<5I", nv,
                          struct.unpack_from("<I", data, p + P_ATTR)[0],
                          struct.unpack_from("<I", data, p + P_TEXPARAM)[0],
                          struct.unpack_from("<I", data, p + P_TEXPAL)[0],
                          struct.unpack_from("<I", data, p + P_WBUFFER)[0])
        for j in range(nv):
            (vi,) = struct.unpack_from("<I", data, p + j*4)
            if vi == 0xFFFFFFFF or vi >= 6144*2:
                rec = None
                break
            v = vram_base + vi * VERTEX
            fx, fy = struct.unpack_from("<2i", data, v + V_FINALPOS)
            cr, cg, cb = struct.unpack_from("<3i", data, v + V_FINALCOL)
            s, tt = struct.unpack_from("<2h", data, v + V_TEXCOORDS)
            z = struct.unpack_from("<i", data, p + P_FINALZ + j*4)[0]
            w = struct.unpack_from("<i", data, p + P_FINALW + j*4)[0]
            rec += struct.pack("<9i2h2x", fx, fy, w, z, cr, cg, cb, 0, 0, s, tt)
        if rec is None:
            continue
        recs += rec
        written += 1

    out += struct.pack("<I", written)
    out += recs
    path.write_bytes(bytes(out))
    print("exported %d polygon(s) to %s" % (written, path))


if __name__ == "__main__":
    raise SystemExit(main())
