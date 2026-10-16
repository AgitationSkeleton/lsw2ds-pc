"""Locate a strongly coloured shape on a captured screen, by colour not by eye.

WHY. The co-op arrows are visible in the lobby, where they should not be, and
what is on screen is "a green wedge". Finding where that is by looking at a
256x192 screenshot is guesswork, and the whole point of the 3D toolchain that
solved the top-screen portrait's face is to stop guessing: once the shape's screen box is known,
`gx_polygons.py --region` names the geometry that covers it, and from there the
texture and the routine that submits it follow.

It reports the pixel count and the bounding box, and it says plainly when it
finds nothing - a colour scan that returns an empty box must not be read as "the
shape is somewhere else".

Usage:
    python tools/find_green.py <capture.png> [--channel green|red|blue]
"""

import sys

try:
    from PIL import Image
except ImportError:
    print("PIL is not available, so no scan was made. That is a missing tool,")
    print("not an absent shape.")
    raise SystemExit(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    path = sys.argv[1]
    channel = "green"
    if "--channel" in sys.argv:
        channel = sys.argv[sys.argv.index("--channel") + 1]
    idx = {"red": 0, "green": 1, "blue": 2}.get(channel, 1)

    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()

    # DOMINANT, not merely present. The cantina floor is a warm grey and the
    # characters are muted, so a channel that leads the other two by a clear
    # margin is what separates a deliberate marker colour from shading.
    hits = []
    for y in range(h):
        for x in range(w):
            c = px[x, y]
            v = c[idx]
            others = [c[i] for i in range(3) if i != idx]
            if v > 90 and v > others[0] + 35 and v > others[1] + 35:
                hits.append((x, y, c))

    print("%s: %dx%d" % (path, w, h))
    print("strongly %s pixels: %d" % (channel, len(hits)))
    if not hits:
        print("NOTHING FOUND. This is a real negative for this capture only -")
        print("the shape may be on the other screen, on the other machine, or")
        print("not present in this state at all.")
        return

    xs = [p[0] for p in hits]
    ys = [p[1] for p in hits]
    print("box x %d..%d  y %d..%d" % (min(xs), max(xs), min(ys), max(ys)))
    # A few samples, so the colour can be sanity-checked against the report's
    # own description rather than trusted.
    for p in hits[:6]:
        print("   (%3d,%3d) rgb%s" % p)


main()
