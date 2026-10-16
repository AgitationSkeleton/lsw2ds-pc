"""Carry the DS wireless radio across a real network - the loopback-to-internet
bridge online play needs.

WHY THIS EXISTS. The port's central goal is that two players connect through
the base game's own Bounty Hunter connection system and play an actual
deathmatch, rather than just being warped onto the map. The cartridge's own
wireless does that, and every verified match has run over it - but the
framework emulates the radio as LOOPBACK UDP between local instances
(`runner/src/wifi_net.cpp`, class LocalMpTransport): each instance binds
127.0.0.1:(base + instanceIndex) and `sendto`s every wireless frame, as one
datagram, to 127.0.0.1:(base + i) for every OTHER local instance. So two copies
on one machine hear each other; two machines across the internet do not, because
the datagrams never leave 127.0.0.1.

WHAT THIS DOES. It is a transport-agnostic relay that carries those datagrams
between two machines over TCP, without touching the game or the framework. On
each machine it:

  * BINDS the loopback port the local instance sends the REMOTE peer's frames to
    (base + remoteInstance) - so it catches exactly what would have gone to the
    peer - and forwards each datagram over the TCP link, length-prefixed.
  * INJECTS each datagram that arrives over the TCP link into
    127.0.0.1:(base + localInstance), the port the local instance is listening
    on, so the instance receives it as if a local peer had sent it. The
    datagram's own `sender` field is untouched, so the cartridge sees the real
    peer index.

The frame is opaque here: the relay never parses the NIFI header, it moves
bytes. Ordering and delivery are TCP's, which is stronger than the UDP the
radio assumed, so nothing the radio needs is lost; the open question that goal
ultimately rests on is LATENCY - whether the DS wireless
handshake tolerates internet round-trips - and that is a live-match measurement,
not something this relay can settle by itself.

TESTABLE ON ONE MACHINE. Two instances launched with DIFFERENT
`--local-wireless-port` bases cannot hear each other directly (instance 0 at
base A sends to A+1; instance 1 at base B listens on B+1; A+1 != B+1). Only a
bridge that catches A+1 and injects B+1 (and the reverse) connects them. That is
exactly the two-machine topology with the network replaced by loopback, and it
is what `selftest` exercises with mock endpoints.

USAGE

    # host machine: accept the joiner's bridge, relay for a 2-player game
    py tools/wireless_bridge.py serve --port 55000 \
        --base 30000 --local 0 --remote 1

    # joiner machine: connect to the host's bridge
    py tools/wireless_bridge.py connect --host HOST --port 55000 \
        --base 40000 --local 1 --remote 0

    # one-machine proof, no game: a datagram crosses both ways
    py tools/wireless_bridge.py selftest
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
import time

LOOPBACK = "127.0.0.1"


class Relay:
    """One machine's half of the bridge: catch outbound frames on `catch_port`,
    forward over the TCP `link`, and inject frames received from the link into
    `inject_port`. The TCP link is already connected."""

    def __init__(self, link: socket.socket, catch_port: int, inject_port: int):
        self._link = link
        self._catch_port = catch_port
        self._inject_port = inject_port
        self._catch = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._catch.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._catch.bind((LOOPBACK, catch_port))
        self._catch.settimeout(0.2)
        self._inject = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._stop = threading.Event()
        self.tx = 0   # datagrams caught and forwarded
        self.rx = 0   # datagrams received from the link and injected

    def start(self) -> None:
        self._t_out = threading.Thread(target=self._pump_outbound, daemon=True)
        self._t_in = threading.Thread(target=self._pump_inbound, daemon=True)
        self._t_out.start()
        self._t_in.start()

    def stop(self) -> None:
        self._stop.set()

    # local instance -> the peer: UDP catch -> length-prefixed TCP
    def _pump_outbound(self) -> None:
        while not self._stop.is_set():
            try:
                data, _ = self._catch.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                self._link.sendall(struct.pack("<I", len(data)) + data)
                self.tx += 1
            except OSError:
                break

    # the peer -> local instance: TCP length-prefixed -> UDP inject
    def _pump_inbound(self) -> None:
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._link.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 4:
                (n,) = struct.unpack("<I", buf[:4])
                if len(buf) < 4 + n:
                    break
                frame = buf[4 : 4 + n]
                buf = buf[4 + n :]
                try:
                    self._inject.sendto(frame, (LOOPBACK, self._inject_port))
                    self.rx += 1
                except OSError:
                    pass


def _relay_from_args(link: socket.socket, args) -> Relay:
    # Catch what the local instance sends to the remote's slot; inject into the
    # local instance's own listening port.
    catch = args.base + args.remote
    inject = args.base + args.local
    print(
        f"[bridge] relaying: catch 127.0.0.1:{catch} (frames for peer {args.remote}) "
        f"-> link -> inject 127.0.0.1:{inject} (local instance {args.local})",
        flush=True,
    )
    return Relay(link, catch, inject)


def cmd_serve(args) -> int:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"[bridge] serve: waiting for a peer bridge on :{args.port}", flush=True)
    link, who = srv.accept()
    print(f"[bridge] serve: peer bridge connected from {who}", flush=True)
    link.settimeout(0.2)
    relay = _relay_from_args(link, args)
    relay.start()
    _run_until_interrupt(relay)
    return 0


def cmd_connect(args) -> int:
    for attempt in range(60):
        try:
            link = socket.create_connection((args.host, args.port), timeout=5)
            break
        except OSError:
            if attempt == 0:
                print("[bridge] connect: waiting for the host bridge...", flush=True)
            time.sleep(1)
    else:
        print("[bridge] connect: could not reach the host bridge", file=sys.stderr)
        return 1
    print(f"[bridge] connect: linked to {args.host}:{args.port}", flush=True)
    link.settimeout(0.2)
    relay = _relay_from_args(link, args)
    relay.start()
    _run_until_interrupt(relay)
    return 0


def _run_until_interrupt(relay: Relay) -> None:
    try:
        while True:
            time.sleep(2)
            print(f"[bridge] tx {relay.tx}  rx {relay.rx}", flush=True)
    except KeyboardInterrupt:
        relay.stop()


def cmd_selftest(_args) -> int:
    """Two relays over a loopback TCP pair, two mock instances on DIFFERENT
    bases, and a datagram that must cross each way. No game."""
    base_a, base_b = 47000, 48000
    local_a, remote_a = 0, 1
    local_b, remote_b = 1, 0

    # The mock instances' listening sockets (where injected frames land).
    inst_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    inst_a.bind((LOOPBACK, base_a + local_a))
    inst_a.settimeout(2)
    inst_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    inst_b.bind((LOOPBACK, base_b + local_b))
    inst_b.settimeout(2)

    # The TCP link between the two bridges, over loopback.
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((LOOPBACK, 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    link_b = socket.create_connection((LOOPBACK, port))
    link_a, _ = srv.accept()
    link_a.settimeout(0.2)
    link_b.settimeout(0.2)

    relay_a = Relay(link_a, base_a + remote_a, base_a + local_a)  # catch A+1, inject A+0
    relay_b = Relay(link_b, base_b + remote_b, base_b + local_b)  # catch B+0, inject B+1
    relay_a.start()
    relay_b.start()
    time.sleep(0.2)

    checks = 0
    failed = 0

    def check(ok: bool, what: str) -> None:
        nonlocal checks, failed
        checks += 1
        if ok:
            print(f"  ok: {what}")
        else:
            failed += 1
            print(f"  FAIL: {what}")

    # A's instance sends a frame destined for peer 1 -> A's relay catches A+1 ->
    # link -> B's relay injects B+1 -> B's instance receives it.
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload_ab = b"NIFI-from-A-to-B" + bytes(range(32))
    sender.sendto(payload_ab, (LOOPBACK, base_a + remote_a))
    try:
        got, _ = inst_b.recvfrom(4096)
        check(got == payload_ab, "a frame from instance 0 reaches instance 1 across the bridge")
    except socket.timeout:
        check(False, "a frame from instance 0 reaches instance 1 across the bridge (timed out)")

    # And the reverse: B's instance sends to peer 0 -> B catches B+0 -> A injects A+0.
    payload_ba = b"NIFI-from-B-to-A" + bytes(range(40))
    sender.sendto(payload_ba, (LOOPBACK, base_b + remote_b))
    try:
        got, _ = inst_a.recvfrom(4096)
        check(got == payload_ba, "a frame from instance 1 reaches instance 0 across the bridge")
    except socket.timeout:
        check(False, "a frame from instance 1 reaches instance 0 across the bridge (timed out)")

    # The bytes are opaque and exact - the relay must not reframe or truncate.
    check(relay_a.tx == 1 and relay_b.rx == 1, "A forwarded exactly one, B injected exactly one")
    check(relay_b.tx == 1 and relay_a.rx == 1, "B forwarded exactly one, A injected exactly one")

    # A datagram larger than one TCP recv still arrives whole (framing works).
    big = b"X" * 2000
    sender.sendto(big, (LOOPBACK, base_a + remote_a))
    try:
        got, _ = inst_b.recvfrom(4096)
        check(got == big, "a 2000-byte frame crosses whole (TCP length-prefix framing holds)")
    except socket.timeout:
        check(False, "a 2000-byte frame crosses whole (timed out)")

    relay_a.stop()
    relay_b.stop()
    print(f"{checks} checks, {failed} failed")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name in ("serve", "connect"):
        p = sub.add_parser(name)
        if name == "connect":
            p.add_argument("--host", required=True)
        p.add_argument("--port", type=int, required=True, help="TCP link port")
        p.add_argument("--base", type=int, required=True, help="this instance's --local-wireless-port")
        p.add_argument("--local", type=int, required=True, help="this machine's instance index")
        p.add_argument("--remote", type=int, required=True, help="the peer's instance index")

    sub.add_parser("selftest")

    args = ap.parse_args(argv)
    return {"serve": cmd_serve, "connect": cmd_connect, "selftest": cmd_selftest}[args.cmd](args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
