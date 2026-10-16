"""See and drive the running port over its debug surface.

WHY THIS EXISTS. Up to now the only way to know what the port was doing was to
read its log and ask someone watching what was on screen. That is how a
completely blank top screen went unnoticed for a long time: the log was clean, the frame rate was right,
and nothing said "the picture is empty".

The framework's runner ships a TCP control surface (`--serve`) offering
framebuffer capture, memory and register reads, and input injection. Our shell
now implements the input side of it, so the game can be piloted and observed
directly.

=========================================================================
`shot` CANNOT SEE THE FORK'S MENUS. READ THIS BEFORE BELIEVING A CAPTURE.
=========================================================================

MEASURED at the source. `shot` sends {"cmd":"framebuffer"} and the
framework answers it in runner/src/debug_server.cpp with

    nds_gpu2d_framebuffer(screen)

which is the GPU2D's raw output for engine A or B. The fork's menus are NOT in
it. They are drawn host-side at present time, in frontend.cpp:

    if (screen == 1)
        pixels = lsw2_menu_composite(pixels, src_w, lsw2_menuhost_current());

onto a local buffer that goes straight to the window. Nothing writes it back,
so a capture taken while a menu is open shows the guest's screen UNDERNEATH the
menu - typically menubackdrop's bare starfield - and looks exactly like a menu
that failed to draw.

WHAT THIS ALREADY COST. A whole investigation once concluded "the
arena menu opens invisible and swallows input", from a capture of a starfield
with no rows on it, while the port's own guard instrumentation reported that
the composite ran and tripped none of its four exits. The instrumentation was
right and the picture was wrong. "touch-drives-the-menu never visually
confirmed" is the same fact wearing a different hat: it is not confirmable this
way, and no amount of re-capturing will change that.

SO: a capture is evidence about the GUEST's rendering only. For anything the
port draws - menus, the row highlight, the scroll window, the font - trust the
port's own logging, or fix this path first. Fixing it means getting the
composited pixels into the capture, which is a port-side change: the framework
is vendored and the debug server is its code, not ours.

Usage:
    python tools/pilot.py shot out.png          # capture both screens
    python tools/pilot.py shot out.png --screen 0
    python tools/pilot.py press start           # tap a button
    python tools/pilot.py hold right 30         # hold it for N frames
    python tools/pilot.py touch 128 96          # tap the stylus
    python tools/pilot.py stats                 # frame/dispatch counters
    python tools/pilot.py raw '{"cmd":"ping"}'  # anything else

Buttons: a b start select up down left right l r ds_x ds_y

The image is written as PNG when Pillow is available and as PPM otherwise, so
this has no hard dependency - a PPM still opens in anything that matters and can
be converted later.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import os
import socket
import struct
import sys
import time

HOST = "127.0.0.1"

# LSW2_PILOT_PORT picks which running instance to drive. A bounty hunter match
# needs two of them - the cartridge's own receive pump is gated on the player
# count being above one, so a solo lobby cannot start a round - and
# they are told apart by their debug port: launch the host with --port 19842 and
# the joiner with --port 19843, then drive the joiner with
# LSW2_PILOT_PORT=19843.
PORT = int(os.environ.get("LSW2_PILOT_PORT", "19842"))
SCREEN_W, SCREEN_H = 256, 192


class Surface:
    def __init__(self, host=HOST, port=PORT, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode("utf-8"))

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("the port closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def command(self, **kw):
        self.send(kw)
        return self.line()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect():
    try:
        return Surface()
    except OSError as exc:
        print("cannot reach the port on %s:%d - %s" % (HOST, PORT, exc))
        print("Start it with the debug surface enabled:")
        print("    port/launch.sh --serve")
        sys.exit(1)


def save_rgb(path: pathlib.Path, rgb: bytes, width, height):
    """rgb: packed RGB888, row-major."""
    try:
        from PIL import Image                      # noqa: PLC0415
        Image.frombytes("RGB", (width, height), bytes(rgb)).save(path)
        return path
    except Exception:                              # noqa: BLE001
        ppm = path.with_suffix(".ppm")
        with ppm.open("wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (width, height))
            f.write(bytes(rgb))
        return ppm


def cmd_shot(args):
    """The surface answers {"w","h","rgb"} with rgb as plain hex, one byte per
    channel. Engine A is the top screen, engine B the bottom."""
    s = connect()
    engines = ["A", "B"] if args.screen is None else (["A"] if args.screen == 0 else ["B"])
    out = pathlib.Path(args.out)
    written = []
    for eng in engines:
        reply = s.command(cmd="framebuffer", engine=eng)
        hexrgb = reply.get("rgb")
        if not hexrgb:
            print("engine %s: %s" % (eng, json.dumps(reply)[:200]))
            continue
        raw = bytes.fromhex(hexrgb)
        w, h = int(reply.get("w", SCREEN_W)), int(reply.get("h", SCREEN_H))
        name = out if len(engines) == 1 else out.with_name(
            "%s-%s%s" % (out.stem, "top" if eng == "A" else "bottom", out.suffix))
        written.append(save_rgb(name, raw, w, h))
    s.close()
    for w in written:
        print("wrote %s" % w)
    if not written:
        sys.exit(1)


def cmd_press(args):
    s = connect()
    print(s.command(cmd="frontend_input", action="key", key=args.button, down=True))
    time.sleep(args.frames / 60.0)
    print(s.command(cmd="frontend_input", action="key", key=args.button, down=False))
    s.close()


def cmd_touch(args):
    s = connect()
    print(s.command(cmd="frontend_input", action="touch",
                    x=args.x, y=args.y, down=True))
    time.sleep(args.frames / 60.0)
    print(s.command(cmd="frontend_input", action="touch",
                    x=args.x, y=args.y, down=False))
    s.close()


def _do_press(s, button, frames):
    s.command(cmd="frontend_input", action="key", key=button, down=True)
    time.sleep(frames / 60.0)
    s.command(cmd="frontend_input", action="key", key=button, down=False)


def _do_touch(s, x, y, frames):
    s.command(cmd="frontend_input", action="touch", x=x, y=y, down=True)
    time.sleep(frames / 60.0)
    s.command(cmd="frontend_input", action="touch", x=x, y=y, down=False)


def cmd_script(args):
    """Run a whole input sequence over ONE persistent connection.

    A drive that presses ten keys used to spawn ten `python pilot.py` processes,
    and each console-subsystem launch flashes a window that steals focus from a
    fullscreen app - an unwanted alt-tab out of whatever is in front. This
    runs the lot on a single socket from a single process (launch it hidden and
    there is no flash at all). One command per line, '#' starts a comment:

        press <button> [frames]
        hold  <button> <frames>
        touch <x> <y> [frames]
        sleep <seconds>
        echo  <text>
        raw   <json>          # send arbitrary json, print the reply

    Reads the script from the file in `args.path`, or stdin when it is '-'.
    """
    text = sys.stdin.read() if args.path == "-" else pathlib.Path(args.path).read_text()
    s = connect()
    try:
        for lineno, raw_line in enumerate(text.splitlines(), 1):
            line = raw_line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            op = parts[0].lower()
            try:
                if op in ("press", "hold"):
                    btn = parts[1]
                    frames = int(parts[2]) if len(parts) > 2 else 6
                    _do_press(s, btn, frames)
                elif op == "touch":
                    x, y = int(parts[1]), int(parts[2])
                    frames = int(parts[3]) if len(parts) > 3 else 6
                    _do_touch(s, x, y, frames)
                elif op == "sleep":
                    time.sleep(float(parts[1]))
                elif op == "echo":
                    print(line[len("echo"):].strip())
                elif op == "raw":
                    s.send(json.loads(line[len("raw"):].strip()))
                    print(json.dumps(s.line())[:400])
                else:
                    print("line %d: unknown op %r" % (lineno, op))
            except (IndexError, ValueError) as exc:
                print("line %d: bad %r (%s)" % (lineno, line, exc))
    finally:
        s.close()


def cmd_stats(args):
    s = connect()
    for what in ("frontend_stats", "frontend_input_stats", "dispatch_stats"):
        try:
            print("%-22s %s" % (what, json.dumps(s.command(cmd=what))[:400]))
        except Exception as exc:                   # noqa: BLE001
            print("%-22s failed: %s" % (what, exc))
    s.close()


def cmd_raw(args):
    s = connect()
    s.send(json.loads(args.json))
    print(json.dumps(s.line(), indent=1)[:4000])
    s.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="what", required=True)

    p = sub.add_parser("shot"); p.add_argument("out")
    p.add_argument("--screen", type=int, choices=[0, 1])
    p.set_defaults(fn=cmd_shot)

    p = sub.add_parser("press"); p.add_argument("button")
    p.add_argument("--frames", type=int, default=6)
    p.set_defaults(fn=cmd_press)

    p = sub.add_parser("hold"); p.add_argument("button")
    p.add_argument("frames", type=int)
    p.set_defaults(fn=cmd_press)

    p = sub.add_parser("touch")
    p.add_argument("x", type=int); p.add_argument("y", type=int)
    p.add_argument("--frames", type=int, default=6)
    p.set_defaults(fn=cmd_touch)

    p = sub.add_parser("script"); p.add_argument("path")
    p.set_defaults(fn=cmd_script)

    p = sub.add_parser("stats"); p.set_defaults(fn=cmd_stats)
    p = sub.add_parser("raw"); p.add_argument("json"); p.set_defaults(fn=cmd_raw)

    args = ap.parse_args()
    args.fn(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
