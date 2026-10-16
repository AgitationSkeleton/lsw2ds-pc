"""Read guest memory from ONE console, chosen by port.

WHY THIS EXISTS ALONGSIDE read_guest_mem.py. That tool reads BOTH consoles at a
fixed pair of ports, which is right for the two-console recipe and wrong for
every other shape of question - a solo console reaching the cantina, or a third
instance brought up alongside a live run to take a control reading. Trimming
read_guest_mem's output to one console is exactly the mistake that once cost a
whole round of investigation, so the answer is a tool that reads one console on
purpose rather than a pipeline that discards half of one that read two.

    py tools/read_port_mem.py <addr> <len> [port]        default port 19842
    py tools/read_port_mem.py 0x02109E90 4 19844

Prints the bytes in memory order. A refused connection is reported as a
refusal, never as zeroes - caveat (e): an empty read is not a result.
"""
import json
import socket
import sys


def read(port, addr, length):
    s = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    try:
        f = s.makefile("rwb")
        f.write((json.dumps({"cmd": "read_mem", "cpu": 9,
                             "addr": addr, "len": length}) + "\n").encode())
        f.flush()
        line = f.readline().decode()
    finally:
        s.close()
    return json.loads(line).get("hex", "")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    addr = int(sys.argv[1], 0)
    length = int(sys.argv[2])
    port = int(sys.argv[3], 0) if len(sys.argv) > 3 else 19842

    try:
        hexs = read(port, addr, length)
    except (OSError, ValueError) as exc:
        print("port %d: READ REFUSED (%s) - this is NOT a zero" % (port, exc))
        return 1

    if not hexs:
        print("port %d: EMPTY REPLY - this is NOT a zero" % port)
        return 1

    print("port %d  %08X" % (port, addr))
    for i in range(0, len(hexs), 32):
        print("   +%02X %s" % (i // 2, hexs[i:i + 32]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
