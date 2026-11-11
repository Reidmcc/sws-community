# Lock-ons on a resized ship: research record (issue #50, 2026-11-11)

Reported from matches: flying a resized ship (Y-wing at 0.77, TIE Reaper at 0.83), lock-on auxiliaries wouldn't lock onto
AI ships or players, as host and as joiner; the Goliath did lock the Raider and corvette, multi-lock missiles didn't.

**Cause.** The mod's resize scales the ship's root part, so every component under it has world axes of length s. The
game's lock tests use a component's forward axis as if it were a unit vector (the target-lock cone test: checked by
hand in the disassembly, 0x1418856a0, the dot at 0x1418857c1..0x14188580c, the transform from 0x141885850; the
multi-lock's radar query: the research agent's reading). With the axis 0.77 long the dot can't exceed 0.77, so every
target reads as at least 39.6 degrees off the nose.

**What was built** (sws_probe.cpp, "Lock-ons on a resized ship"): as the blueprint loads, after the root part is
scaled, the scale is pushed through the components the tests read: every StarshipWeaponComponentData,
TargetComputerComponentData and RadarTargetComponentData under the root part (ClassInfos 0x144a079c0, 0x144a4d940,
0x144a05710) gets its axes divided by s, and each of its children gets its axes and translation multiplied by s. Each
of those components then has unit world axes at its scaled place, and everything under it keeps the world size and
place it had (fire points, the computer's meshes). This is fix option 1 below, widened to the targeting computer and
done as a push-through, so nothing under a weapon moves. `lock_log=1` under `[server]` logs, per weapon every 5 s, the
cone tests and passes, the forward axis's length the test read, and the last test's numbers.

**Still inferred:** that the component at `[firing+0x190]` is the weapon component (so that the weapons are what the
target-lock test reads). `lock_log` shows the axis length the test actually reads: 1.000 on a resized ship means the
fix reached it.

Not flown yet.

---

The research agent's findings, as written (static, read-only):

Steam build addresses. The research agent's helper scripts and dumps aren't in the repo.
[V] = read in the disassembly or the asset; [I] = inferred.

## Verdict

The hypothesis holds, for both lock kinds. Both lock tests take the shooter's matrix from a component's world transform
(a component under the root PartComponentData that the mod scales) and dot the forward axis, as stored, with a
normalized direction to the target. With |forward| = s the dot can't exceed s, so with s = 0.77 or 0.83 no target
ever passes a cone narrower than acos(s) off the nose (39.6 degrees at 0.77, 33.9 degrees at 0.83). Every stock lock
cone is narrower than that. Range and line-of-sight are not affected.

## Q1. The functions

Runtime classes (from notes\sws_runtime_types.json, ClassInfo found by qword scan, getType `lea rax,[ci]; ret`):
- TargetLockWeaponFiring: type 0x14493e6f0, ClassInfo 0x144b558c0, getType 0x141882a30, vtable 0x143638248, ctor
  0x141882a40 (object size 0x360, factory 0x14124d3b0), per-frame update = vtable slot 6, 0x141883910 [V].
- AutoLockWeaponFiring: ClassInfo 0x144a24e40, vtable 0x143637d30. MultiTargetAutoLockWeaponFiring: ClassInfo
  0x144b55940, vtable 0x143638090 [V].
- Attribute getters go through the ship's attribute table by id (0x1415be2b0), reading [firing+0x208]->+8->+0x60 (the
  firing data) at these field offsets [V]: 0x14187ac70 = +0x80, 0x14187ca30 = +0x88 (GetLockConeNear/Far for target
  lock, AutoLockWeaponFiringRadius / AutoLockWeaponConeAngle for auto-lock: the same offsets in both data types),
  0x141885c10 = +0x90 KeepLockConeNear, 0x141885c80 = +0x98 KeepLockConeFar, 0x141885ac0 = +0xb0 ActiveConeDuration,
  0x141885b30 = +0xb8 MinLockDistance, 0x141885ba0 = +0xc0 MaxLockDistance.

