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
  - angles: within -180..180 (-360..360 where the game ships a wider one, like turret targeting cones);
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
# laser and ion damage against capital ships: multipliers on material-grid cells (the DLL's grid settings, stock 1)
GRID = ["materialgrid"]
# match rules: Fleet Battles' and Dogfight's IncomSchematicAttributesConfig (morale per kill, phase sizes, timers),
# their values in registration lists named by mod/schematic_attrs.h
MATCH = ["incomschematicattributesconfig"]
SCHEMATIC_H = os.path.join(PROJECT, "mod", "schematic_attrs.h")
# capital ships and their turrets (each its own category; the same list is in sws_probe.cpp and app/Core.cs)
CAPITAL = ["capitalshiphealthsettings", "damagesectionsettings", "capitalshipsubsystemsettings",
           "capitalshipweakpointssettings", "capitalshipweaponsubsystemsettings", "capitalshipenginewashdamageareasettings",
           "damageovertimesettings", "shieldhealthsettings", "capitalshipresupplysubsystemsettings",
           "turrethealthsettings", "turretaimingsettings", "turrettargetcomputersettings",
           "turretfirepatternsettingsasset"]
CATEGORIES = ["starshipmovementconfig", "starshipshielddata", "starshiphealthdamagedata", "starshippowermanagement",
              "starshipweaponconfig", "bulletconfig", "missileconfig", "explosionconfig"] + COMPONENTS + CAPITAL + GRID + MATCH
# Capital ships and turrets have their own movement, weapon, bullet, missile and explosion configs, of the same types
# as the starfighters'. Pooled together, a Star Destroyer's turn torque would set the X-wing's ceiling, so those configs
# are limited on their own: <type>_capital.<field>, and listed under [capital] for the DLL and the app.
FIGHTERS = ["xwing", "ywing", "awing", "uwing", "bwing", "tiefighter", "tiebomber", "tieinterceptor", "tiereaper",
            "tiedefender"]
CAPITAL_SHIPS = ["stardestroyer", "mc75", "corvette", "frigate", "raider", "lightcruiser", "starhawk", "quasar",
                 "gozanti", "gr75", "igv55", "zeta", "bff"]   # the same list as the app's Catalog.CapitalShips
SPEED_CAP = 300.0
SPEED_FIELDS = {"starshipmovementconfig.maxspeeds", "starshipmovementconfig.afterburnermaxspeed",
                "starshipmovementconfig.minspeed"}
ANGLE = re.compile(r"angle(attribute)?$|anglethreshold$|anglezone$")
FRACTION = re.compile(r"(ratio|threshold)(attribute)?$|percent|normalized|healthratio")
INPUT_DEVICE = re.compile(r"gamepad|joystick|mouse")   # anywhere in the field path
POWER_LEVEL = re.compile(r"powerlevel$|powerchargelevel$|regenlevel$|powerfloor$")


def category(asset, keys=()):
    """the asset's limit category: its type, with weapons split into primaries and aux (Pseudo, PR #13: "Do lasers
    separately from aux weapons"). A primary (lasers, ion cannons, burst, autocannon, charge cannon) is a weapon config
    with laser regen pips; no aux weapon has them."""
    c = next((c for c in CATEGORIES if asset.startswith(c)), None)
    if c in COMPONENTS:
        return "starshipcustomconfig"
    if c and c not in CAPITAL and is_capital(asset):
        return c + "_capital"
    if c == "starshipweaponconfig":
        c += "_primary" if any(k.startswith("weaponfiring.laserregenpips") for k in keys) else "_aux"
    return c


def is_capital(asset):
    """a capital ship's or a capital/station turret's config (not a starfighter's, not a laser mine's turret)"""
    parts = asset.split("_")
    if any(p.startswith(f) for p in parts for f in FIGHTERS) or "lasermine" in parts:
        return False
    return any(p.startswith(c) for p in parts for c in CAPITAL_SHIPS) or any(p.startswith("turret") for p in parts)


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
    pools, primaries, capital = {}, [], []
    for asset, fields in sections.items():
        c = category(asset, fields)
        if not c:
            continue
        if c == "starshipweaponconfig_primary":
            primaries.append(asset)
        if c.endswith("_capital"):
            capital.append(asset)
        for path, vals in fields.items():
            pools.setdefault(f"{c}.{path}", []).extend(vals)
    return pools, sorted(primaries), sorted(capital)


def int_fields():
    """field names the type table marks as integers (kind 4, IncomAttributeInt; 11 and 12, yes/no)"""
    names = set()
    for m in re.finditer(r'\{"([a-z0-9]+)", 0x[0-9a-f]+, (?:4|11|12), ', open(TYPES_H, encoding="utf-8").read()):
        names.add(m.group(1))
    return names | schematic_names(1) | schematic_names(2)


def schematic_names(kind):
    """match-rule attribute names registered as integers (kind 1) or yes/no (kind 2)"""
    return set(re.findall(r'\{ \d+u, "([a-z0-9_]+)", "[^"]*", ' + str(kind) + r' \}', open(SCHEMATIC_H, encoding="utf-8").read()))


