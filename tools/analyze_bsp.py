#!/usr/bin/env python3
"""Validate and summarize the custom LSW2 DS BSP version-7 container."""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path


HEADER = struct.Struct("<4s4I")
PAIR = struct.Struct("<HH")
FIXED = struct.Struct("<IhhhH")


def analyze(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError(f"{path}: truncated BSP header")

    magic, version, kind, declared_size, fixed_offset = HEADER.unpack_from(data)
    if magic != b"BSP\0":
        raise ValueError(f"{path}: expected BSP\\0, got {magic!r}")
    if declared_size != len(data):
        raise ValueError(
            f"{path}: declared size {declared_size} != file size {len(data)}"
        )
    if fixed_offset < HEADER.size + 4 or fixed_offset > len(data):
        raise ValueError(f"{path}: invalid fixed-record offset 0x{fixed_offset:x}")

    # The word immediately before the fixed section is a constant terminator.
    # All preceding words after the header are offsets to variable records.
    list_count = (fixed_offset - HEADER.size - 4) // 4
    offsets = list(struct.unpack_from(f"<{list_count}I", data, HEADER.size))
    terminator = struct.unpack_from("<I", data, fixed_offset - 4)[0]
    if not offsets:
        raise ValueError(f"{path}: no variable-record offsets")
    if offsets != sorted(offsets) or len(set(offsets)) != len(offsets):
        raise ValueError(f"{path}: variable-record offsets are not strictly ordered")
    if offsets[0] < fixed_offset or offsets[-1] >= len(data):
        raise ValueError(f"{path}: variable-record offsets are out of range")

    fixed_size = offsets[0] - fixed_offset
    if fixed_size % 12:
        raise ValueError(f"{path}: fixed section is not divisible into 12-byte records")
    fixed_count = fixed_size // 12

    fixed_encodings: Counter[str] = Counter()
    fixed_low_flags: Counter[int] = Counter()
    packed_axes: Counter[int] = Counter()
    packed_directions: Counter[int] = Counter()
    plane_zero_components: Counter[int] = Counter()
    plane_normal_length_squared: Counter[int] = Counter()
    plane_component_min = [None, None, None]
    plane_component_max = [None, None, None]
    alternate_indices: Counter[int] = Counter()
    fixed_records = []
    for fixed_index in range(fixed_count):
        start = fixed_offset + fixed_index * FIXED.size
        word0, component_x, component_y, component_z, alternate_index = (
            FIXED.unpack_from(data, start)
        )
        word1_signed = struct.unpack_from("<i", data, start + 4)[0]
        fixed_low_flags[word0 & 0x03] += 1
        alternate_indices[alternate_index] += 1

        # 0x0204c8a8 makes this exact signed comparison. In the packed branch,
        # word 1 bits 0..1 select an AABB axis and bit 2 selects its direction.
        # Otherwise the same six bytes are three signed plane-normal components.
        if word1_signed >= 0x40000000:
            word1_unsigned = word1_signed & 0xFFFFFFFF
            fixed_encodings["packed_axis"] += 1
            packed_axes[word1_unsigned & 0x03] += 1
            packed_directions[(word1_unsigned >> 2) & 0x01] += 1
            normal = [0, 0, 0]
            axis = word1_unsigned & 0x03
            if axis < 3:
                normal[axis] = 4096 if ((word1_unsigned >> 2) & 1) else -4096
            encoding = "packed_axis"
        else:
            fixed_encodings["plane"] += 1
            components = (component_x, component_y, component_z)
            plane_zero_components[
                int(component_x == 0)
                + int(component_y == 0)
                + int(component_z == 0)
            ] += 1
            plane_normal_length_squared[sum(value * value for value in components)] += 1
            for axis, value in enumerate(components):
                if plane_component_min[axis] is None or value < plane_component_min[axis]:
                    plane_component_min[axis] = value
                if plane_component_max[axis] is None or value > plane_component_max[axis]:
                    plane_component_max[axis] = value
            normal = [component_x, component_y, component_z]
            encoding = "plane"
        fixed_records.append({
            "index": fixed_index,
            "offset": start,
            "word0_signed": struct.unpack_from("<i", data, start)[0],
            "flags": word0 & 0x03,
            "plane_constant_without_flags": struct.unpack_from(
                "<i", struct.pack("<I", word0 & 0xFFFFFFFC)
            )[0],
            "encoding": encoding,
            "normal_fx12": normal,
            "alternate_index": alternate_index,
        })

    counts: Counter[int] = Counter()
    codes: Counter[int] = Counter()
    indices: Counter[int] = Counter()
    records: list[dict[str, object]] = []
    for record_index, start in enumerate(offsets):
        end = offsets[record_index + 1] if record_index + 1 < list_count else len(data)
        count = struct.unpack_from("<H", data, start)[0]
        expected_size = 2 + count * PAIR.size
        if end - start != expected_size:
            raise ValueError(
                f"{path}: variable record {record_index} has size {end-start}, "
                f"expected {expected_size}"
            )
        pairs = []
        for item_index in range(count):
            index, code = PAIR.unpack_from(data, start + 2 + item_index * PAIR.size)
            if index >= fixed_count:
                raise ValueError(
                    f"{path}: record {record_index} references fixed index {index}, "
                    f"but only {fixed_count} records exist"
                )
            indices[index] += 1
            codes[code] += 1
            pairs.append({"index": index, "code": code})
        counts[count] += 1
        records.append(
            {
                "record": record_index,
                "offset": start,
                "count": count,
                "pairs": pairs,
            }
        )

    missing_indices = sorted(set(range(fixed_count)) - set(indices))
    return {
        "source": str(path),
        "header": {
            "magic": "BSP\\0",
            "version": version,
            "kind": kind,
            "declared_size": declared_size,
            "fixed_record_offset": fixed_offset,
            "terminator": terminator,
        },
        "fixed_records": {
            "record_size": 12,
            "count": fixed_count,
            "semantics": (
                "0x0204c8a8 boolean AABB traversal nodes; packed-axis or "
                "signed-plane encoding"
            ),
            "encoding_histogram": dict(sorted(fixed_encodings.items())),
            "word0_low_flag_histogram": dict(sorted(fixed_low_flags.items())),
            "packed_axis_histogram": dict(sorted(packed_axes.items())),
            "packed_direction_histogram": dict(sorted(packed_directions.items())),
            "plane_zero_component_histogram": dict(
                sorted(plane_zero_components.items())
            ),
            "decoded_normal_semantics": {
                "packed_axis": (
                    "axis=word1 bits 0..1; bit 2 clear=-4096, set=+4096"
                ),
                "plane": "signed halfwords at +4/+6/+8, sign-extended",
            },
            "plane_normal_components": {
                "min_xyz": plane_component_min,
                "max_xyz": plane_component_max,
                "length_squared_min": (
                    min(plane_normal_length_squared)
                    if plane_normal_length_squared
                    else None
                ),
                "length_squared_max": (
                    max(plane_normal_length_squared)
                    if plane_normal_length_squared
                    else None
                ),
                "length_squared_unique": len(plane_normal_length_squared),
                "exact_unit_20_12": plane_normal_length_squared.get(4096 * 4096, 0),
                "within_one_percent_of_unit_20_12": sum(
                    count
                    for length_squared, count in plane_normal_length_squared.items()
                    if abs(length_squared - 4096 * 4096) <= (4096 * 4096) // 100
                ),
            },
            "alternate_index": {
                "unique": len(alternate_indices),
                "min": min(alternate_indices),
                "max": max(alternate_indices),
                "in_fixed_range": sum(
                    count
                    for index, count in alternate_indices.items()
                    if index < fixed_count
                ),
            },
            "referenced_unique": len(indices),
            "missing_indices": missing_indices,
            "reference_count_min": min(indices.values()),
            "reference_count_max": max(indices.values()),
            "nodes": fixed_records,
        },
        "variable_records": {
            "count": list_count,
            "encoding": "u16 count; count * (u16 fixed_index, u16 code)",
            "item_count_histogram": dict(sorted(counts.items())),
            "code_histogram": dict(sorted(codes.items())),
            "records": records,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    result = {path.stem: analyze(path) for path in args.inputs}
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
