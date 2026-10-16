#!/usr/bin/env python3
"""Read a live melonDS instance's memory over its GDB stub.

This is the savestate reader's live counterpart. Instead of saving a state
and parsing it offline, attach to the running emulator and read
main RAM directly - so a screen can be interrogated while it is on screen.

REQUIREMENTS. `JIT.Enable` must be false in melonDS.toml: melonDS force-
disables the GDB stub whenever the JIT recompiler is on. Instance 0 listens on
3333 (ARM9) and 3334 (ARM7); instance 1 on 3335/3336, if its config was in
place before it was launched.

SAFETY. Memory reads need the CPU halted, so this sends an interrupt, reads,
and *always* resumes in a finally block. A crash mid-read leaves the game
paused, not broken - reconnecting and sending `c` frees it. Nothing is ever
written to the target.

Run:
    python tools/gdb_read.py --probe
    python tools/gdb_read.py --dump-ram out/work/live_ram.bin
    python tools/gdb_read.py --read 0x02000000 64
"""

from __future__ import annotations

import argparse
import socket
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_RAM_BASE = 0x02000000
MAIN_RAM_SIZE = 4 * 1024 * 1024


class GdbClient:
    """A minimal GDB remote serial protocol client - reads only."""

    def __init__(self, host: str = "127.0.0.1", port: int = 3333, timeout: float = 10.0):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.socket.settimeout(timeout)
        # melonDS blocks in WaitAckBlocking() right after accept() and demands a
        # bare '+' as the first byte, within one second. Open with a packet
        # instead and it consumes the '$' as that ack, leaving the rest to parse
        # as garbage - which looks exactly like the stub ignoring you.
        self.socket.sendall(b"+")
        self.buffer = b""
        self.packet_size = 0x400
        self.halted = False

    def close(self) -> None:
        try:
            self.socket.close()
        except OSError:
            pass

    def _read_byte(self) -> bytes:
        if not self.buffer:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionError("the stub closed the connection")
            self.buffer = chunk
        byte, self.buffer = self.buffer[:1], self.buffer[1:]
        return byte

    def _recv_packet(self) -> str:
        """Read one $...#xx packet, skipping acks, and acknowledge it."""
        while True:
            byte = self._read_byte()
            if byte == b"$":
                break
        body = b""
        while True:
            byte = self._read_byte()
            if byte == b"#":
                break
            body += byte
        self._read_byte()
        self._read_byte()  # discard the checksum; the transport is loopback
        self.socket.sendall(b"+")
        return body.decode("latin-1")

    def command(self, text: str) -> str:
        checksum = sum(text.encode("latin-1")) & 0xFF
        self.socket.sendall(f"${text}#{checksum:02x}".encode("latin-1"))
        return self._recv_packet()

    def interrupt(self) -> str:
        """Halt the CPU with a raw 0x03, as GDB's Ctrl-C does."""
        self.socket.sendall(b"\x03")
        reply = self._recv_packet()
        self.halted = True
        return reply

    def resume(self) -> None:
        """Resume without waiting for the next stop reply."""
        self.socket.sendall(b"$c#63")
        self.halted = False

    def handshake(self) -> str:
        reply = self.command("qSupported:swbreak+;hwbreak+")
        for feature in reply.split(";"):
            if feature.startswith("PacketSize="):
                # Leave room for the $, #, checksum and any escaping.
                self.packet_size = max(0x100, int(feature.split("=")[1], 16) - 32)
        return reply

    def read_memory(self, address: int, length: int, progress: bool = False) -> bytes:
        """Read `length` bytes. Two hex digits per byte, so chunk by packet size."""
        chunk = max(64, (self.packet_size // 2) & ~0xF)
        out = bytearray()
        while len(out) < length:
            want = min(chunk, length - len(out))
            reply = self.command(f"m{address + len(out):x},{want:x}")
            if reply.startswith("E") and len(reply) <= 3:
                # Unreadable region: record zeros and keep going.
                out += b"\x00" * want
            else:
                data = bytes.fromhex(reply.strip())
                if not data:
                    out += b"\x00" * want
                else:
                    out += data
                    if len(data) < want:
                        out += b"\x00" * (want - len(data))
            if progress and len(out) % (256 * 1024) < chunk:
                print(f"  {len(out)/1e6:.1f} / {length/1e6:.1f} MB", flush=True)
        return bytes(out[:length])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--probe", action="store_true", help="handshake and read 64 bytes")
    parser.add_argument("--read", nargs=2, metavar=("ADDR", "LEN"),
                        help="read LEN bytes at ADDR and hex-dump them")
    parser.add_argument("--dump-ram", type=Path, help="dump all 4 MB of main RAM")
    arguments = parser.parse_args()

    client = GdbClient(arguments.host, arguments.port)
    try:
        supported = client.handshake()
        print(f"connected to {arguments.host}:{arguments.port}")
        print(f"  packet size {client.packet_size:#x}")
        print(f"  qSupported: {supported[:120]}")
        stop = client.interrupt()
        print(f"  halted: {stop[:60]}")

        if arguments.probe or not (arguments.read or arguments.dump_ram):
            data = client.read_memory(MAIN_RAM_BASE, 64)
            print(f"\n{MAIN_RAM_BASE:#010x}: {data[:32].hex(' ')}")
            print(f"{MAIN_RAM_BASE+32:#010x}: {data[32:].hex(' ')}")

        if arguments.read:
            address = int(arguments.read[0], 0)
            length = int(arguments.read[1], 0)
            data = client.read_memory(address, length)
            for offset in range(0, len(data), 16):
                row = data[offset : offset + 16]
                text = "".join(chr(c) if 32 <= c < 127 else "." for c in row)
                print(f"{address+offset:#010x}: {row.hex(' '):<47}  {text}")

        if arguments.dump_ram:
            print(f"\nreading {MAIN_RAM_SIZE/1e6:.0f} MB of main RAM...")
            data = client.read_memory(MAIN_RAM_BASE, MAIN_RAM_SIZE, progress=True)
            arguments.dump_ram.parent.mkdir(parents=True, exist_ok=True)
            arguments.dump_ram.write_bytes(data)
            print(f"-> {arguments.dump_ram}  ({len(data)} bytes, "
                  f"{sum(1 for b in data if b):,} non-zero)")
    finally:
        # Never leave the running game halted.
        try:
            client.resume()
            print("\nresumed")
        except OSError:
            print("\nWARNING: could not resume; reconnect and send 'c'")
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
