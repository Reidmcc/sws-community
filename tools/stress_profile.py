"""Write stress profiles for issue #11: one ship's values in one system pushed past stock, to find the engine's limits.

Reads mod/build/defaults.ini (written by the DLL at every launch: the stock value of every changeable field of every
config the game loaded) and writes mod/build/profiles/Stress_<Ship>_<System>.ini, one per system, with every positive
stock value of that ship's own configs multiplied by the factor. Fly one match per profile with observation on; the
recording (tools/observe_report.py) shows which values the game followed and where it stopped. Values move in their
strong direction: amounts are multiplied by the factor; intervals (time between shots, cooldowns, recharge delays) are
divided by it.

  movement  starshipmovementconfig_* except turning, starshippowermanagement_* (power is mostly levels, left out
            below, so the conversion rates that remain ride with movement)
  turning   starshipmovementconfig_* turn rates, torques, steering and rotation limits only. Flown apart from movement
            because the turn-rate curves take speed as a fraction of max speed: raising max speeds in the same flight
            moves where the ship sits on the curve, which confounds the turn rates.
  defense   starshipshielddata_*, starshiphealthdamagedata_*
  weapons   starshipweaponconfig_*, bulletconfig_*, missileconfig*, explosionconfig*

A ship's own configs are those whose name has the ship as one of its _-separated parts, without an ai / bot / pvb / sp
part (those are the AI and single-player variants). Negative stock values (-1 means unlimited) and zeros are left alone,
and so are four kinds of field whose limits don't come from the engine, or which would spoil the flight (SKIP below):
input tuning, power levels, fractions / thresholds / angles, and multipliers on another field (X and Xfactor or Xmultiplier).

Usage: .venv/Scripts/python.exe -I tools/stress_profile.py <ship> [--factor 3] [--systems movement,turning,defense,weapons]
"""
import argparse
import os
import re

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(PROJECT, "mod", "build")
SYSTEMS = {
    "movement": ("starshipmovementconfig_", "starshippowermanagement_"),
    "turning": ("starshipmovementconfig_",),
    "defense": ("starshipshielddata_", "starshiphealthdamagedata_"),
    "weapons": ("starshipweaponconfig_", "bulletconfig_", "missileconfig", "explosionconfig"),
}
VARIANT_PARTS = {"ai", "bot", "pvb", "sp"}
TURNING = re.compile(r"turn|axis|torque|steer|rotation|roll|yaw|pitch|angular")   # on the key's first part
# (reason, pattern on the field's own name: the key's last dotted part, array indices removed; input configs by prefix)
SKIP = [
    ("input tuning: would change how the controls feel, not the ship",
     re.compile(r"gamepad|joystick|mouse|stickreleasedelay")),   # the same input-device fields gen_limits locks
    ("power level: its ceiling is the length of the per-level tables (maxspeeds, shield regen by level)",
     re.compile(r"powerlevel|powerchargelevel|regenlevel$|powerfloor")),
    ("fraction, threshold or angle: the limit comes from what it means (0..1, 0..180)",
     re.compile(r"(ratio|threshold|angle)(attribute)?$|percent|normalized")),
]
# Intervals: the strong direction is shorter, so these are divided by the factor rather than multiplied (tripling
# the time between laser shots made the X-wing fire a third as fast: weak and unplayable, and no limit in sight).
INVERT = re.compile(r"timebetween|cooldown|rechargedelay")
COMPOUND = "multiplier on another field of the config (X and Xfactor / Xmultiplier): raising both would compound"


def skip_reason(key, section):
    k = re.sub(r"\[\d+\]", "", key)
    name = k.rsplit(".", 1)[-1]
    for why, pat in SKIP:
        if pat.search(name) or (pat is SKIP[0][1] and pat.search(k)):
            return why
    for suffix in ("factor", "multiplier"):
        if name.endswith(suffix) and len(name) > len(suffix):
            base = name[:-len(suffix)]   # e.g. maxspeeds (maxspeeds[i]), costpershot (laserregenpips[i].costpershot)
            for o in section:
                on = re.sub(r"\[\d+\]", "", o).rsplit(".", 1)[-1]
                if o != key and (on == base or on.startswith(base)):
                    return COMPOUND
    return None


def read_defaults(path):
    sections, cur = {}, None
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = sections.setdefault(line[1:-1], {})
        elif cur is not None and "=" in line:
            k, v = line.split("=", 1)
            try:
                cur[k.strip()] = float(v)
            except ValueError:
                pass
    return sections


def own_config(name, ship):
    parts = name.split("_")
    return ship in parts and not (VARIANT_PARTS & set(parts))


def fmt(stock, factor, key=""):
    name = re.sub(r"\[\d+\]", "", key).rsplit(".", 1)[-1]
    v = stock / factor if INVERT.search(name) else stock * factor
    return f"{v:.6g}"   # never rounded: 20 / 3 must stay 6.66667 (whole-number stock x whole factor stays whole anyway)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ship", help="ship name as it appears in config names, e.g. xwing, tiefighter")
    ap.add_argument("--factor", type=float, default=3.0)
    ap.add_argument("--systems", default=",".join(SYSTEMS))
    ap.add_argument("--defaults", default=os.path.join(BUILD, "defaults.ini"))
    args = ap.parse_args()
    ship = args.ship.lower()
    sections = read_defaults(args.defaults)
    os.makedirs(os.path.join(BUILD, "profiles"), exist_ok=True)
    for system in args.systems.split(","):
        prefixes = SYSTEMS[system]
        configs = sorted(n for n in sections if n.startswith(prefixes) and own_config(n, ship))
        lines = [f"; Stress profile for issue #11: every positive stock value of the {ship}'s {system} configs x{args.factor:g}.",
                 f"; Generated by tools/stress_profile.py from defaults.ini. Fly one match, then: tools/observe_report.py.",
                 "[rules]", "no_zero_throttle_pinball=0", "no_multidrift=1", "", "[values]"]
        n, skipped = 0, {}
        for c in configs:
            for k, stock in sorted(sections[c].items()):
                if stock <= 0:
                    continue
                if c.startswith("starshipmovementconfig_") and system in ("movement", "turning"):
                    if bool(TURNING.search(k.split(".")[0])) != (system == "turning"):
                        continue
                why = skip_reason(k, sections[c])
                if why:
                    skipped.setdefault(why, []).append(f"{c}.{k}")
                    continue
                lines.append(f"{c}.{k}={fmt(stock, args.factor, k)}")
                n += 1
        out = os.path.join(BUILD, "profiles", f"Stress_{ship}_{system}.ini")
        open(out, "w", encoding="utf-8", newline="\r\n").write("\n".join(lines) + "\n")
        print(f"{system:9s} {len(configs):3d} configs, {n:4d} values -> {out}")
        for why, keys in skipped.items():
            print(f"            left out {len(keys):3d}: {why}")


if __name__ == "__main__":
    main()
