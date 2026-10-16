"""Map an LSW2_SAMPLE_PROF dump to the exe's symbols.

    python tools/sampleprof_report.py <dump.txt> [exe] [top]

The dump holds `base <hex>` (where the exe was loaded) and `<rip> <count>`
rows. Addresses are rebased onto the exe's preferred ImageBase and resolved
against `nm` (the port's exe keeps its symbol table), then totalled per
function and per subsystem. See port/frontend/sampleprof.h.
"""
import bisect
import re
import subprocess
import sys


def image_base(exe):
    out = subprocess.run(["objdump", "-p", exe], capture_output=True, text=True).stdout
    m = re.search(r"^ImageBase\s+([0-9a-fA-F]+)", out, re.M)
    return int(m.group(1), 16) if m else 0x140000000


def symbols(exe):
    out = subprocess.run(["nm", "-C", "--defined-only", exe], capture_output=True,
                         text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3 or parts[1] not in ("t", "T"):
            continue
        name = parts[2]
        if name.startswith("."):
            continue
        syms.append((int(parts[0], 16), name))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


CATEGORIES = [
    ("arm9 recompiled", re.compile(r"lsw2_arm9_|^FUN_02|lsw2_arm9")),
    ("arm7", re.compile(r"lsw2_arm7_|ARMv4|ARM7|arm7", re.I)),
    ("interpreter/dispatch", re.compile(r"interp|dispatch|runtime_|execute_|decode", re.I)),
    ("gpu3d", re.compile(r"gpu3d|GPU3D|Rasteriz|SoftRender|polygon|GX", re.I)),
    ("gpu2d", re.compile(r"gpu2d|GPU2D|bg_|obj_|Render", re.I)),
    ("spu/audio", re.compile(r"spu|SPU|audio|Audio|mix", re.I)),
    ("wifi", re.compile(r"Wifi|wifi|MP_|local_mp|Local", re.I)),
    ("scheduler/bus", re.compile(r"scheduler|bus_|Bus|mem|dma|DMA", re.I)),
    ("port frontend", re.compile(r"lsw2_(?!arm)", re.I)),
]


def category(name):
    for label, rx in CATEGORIES:
        if rx.search(name):
            return label
    return "other"


def main():
    dump = sys.argv[1]
    exe = sys.argv[2] if len(sys.argv) > 2 else "build/lsw2_bountyhunter.exe"
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    base = 0
    header = {}
    rows = []
    for line in open(dump):
        k, v = line.split()
        if k in ("base",):
            base = int(v, 16)
        elif k in ("samples", "state", "fps"):
            header[k] = v
        else:
            rows.append((int(k, 16), int(v)))
    pref = image_base(exe)
    addrs, names = symbols(exe)
    per_fn = {}
    per_cat = {}
    total = 0
    for rip, n in rows:
        va = rip - base + pref
        i = bisect.bisect_right(addrs, va) - 1
        name = names[i] if 0 <= i < len(names) else "?"
        if not (addrs[0] <= va <= addrs[-1] + 0x100000):
            name = "<outside exe: system/driver>"
        per_fn[name] = per_fn.get(name, 0) + n
        c = category(name) if not name.startswith("<") else "system/driver"
        per_cat[c] = per_cat.get(c, 0) + n
        total += n
    print(f"{dump}: state {header.get('state')} fps {header.get('fps')} "
          f"samples {total}")
    print("-- by subsystem")
    for c, n in sorted(per_cat.items(), key=lambda x: -x[1]):
        print(f"  {100.0 * n / total:5.1f}%  {c}")
    print(f"-- top {top} functions")
    for name, n in sorted(per_fn.items(), key=lambda x: -x[1])[:top]:
        print(f"  {100.0 * n / total:5.1f}%  {name[:110]}")


if __name__ == "__main__":
    main()
