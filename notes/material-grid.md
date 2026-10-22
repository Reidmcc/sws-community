# Material grid: can it nerf lasers vs capital ships as pure data?

Research for Pseudo's goal: starfighter lasers do trivial damage to capital ships, torpedoes and bombs keep full damage.
Read-only study of `game/levels/rootlevel/rootlevel/materialgrid_win32` (dump: `out/materialgrid_win32.json`), the
projectile and capital-ship assets, the capital ships' Havok physics RES headers, and `work/starwarssquadrons.exe`.

Tags: **[V-data]** read from game data, **[V-code]** read from disassembly / Ghidra decompile, **[I]** inferred,
**[V-run]** measured in the game.

## Measured in the game, 2026-10-22 (after this study)

`material_log=1` logs each distinct (attacker, target, caller) pair the lookup is asked for. Pseudo flew Fleet Battles
with it on and shot each capital ship. **[V-run]**:

* **Starfighters are grid 12, not the default.** So the risk in the verdict below (a corvette fix shared with laser vs
  fighter shields) does not exist: fighters have their own cell.
* **Laser direct hits on capital ships use** (3,11) for hulls and turrets, (3,6) for capital shields (both effect
  variants), and (3,36) for destroyed sections and weak points.
* **The Raider's hull uses the default material** (3,0). It was the first ship Pseudo shot, and its hull function asked
  for (3,0). The CR90 is the same by its physics table; that hasn't been measured. (3,0) is also laser vs missiles in
  flight, which the missile's own health component asks for.
* **Torpedoes and missiles** reach the blast functions, which don't use the multiplier.

**What the mod does with this** (`sws_probe.cpp`, the material-lookup hook; profile keys
`materialgrid.{laser,ion}damage.capital{hull,shields,turrets}`):

* **It doesn't edit the grid.** When the server's capital hull (return address 0x1417db400), capital shield
  (0x140582ba0) or turret (0x1416edef6) direct-hit function asks for a laser or ion cell, it gets a private copy of the
  stock capital cell with the multiplier changed.
* **The client's shield figure** (ClientShieldHealthComponent slot 41, 0x140546f7e) reads the same multiplier, so it
  gets the same copy.
* **Corvette hulls (default material)** and SD/MC75 parts (grid 17) get the hull copy, so every capital hull takes the
  same setting. Missiles keep their cell.
* **Verified in flight:** the substitutions show in the log, Pseudo felt lasers at a tenth against hulls and shields,
  and returning to the menu afterwards was clean.

## Verdict

**Yes, partly, and it's a real server-side lever. It works per category (laser vs ion vs explosive), not per weapon.**

* The server's starship, capital-ship, turret, shield and weak-point health components look up
  `MaterialRelationDamageData` on every **direct (bullet/beam) hit**. The lookup uses (projectile material, hit-part material).
  They multiply the hit's damage by `DamageProtectionMultiplier` and drop the hit if it is under `DamageProtectionThreshold`.
  [V-code, `0x1417db200` and siblings]
* **Explosion damage ignores `DamageProtectionMultiplier`.** The blast path reads the same cell but applies the raw blast damage.
  It only passes `DamagePenetrationMultiplier` back to its caller. Torpedoes and bombs do all their damage by explosion:
  torpedo missile `Damage` = 0, explosion `BlastDamage` = 4000. [V-code `0x1417dbca0`/`0x1417dbb40`, V-data]
  Lowering the laser cell's protection multiplier therefore **cannot** touch torpedo or bomb damage. That's what Pseudo wants.
* Every primary laser shares **one** damage material: rebel and imperial lasers, autocannon, burst/rapid-fire, chaingun, SMG,
  charge cannon, "supercharged", B-wing and TIE Defender lasers, *and* the capital-ship turret lasers. That is property index Y=3,
  grid index **3**. Ion cannons are separate (**10**), and so are beams (**21**) [V-data]. So per-*category* is real. Per-*weapon*
  (say X-wing lasers vs TIE lasers) is **not** possible through the grid.

**What a mod would write** (runtime, in memory, on the host/server). The objects live in partition
`525adf3d-8176-07e1-6ee3-ede78578e278` (MaterialGridData). Field `DamageProtectionMultiplier` is Float32 at **+0x20** of each
`MaterialRelationDamageData` instance (type size 56). The code reads it on every hit, with no caching seen [V-code].

