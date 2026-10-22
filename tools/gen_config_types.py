"""Generate mod/config_types.h: the type table the runtime value engine (issues #3, #6) walks.

From notes/sws_runtime_types.json (the game's runtime type info, decoded from the exe by our own tools) and the exe
copy in work/ (each class's runtime ClassInfo: the one .rdata object whose first qword is the type description).

The engine walks each root config's object graph and gives every number a dotted key, for example
  starshipweaponconfig_las_reb_xwing.weaponfiring.laserregenpips[3].costpershot
It follows only the ship's own sub-objects: classes in FOLLOW (and their subclasses), attribute objects, and inline
value types. Plain Int32 fields are identifiers or selectors and are left out. Shared things (sounds, blueprints, curve assets, UI) are left out. Fields of the config base classes (the
attribute registration lists, identifiers) are not exposed; the engine keeps the lists in step itself.

Field kinds (must match sws_probe.cpp):
  0 F32  1 I32  2 VEC3 (inline x,y,z)  3 ATTR_F  4 ATTR_I  5 ATTR_V3 (pointer to an IncomAttribute*, Value at +32)
  6 SUB (pointer to an object; the engine resolves its actual class)  7 INLINE (value type)  8 ARR_F32
  9 ARR_INLINE (array of value types)  11 BOOL (a byte)  12 ATTR_B (pointer to an IncomAttributeBool, Value byte at +32)
  10 ARR_MOD (array of pointers to attribute modifiers: a component's
  AttributeModifier list; each Float modifier is a leaf named by its attribute, Value at +0x30)

Usage: .venv/Scripts/python.exe -I tools/gen_config_types.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pemap import PE  # noqa: E402

PROJECT = os.path.dirname(HERE)
TYPES = os.path.join(PROJECT, "notes", "sws_runtime_types.json")
EXE = os.path.join(PROJECT, "work", "starwarssquadrons.exe")
OUT = os.path.join(PROJECT, "mod", "config_types.h")

ROOTS = ["StarshipMovementConfig", "StarshipShieldData", "StarshipHealthDamageData", "IncomBulletConfig",
         "StarshipWeaponConfig", "StarshipPowerManagement", "IncomMissileConfig", "IncomExplosionData",
         "StarshipEngineCustomConfig", "StarshipHullCustomConfig", "StarshipShieldCustomConfig",
         # capital ships: health, damage sections, subsystems, weak points, shields, engine wash, damage over time
         "CapitalShipHealthSettings", "DamageSectionSettings", "CapitalShipSubsystemSettings",
         "CapitalShipWeakPointsSettings", "CapitalShipWeaponSubsystemSettings", "CapitalShipEngineWashDamageAreaSettings",
         "DamageOverTimeSettings", "ShieldHealthSettings", "CapitalShipResupplySubsystemSettings",
         # turrets: health, aiming, targeting computers, fire patterns. The last two are plain policy assets with no
         # attribute registration lists; their fields are plain floats, which the engine writes without list sync.
         "TurretHealthSettings", "TurretAimingSettings", "TurretTargetComputerSettings", "TurretFirePatternSettings"]
# The component configs' parent (StarshipCustomConfig, which holds the AttributeModifier list) is missing from the
# decoded type records; the runtime types give all three the same 120-byte StarshipCustomConfig layout.
SUPER_FIX = {"StarshipEngineCustomConfig": "StarshipCustomConfig", "StarshipHullCustomConfig": "StarshipCustomConfig",
             "StarshipShieldCustomConfig": "StarshipCustomConfig"}
FOLLOW = ["StarshipWeaponFiringData", "StarshipPowerData"]          # sub-object classes followed (with subclasses)
STOP_SUPERS = {"DataContainer", "GameDataContainer", "StarshipBaseConfig", "BaseConfig", "DataContainerPolicyAsset", "Asset"}
ATTR = {"IncomAttributeFloat": 3, "IncomAttributeInt": 4, "IncomAttributeVec3": 5}
# Yes/no fields are opt-in, by the class that declares them: many are technical (barrel transforms, feedback, input),
# so each one is chosen (Pseudo, 2026-10-22: passive damage's two switches first).
BOOL_ALLOW = {
    "DamageOverTimeSettings": {"DamageStarshipTargetsInOrderAttribute", "SplitDamageBetweenStarshipsAttribute"},
}


def main():
    recs = json.load(open(TYPES, encoding="utf-8"))
    by = {r["name"]: r for r in recs if isinstance(r, dict) and "name" in r}
    pe = PE(EXE)
    data = bytes(pe.m)

    def class_info(name):
        needle = int(by[name]["va"], 16).to_bytes(8, "little")
        hits, i = [], data.find(needle)
        while i != -1:
            for sname, sva, vsz, rptr, rsz in pe.sections:
                if rptr <= i < rptr + rsz and sname == ".rdata":
                    hits.append(pe.base + sva + i - rptr)
            i = data.find(needle, i + 1)
        if len(hits) != 1:
            raise SystemExit(f"{name}: expected one ClassInfo, found {[hex(h) for h in hits]}")
        return hits[0]

    def supers(name):
        out, n = [], by[name].get("super") or SUPER_FIX.get(name)
        while n and n in by:
            out.append(n)
            n = by[n].get("super")
        return out

    def is_followed(name):
        return name in FOLLOW or any(s in FOLLOW for s in supers(name))

    followed = sorted(n for n, r in by.items() if r.get("kind") == "Class" and is_followed(n))

    def all_fields(name):
        """own fields plus inherited ones, stopping at the config/base classes"""
        chain = [name] + [s for s in supers(name) if s not in STOP_SUPERS]
        cut = next((i for i, s in enumerate([name] + supers(name)) if s in STOP_SUPERS), None)
        if cut is not None:
            chain = ([name] + supers(name))[:cut]
        fl = []
        for c in chain:
            fl.extend(dict(x, _owner=c) for x in (by[c].get("fields") or []))
        return sorted(fl, key=lambda f: f["offset"])

    types, index = [], {}

    def add_type(name):
        if name in index:
            return index[name]
        index[name] = len(types)
        rec = by[name]
        t = {"name": name, "kind": rec.get("kind"), "size": rec.get("size") or 0,
             "ci": class_info(name) if rec.get("kind") == "Class" else 0, "fields": []}
        types.append(t)
        for f in all_fields(name):
            ft = f["type"] or ""
            base = ft[:-6] if ft.endswith("-Array") else ft
            arr = ft.endswith("-Array")
            sub = by.get(base, {})
            nm = f["name"].lower()
            if ft == "Float32":
                t["fields"].append((nm, f["offset"], 0, -1))
            elif ft == "Int32":
                # Plain Int32 fields in these configs are identifiers and selectors (input actions, power-transfer
                # combos, AI weapon type, lock type), not amounts; the integer amounts are IncomAttributeInts.
                # Changing one would break controls or behavior, not rebalance it, so they are not exposed.
                continue
            elif ft == "Vec3":
                t["fields"].append((nm, f["offset"], 2, -1))
            elif ft in ATTR:
                t["fields"].append((nm, f["offset"], ATTR[ft], -1))
            elif ft in ("Boolean", "IncomAttributeBool"):
                if f["name"] in BOOL_ALLOW.get(f.get("_owner", ""), set()):
                    t["fields"].append((nm, f["offset"], 11 if ft == "Boolean" else 12, -1))
            elif ft == "Float32-Array":
                t["fields"].append((nm, f["offset"], 8, -1))
            elif ft == "IncomAttributeModifier-Array":
                t["fields"].append((nm, f["offset"], 10, add_type("IncomAttributeModifierFloat")))
            elif sub.get("kind") == "Class" and not arr and is_followed(base):
                t["fields"].append((nm, f["offset"], 6, add_type(base)))
            elif sub.get("kind") == "ValueType" and base not in ("Vec3",) and sub.get("fields"):
                ti = add_type(base)
                if types[ti]["fields"]:
                    t["fields"].append((nm, f["offset"], 9 if arr else 7, ti))
        return index[name]

    roots = [add_type(r) for r in ROOTS]
    for n in followed:                     # every subclass of a followed class, so the engine can resolve them
        add_type(n)

    L = ["// Generated by tools/gen_config_types.py from the game's runtime type info. Do not edit by hand.",
         "// Field kinds: 0 F32, 1 I32, 2 VEC3, 3 ATTR_F, 4 ATTR_I, 5 ATTR_V3, 6 SUB, 7 INLINE, 8 ARR_F32, 9 ARR_INLINE,",
         "//              10 ARR_MOD (type = IncomAttributeModifierFloat, for its ClassInfo), 11 BOOL, 12 ATTR_B",
         "#pragma once",
         "struct VeType { const char* name; uint64_t classInfo; uint32_t size; int firstField; int fieldCount; int followRoot; };",
         "struct VeField { const char* name; uint32_t offset; int kind; int type; };",
         "static const VeField kVeFields[] = {"]
    first = []
    for t in types:
        first.append(sum(len(x["fields"]) for x in types[:types.index(t)]))
        for nm, off, kind, ti in t["fields"]:
            L.append(f'    {{"{nm}", 0x{off:x}, {kind}, {ti}}},   // {t["name"]}')
    L.append("};")
    # followRoot: for followed classes, the index of the FOLLOW base they descend from (for subclass resolution)
    L.append("static const VeType kVeTypes[] = {")
    for i, t in enumerate(types):
        fr = -1
        if t["kind"] == "Class" and t["name"] not in ROOTS and is_followed(t["name"]):
            base = t["name"] if t["name"] in FOLLOW else next(s for s in supers(t["name"]) if s in FOLLOW)
            fr = index[base]
        L.append(f'    {{"{t["name"]}", 0x{t["ci"]:x}ull, {t["size"]}, {first[i]}, {len(t["fields"])}, {fr}}},   // {i}')
    L.append("};")
    L.append("static const int kVeRoots[] = { " + ", ".join(str(r) for r in roots) + " };")
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(L) + "\n")
    print(f"{len(types)} types ({len(roots)} roots, {len(followed)} followed classes), "
          f"{sum(len(t['fields']) for t in types)} fields -> {OUT}")


if __name__ == "__main__":
    main()
