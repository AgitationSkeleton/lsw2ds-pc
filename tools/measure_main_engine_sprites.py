"""Player-colour ring: is the top-screen ring an OAM sprite the fork can recolour?

slotcolour.cpp says no, on the strength of ONE state.json in which oam_main is
empty. There are 286,097 captured frames. This samples them.
"""
import json, os, collections

ROOT = "out/work"
STEP = 25
per_stream = {}

for d in sorted(os.listdir(ROOT)):
    p = os.path.join(ROOT, d)
    if not os.path.isdir(p): continue
    for inst in ("inst0", "inst1", ""):
        q = os.path.join(p, inst) if inst else p
        if not os.path.isdir(q): continue
        try: names = sorted(x for x in os.listdir(q) if x.endswith("state.json"))
        except OSError: continue
        if not names: continue
        st = per_stream.setdefault(q, collections.Counter())
        pal = per_stream.setdefault(q + "|pal", collections.Counter())
        for name in names[::STEP]:
            try: dd = json.load(open(os.path.join(q, name)))
            except Exception: continue
            if not isinstance(dd, dict): continue
            a = dd.get("engine_a_main")
            if not isinstance(a, dict): continue
            la = a.get("layers_enabled")
            if not (isinstance(la, list) and len(la) == 5): continue
            st["frames"] += 1
            if not la[4]: continue
            st["obj_on"] += 1
            om = dd.get("oam_main")
            if not isinstance(om, list) or not om: continue
            st["oam_nonempty"] += 1
            hits = [e for e in om
                    if isinstance(e, dict) and 4 <= (e.get("palette") or -1) <= 7]
            if hits:
                st["player_colour"] += 1
                for e in hits: pal[e["palette"]] += 1

print(f"{'stream':44} {'frames':>7} {'obj on':>7} {'oam!=0':>7} {'pal 4-7':>8}")
tot = collections.Counter()
for q in sorted(k for k in per_stream if not k.endswith("|pal")):
    c = per_stream[q]
    if not c["frames"]: continue
    for k in c: tot[k] += c[k]
    if c["player_colour"]:
        print(f"{q:44} {c['frames']:7} {c['obj_on']:7} "
              f"{c['oam_nonempty']:7} {c['player_colour']:8}   "
              f"palettes {dict(per_stream[q+'|pal'])}")
print("-" * 80)
print(f"{'TOTAL':44} {tot['frames']:7} {tot['obj_on']:7} "
      f"{tot['oam_nonempty']:7} {tot['player_colour']:8}")
