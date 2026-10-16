#!/usr/bin/env python3
"""Export textures and attachment models for all runtime character composites."""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
from pathlib import Path

from PIL import Image

from analyze_character_composites import ARM9_BASE, COMPOSITE_TABLE, NONE_PART, record_for


ROOT = Path(__file__).resolve().parent.parent
REGISTRY_BASE = 0x020EB7B8
NONE_ASSET = 0xFFFF
TTGA = 0x41475454
SLOT_NAMES = {0: "head", 1: "legs", 2: "torso", 3: "gear", 4: "back_gear"}


def rgb555(value: int, alpha: int = 255) -> tuple[int, int, int, int]:
    red = value & 0x1F
    green = (value >> 5) & 0x1F
    blue = (value >> 10) & 0x1F
    return (
        (red << 3) | (red >> 2),
        (green << 3) | (green >> 2),
        (blue << 3) | (blue >> 2),
        alpha,
    )


def decode_companion(source: Path, output: Path) -> dict[str, object]:
    """Decode raw format-4 or TTGA A3I5 32x32 actor-part textures."""
    data = source.read_bytes()
    ttga = len(data) >= 8 and struct.unpack_from("<I", data)[0] == TTGA
    if ttga:
        palette_bytes = struct.unpack_from("<I", data, 4)[0]
        data = data[8:]
    else:
        palette_bytes = 512
    palette_data = data[:palette_bytes]
    texels = data[palette_bytes:]
    if palette_bytes <= 0 or palette_bytes % 2 or len(texels) != 1024:
        raise ValueError(f"unsupported split palette={palette_bytes}, texels={len(texels)}")
    palette_values = [value for (value,) in struct.iter_unpack("<H", palette_data)]
    if not palette_values:
        raise ValueError("appearance has an empty palette")
    pixels = []
    if ttga:
        # TTGA stores DS A3I5 texels: low five bits are the palette index and
        # high three bits are alpha.  This is used by all force-ghost slots.
        for texel in texels:
            index = texel & 0x1F
            if index >= len(palette_values):
                raise ValueError("A3I5 palette index exceeds decoded palette")
            alpha3 = texel >> 5
            pixels.append(rgb555(palette_values[index], (alpha3 * 255 + 3) // 7))
        used_indices = len({texel & 0x1F for texel in texels})
        encoding = "a3i5"
    else:
        if max(texels) >= len(palette_values):
            raise ValueError("indexed texels exceed the decoded palette")
        pixels = [rgb555(palette_values[index]) for index in texels]
        used_indices = len(set(texels))
        encoding = "format4"
    image = Image.new("RGBA", (32, 32))
    image.putdata(pixels)
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output)
    return {
        "png": output.name,
        "ttga": ttga,
        "encoding": encoding,
        "palette_colors": len(palette_values),
        "used_indices": used_indices,
    }


def registry_entry(data: bytes, selector: int, base: int) -> dict[str, int] | None:
    if selector == 0x276:
        return None
    offset = REGISTRY_BASE - base + selector * 8
    if offset < 0 or offset + 8 > len(data):
        return None
    handler, asset_id, flags = struct.unpack_from("<IHH", data, offset)
    return {"selector": selector, "handler": handler, "asset": asset_id, "flags": flags}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--entries", type=Path, default=Path("out/raw/archive/entries"))
    parser.add_argument("--output", type=Path, default=Path("out/exported/runtime_character_components"))
    parser.add_argument("--base", type=lambda value: int(value, 0), default=ARM9_BASE)
    parser.add_argument("--table", type=lambda value: int(value, 0), default=COMPOSITE_TABLE)
    parser.add_argument("--rows", type=int, default=72)
    args = parser.parse_args()

    data = args.binary.read_bytes()
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))["entries"]
    catalog_by_id = {entry["asset_id"]: entry for entry in catalog}
    entries = (ROOT / args.entries).resolve()
    output = (ROOT / args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    table_offset = args.table - args.base
    manifest: dict[str, object] = {
        "source_table": f"0x{args.table:08x}",
        "row_count": args.rows,
        "slot_names": SLOT_NAMES,
        "runtime_appearances": [],
        "exported_models": {},
    }
    exported_models: dict[int, dict[str, object]] = {}
    apicula = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"

    for runtime_id in range(args.rows):
        raw_parts = data[table_offset + runtime_id * 5 : table_offset + runtime_id * 5 + 5]
        row_dir = output / f"runtime_{runtime_id:03d}"
        parts = []
        for actor_id in raw_parts:
            if actor_id == NONE_PART:
                continue
            record = record_for(data, actor_id, args.base)
            mode = record[8]
            fields = struct.unpack_from("<5H", record)
            companion_id = fields[3]
            part: dict[str, object] = {
                "actor_part": actor_id,
                "mode": mode,
                "slot": SLOT_NAMES.get(mode, f"mode_{mode}"),
                "group": record[9],
                "field_0": fields[0],
                "field_2": fields[1],
                "model_selector": fields[2],
                "companion_asset": None if companion_id == NONE_ASSET else companion_id,
            }
            model = registry_entry(data, fields[2], args.base)
            if model is not None:
                entry = catalog_by_id.get(model["asset"])
                model["extension"] = entry["extension"] if entry else None
                model["names"] = entry.get("names", [])[:4] if entry else []
                part["model"] = model
                asset_id = model["asset"]
                if entry and entry["extension"] == "nsbmd" and asset_id not in exported_models:
                    matches = list(entries.glob(f"{asset_id:04d}.*"))
                    model_dir = output / "models" / f"asset_{asset_id:04d}"
                    result: dict[str, object] = {"asset": asset_id, "names": entry.get("names", [])[:4]}
                    if len(matches) == 1:
                        model_dir.mkdir(parents=True, exist_ok=True)
                        completed = subprocess.run(
                            [str(apicula), "convert", str(matches[0]), "-o", str(model_dir),
                             "-f", "glb", "--more-textures", "--overwrite"],
                            cwd=ROOT, text=True, capture_output=True,
                        )
                        result["exported"] = completed.returncode == 0
                        if completed.returncode != 0:
                            result["error"] = completed.stderr.strip() or completed.stdout.strip()
                    else:
                        result["exported"] = False
                        result["error"] = f"archive match count {len(matches)}"
                    exported_models[asset_id] = result
            if companion_id != NONE_ASSET and mode in SLOT_NAMES:
                matches = list(entries.glob(f"{companion_id:04d}.*"))
                if len(matches) == 1:
                    png_path = row_dir / f"{mode}_{SLOT_NAMES[mode]}_asset_{companion_id:04d}.png"
                    try:
                        part["texture"] = decode_companion(matches[0], png_path)
                    except ValueError as error:
                        part["texture_error"] = str(error)
                else:
                    part["texture_error"] = f"archive match count {len(matches)}"
            parts.append(part)
        manifest["runtime_appearances"].append(
            {"runtime_id": runtime_id, "raw_parts": list(raw_parts), "parts": parts}
        )
    manifest["exported_models"] = {str(key): value for key, value in sorted(exported_models.items())}
    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    decoded = sum(1 for row in manifest["runtime_appearances"] for part in row["parts"] if "texture" in part)
    errors = sum(1 for row in manifest["runtime_appearances"] for part in row["parts"] if "texture_error" in part)
    print(
        f"Exported {args.rows} runtime rows: {decoded} textures, {errors} texture exceptions, "
        f"{len(exported_models)} attachment models -> {manifest_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
