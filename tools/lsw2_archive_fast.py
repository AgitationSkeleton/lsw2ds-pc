#!/usr/bin/env python3
"""Memory-reusing frontend for :mod:`lsw2_archive` full-catalog extraction."""

from __future__ import annotations

import struct

import lsw2_archive as core


class ReusableType6Decoder:
    """Reuse one isolated Unicorn address space across independent streams."""

    def __init__(self, itcm: bytes):
        self.emu = core.Uc(core.UC_ARCH_ARM, core.UC_MODE_ARM)
        self.emu.mem_map(core.ITCM_ADDRESS, core.align_page(len(itcm)))
        self.emu.mem_map(core.SOURCE_ADDRESS, core.MAX_EXPANDED_SIZE)
        self.emu.mem_map(core.OUTPUT_ADDRESS, core.MAX_EXPANDED_SIZE)
        self.emu.mem_map(core.STACK_ADDRESS, 0x10000)
        self.emu.mem_map(core.RETURN_ADDRESS, 0x1000)
        self.emu.mem_write(core.ITCM_ADDRESS, itcm)
        self.returned = False

        def stop_on_return(instance: core.Uc, address: int, _size: int, _data: object) -> None:
            if address == core.RETURN_ADDRESS:
                self.returned = True
                instance.emu_stop()

        self.emu.hook_add(core.UC_HOOK_CODE, stop_on_return)

    def decode(self, wrapped: bytes) -> bytes:
        if len(wrapped) < 16:
            raise ValueError("type-6 stream is shorter than its headers")
        header = struct.unpack_from("<I", wrapped, 0)[0]
        codec = (wrapped[0] >> 4) & 7
        expanded_size = header >> 8
        if codec != 6:
            raise ValueError(f"expected codec 6, got {codec}")
        if not 0 < expanded_size <= core.MAX_EXPANDED_SIZE:
            raise ValueError(f"implausible expanded size: {expanded_size}")
        payload = wrapped[4:]
        if len(payload) > core.MAX_EXPANDED_SIZE:
            raise ValueError(f"compressed payload is too large: {len(payload)}")

        self.emu.mem_write(core.SOURCE_ADDRESS, payload)
        self.emu.reg_write(core.UC_ARM_REG_R0, core.SOURCE_ADDRESS)
        self.emu.reg_write(core.UC_ARM_REG_R1, core.OUTPUT_ADDRESS)
        self.emu.reg_write(core.UC_ARM_REG_R2, core.STACK_ADDRESS)
        self.emu.reg_write(core.UC_ARM_REG_SP, core.STACK_ADDRESS + 0xFFF0)
        self.emu.reg_write(core.UC_ARM_REG_LR, core.RETURN_ADDRESS)
        self.returned = False
        try:
            self.emu.emu_start(
                core.ITCM_ADDRESS,
                core.RETURN_ADDRESS + 4,
                count=100_000_000,
            )
        except core.UcError as exc:
            raise ValueError(f"ITCM decoder failed: {exc}") from exc
        if not self.returned:
            raise ValueError("ITCM decoder hit its instruction limit")
        return bytes(self.emu.mem_read(core.OUTPUT_ADDRESS, expanded_size))


core.Type6Decoder = ReusableType6Decoder

if __name__ == "__main__":
    raise SystemExit(core.main())