| Edit (object `$id` / instance GUID) | Grid cells (row = attacker, col = target; the damage side is symmetric) | Meaning | Side effects |
|---|---|---|---|
| obj 59 `55db1433-3aec-4450-a1f3-cf4ee56f605b` | (3,11) (11,3) **(3,27) (27,3)** | laser vs capital-ship hull / turret material g11 | g27 = weak-point decal (`prp_gen_weakpoint_decal_02`) gets the same nerf; capital-ship turret lasers vs other capital ships; level props using Y=11 (heist-station greebles, satellite turrets) |
| obj 199 `ecfaa4aa-fd0e-45a4-9c6c-4b17a1859de5` | (3,17) (17,3) | laser vs the extra SD/MC75 part material g17 (8 parts each; likely subsystems) | none found |
| obj 212 `ce4e0db8-e2e4-49d3-936a-de31821911a1` | (3,6) (6,3) (6,21) (21,6) | laser vs capital shield-bubble material g6 | beams (g21) vs capital shields too |
| obj 118 `44ec0363-0895-4150-900c-402ba5c42e4b` | (2,3) (3,2) (2,21) (21,2) (3,47) (47,3) (21,47) (47,21) | laser vs capital shield-bubble material g2 | beams vs shields; laser/beam vs piercing-torpedo body g47 |

A value like 0.05 makes lasers do 5% against capital hulls. Leave ion (`obj 234`, row 10) alone unless ions should be nerfed too.
obj 234 is shared by 16 cells, including the B-wing ion beam (g45).

**Caveats you must know before relying on it:**
1. **Which cell a laser hit on a capital ship actually uses is not fully pinned down [I].** The target material comes from the hit
   rigid body's per-part material, `body->vtbl[0x2d0](out, partIndex)` [V-code]. The RES material tables
   give SD/MC75/frigate/light cruiser/Gozanti/Quasar/turret hulls **g11**, but the corvette (CR90), Raider, IGV-55, Zeta and BFF-1
   tables only hold `Y1` and default (`0x80`, grid **g0**). If those ships' hulls resolve to g0, a laser hit uses cell (3,0)
   (obj 126). That cell is *also* laser vs **starfighter shields** (`StarshipShieldData.ShieldMaterial` is Y=0), so it cannot be
   nerfed without breaking dogfights. **Verify with a runtime probe before shipping:** hook `0x14057dd20` on the server and log
   both packed decls plus the return address during a Fleet Battle vs bots (details in §5).
2. Some layouts compare the threshold to the **scaled** damage: turret, shield, starfighter and weak point. With
   `DamageProtectionThreshold` = 2.0 and laser damage around 30–60 (`StartDamage` 60 / `EndDamage` 30 on the X-wing), any
   multiplier below about 0.067 makes long-range laser hits do **zero** to turrets and shields. The capital hull compares the
   threshold to **raw** damage, so the hull always takes the scaled damage. Raise or lower `DamageProtectionThreshold` (+0x28)
   in the same object if that matters.
3. Editing an object in place changes every cell that points at it (table above). For an isolated change, allocate a copy and
   repoint `InteractionGrid[3].Items[11].PhysicsPropertyProperties[0]` and `[11].Items[3]...[0]`. That takes more code.
4. It's a server-authoritative path: every function found is a `Server*` component. Client components call the same lookup
   for prediction and effects, so client-side hit feedback may disagree. `SyncedGameSettings.AllowClientSideDamageArbitration`
   exists (see spike-log) and was not studied here.

## 1. MaterialDecl.Packed → grid index

**Decoding [V-code + V-data]:**

```
Packed bit 7 (0x80)    "set" flag; if clear, the engine uses MaterialGridManager.defaultMaterial
bits 0..6              other flags (0x09 seen on capital shield bubbles, piercing torpedo, one SP hangar; meaning unknown)
bits 8..19   X         physics MATERIAL index  -> effects/sound/decal side of the grid
bits 20..31  Y         physics PROPERTY index  -> gameplay side: damage, penetration, subsystem, health
grid index = MaterialGridData.MaterialIndexMap[Y]  (52 entries -> 49 rows; out of range -> DefaultMaterialIndex 0)
```

