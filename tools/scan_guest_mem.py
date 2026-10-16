"""Find every occurrence of a 32-bit value in the guest's main RAM.

WHY THIS EXISTS. When the game replaces an actor, anything still holding the OLD
actor pointer is by definition stale - and a stale pointer is exactly what a HUD
that keeps drawing the previous character would look like. read_guest_mem.py
answers "what is at this address"; this answers the question that actually
locates a bug, "who is still pointing at the thing that went away".

Usage:  py tools/scan_guest_mem.py <value> [start] [end] [port]
        py tools/scan_guest_mem.py 0x0211FFA0

Defaults scan main RAM, 0x02000000-0x02400000, on the host instance. Reads in
64KB chunks because one 4MB read over the debug socket is a single enormous JSON
line and a timeout waiting to happen.
"""
import socket, json, sys

def rd(port, addr, ln):
    s = socket.create_connection(("127.0.0.1", port), timeout=30.0)
    f = s.makefile("rwb")
    f.write((json.dumps({"cmd": "read_mem", "cpu": 9,
                         "addr": addr, "len": ln}) + "\n").encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    return json.loads(line).get("hex", "")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    value = int(sys.argv[1], 0) & 0xFFFFFFFF
    start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x02000000
    end   = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x02400000
    port  = int(sys.argv[4], 0) if len(sys.argv) > 4 else 19842

    # The needle as it sits in memory: little-endian, hex, lower case - the same
    # form the read returns, so the search is a substring test and not a parse.
    needle = "".join("%02x" % ((value >> (8 * i)) & 0xFF) for i in range(4))

    chunk = 0x10000
    hits = []
    addr = start
    while addr < end:
        n = min(chunk, end - addr)
        try:
            h = rd(port, addr, n).lower()
        except Exception as e:
            print("read failed at %08X: %s" % (addr, e))
            addr += n
            continue
        # Only word-aligned matches: a pointer is stored aligned, and byte-offset
        # coincidences inside unrelated data are noise, not leads.
        for off in range(0, len(h) - 8 + 1, 8):
            if h[off:off + 8] == needle:
                hits.append(addr + off // 2)
        addr += n
    print("value %08X: %d aligned hit(s)" % (value, len(hits)))
    for a in hits:
        print("   %08X" % a)
    return 0

if __name__ == "__main__":
    sys.exit(main())
