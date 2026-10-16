"""Dump the running port's wireless frame trace.

WHY. An earlier investigation located the one thing blocking the port's central
requirement, two consoles playing a match together:
at the instant the match-start message arrives, the JOINER transmits an 802.11
DEAUTHENTICATION frame and the emulated radio correctly ends the multiplayer
session. Both consoles then show the cartridge's Communication Error.

The question left is why the guest sends it, and the framework already keeps the
answer: a network trace ring where every entry records the event kind, the
direction, both MAC addresses, the payload length AND the ARM9 and ARM7 program
counters at the moment it happened. `net_ring_dump` returns many entries in ONE
request, so unlike the 3D write ring (which laps thousands of times a frame and
cannot be walked over a socket) this can be read whole.

WHAT TO LOOK FOR. A management frame's subtype lives in the low byte of the
frame control word: 0x00C0 is deauthentication, which is the one melonDS reacts
to at Wifi.cpp:1036. The trace does not carry the frame body, so the deauth is
identified by WHERE it sits - the last transmit before the session drops - and
the PCs are what name the code that built it.

Usage:
    python tools/net_frames.py [port] [--max N] [--filter KIND] [--tx-only]

Kinds include wifi_tx_begin, wifi_tx_frame, wifi_rx_frame, wifi_association,
wifi_state_change, wifi_irq, wifi_reg_read, wifi_reg_write.
"""

import json
import socket
import sys

WIFI_KINDS = ("wifi_tx_begin", "wifi_tx_frame", "wifi_rx_frame",
              "wifi_association", "wifi_state_change", "wifi_irq")


def q(port, obj):
    s = socket.create_connection(("127.0.0.1", port), timeout=20.0)
    f = s.makefile("rwb")
    f.write((json.dumps(obj) + chr(10)).encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    return json.loads(line) if line.strip() else {}


def mac(h):
    if not h or len(h) < 12:
        return "-"
    return ":".join(h[i:i + 2] for i in range(0, 12, 2))


def main():
    port = 19843
    want = "all"
    limit = 200
    tx_only = False
    a = sys.argv[1:]
    i = 0
    while i < len(a):
        if a[i] == "--max":
            limit = int(a[i + 1], 0); i += 2
        elif a[i] == "--filter":
            want = a[i + 1]; i += 2
        elif a[i] == "--tx-only":
            tx_only = True; i += 1
        else:
            port = int(a[i], 0); i += 1

    st = q(port, {"cmd": "net_state"})
    produced = st.get("produced", 0)
    oldest = st.get("oldest", 0)
    cap = st.get("capacity", 0)
    print("net ring: produced %s, oldest %s, capacity %s" %
          (produced, oldest, cap))
    if not produced:
        print("  NOTHING HAS BEEN TRACED. That is a statement about the ring,")
        print("  not about the radio - do not read it as 'no frames were sent'.")
        return

    r = q(port, {"cmd": "net_ring_dump", "max": limit, "filter": want})
    if "error" in r:
        print("  %s" % r["error"])
        return
    ev = r.get("events", [])
    print("returned %d event(s)%s" % (len(ev), " (tx only)" if tx_only else ""))
    if len(ev) == 0:
        print("  none of this kind in the last %d entries." % limit)
        return

    shown = 0
    for e in ev:
        kind = e.get("kind", "?")
        if tx_only and "tx" not in kind:
            continue
        if want == "all" and kind not in WIFI_KINDS:
            continue
        print("  #%-8s %-18s dir=%s  src %s -> dst %s  len=%-5s aux=%-10s "
              "arm9=%08X arm7=%08X"
              % (e.get("count"), kind, e.get("direction"),
                 mac(e.get("src_mac", "")), mac(e.get("dst_mac", "")),
                 e.get("payload_len"), e.get("aux"),
                 e.get("arm9_pc", 0), e.get("arm7_pc", 0)))
        shown += 1
    if shown == 0:
        print("  the dump held only non-wireless events; pass --filter to widen.")


main()
