"""Summarize the observation records (type 9) in a flight record: what each ship system actually did.

Usage: .venv/Scripts/python.exe -I tools/observe_report.py <server_*.bin> [--all]

For every client component the sampler saw (movement, health, power, weapon sub-outputs), prints the sample count,
time span and min / max of each field. Without --all, only components whose values changed during the session are
shown (ships that just sat there are summarized in one line). Layouts: StarshipMovement/Health/PowerManagement
ComponentOutputData, LaserRegen/LaserCharge/StarshipAmmo OutputData (the game's runtime type info).
"""
import struct
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else ".")
from probe_read import load  # noqa: E402

# kind -> list of (word index from the record's base, name, type 'f' float / 'i' int / 'b' first byte)
LAYOUT = {
    1: [(0, "AfterburnerCharge", "f"), (1, "CurrentSpeed", "f"), (2, "ThrottleSpeed", "f"), (3, "NormalizedSpeed", "f"),
        (6, "MaxPipMaxSpeed", "f"), (7, "AfterburnerMaxSpeed", "f"), (8, "TargetSpeed", "f"), (11, "CurrentMaxSpeed", "f"),
        (18, "NormalizedAngularSpeed", "f"), (19, "PitchSpeed", "f"), (20, "RollSpeed", "f"), (21, "YawSpeed", "f"),
        (22, "DriftMomentumStrength", "f")],
    2: [(4, "CurrentMaxHullHealth", "f"), (5, "CurrentHealthNormalized", "f"), (6, "CurrentHealth", "f"),
        (7, "CurrentFrontShieldHealth", "f"), (8, "CurrentBackShieldHealth", "f"), (9, "CurrentMaxShieldHealth", "f"),
        (10, "CurrentMaxShieldBuffHealth", "f"), (11, "CurrentShieldBuffHealth", "f"), (14, "SecondLayerShieldPercentThreshold", "f")],
    3: [(4, "CurrentEnginePowerLevel", "f"), (6, "CurrentWeaponPowerLevel", "f"), (8, "CurrentShieldPowerLevel", "f")],
    4: [(0, "CurrentEnergyNormalized", "f"), (1, "SuperChargedEnergyThreshold", "f"), (2, "IsSuperCharged", "b")],
    5: [(0, "ChargeNormalized", "f"), (1, "SuperChargeChargeNormalized", "f"), (2, "IsCharging", "b")],
    6: [(0, "MaxAmmo", "i"), (1, "CurrentAmmo", "i"), (2, "CooldownTime", "f"), (3, "MaxCooldownTime", "f"),
        (5, "ActiveTime", "f"), (6, "MaxActiveTime", "f"), (8, "PreFireDelayTime", "f"), (9, "MaxPreFireDelayTime", "f")],
    7: [(1, "CurrentBarrelIndex", "i"), (3, "IsWeaponActive", "b")],
}
KIND = {1: "movement", 2: "health", 3: "power", 4: "laser regen", 5: "laser charge", 6: "ammo", 7: "weapon"}


def decode(rows, word, typ):
    raw = rows["f"][:, 1 + word].copy().view(np.uint32)
    if typ == "f":
        return raw.view(np.float32).astype(np.float64)
    if typ == "i":
        return raw.view(np.int32).astype(np.float64)
    return (raw & 0xFF).astype(np.float64)


def main():
    path = sys.argv[1]
    show_all = "--all" in sys.argv
    a, freq, t0 = load(path)
    obs = a[a["type"] == 9]
    print(f"{path}: {len(obs)} observation samples")
    if not len(obs):
        return
    kinds = obs["f"][:, 0].astype(int)
    quiet = 0
    for comp in np.unique(obs["state"]):
        for k in sorted(set(kinds[obs["state"] == comp])):
            rows = obs[(obs["state"] == comp) & (kinds == k)]
            span = (rows["qpc"].max() - rows["qpc"].min()) / freq
            fields = []
            changed = False
            for word, name, typ in LAYOUT[k]:
                v = decode(rows, word, typ)
                v = v[np.isfinite(v)]
                if not len(v):
                    continue
                lo, hi = v.min(), v.max()
                changed |= lo != hi
                fields.append(f"{name} {lo:.4g}" + ("" if lo == hi else f"..{hi:.4g}"))
            if not changed and not show_all:
                quiet += 1
                continue
            print(f"\n{KIND[k]} {int(comp):#x}: {len(rows)} samples over {span:.1f} s")
            for f in fields:
                print("   " + f)
    if quiet:
        print(f"\n({quiet} component outputs never changed; --all shows them)")


if __name__ == "__main__":
    main()