* Code: `0x14057dd20` (damage-relation lookup) and `0x141697f30` (self-property lookup) both do
  `idx = (packed >> 0x14) & 0xfff; g = MaterialIndexMap[idx]` (`[grid+0x30]`). They clamp `g` to the default index `[mgr+0x14]`
  if it is ≥ the row count, and substitute `[mgr+0x10]` (default decl) when `packed & 0x80 == 0`. The manager holds
  `MaterialGridData*` at +8.
* Both X and Y run through the same MaterialIndexMap into one 49-wide index space. Gameplay relations sit at `[gY][gY]`, in the
  `PhysicsPropertyProperties` list. Effect/sound relations sit at `[gX][gX]`, in `PhysicsMaterialProperties` [V-data]. All 235
  damage refs are in property lists. Row 3 (laser property) holds damage data. Row 4 (laser material, X=4) holds
  effect/sound data against columns 8 (fighter hull X=8), 9 (capital X=9), 18/30 (shield X=19/32).
* MaterialIndexMap: identity for Y 0–12; Y 13, 27, 47 → 0 (unused); Y 14–26 → Y−1; Y 28–46 → Y−2; Y 48–51 → Y−3.
* Cross-checks [V-data]: `PhysicsPropertySubsystemData.IsSubsystemDamagingMaterial` = true only on g10 (ion), g25 (ion bomb)
  and g45 (B-wing ion beam). `DestroyedDamageSectionMaterialOverride` is Y=38 → g36, and g36 has DPM 0 against laser, missile,
  bomb and ion bomb.

## 2. Projectile materials [V-data]

From a full scan of all 97,444 EBX assets for MaterialDecl values (1,724 assets carry one).

| Category | Asset field | Packed | Y → grid |
|---|---|---|---|
| Primary lasers, rebel and imperial (`projectile_las_reb_xwing`, `_ywing`, `_awing`, `_uwing`, `projectile_las_imp_tiefighter`, `_tieinterceptor`, `_tiebomber`, `_tiereaper`), supercharged variants use X=16 | IncomBulletEntityData.MaterialPair | 0x300480 / 0x301080 | 3 → **3** |
| Autocannon / chaingun / burst (rapid-fire) / SMG (`projectile_chaingun_ywing_autocannon`, `projectile_burst_xwing`, `projectile_burst_tiefighter`, `projectile_smg_reb_awing`, `projectile_las_reb_uwing_autocannon`) | same | 0x300480 | 3 → **3** |
| Charge cannons (`projectile_chargecannon_xwing/awing` X=30, `_tiefighter/_tieinterceptor` X=31) | same | 0x301e80 / 0x301f80 | 3 → **3** |
| B-wing / TIE Defender lasers, chaingun, autoaim (liveservice) | same | 0x300480 / 0x301f80 | 3 → **3** |
| Capital/station turret lasers (`common/projectiles/projectile_las_imp_turret_large/medium/small`, `..._reb_turret_heavy`, ghost, dangermode, lasermine) | same | 0x300480 | 3 → **3** |
| Ion cannons (all ships, incl. ion turrets `projectile_ion_imp_turret`) | same | 0xa00780 | 10 → **10** |
| Beams (Y-wing / TIE bomber `starshipweaponconfig_beam_*`) | StarshipBeamParameters.MaterialPair | 0x1602e80 | 22 → **21** |
| B-wing ion beam | same | 0x3003180 | 48 → **45** |
| Proton torpedo (`projectile_highyieldtorpedo`, uses `missileconfig_torpedo`) | missile body / **explosion** | 0xf00e80 / **0xf01880** | 15 → **14** / 15 → **14** |
| Ion torpedo, ion homing missile | body / explosion | 0xa00780 | 10 → **10** |
| Piercing torpedo (liveservice) | body / explosion | 0x3203389 (flags 0x89) / 0xf01880 | 50 → **47** / 15 → **14** |
| Bomb (`projectile_bomb`, `_baradium`) | body / explosion | 0x1500e80 | 21 → **20** |
| Ion bomb (`projectile_bomb_ion`) | body 0x1a00e80 / explosion 0x1a00780 | | 26 → **25** |
| Homing, multilock, swarm, big-bang missiles, flare, magnetic mine | body / explosion | 0xf00e80 | 15 → **14** |
| Rockets, anti-fighter missile | body 0xf00e80 / explosion 0xf01780 | | 15 → **14** |
| Ion rocket (liveservice) | body / explosion | 0xa00e80 / 0xa01780 | 10 → **10** |
| Capital-ship missile-launcher turrets | fire the common homing missiles | | **14** |
| Hangar area damage to enemy players (`CapitalShipHangarAreaComponentData.EnemyPlayerDamageMaterialPair`, SD and MC75) | | 0x301080 | 3 → **3** |