**Target lock (Goliath, torpedoes, homing missiles): FUN_1418856a0** (cone-and-range test) [V]. Signature: rcx = the
firing, xmm1 = near angle, xmm2 = far angle, r9b = also test max distance, 5th arg (stack) = the target's radar-target
interface. Callers: get-lock (near/far = +0x80/+0x88, r9b=1): 0x141884ec6, 0x14188432e, 0x141884749, 0x141884962;
keep-lock (+0x90/+0x98): 0x141884405, 0x1418846be, 0x141884924; one more at 0x141884aad [V].

**Multi-lock (Y-wing multilock):** acquisition FUN_14187b610 (AutoLockWeaponFiring; called from 0x14187bd91, 0x14187c39f
and the multi-target class at 0x1418812c2) [V]. With AutoLockOnTaggedTarget false (the Y-wing multilock's data, asset
starshipweaponconfig_multilock_ywing #3 [V]) it builds a query {pos +0, forward +0x10, exclude +0x20, cone +0x28, min
+0x2c, radius +0x30, team mask +0x34, tags +0x38} and calls the radar query **FUN_1416d7400** [V]. When
AutoLockOnTaggedTarget is set it uses **FUN_14187b940** instead (same math, plus the capital-ship shortcut below) [V].

**Client, server or both:** the firing classes are shared code; one copy per realm, told apart by [firing+0x1a0]
(0x14187b7c7 picks the radar manager [0x1441c87f0] when it's nonzero, [0x1441c87f8] when zero) [V]. [0x1441c87f0] is
the server's: 7 of its refs are in the AI squad code 0x14182xxxx [I, strong]. So the same cone test runs on the server
and on the client. Which side is authoritative for the lock wasn't established.

## Q2. Where the shooter's position and aim come from

- **Target lock:** the matrix is `FUN_141885850(firing)` [V]: comp = [firing+0x190]; coll = [comp+8]; slot =
  byte[coll + 0x14 + byte[comp+0x18]*0x20]; slot 0xff -> the entity's matrix (coll+0x10+b9*0x20+ba*0x40, the same formula
  as the ServerStarshipEntity's), else the component's own world matrix coll+0x50+b9*0x20+(ba+slot)*0x40.
  [firing+0x190] is the component that owns the firing, set by the base constructor 0x1417af130 at 0x1417af1fd from the
  creation info's first qword [V]; that it is the StarshipWeaponComponent is [I]: the factory 0x14124d3b0 takes the
  weapon config from the same info (+0x20), and 0x1417af598 reaches the starship entity through it.
- **Multi-lock:** the matrix is the shooter's own radar-target interface, vtable slot 10 = 0x14169b400 (the same
  slot-lookup formula, from the interface at component+0x40); position = slot 16 0x1416c55b0 (that matrix +0x30) [V].
  It's the RadarTargetComponent.
