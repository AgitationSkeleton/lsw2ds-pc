"""Write a 32-bit value into guest RAM and read it straight back.

THE READ-BACK IS THE POINT. An externally injected write can be overwritten by
the guest within the same frame, and a poke that silently reverted has burned
diagnoses in this project before - "nothing changed on screen" is worthless if
the value was never there when the frame was drawn. So this always reports what
the address held before, what was asked for, and what it holds after.

Usage:  py tools/poke_guest_mem.py <addr> <value> [port]
"""
import socket, json, sys

def cmd(port, obj):
    s = socket.create_connection(("127.0.0.1", port), timeout=15.0)
    f = s.makefile("rwb")
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    return json.loads(line)

def read32(port, addr):
    h = cmd(port, {"cmd": "read_mem", "cpu": 9, "addr": addr, "len": 4}).get("hex", "")
    return int.from_bytes(bytes.fromhex(h), "little") if len(h) == 8 else None

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    addr = int(sys.argv[1], 0)
    val = int(sys.argv[2], 0) & 0xFFFFFFFF
    port = int(sys.argv[3], 0) if len(sys.argv) > 3 else 19842
    before = read32(port, addr)
    reply = cmd(port, {"cmd": "write_mem", "addr": addr, "value": val, "width": 32})
    after = read32(port, addr)
    print("%08X: was %s, wrote %08X, now %s%s"
          % (addr,
             "%08X" % before if before is not None else "??",
             val,
             "%08X" % after if after is not None else "??",
             "" if after == val else "   <- DID NOT STICK"))
    if reply.get("error"):
        print("   surface said: %s" % reply["error"])
    return 0

if __name__ == "__main__":
    sys.exit(main())