Lasers (3) and torpedoes/bombs (14/20/25 body and explosion) are on **different** property materials. Many projectile
blueprints also carry an IncomExplosionEntityData with the default material 0x80. In the torpedo that is a small
`BlastDamage` 100 blast; the main one is Y15 with `BlastDamage` 8000 in the entity, overridden to 4000 by
`explosionconfig_missile_highyieldtorpedo`.

## 3. Capital-ship materials

* `StarshipEntityData.MaterialPair` = **0xb00980 (X9, Y11 → g11)** for every capital ship (SD, MC75, frigate, light cruiser,
  corvette, Raider, Quasar, Gozanti, GR-75, IGV-55, Zeta, BFF-1, Starhawk) and every capital `TurretEntityData` [V-data].
  Starfighters use 0xc00880 (Y12 → g12); the Y-wing and B-wing use X32.
* Capital `RigidBodyData.Material` = 0x80 (default) and `PartMaterials` = [], except the GR-75 body (0x980) [V-data]. The
  per-part materials live **in the HavokAsset RES** (type 0x91043f65). Its header holds a material table (u32 count at +0x28,
  u32 offset at +0x2c) and a per-part u8 array (count at +0x1c, offset at +0x20). The table values are read directly [V-data].
  That the per-part byte indexes the table is [I]: it fits X-wing → Y12, SD → Y11/Y18, MC75 → Y11/Y18. The materials:

| Physics RES | material table (Y → grid) | per-part byte → material |
|---|---|---|
| stardestroyer (40 parts) | Y1 g1, **Y11 g11**, **Y18 g17**, def | 32 parts → g11; parts 5–8, 20–23 → g17 |
| mc75 (41 parts) | Y1, def, **Y11**, **Y18**, def | 33 → g11; parts 9,10,21,22,36–39 → g17 |
| frigate (14) / lightcruiser (10) / gozanticruiser (7) / gr75 (2) | Y1, (def), Y11, def | part 0 (and Gozanti part 6) → default g0, rest → g11 |
| quasar (9) | Y1, Y11, def | all → g11 |
| starhawk (39) | Y1, Y11, **Y17 g16**, def | parts 1,2,30,31 → g16 (DPM 0 vs everything = invulnerable) |
| **corvette (1), raider (1), igv-55 (13), zetashuttle (5), bff-1 (6)** | Y1, def, def | all → **default g0** [I] |
| capital turrets (`turret_*_laser_*`, missile launcher, tractor beam) | Y1, Y11, (Y17 on MC75 large turrets), def | → g11 (MC75 large turret part 3 → g16) |
| capital shield bubbles (`veh_*_shield_01`, MC75/SD/frigate/corvette/raider/quasar/gr75) | **Y2 g2 (flags 0x89)**, **Y6 g6**, def(0x89) | → g6 [I]; the shield component gets material by hit position, so g2 is also possible |
| starfighters | Y1 g1, Y12 g12, def | all → g12 |

* g17 (Y18) appears only on the SD and MC75. Their subsystem damage sections (shield generator, power, targeting, hangar) are
  the obvious candidates [I].
* Subsystems also carry a `DamageSectionSettings.DestroyedDamageSectionMaterialOverride` = 0x2602980 (Y38 → g36). g36 has DPM 0
  vs lasers, missiles, bombs and ion bombs, so destroyed sections stop taking hits [V-data; the override mechanism is I].
