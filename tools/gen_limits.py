"""Generate the limits table (issue #11): the range the DLL clamps every changeable value to.

Reads mod/build/defaults.ini (written by the DLL: the stock value of every changeable field of every config the game
loaded) and mod/config_types.h (to know which fields are integers), and writes mod/build/limits.ini, which the DLL reads
at startup and the settings app will use to constrain inputs. The file is derived from game data, so it is generated
locally and shipped in releases, not committed.

A field is keyed by its config type (the asset name's prefix) and its path without array indices, e.g.
  starshipmovementconfig.maxspeeds        starshipweaponconfig.weaponfiring.laserregenpips.costpershot
because a field means the same thing on every ship. Its stock values are pooled across every config the game loads, AI
and creep included: all of them are values the game itself runs.

The rule (Pseudo, 2026-10-21): up to 5x the highest stock value; a floor just above zero.
  - max = 5 x the highest stock value. The game's own custom-match modifiers allow up to 5x on the attributes they cover
    (incomcustommatchconfigdata), so 5x is a multiplier the engine is built to take.
  - min = 0.1 x the lowest stock value (the custom-match floor). Where the game itself ships a 0, 0 is allowed.
  - Negative fields (torques, decay rates) get the same rule mirrored; a field with both signs keeps 5x on each side.
  - A -1 stock value is the "unlimited" marker (ammo): -1 stays allowed, as itself.
Then the bounds that come from the engine or from what a field means, each applied only where the stock values agree:
  - speed: actual speed caps at 300.00 (measured in flight), so maxspeeds, afterburnermaxspeed and minspeed max 300;
  - fractions (ratio, percent, normalized, non-angle thresholds): max 1;
  - angles: max 180;
  - power levels: max 4 (the per-level tables, maxspeeds and shield regen by level, have 9 entries: levels 0..4).
Integer fields are rounded inward, with a positive floor of at least 1.

Usage: .venv/Scripts/python.exe -I tools/gen_limits.py
"""
import math
import os
import re

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(PROJECT, "mod", "build")
DEFAULTS = os.path.join(BUILD, "defaults.ini")
TYPES_H = os.path.join(PROJECT, "mod", "config_types.h")
OUT = os.path.join(BUILD, "limits.ini")

COMPONENTS = ["enginecustom", "hullcustom", "shieldcustom"]   # engine / hull / shield components: one category
CATEGORIES = ["starshipmovementconfig", "starshipshielddata", "starshiphealthdamagedata", "starshippowermanagement",
              "starshipweaponconfig", "bulletconfig", "missileconfig", "explosionconfig"] + COMPONENTS
SPEED_CAP = 300.0
SPEED_FIELDS = {"starshipmovementconfig.maxspeeds", "starshipmovementconfig.afterburnermaxspeed",
                "starshipmovementconfig.minspeed"}
ANGLE = re.compile(r"angle(attribute)?$|anglethreshold$|anglezone$")
FRACTION = re.compile(r"(ratio|threshold)(attribute)?$|percent|normalized")
POWER_LEVEL = re.compile(r"powerlevel$|powerchargelevel$|regenlevel$|powerfloor$")


def category(asset, keys=()):
    """the asset's limit category: its type, with weapons split into primaries and aux (Pseudo, PR #13: "Do lasers
    separately from aux weapons"). A primary (lasers, ion cannons, burst, autocannon, charge cannon) is a weapon config
    with laser regen pips; no aux weapon has them."""
    c = next((c for c in CATEGORIES if asset.startswith(c)), None)
    if c in COMPONENTS:
        return "starshipcustomconfig"
    if c == "starshipweaponconfig":
        c += "_primary" if any(k.startswith("weaponfiring.laserregenpips") for k in keys) else "_aux"
    return c


def read_defaults():
    sections, cur = {}, None
    for line in open(DEFAULTS, encoding="utf-8"):
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            cur = sections.setdefault(line[1:-1], {})
        elif cur is not None and "=" in line and not line.startswith(";"):
            k, v = line.split("=", 1)
            try:
                cur[re.sub(r"\[\d+\]", "", k.strip())] = cur.get(re.sub(r"\[\d+\]", "", k.strip()), []) + [float(v)]
            except ValueError:
                pass
    pools, primaries = {}, []
    for asset, fields in sections.items():
        c = category(asset, fields)
        if not c:
            continue
        if c == "starshipweaponconfig_primary":
            primaries.append(asset)
        for path, vals in fields.items():
            pools.setdefault(f"{c}.{path}", []).extend(vals)
    return pools, sorted(primaries)