- Blueprint (game/gameplay/starships/ywing/starship_ywing, its component tree) [V]: root PartComponentData #71
  (t 0,-0.77,-7.20) > StarshipEntryComponentData #86 (t 0,0.77,7.20) > every StarshipWeaponComponentData (#99 aux,
  #98 aux, #101 countermeasures, #100 primary; identity transforms, locator children = barrels) and
  TargetComputerComponentData #102 > RadarTargetComponentData #79 (t 0,-0.70,-7.30). TIE Reaper (its component tree):
  root part #76 > entry #86 > TargetComputer #103 > the weapon components and RadarTarget #79 [V]. So both components
  sit under the scaled root part, and their world axes have length s [I: that they have their own transform slots, not
  0xff, isn't visible statically; the Goliath failing is the evidence].

## Q3. The math (unit axes assumed)

FUN_1418856a0 [V]: d = targetPos − M.trans (0x14188573d/0x141885741); dist = |d| (0x141885763); fail if dist > max (when
r9b) or dist < min (0x14188576c..0x141885774); t = clamp(dist/max, 0, 1); angle = (1−t)·near + t·far (0x141885788..
0x1418857c5); dot = dot(d/|d|, **M.forward as stored** [rbx+0x20]) (0x1418857c1..0x14188580c, no normalize); pass if
dot ≥ cos(angle·π/180) (0x1418857e3, 0x1418857ef, cosf 0x1429bcfc6, 0x141885816). **The angle is a half angle** (no
×0.5): the Goliath's 8 is 8 degrees off the nose.
FUN_1416d7400 [V]: c = cos(cone·π/180·0.5) (0x1416d741b..0x1416d7456); min²/max² on |d|² (0x1416d7580/0x1416d758a);
dot(d/|d|, query.forward) ≥ c (0x1416d75d1..0x1416d75ee). **The multi-lock cone is a full angle** (60 means 30 off the
nose). FUN_14187b940 is the same (×0.5 at 0x14187ba74, forward [r14+0x20] at 0x14187bb02, test at 0x14187bbfd) [V].
The forward isn't normalized in any of the three; d is.
- s < 1: dot = s·cos(θ) ≤ s. For s = 0.77 the passes need cos(half) ≤ 0.77 (half ≥ 39.6 degrees). For s = 0.83 the half
  angle must be at least 33.9 degrees. Goliath 8 (cos 0.990), keep 16 (0.961), torpedo 10/20, multi-lock 30 (0.866): all
  fail at both scales, at every distance.
- s > 1: the cone widens to acos(cos(half)/s), e.g. s = 1.2: Goliath 8 -> 34.4 degrees, multi-lock 30 -> 43.8 degrees
  half. Locks get easier.
- Range: d and dist come from translations only; the shooter's world translation is the scaled small offset (cm to
  m), so ranges hold. The LOS raycast FUN_141885100 reads only M+0x30 (0x1418852f8) [V].

## Q4. Goliath vs multi-lock on the Raider/corvette, and the target's size

- The cone tests treat the target as one point: its radar-target position, vtable +0x80 (0x141885734, 0x1416d7558)
  [V]. The target's size doesn't enter the cone.
- The only size-aware path is a **server-only capital-ship shortcut** in FUN_1418856a0 (0x1418856f8..0x141885716) [V]:
  if [firing+0x326] is set and dist2 = [firing+0x328] is within [min, max], the test passes with no cone at all. The
  update copies both, only when [firing+0x1a0] != 0 and [[firing+0x190]+0x28] bit 0, from the
  ServerTargetComputerComponent at [firing+0x2f0] (+0x188 -> +0x326, +0x19c -> +0x328; 0x141883bb9..0x141883be5) [V].
  That component (ClassInfo 0x1449fdb80, ctor 0x1416dc770) fills them in FUN_1416dd7d0 [V] (named by its string
  "ServerTargetComputerComponent::updateTargetLockOnCapitalShipState::rayCast"). Only for the current target, and only
  if it's a capital ship (byte +0x3f set, +0x3e clear: 0x1416dd86f/0x1416dd88e). +0x19c = the distance to the target's
  nearest point, from a physics query (FUN_1416d4900, 0x1416dd930). +0x188 = whether a ray from the owner's transform
  along its forward, of length MaxCapitalShipTargetLockDistance (TargetComputerSettings +0xa8 = 2500 in
  targetcomputersettings_common) × |forward|, hits that target first (0x1416dd9d1..0x1416ddaee).
  This is the "aim anywhere at the hull" lock the player describes [V].
- The Goliath is a target lock, so it reaches this shortcut, which bypasses the broken cone. The Y-wing multilock has
  AutoLockOnTaggedTarget = false, so it goes through FUN_1416d7400, which has no shortcut. That explains the difference
  [V for both paths; I for which path the Goliath actually took].
- "Only for a moment, only when very close": the scaled forward alone doesn't predict this. The ray is 2500·0.77 =
  1925 m, longer than the Goliath's 1000 m maximum. Possible causes, all [I]:
  (a) the shortcut exists only on the server copy; the client's copy fails the cone every frame, so if the client
      drives or cancels the lock, a server-granted lock would flicker;
  (b) the nearest-point query FUN_1416d4900 searches only a limited radius (sqrt(2x²+y²) from a per-world table at
      0x14419b890 + [target+0x28]·8, entry +0x10), so +0x19c can be valid only close in. It keeps its last value when
      the query finds nothing (never reset; only set to FLT_MAX in the ctor at 0x1416dc964).
  The owner transform behind the ray ([comp+0x38] vtable +0x180) wasn't identified.

