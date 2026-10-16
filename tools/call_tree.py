#!/usr/bin/env python3
"""Walk the ARM9 call graph up and down from a seed address.

Built for context archaeology: start from something whose meaning is known -
the character-portrait field in the runtime descriptor table, say - and see
what reaches it and what it reaches. Neighbours in the call graph tend to be
neighbours in purpose, so the routine that loads a portrait sits near the ones
that load the rest of the HUD.

Two directions:

  callers  who calls this, transitively - the context the function serves
  callees  what this calls, transitively - the work it delegates

Indirect calls through vtables (`blx rN`) are invisible to a static scan, so a
gap in the tree is not proof of a gap in the program. Call sites reached only
through a function pointer are reported separately where the pointer is a
literal in the same function.

Run:
    python tools/call_tree.py 0x0200f5d0 --depth 3
    python tools/call_tree.py --seed-literal 0x020ef57c --depth 2
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import defaultdict
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, Cs

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"
BASE = 0x02000000


def decode(data: bytes) -> dict:
    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    decoded = {}
    for offset in range(0, len(data) - 3, 4):
        for instruction in decoder.disasm(data[offset : offset + 4], BASE + offset):
            decoded[BASE + offset] = instruction
    return decoded


def build_graph(decoded: dict) -> tuple[dict, dict]:
    """Direct bl edges, attributed to the enclosing function."""
    starts = sorted(
        address for address, instruction in decoded.items()
        if instruction.mnemonic == "push" and "lr" in instruction.op_str
    )

    def enclosing(address: int) -> int:
        low, high = 0, len(starts) - 1
        best = starts[0] if starts else address
        while low <= high:
            mid = (low + high) // 2
            if starts[mid] <= address:
                best = starts[mid]
                low = mid + 1
            else:
                high = mid - 1
        return best

    callees = defaultdict(set)
    callers = defaultdict(set)
    for address, instruction in decoded.items():
        if instruction.mnemonic != "bl":
            continue
        try:
            target = int(instruction.op_str.lstrip("#"), 0)
        except ValueError:
            continue
        owner = enclosing(address)
        callees[owner].add(target)
        callers[target].add(owner)
    return callers, callees


def literal_users(decoded: dict, data: bytes, value: int) -> list[int]:
    """Functions that load a given constant from a literal pool."""
    users = []
    for address, instruction in decoded.items():
        if instruction.mnemonic != "ldr" or "pc," not in instruction.op_str:
            continue
        if "#" not in instruction.op_str:
            continue
        try:
            target = address + 8 + int(instruction.op_str.split("#")[-1].rstrip("]"), 0)
        except ValueError:
            continue
        try:
            loaded = struct.unpack_from("<I", data, target - BASE)[0]
        except struct.error:
            continue
        if loaded == value:
            users.append(address)
    return sorted(users)


def walk(graph: dict, seed: int, depth: int) -> dict:
    seen = {seed: 0}
    frontier = [seed]
    for level in range(1, depth + 1):
        nxt = []
        for node in frontier:
            for neighbour in sorted(graph.get(node, ())):
                if neighbour in seen:
                    continue
                seen[neighbour] = level
                nxt.append(neighbour)
        frontier = nxt
        if not frontier:
            break
    return seen


def render(graph: dict, seed: int, depth: int, label: str) -> list[str]:
    lines = [f"{label} of 0x{seed:08x}:"]
    visited = {seed}

    def recurse(node: int, level: int, prefix: str) -> None:
        if level > depth:
            return
        children = sorted(graph.get(node, ()))
        for index, child in enumerate(children):
            last = index == len(children) - 1
            mark = "`- " if last else "|- "
            repeated = child in visited
            lines.append(f"{prefix}{mark}0x{child:08x}{'  (seen)' if repeated else ''}")
            if repeated:
                continue
            visited.add(child)
            recurse(child, level + 1, prefix + ("   " if last else "|  "))

    recurse(seed, 1, "  ")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seed", nargs="?", type=lambda v: int(v, 0))
    parser.add_argument("--arm9", type=Path, default=DEFAULT_ARM9)
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--seed-literal", type=lambda v: int(v, 0),
                        help="seed from every function that loads this constant")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if arguments.seed is None and arguments.seed_literal is None:
        parser.error("give a seed address or --seed-literal")

    data = arguments.arm9.read_bytes()
    decoded = decode(data)
    callers, callees = build_graph(decoded)

    seeds: list[int] = []
    if arguments.seed_literal is not None:
        sites = literal_users(decoded, data, arguments.seed_literal)
        print(f"{len(sites)} sites load 0x{arguments.seed_literal:08x}")
        starts = sorted(a for a, i in decoded.items()
                        if i.mnemonic == "push" and "lr" in i.op_str)
        for site in sites:
            owner = max((s for s in starts if s <= site), default=site)
            if owner not in seeds:
                seeds.append(owner)
        print("enclosing functions:", ", ".join(f"0x{s:08x}" for s in seeds))
    if arguments.seed is not None:
        seeds.append(arguments.seed)

    report = {"seeds": [f"0x{s:08x}" for s in seeds], "trees": []}
    for seed in seeds:
        up = render(callers, seed, arguments.depth, "callers")
        down = render(callees, seed, arguments.depth, "callees")
        print()
        print("\n".join(up))
        print("\n".join(down))
        report["trees"].append({
            "seed": f"0x{seed:08x}",
            "callers": [f"0x{a:08x}" for a in sorted(walk(callers, seed, arguments.depth))],
            "callees": [f"0x{a:08x}" for a in sorted(walk(callees, seed, arguments.depth))],
        })

    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"\n-> {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
