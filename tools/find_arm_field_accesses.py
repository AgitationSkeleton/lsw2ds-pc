#!/usr/bin/env python3
'''Find ARM structure-field accesses with optional nearby-field filtering.'''

from __future__ import annotations

import argparse
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, CS_OP_MEM


def memory_accesses(instruction, values: set[int]) -> list[bool]:
    if instruction.id == 0:
        return []
    return [
        operand.type == CS_OP_MEM and operand.mem.disp in values
        for operand in instruction.operands
    ]


def accesses_displacement(instruction, values: set[int]) -> bool:
    return any(memory_accesses(instruction, values))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--base', type=lambda value: int(value, 0), default=0x02000000)
    parser.add_argument('--start', type=lambda value: int(value, 0))
    parser.add_argument('--end', type=lambda value: int(value, 0))
    parser.add_argument('--field', type=lambda value: int(value, 0), action='append', required=True)
    parser.add_argument('--near-field', type=lambda value: int(value, 0), action='append', default=[])
    parser.add_argument('--context', type=int, default=8)
    parser.add_argument('--write-only', action='store_true')
    parser.add_argument('--same-base', action='store_true')
    args = parser.parse_args()

    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    # ARM9 sections interleave literal/data pools with code. Continue across
    # undecodable words so later functions are still searchable.
    decoder.skipdata = True
    decoder.detail = True
    instructions = list(decoder.disasm(args.binary.read_bytes(), args.base))
    fields = set(args.field)
    near_fields = set(args.near_field)
    hits = 0
    for index, instruction in enumerate(instructions):
        if args.start is not None and instruction.address < args.start:
            continue
        if args.end is not None and instruction.address >= args.end:
            continue
        if not accesses_displacement(instruction, fields):
            continue
        if args.write_only and not instruction.mnemonic.startswith('str'):
            continue
        start = max(0, index - args.context)
        end = min(len(instructions), index + args.context + 1)
        window = instructions[start:end]
        if near_fields:
            if args.same_base:
                target_bases = {
                    operand.mem.base
                    for operand in instruction.operands
                    if operand.type == CS_OP_MEM and operand.mem.disp in fields
                }
                found_near = any(
                    candidate.id != 0
                    and any(
                        operand.type == CS_OP_MEM
                        and operand.mem.disp in near_fields
                        and operand.mem.base in target_bases
                        for operand in candidate.operands
                    )
                    for candidate in window
                )
            else:
                found_near = any(
                    accesses_displacement(candidate, near_fields) for candidate in window
                )
            if not found_near:
                continue
        hits += 1
        print(f'\n== access 0x{instruction.address:08x} ==')
        for candidate in window:
            marker = '>' if candidate.address == instruction.address else ' '
            print(
                f'{marker} {candidate.address:08x}: '
                f'{candidate.mnemonic:<8} {candidate.op_str}'
            )
    print(f'\nDisplayed {hits} access(es)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
