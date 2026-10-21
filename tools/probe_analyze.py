"""Summarise a probe log: per movement-state timeline of drift presses, window resets, speed and heading jumps.

usage: probe_analyze.py <log.bin> [--state 0x...] [--csv out.csv]
Writes a readable report to stdout. Velocity comes from DRIVE records (f[7:10]); heading change is the angle
between consecutive velocity vectors of the same state.
"""
import sys, argparse, numpy as np, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_read import load, NAMES

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('log'); ap.add_argument('--state'); ap.add_argument('--csv')
    ap.add_argument('--jump-deg', type=float, default=20.0); ap.add_argument('--speed-jump', type=float, default=40.0)
    a = ap.parse_args()
    rec, freq, t0 = load(a.log)
    t = (rec['qpc'] - t0) / freq
    marks = [(float(t[i]), int(rec['f'][i][0])) for i in np.nonzero(rec['type'] == 5)[0]]
    print(f'{len(rec)} records over {t.max():.1f} s; marks: {[(round(x,2), m) for x, m in marks]}')
    states = np.unique(rec['state'][rec['type'] == 2])  # states that received drift input = player-controlled
    if a.state: states = [int(a.state, 16)]
    print('states with drift input:', [hex(int(s)) for s in states])
    for s in states:
        sel = rec['state'] == s
        print(f'\n=== state {int(s):#x}')
        ticks = np.nonzero(sel & (rec['type'] == 1))[0]
        drives = np.nonzero(sel & (rec['type'] == 4))[0]
        events = np.nonzero(sel & np.isin(rec['type'], (2, 3)))[0]
        if len(ticks):
            f = rec['f'][ticks]
            print(f'  ticks {len(ticks)}; cmdSpeed range {f[:,0].min():.1f}..{f[:,0].max():.1f}; topCruise {f[0,19]:.1f}; '
                  f'dbForce {f[0,17]:.0f}; dbTime {f[0,18]:.2f}')
        # state-change timeline from ticks (afterburner, drifting, timer resets, above-cruise)
        prev = None
        print('  timeline (state changes):')
        for i in ticks:
            f = rec['f'][i]
            cur = (int(f[2]), int(f[3]), int(f[7]))
            if prev is None or cur != prev[0] or (f[6] < prev[1] - 0.05):
                why = []
                if prev and cur[0] != prev[0][0]: why.append(f'afterburner {prev[0][0]}->{cur[0]}')
                if prev and cur[1] != prev[0][1]: why.append(f'aboveCruise {prev[0][1]}->{cur[1]}')
                if prev and cur[2] != prev[0][2]: why.append(f'drifting {prev[0][2]}->{cur[2]}')
                if prev and f[6] < prev[1] - 0.05: why.append('WINDOW RESET')
                print(f'    t={t[i]:8.3f} cmd={f[0]:6.1f} energy={f[4]:.2f} timer={f[6]:.2f} ab={cur[0]} above={cur[1]} drift={cur[2]}  {"; ".join(why)}')
            prev = (cur, f[6])
        pre = [i for i in events if rec['type'][i] == 2]
        print(f'  drift input calls: {len(pre)} (listing changes of the pressed value only)')
        last = None
        for i in pre:
            f = rec['f'][i]
            if int(f[22]) != last:
                print(f'    t={t[i]:8.3f} pressed={int(f[22])} before: drifting={int(f[7])} cmd={f[0]:.1f} timer={f[6]:.2f}')
                last = int(f[22])
        # velocity jumps from DRIVE
        if len(drives) > 1:
            v = rec['f'][drives][:, 7:10].astype(np.float64)
            sp = np.linalg.norm(v, axis=1)
            u = v / np.maximum(sp, 1e-6)[:, None]
            ang = np.degrees(np.arccos(np.clip((u[1:] * u[:-1]).sum(1), -1, 1)))
            dsp = np.diff(sp)
            td = t[drives]
            print(f'  drive calls {len(drives)}; speed {sp.min():.1f}..{sp.max():.1f}')
            big = np.nonzero((ang > a.jump_deg) | (np.abs(dsp) > a.speed_jump))[0]
            print(f'  one-step jumps (> {a.jump_deg} deg or > {a.speed_jump} speed): {len(big)}')
            for k in big[:60]:
                print(f'    t={td[k+1]:8.3f} heading {ang[k]:6.1f} deg, speed {sp[k]:6.1f} -> {sp[k+1]:6.1f}, '
                      f'gainFwd={rec["f"][drives[k+1]][21]:.0f} timer={rec["f"][drives[k+1]][22]:.2f} drifting={int(rec["f"][drives[k+1]][23])}')
            if a.csv:
                import csv
                with open(a.csv, 'w', newline='') as fh:
                    w = csv.writer(fh); w.writerow(['t', 'speed', 'heading_step_deg', 'gainFwd', 'timer', 'drifting'])
                    for k in range(1, len(drives)):
                        ff = rec['f'][drives[k]]
                        w.writerow([round(td[k], 4), round(sp[k], 2), round(ang[k-1], 2), ff[21], ff[22], int(ff[23])])

if __name__ == '__main__':
    main()
