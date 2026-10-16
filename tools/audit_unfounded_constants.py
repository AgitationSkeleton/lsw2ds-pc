#!/usr/bin/env python3
"""Flag Godot-port constants that cite no ROM address and no other evidence.

The failure mode this catches is real and has shipped more than once: a constant
invented to make something look right, with a comment that REASONS toward it
convincingly. The turret team colours were four RGB values justified by an
argument about palettes; the cartridge plays a layered animation and tints
nothing. A plausible comment is not evidence - an address is.

Scans each `const NAME := <value with a digit>` and the comment block directly
above it. Reports anything whose comment contains no 0x02xxxxxx address, and
separates out the ones that cite no measurement, capture frame, doc or config
either - those are the ones to check first.

Not every hit is a defect. Godot-side layout, network ports and screen sizes
have no ROM counterpart. The point is to make the unfounded ones VISIBLE rather
than let them sit among the derived ones looking equally authoritative.
"""

import re, glob
BS = chr(92)
pat = re.compile(r'^\s*const\s+([A-Z_0-9]+)\s*:?=\s*(.+)$')
addr = re.compile(r'0x02[0-9A-Fa-f]{6}')
evid = re.compile(r'measured|capture|f\d{4,}|docs/\d+|config/', re.I)
rows = []
for f in sorted(glob.glob('godot/**/*.gd', recursive=True)):
    norm = f.replace(BS, '/')
    if '/tests/' in norm:
        continue
    L = open(f, encoding='utf-8', errors='replace').read().split('\n')
    for i, l in enumerate(L):
        m = pat.match(l)
        if not m:
            continue
        name, val = m.group(1), m.group(2).strip()
        if not re.search(r'\d', val):
            continue
        ctx = []
        j = i - 1
        while j >= 0 and (L[j].strip().startswith('#') or L[j].strip() == ''):
            ctx.append(L[j]); j -= 1
            if len(ctx) > 34:
                break
        blob = '\n'.join(ctx)
        if not addr.search(blob):
            rows.append((norm, i + 1, name, val[:44], bool(evid.search(blob))))
noev = [r for r in rows if not r[4]]
print("constants with NO ROM address AND no other evidence cited:\n")
for r in noev:
    print("  %-40s:%-5d %-30s = %s" % (r[0], r[1], r[2], r[3]))
print("\n%d constants lack a ROM address; %d cite no evidence at all" % (len(rows), len(noev)))