def int_fields():
    """field names the type table marks as integers (kind 4, IncomAttributeInt)"""
    names = set()
    for m in re.finditer(r'\{"([a-z0-9]+)", 0x[0-9a-f]+, 4, ', open(TYPES_H, encoding="utf-8").read()):
        names.add(m.group(1))
    return names


def limit(key, vals, is_int):
    sentinel = -1.0 in vals
    v = [x for x in vals if x != -1.0] or [0.0]
    mn, mx = min(v), max(v)
    src = []
    hi = 5 * mx if mx > 0 else (0.0 if mx == 0 else 0.1 * mx)
    lo = 0.1 * mn if mn > 0 else (0.0 if mn == 0 else 5 * mn)
    src.append("5x highest stock" if mx > 0 else ("stock max 0" if mx == 0 else "0.1x nearest-zero stock"))
    src.append("0.1x lowest stock" if mn > 0 else ("stock has 0" if mn == 0 else "5x most negative stock"))
    name = key.rsplit(".", 1)[-1]
    if key.startswith("starshipcustomconfig."):
        # components are percentage modifiers (0.3 = +30%). Pseudo, 2026-10-21: max 5x the highest stock percentage,
        # min -0.9, so the result never drops below 10% of the base; where the game ships lower (-1), that is allowed.
        hi, lo = max(0.0, 5 * mx), min(-0.9, mn)
        src = ["5x highest stock %" if mx > 0 else "0 (stock never raises it)", "-0.9 (10% of base)" if mn >= -0.9 else "lowest stock %"]
    elif key in SPEED_FIELDS and hi > SPEED_CAP and mx <= SPEED_CAP:
        hi = SPEED_CAP; src[0] = "speed cap 300 (measured)"
    elif ANGLE.search(name) and hi > 180 and mx <= 180:
        hi = 180.0; src[0] = "angle, 180"
    elif FRACTION.search(name) and not ANGLE.search(name) and hi > 1 and mx <= 1:
        hi = 1.0; src[0] = "fraction, 1"
    if is_int:
        lo, hi = math.ceil(lo - 1e-9), math.floor(hi + 1e-9)
        if 0 < mn and lo < 1:
            lo = 1
    locked = None
    if POWER_LEVEL.search(name):
        locked = "power level: locked (Pseudo, PR #13: power management is too core to change)"
    elif lo == hi == 0:
        locked = "every config ships 0: locked (Pseudo, PR #13: probably invariant for a reason)"
    return lo, hi, sentinel, locked or "; ".join(src), mn, mx, bool(locked)


def main():
    pools, primaries = read_defaults()
    ints = int_fields()
    lines = ["; Limits table for issue #11, generated by tools/gen_limits.py from defaults.ini. Do not edit by hand.",
             "; <category>.<field path, no indices> = min,max,flags",
             ";   flags: i = integer, n = -1 (unlimited) also allowed, x = locked (never changed)",
             "; Sources follow each line. Rule: up to 5x the highest stock value, floor 0.1x the lowest (0 where the",
             "; game ships 0), then the speed cap (300), fractions (1) and angles (180). Locked: power levels, and",
             "; fields every config ships as 0. Weapons are pooled as primaries and aux separately ([primary] lists",
             "; the primary weapon configs; every other weapon config is aux).",
             "[limits]"]
    locked = 0
    for key in sorted(pools):
        name = key.rsplit(".", 1)[-1]
        lo, hi, sentinel, src, mn, mx, lk = limit(key, pools[key], name in ints)
        flags = ("i" if name in ints else "") + ("n" if sentinel else "") + ("x" if lk else "")
        locked += lk
        lines.append(f"{key}={lo:.9g},{hi:.9g},{flags}   ; {src}; stock {mn:.9g}..{mx:.9g} over {len(pools[key])}")
    lines += ["", "[primary]"] + [f"{a}=1" for a in primaries]
    open(OUT, "w", encoding="utf-8", newline="\r\n").write("\n".join(lines) + "\n")
    print(f"{len(pools)} fields -> {OUT} ({locked} locked; {len(primaries)} primary weapon configs)")


if __name__ == "__main__":
    main()
