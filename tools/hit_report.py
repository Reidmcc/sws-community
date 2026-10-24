"""Where lasers land on each starfighter: reads a server log written with hit_log=1 and draws, per ship, the hits in the
ship's own frame (seen from above: x across, z along, nose up) over its laser-hitbox meshes' boxes (mod/hitbox_shapes.h).
Usage: python -I tools/hit_report.py [status log]   (default: the newest probe_logs/server_*.status.txt)"""
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HIT = re.compile(r"hit: (\S+) part (-?\d+) kind (-?\d+) material (\d+) damage ([-\d.]+) at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)")
INFO = re.compile(r"hit: (\S+): (transform found .*|no transform found .*)")


def ship_key(asset):
    m = re.search(r"starships/([^/]+)/", asset.lower())   # Game/Gameplay/Starships/<Ship>/Config/...
    k = m.group(1) if m else asset.lower().replace("starshiphealthdamagedata_", "")
    for suf in ("_bot_pvb", "_pvb", "_bot", "_ai", "_sp"):
        k = k.replace(suf, "")
    return k


def mesh_boxes():
    t = open(os.path.join(ROOT, "mod", "hitbox_shapes.h")).read()
    boxes = {}
    for m in re.finditer(r'\{"(\w+)", (\d+), (\w+), (\d+), \{([^}]*)\}', t):
        f = struct.unpack("<8f", bytes(int(x, 16) for x in m.group(5).split(",")))
        lo, hi = f[0:3], f[4:7]
        if max(hi[i] - lo[i] for i in range(3)) < 0.2:
            continue   # the tiny marker meshes
        boxes.setdefault(m.group(1), []).append((lo, hi))
    return boxes


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(ROOT, "probe_logs", "server_*.status.txt")), key=os.path.getmtime)
    hits, info = {}, []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = HIT.search(line)
        if m:
            hits.setdefault(m.group(1), []).append((int(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5)),
                                                    float(m.group(6)), float(m.group(7)), float(m.group(8))))
            continue
        m = INFO.search(line)
        if m:
            info.append(f"{m.group(1)}: {m.group(2)}")
    print(path)
    for i in info:
        print("  " + i)
    boxes = mesh_boxes()
    for asset, hs in sorted(hits.items(), key=lambda kv: -len(kv[1])):
        key = ship_key(asset)
        bx = boxes.get(key, [])
        xs, ys, zs = [h[4] for h in hs], [h[5] for h in hs], [h[6] for h in hs]
        mats = {}
        for h in hs:
            mats[h[2]] = mats.get(h[2], 0) + 1
        print(f"\n{asset} ({key}): {len(hs)} hits; materials {dict(sorted(mats.items()))}")
        print(f"  x {min(xs):.2f}..{max(xs):.2f}  y {min(ys):.2f}..{max(ys):.2f}  z {min(zs):.2f}..{max(zs):.2f}")
        for i, (lo, hi) in enumerate(bx):
            inside = sum(1 for h in hs if all(lo[k] - 0.3 <= h[4 + k] <= hi[k] + 0.3 for k in range(3)))
            print(f"  mesh box {i}: x {lo[0]:.2f}..{hi[0]:.2f}  y {lo[1]:.2f}..{hi[1]:.2f}  z {lo[2]:.2f}..{hi[2]:.2f}: {inside} hits within 0.3 m")
        outside = [h for h in hs if not any(all(lo[k] - 0.3 <= h[4 + k] <= hi[k] + 0.3 for k in range(3)) for lo, hi in bx)]
        print(f"  {len(outside)} of {len(hs)} hits fall outside every mesh box" if bx else "  (no mesh boxes known for this ship)")
        # top-down picture, 1 m cells: digits = hits in the cell (9 = 9 or more), '.' = inside a mesh box, nose up
        x0 = int(min([b[0][0] for b in bx] + xs)) - 1
        x1 = int(max([b[1][0] for b in bx] + xs)) + 1
        z0 = int(min([b[0][2] for b in bx] + zs)) - 1
        z1 = int(max([b[1][2] for b in bx] + zs)) + 1
        if x1 - x0 > 70 or z1 - z0 > 70:
            print("  (too spread out to draw)")
            continue
        grid = {}
        for h in hs:
            c = (int(h[4] // 1), int(h[6] // 1))
            grid[c] = grid.get(c, 0) + 1
        print(f"  seen from above, 1 m cells, x {x0}..{x1} across, z {z1} (top) .. {z0}:")
        for z in range(z1, z0 - 1, -1):
            row = ""
            for x in range(x0, x1 + 1):
                n = grid.get((x, z), 0)
                if n:
                    row += str(min(n, 9))
                elif any(lo[0] <= x + 0.5 <= hi[0] and lo[2] <= z + 0.5 <= hi[2] for lo, hi in bx):
                    row += "."
                else:
                    row += " "
            print(f"  {z:4d} |{row}|")


if __name__ == "__main__":
    main()