def bool_fields():
    """field names the type table marks as yes/no (kinds 11 and 12)"""
    return set(re.findall(r'\{"([a-z0-9]+)", 0x[0-9a-f]+, (?:11|12), ', open(TYPES_H, encoding="utf-8").read())) | schematic_names(2)


def limit(key, vals, is_int):
    # -1 is ammo's "unlimited" marker; on a component it is a real -100%, so components pool it like any value
    sentinel = -1.0 in vals and not key.startswith("starshipcustomconfig.")
    v = ([x for x in vals if x != -1.0] if sentinel else list(vals)) or [0.0]
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
    elif key.startswith("materialgrid."):
        # Pseudo, 2026-10-22: starfighter lasers should be able to do trivial damage to capital ships, so the floor is 0
        # (no damage at all); the ceiling is the usual 5x
        lo, src[1] = 0.0, "0 (no damage)"
    elif key in SPEED_FIELDS and hi > SPEED_CAP and mx <= SPEED_CAP:
        hi = SPEED_CAP; src[0] = "speed cap 300 (measured)"
    elif ANGLE.search(name):
        # 180 (360 where the game itself ships a wider one: turret targeting cones), and the same below zero
        cap = 180.0 if mx <= 180 else 360.0
        if hi > cap:
            hi = cap; src[0] = f"angle, {cap:g}"
        if lo < -cap:
            lo = -cap; src[1] = f"angle, -{cap:g}"
    elif FRACTION.search(name) and not ANGLE.search(name) and hi > 1 and mx <= 1:
        hi = 1.0; src[0] = "fraction, 1"
    if is_int:
        lo, hi = math.ceil(lo - 1e-9), math.floor(hi + 1e-9)
        if 0 < mn and lo < 1:
            lo = 1
    locked = None
    if INPUT_DEVICE.search(key):
        locked = "input device: locked (Pseudo, 2026-10-21: changing how people's hardware drives the game would change the feel for no benefit)"
    elif POWER_LEVEL.search(name):
        locked = "power level: locked (Pseudo, PR #13: power management is too core to change)"
    elif lo == hi == 0:
        locked = "every config ships 0: locked (Pseudo, PR #13: probably invariant for a reason)"
    return lo, hi, sentinel, locked or "; ".join(src), mn, mx, bool(locked)


def main():
    pools, primaries, capital = read_defaults()
    ints = int_fields()
    bools = bool_fields()
    lines = ["; Limits table for issue #11, generated by tools/gen_limits.py from defaults.ini. Do not edit by hand.",
             "; <category>.<field path, no indices> = min,max,flags",
             ";   flags: i = integer, n = -1 (unlimited) also allowed, x = locked (never changed)",
             "; Sources follow each line. Rule: up to 5x the highest stock value, floor 0.1x the lowest (0 where the",
             "; game ships 0), then the speed cap (300), fractions (1) and angles (180). Locked: power levels, and",
             "; fields every config ships as 0. Weapons are pooled as primaries and aux separately ([primary] lists",
             "; the primary weapon configs; every other weapon config is aux). Capital ships' and turrets' configs of",
             "; the starfighter types are pooled on their own as <type>_capital ([capital] lists them).",
             "[limits]"]
    locked = 0
    for key in sorted(pools):
        name = key.rsplit(".", 1)[-1]
        lo, hi, sentinel, src, mn, mx, lk = limit(key, pools[key], name in ints)
        if name in bools:   # a yes/no switch: 0 or 1, whatever the stock configs ship
            lo, hi, sentinel, src, lk = 0, 1, False, "yes/no: 0 = no, 1 = yes", False
        flags = ("i" if name in ints else "") + ("n" if sentinel else "") + ("x" if lk else "")
        locked += lk
        lines.append(f"{key}={lo:.9g},{hi:.9g},{flags}   ; {src}; stock {mn:.9g}..{mx:.9g} over {len(pools[key])}")
    # ship size (#20): one uniform scale per starfighter (shipscale_<ship>.scale, stock 1), applied as the blueprint loads.
    # 0.5 is the smallest size flown and measured (2026-10-23: model and hitbox at half size, clean); 1.5 the matching
    # step up. Wider is untried: big ships may meet spawn points, hangars and capital ships in ways not yet seen.
    lines.append("shipscale.scale=0.5,1.5,   ; ship size: 0.5 measured in flight, 1.5 the same step up (#20); stock 1")
    lines += ["", "[primary]"] + [f"{a}=1" for a in primaries]
    lines += ["", "[capital]"] + [f"{a}=1" for a in capital]
    open(OUT, "w", encoding="utf-8", newline="\r\n").write("\n".join(lines) + "\n")
    print(f"{len(pools)} fields -> {OUT} ({locked} locked; {len(primaries)} primary weapon configs; "
          f"{len(capital)} capital-ship configs of starfighter types)")


if __name__ == "__main__":
    main()