* `PhysicsPropertySubsystemData` (the grid's self-properties) flags only the ion-type materials as subsystem-damaging. That
  identifies weapons, not ship parts [V-data].
* Fighter shields: `StarshipShieldData.ShieldMaterial` = X19/X32, **Y0 → g0** [V-data].

## 4. Grid cells

How it's built [V-data]: `InteractionGrid` is 49 rows × 49 `Items`. Each item is a `MaterialRelationPropertyPair` with two
pointer lists: `PhysicsMaterialProperties` (effects/sounds/decals, keyed by X) and `PhysicsPropertyProperties` (damage,
penetration, keyed by Y). `MaterialProperties[49]` holds per-material self properties (subsystem flag, cockpit FX, physics).
`MaterialRelationDamageData` objects are shared: one object is referenced from many cells (48 objects across 235 refs). The
damage side is fully symmetric ((a,b) and (b,a) point at the same objects); only 18 effect/sound pairs differ.
Defaults: CollisionDamageThreshold 30, DamageProtectionThreshold 2, ExplosionCoverDamageModifier 1.

| cell (attacker g, target g) | obj | CollMul | **ProtMul** | PenMul | notes |
|---|---|---|---|---|---|
| laser 3 × capital hull 11 | 59 | 1 | **1** | 1 | also (3,27) weak-point decal |
| laser 3 × SD/MC75 parts 17 | 199 | 1 | 1 | 1 | |
| laser 3 × capital shield 2 | 118 | 1 | 1 | 1 | |
| laser 3 × capital shield 6 | 212 | 1 | 1 | 1 | |
| laser 3 × starfighter hull 12 | 139 | 1 | 1 | 1 | |
| laser 3 × default/fighter shield 0 | 126 | 1 | 1 | 1 | |
| laser 3 × Y1 shapes 1 | 158 | 1 | 1 | 1 | |
| laser 3 × invulnerable 16 / destroyed section 36 | 276 / 198 | 0 / 1 | **0** | 1 | |
| laser 3 × heist-station destructible 43 | 209 | 1 | **0.15** | 1 | a level prop, not a ship |
| ion 10 × capital hull 11 (also 17, 27, 43; beam 45) | 234 | **0** | 1 | 1 | |
| ion 10 × capital shield 2 | 11 | 1 | 1 | **4** | |
| ion 10 × fighter hull 12 | 223 | 0 | 1 | 1 | |
| missile/torpedo 14, bomb 20, ion bomb 25 × capital hull 11 / 17 | 54 | 1 | 1 | 1 | explosion path ignores ProtMul |
| 14/20/25 × capital shield 2 | 75 | 1 | **0.3** | **0.3** | only PenMul is used on the blast path |
| 14/20/25 × capital shield 6 | 112 | 0.5 | 1 | 1 | |
| 14/20/25 × fighter hull 12 | 267 | 1 | 1 | 1 | |
| 14/20/25 × weak-point decal 27 | 1 | 1 | **0** | 1 | |
| 14/20/25 × destroyed section 36 / invulnerable 16 | 256 / 272 | 1 / 0 | **0** | 1 | |
| beam 21 × capital hull 11 | 110 | 1 | 1 | 1 | |

## 5. Does the damage code use it? Yes, on the server, for direct hits

The lookup chain [V-code]:
* `0x140580fb0`: scans a `PhysicsPropertyProperties` array for the first object whose class id is in
  MaterialRelationDamageData's range (reads `ClassInfo 0x1449f9290` +0x48/+0x4a). It is the **only** code that references that
  ClassInfo, so all damage-relation reads go through it.
* `0x14057dd20` `getDamageRelation(mgr, MaterialDecl* attacker, MaterialDecl* target)`: decodes both decls (§1). It takes
  `InteractionGrid[gA].Items[gB].PhysicsPropertyProperties` and calls `0x140580fb0`, with a default-cell fallback via
  `0x1411bc6f0`. The manager is reached as `component->...+0x30 → +0x98`. It has 70 direct callers. Their owner classes,
  found through vtables: getType slot 0 → slot 39 = collision, slot 41 = direct hit, slot 42 = blast.

| class (server) | slot 41 direct hit | slot 42 blast | other |
|---|---|---|---|
| ServerCapitalShipHealthComponent | `0x1417db200` | `0x1417dbca0` → `0x1417dbb40` | slot 29 `0x1417db510` (also `0x1417dbb40`) |
| ServerIncomShieldHealthComponent (capital shield bubble) | `0x140582ab0` | `0x140582d90` | |
| ServerTurretHealthComponent | `0x1416edb50` | `0x1416ee010` | |
| ServerStarshipHealthComponent (fighters) | `0x14168a8b0` | `0x14168ad70` | slot 39 `0x14168a140` |
| ServerCapitalShipWeakPointHealthComponent (Banger impl.) | `0x140be2a90` | `0x140be2d70` | |
| ServerIncomMissileHealthComponent | `0x1415a3d20` | `0x1415a3fe0` | |

The client classes (ClientIncomShield…, ClientStarship… slot 39, ClientCapitalShipWeakPoint…) also call it. Frostbite's
explosion entity `0x140c39a80` uses only `ExplosionCoverDamageModifier` (+0x2c).

**Formula, read from `0x1417db200` (capital hull, direct hit)** [V-code]:
```
target  = physicsComp->vtbl[0x1c8](out, info.partIndex /*+0x58*/)
          (ServerStarshipPhysicsComponent/ServerTurretPhysicsComponent use 0x141ca4ec0, which forwards
           out and partIndex unchanged to rigidBody->vtbl[0x2d0], so it is per part)
rel     = getDamageRelation(mgr, &info.attackerMaterial /*+0x20*/, &target)
if info.type /*+0x60*/ == 2:  mul = rel.CollisionDamageMultiplier; thr = rel.CollisionDamageThreshold
else:                         mul = rel.DamageProtectionMultiplier; thr = rel.DamageProtectionThreshold; pen = rel.DamagePenetrationMultiplier
dmgInfo.damage(+0x88) = info.damage(+0x84) * mul
if thr <= info.damage:   apply via 0x1417dbe90 -> 0x14168ffb0 (team/friendly-fire filter, starts from the scaled damage)
return {healthBefore - healthAfter, pen, flags}
```
Turret (`0x1416edb50`), shield (`0x140582ab0`), starfighter (`0x14168a8b0`) and weak point (`0x140be2a90`) do the same, but
test `thr <= damage*mul`. The turret first diverts to a subsystem path when the attacker material's
`IsSubsystemDamagingMaterial` is set (ion).

**Blast (`0x1417dbca0` → `0x1417dbb40`)** [V-code]: `rel = lookup(info+0x20, info+0x24)`. It copies the four fields to a
caller-local block, but only `DamagePenetrationMultiplier` is used (returned as result[1]). The applied damage is the raw
`info+0x74`. The shield (`0x140582d90`), turret (`0x1416ee010`), starfighter (`0x14168ad70`) and weak point (`0x140be2d70`)
blast paths do the same: the PenMul read, the demolition flag, raw damage.

**Server or client:** every damage-applying function above belongs to a `Server*` component [V-code].

## Open questions

1. **Which material does a laser hit on each capital ship resolve to?** Above all, do the corvette, Raider, IGV-55, Zeta and
   BFF-1 resolve to g0 (fighter-shield cell) or g11? Settle it at runtime: hook `0x14057dd20(mgr, A*, B*)` on the server and log
   `*A`, `*B` and the return address (`0x1417db200` range = capital hull bullet, `0x140582ab0` = capital shield,
   `0x1416edb50` = turret). Shoot each ship type once.
2. **Who consumes result[1] (`DamagePenetrationMultiplier`)?** It matters for whether the 0.3 (missiles vs capital shield) and
   4.0 (ion vs capital shield) values have any effect. Not found; the health components' slot 41/42 are called virtually
   (vtable +0x148/+0x150).
3. What `info+0x24` holds on the blast path (target material vs explosion `SecondaryMaterialPair`). It doesn't affect the
   verdict, since the blast ignores ProtMul.
4. Meaning of the flag bits 0x09 in Packed; whether g17 is exactly the SD/MC75 subsystem sections.

Dead ends: no MaterialContainer (material names) ships with the game, so materials are identified only by usage. The
`MaterialRelationDamageData` ClassInfo has no code xrefs except the class-range check in `0x140580fb0`.

Outside scope but relevant: `StarshipHealthDamageData` already has Shield/Hull `DamageMultiplierFromLaser`/`FromNonLaser`, and
there are attribute ids `AttributeId_HullDamageMultiplier_Laser` etc. If the capital-ship health path honours those attributes,
that per-target laser/non-laser knob has none of the grid's sharing problems. Worth checking next.

Helper scripts used (kept locally in `local/tools/matgrid/`, not in the repo, because they use the local data reader):
`matscan.py` (all MaterialDecl values), `grid.py`, `havokmat2.py` / `havokparts.py` (RES header material tables), `ciref.py`,
`calls.py`, `owner.py` (vtable owner of a function).