## Q5. Lasers

- Laser firing has no cone gate on the ship's forward that I found. The client target computer's aim code
  (0x1416c9480, "ClientTargetComputerComponent::rayCast") builds its aim vector from point differences and normalizes
  it (0x1416c9d61..0x1416c9dbc) [V for that block]. Projectiles spawn at barrel locators (scaled positions, still on
  the ship) [I].
- Not established: whether laser aim assist (StarshipWeaponFiringData +0x68 TargetComputerAimAssistSettings cone
  attributes) uses an unnormalized axis. If it does, aim assist on resized ships is weakened or absent, which players
  may not notice.

## Q6. Fixes, ranked

1. **Data, mirroring the cockpit fix (smallest blast radius).** After scaling the root part, walk its subtree (all
   intermediate parts have identity axes: check, as for the cockpit). For every **RadarTargetComponentData** and every
   **StarshipWeaponComponentData**, divide its axes (+0x30/+0x40/+0x50, x y z) by s and leave its translation alone;
   its world origin stays where it is and its world axes become unit. A RadarTarget is a leaf. A weapon's barrel
   locators (children, Components at +0x70) would then sit at original-size offsets, so multiply each descendant's
   translation (+0x60) by s; MeshComponentData descendants would also need their axes ×s (none under the Y-wing's or
   Reaper's weapons). ClassInfos are still needed for RadarTargetComponentData (type 0x1449538f0) and
   StarshipWeaponComponentData (type 0x1449340f0): find each with the qword scan. Caveat: if [firing+0x190] turns out
   not to be the weapon component, the target-lock half misses; the probe below settles it.
2. **Data, one component:** counter-scale the StarshipEntryComponentData under the root part (Y-wing #86, Reaper #86;
   axes/s, translation (u+t)/s−t as for the cockpit). It covers weapons, computer and radar in one place on both
   ships, but its subtree goes back to full size: barrels, turret, repulsor volumes (#94-96), the computer's meshes
   (#64/#61/#65/#59/#68, possibly cockpit holograms) and the meshes #56/#57/#58/#62. It also carries the seat/HUD data.
   More visual risk [I].
3. **Code:** normalize at use. (a) Hook FUN_1416d7400 to normalize query+0x10 in place (the query lives on the
   caller's stack; a no-op for stock ships, though it also touches every other radar query). (b) For target lock, have
   FUN_141885850 return a pointer to a normalized copy (two callers: 0x141885727, 0x1418852f4; a per-thread buffer).
   (c) FUN_14187b940 reads radar slot 10 directly (needed only for AutoLockOnTaggedTarget weapons). Larger and touches
   shared hot code; only worth it if the data fix leaves cracks.

**One-flight probe:** hook FUN_1418856a0 (keep its 5 args and xmm1/xmm2). Before calling through: M =
FUN_141885850(rcx); log, rate-limited ~2/s per firing: realm [rcx+0x1a0], |M+0x00|, |M+0x10|, |M+0x20|, the ClassInfo
of [[rcx+0x190]] (vtable slot 0 -> getType), dist and dot (compute from target vtable+0x80 and M), cos(angle), the
bytes/float at +0x326/+0x328, and the return value. Also hook FUN_1416d7400 to log |[rdx+0x10]|, [rdx+0x28] and the
return value. Expected on a 0.77 Y-wing: |forward| = 0.77, dot ≤ 0.77, cone fails; stock ship 1.00.

## Side notes (out of scope)

- The mod scales the AI blueprint too (Starship_Ywing_AI). AI torpedoes use the same TargetLockWeaponFiring test, so
  resized AI ships likely can't lock either [I]. Whether the AI's laser fire check FUN_14182a310 takes its forward from
  a scaled component wasn't checked.
