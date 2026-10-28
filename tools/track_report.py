"""How bomber creep attack capital ships, from a flight record made with track_log=1 (R_TRACK records, type 11).

Usage: .venv/Scripts/python.exe -I tools/track_report.py <server_*.bin>

Each record is one bomber creep at one moment (twice a second): its position and its assigned target's point (the
squadron's assignment, as the scorer reads it). Per bomber, with a capital-ship target (one that isn't a starfighter):
- holding station: stretches of at least 3 s within 600 m of the target in which the distance changes by less than
  15 m/s, the "pull up behind it and hold distance" Pseudo described (2026-10-27);
- runs: in from beyond 1,000 m to inside 300 m, then back out beyond 1,000 m.
"""
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else ".")
from probe_read import load  # noqa: E402

HOLD_NEAR, HOLD_RATE, HOLD_MIN = 600.0, 15.0, 3.0
RUN_FAR, RUN_NEAR = 1000.0, 300.0


def main(path):
    a, freq, t0 = load(path)
    r = a[a["type"] == 11]
    if not len(r):
        sys.exit("no track records (type 11): was track_log=1 and record=1 set, with aggressive creep and bomber packs?")
    span = (a["qpc"].max() - t0) / freq
    by = defaultdict(list)
    for x in r:
        f = x["f"]
        owner = int(np.frombuffer(f[10:12].tobytes(), dtype="<u8")[0])
        tags = int(np.frombuffer(f[12:13].tobytes(), dtype="<u4")[0])
        by[int(x["state"])].append(((x["qpc"] - t0) / freq, float(f[9]), owner, 0.0 if tags & 2 else 1.0))   # bit 2: capital ship
    tot_t = tot_cap = tot_hold = 0.0; holds = []; runs = 0; ships = 0; ships_hold = 0
    for ship, rows in by.items():
        rows.sort()
        t = np.array([x[0] for x in rows]); d = np.array([x[1] for x in rows]); fighter = np.array([x[3] for x in rows])
        ships += 1
        dt = np.clip(np.diff(t, append=t[-1]), 0, 1.0); tot_t += dt.sum()
        cap = (d >= 0) & (fighter == 0); tot_cap += dt[cap].sum()
        # holding station
        s = None; had = False
        for i in range(1, len(t) + 1):
            ok = i < len(t) and cap[i] and cap[i - 1] and d[i] <= HOLD_NEAR and t[i] - t[i - 1] <= 1.0 and \
                abs(d[i] - d[i - 1]) / max(t[i] - t[i - 1], 1e-3) < HOLD_RATE
            if ok and s is None: s = t[i - 1]
            elif not ok and s is not None:
                if t[i - 1] - s >= HOLD_MIN: holds.append(t[i - 1] - s); tot_hold += t[i - 1] - s; had = True
                s = None
        ships_hold += had
        # runs: far -> near -> far against a capital target
        state = 0
        for i in range(len(t)):
            if not cap[i]: state = 0; continue
            if state == 0 and d[i] > RUN_FAR: state = 1
            elif state == 1 and d[i] < RUN_NEAR: state = 2
            elif state == 2 and d[i] > RUN_FAR: runs += 1; state = 1
    h = np.array(holds) if holds else np.array([0.0])
    print(f"{len(r)} track records over {span:.0f} s; {ships} bomber ships, {tot_t:.0f} s alive, {tot_cap:.0f} s with a capital-ship target")
    print(f"holding station (>= {HOLD_MIN:.0f} s within {HOLD_NEAR:.0f} m, distance changing < {HOLD_RATE:.0f} m/s): {len(holds)} stretches by "
          f"{ships_hold} ships, {tot_hold:.0f} s = {tot_hold / max(tot_cap, 1):.0%} of the time on a capital target; median {np.median(h):.1f} s, longest {h.max():.1f} s")
    print(f"runs (in from beyond {RUN_FAR:.0f} m to inside {RUN_NEAR:.0f} m and back out): {runs}, "
          f"{runs / max(tot_cap / 60, 1e-9):.2f} per minute on a capital target")


if __name__ == "__main__":
    main(sys.argv[1])
