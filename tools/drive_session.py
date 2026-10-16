"""Drive the running port over its debug socket and report state changes.

    py drive.py <errlog> <shotdir> <tag> press:start x3 wait:60 shot touch:128,96

Every step is applied, then the state log is re-read and any NEW `[state]`
lines are printed. That is the whole point: advertising a co-op game to the
master server, and filling the cartridge's own co-op lobby list, need the
mapping from
"what I just did" to "which state was requested and by whom", and reading the
log after each step is what pairs them.

No window is ever touched - everything goes through 127.0.0.1:19842.
"""
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

HOST, PORT = "127.0.0.1", 19842


class Surface:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=15.0)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("the port closed the socket")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode())

    def cmd(self, obj):
        self.send(obj)
        return self.line()

    def key(self, name, down):
        return self.cmd({"cmd": "frontend_input", "action": "key",
                         "key": name, "down": down})

    def press(self, name, frames=8):
        self.key(name, True)
        time.sleep(frames / 60.0)
        self.key(name, False)
        time.sleep(4 / 60.0)

    def touch(self, x, y, frames=8):
        self.cmd({"cmd": "frontend_input", "action": "touch",
                  "x": x, "y": y, "down": True})
        time.sleep(frames / 60.0)
        self.cmd({"cmd": "frontend_input", "action": "touch",
                  "x": x, "y": y, "down": False})
        time.sleep(4 / 60.0)

    def shot(self, path):
        r = self.cmd({"cmd": "black_band_capture"})
        return r


def new_state_lines(errlog, seen):
    """Every [state] / [fork] / [diag] line not yet reported."""
    try:
        text = Path(errlog).read_bytes().decode("utf-8", "replace")
    except OSError:
        return []
    out = []
    for i, ln in enumerate(text.split("\n")):
        if i < seen[0]:
            continue
        if ("[state]" in ln or "[fork]" in ln or "[diag]" in ln
                or "[trace]" in ln):
            out.append(ln.rstrip())
    seen[0] = len(text.split("\n"))
    return out


def capture(tag, shotdir, n):
    """Use pilot.py for the framebuffer - it already knows the format."""
    out = str(Path(shotdir) / f"{tag}{n:02d}")
    subprocess.run([sys.executable.replace("python", "python"),
                    "tools/pilot.py", "shot", out + ".png"],
                   capture_output=True)
    return out


def main():
    errlog, shotdir, tag = sys.argv[1], sys.argv[2], sys.argv[3]
    steps = sys.argv[4:]
    seen = [0]
    # Report what is already there once, so the first step's output is new.
    new_state_lines(errlog, seen)

    s = Surface()
    shot_n = 0
    for step in steps:
        if step.startswith("press:"):
            spec = step.split(":", 1)[1]
            name, _, times = spec.partition("x")
            for _ in range(int(times) if times else 1):
                s.press(name)
                time.sleep(0.25)
            print(f"--- pressed {name} {times or 1}x")
        elif step.startswith("hold:"):
            name, frames = step.split(":", 1)[1].split(",")
            s.press(name, int(frames))
            print(f"--- held {name} for {frames} frames")
        elif step.startswith("touch:"):
            x, y = step.split(":", 1)[1].split(",")
            s.touch(int(x), int(y))
            print(f"--- touched ({x},{y})")
        elif step.startswith("wait:"):
            f = int(step.split(":", 1)[1])
            time.sleep(f / 60.0)
            print(f"--- waited {f} frames")
        elif step == "shot":
            shot_n += 1
            p = capture(tag, shotdir, shot_n)
            print(f"--- shot {p}")
        else:
            print(f"!!! unknown step {step}")
            continue
        for ln in new_state_lines(errlog, seen):
            print("   ", ln)
    s.sock.close()


if __name__ == "__main__":
    main()
