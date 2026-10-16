"""Dump NSBMD (BMD0/MDL0) material register words straight out of the archive.

Ports the exact struct layout apicula uses (tools/vendor/apicula-src/src/nitro/
model.rs read_material) so the numbers here are the same bytes the DS GPU gets
loaded with at bind time - polygon_attr, polygon_attr_mask, dif_amb, spe_emi,
teximage_param.

    python tools/nsbmd_materials.py out/raw/archive/entries/2836.nsbmd
"""
import struct
import sys
import json


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def read_info_block(b, o, datum_size):
    """Returns (list_of_data_ints, list_of_names, count)."""
    dummy = u8(b, o)
    count = u8(b, o + 1)
    header_size = u16(b, o + 2)
    assert dummy == 0, "info block dummy %d" % dummy
    p = o + 4
    # unknown subheader
    p += 2 + 2 + 4
    p += 4 * count
    size_of_datum = u16(b, p)
    p += 4
    assert size_of_datum == datum_size, "datum size %d != %d" % (size_of_datum, datum_size)
    data = []
    for i in range(count):
        if datum_size == 4:
            data.append(u32(b, p + 4 * i))
        elif datum_size == 2:
            data.append(u16(b, p + 2 * i))
        else:
            data.append(b[p + datum_size * i:p + datum_size * (i + 1)])
    p += datum_size * count
    names = []
    for i in range(count):
        names.append(b[p + 16 * i:p + 16 * i + 16].split(b"\0")[0].decode("ascii", "replace"))
    return data, names


def rgb5(x):
    return (x & 31, (x >> 5) & 31, (x >> 10) & 31)


def read_materials(b, mat_base):
    tex_pairing_off = u16(b, mat_base)
    pal_pairing_off = u16(b, mat_base + 2)
    offs, names = read_info_block(b, mat_base + 4, 4)
    out = []
    for off, name in zip(offs, names):
        o = mat_base + off
        rec = {
            "name": name,
            "dummy": u16(b, o),
            "sizeof": u16(b, o + 2),
            "dif_amb": u32(b, o + 4),
            "spe_emi": u32(b, o + 8),
            "polygon_attr": u32(b, o + 12),
            "polygon_attr_mask": u32(b, o + 16),
            "teximage_param": u32(b, o + 20),
            "unknown3": u32(b, o + 24),
            "pltt_base": u16(b, o + 28),
            "misc": u16(b, o + 30),
            "width": u16(b, o + 32),
            "height": u16(b, o + 34),
        }
        da = rec["dif_amb"]
        se = rec["spe_emi"]
        pa = rec["polygon_attr"]
        rec["diffuse"] = rgb5(da & 0x7FFF)
        rec["diffuse_is_default_vertex_color"] = bool((da >> 15) & 1)
        rec["ambient"] = rgb5((da >> 16) & 0x7FFF)
        rec["specular"] = rgb5(se & 0x7FFF)
        rec["enable_shininess_table"] = bool((se >> 15) & 1)
        rec["emission"] = rgb5((se >> 16) & 0x7FFF)
        rec["light_mask"] = pa & 0xF
        rec["polygon_mode"] = (pa >> 4) & 3
        rec["render_back"] = bool((pa >> 6) & 1)
        rec["render_front"] = bool((pa >> 7) & 1)
        rec["depth_write_translucent"] = bool((pa >> 11) & 1)
        rec["far_clip"] = bool((pa >> 12) & 1)
        rec["render_1dot"] = bool((pa >> 13) & 1)
        rec["depth_equal"] = bool((pa >> 14) & 1)
        rec["fog_enable"] = bool((pa >> 15) & 1)
        rec["alpha"] = (pa >> 16) & 0x1F
        rec["polygon_id"] = (pa >> 24) & 0x3F
        out.append(rec)
    return out


def read_models(path):
    b = open(path, "rb").read()
    stamp = b[0:4]
    assert stamp in (b"BMD0", b"BTX0"), stamp
    num_sections = u16(b, 14)
    sect_offs = [u32(b, 16 + 4 * i) for i in range(num_sections)]
    models = []
    for so in sect_offs:
        if b[so:so + 4] != b"MDL0":
            continue
        offs, names = read_info_block(b, so + 8, 4)
        for off, name in zip(offs, names):
            mo = so + off
            materials_off = u32(b, mo + 8)
            num_materials = u8(b, mo + 0x18)
            num_verts = u16(b, mo + 0x24)
            num_tris = u16(b, mo + 0x28)
            num_quads = u16(b, mo + 0x2A)
            mats = read_materials(b, mo + materials_off)
            models.append({
                "model": name,
                "model_off": mo,
                "num_materials": num_materials,
                "num_verts": num_verts,
                "num_tris": num_tris,
                "num_quads": num_quads,
                "materials": mats,
            })
    return models


if __name__ == "__main__":
    for path in sys.argv[1:]:
        for m in read_models(path):
            print("%s  model %s  (%d materials, %d verts, %d tris, %d quads)" % (
                path, m["model"], m["num_materials"], m["num_verts"], m["num_tris"], m["num_quads"]))
            for mat in m["materials"]:
                print("   %-18s polyattr=0x%08X mask=0x%08X lights=0x%X mode=%d "
                      "front=%d back=%d alpha=%d polyid=%d dif_amb=0x%08X dif=%s vtxcol=%d amb=%s "
                      "spe_emi=0x%08X spe=%s shin=%d emi=%s teximg=0x%08X" % (
                          mat["name"], mat["polygon_attr"], mat["polygon_attr_mask"],
                          mat["light_mask"], mat["polygon_mode"], mat["render_front"],
                          mat["render_back"], mat["alpha"], mat["polygon_id"],
                          mat["dif_amb"], mat["diffuse"], mat["diffuse_is_default_vertex_color"],
                          mat["ambient"], mat["spe_emi"], mat["specular"],
                          mat["enable_shininess_table"], mat["emission"], mat["teximage_param"]))
