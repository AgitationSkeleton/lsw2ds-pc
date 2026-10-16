import socket, json, sys
def rd(port, addr, ln):
    s = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    f = s.makefile("rwb")
    f.write((json.dumps({"cmd":"read_mem","cpu":9,"addr":addr,"len":ln})+"\n").encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    try:
        return json.loads(line).get("hex","")
    except Exception:
        return "ERR " + line[:120]
addr = int(sys.argv[1], 0); ln = int(sys.argv[2])
for port, name in ((19842,"HOST "), (19843,"CHILD")):
    h = rd(port, addr, ln)
    print(name, "%08X" % addr)
    for i in range(0, len(h), 32):
        print("   +%02X %s" % (i//2, h[i:i+32]))
