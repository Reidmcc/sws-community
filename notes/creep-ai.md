# Creep AI vs bot AI: phase-aware aggression and weapon gates

Research note (read-only data study, 2026-10-24). Map studied: Yavin (`game/levels/pvp/pvp_yavin/*`).
Tags used below: **[V]** = verified from data dumps; **[I]** = inferred (engine behaviour not visible in data).

**Where this ended up (2026-10-26):** sections 0-4 are the first research pass; section 5 is the first build, whose
phase switch never worked (see 6); sections 6-8 are what was measured in flight and what shipped. The user-facing
description is [docs/creep.md](../docs/creep.md). The dumps named below are local files (`local/` is not in the repo),
made with the project's reader from the game's own data.

Dumps used: `local/dumps/pf_pvb_botlogic_imp.json`, `bhv_aisquadron_basic_imperial.json`,
`pf_pvp_creeplogic_*tiefighters.json`, `creepscan/pvp_yavin/*.json`, plus new dumps in
`local/dumps/creepai/` (rebel behaviour, X-wing creep logic, frigate/corvette/frontline-state logic,
TIE bomber / TIE fighter AI blueprints, aiming settings).

## 0. Field-id hash (solved) [V]

Field, event and link ids are `djb2-xor` over the case-sensitive name, as int32:
`h = 5381; for c in name: h = (h*33) ^ c  (mod 2^32)`. Checked on `Out`=193453899, `Value`=225375086,
`In`=5862146, `Bool`=2088812747, `BehaviourIndex`=875081154, `In0..In6`, `TargetProviders`=1248225478.
Interface (user-defined) names were recovered by meet-in-the-middle over a game-word vocabulary; only
names that read as plausible were kept (3-word searches produce ~1 random collision per id, so the gibberish
alternatives were discarded). Names used in this note:

| id | name | | id | name |
|---|---|---|---|---|
| 1336394533 | AllowMissileFiring | | -1073460925 | AllowCountermeasures |
| 1524820569 | DisableAttack | | -1401919712 | DisableWeapons |
| 1381602785 | DisableCannons | | -737100000 | DisableBombs |
| -150269651 | DisableSelfHeal | | -125956299 | DisableResupply |
| 789584867 | DisableAssaultShield | | 1474902457 | DisableShieldBuff |
| 1091875546 | DisableSupportAbilities | | -1961739673 | DisableSensoryArray |
| -877306743 | DisableDogfighting | | -730043070 | DisableEvade |
| -996272535 | DisableFlyFormation | | -1552562573 | TailAttackModeForPlayerStarships |
| 215512070 | (DogfighterInfo) IsBot | | -648868903 | IsInSquadron |
| -1860559003 | IsSquadronLeader | | 252579369 | ShipClass |
| -645606250 | (PDV) HasValueSet | | -1536525709 | (CompareEnum) IsEqual |
| -1063341324 | (bot logic) PhaseIndex | | 1248225478 | (DynSquad) TargetProviders |
| -1289085557 | (provider) FriendlyProtectTargets | | 728516607 | (provider) TargetVehicles |
| 829970700 | FriendlyFlagship | | -717561619/-18 | FriendlyMedCap1/2 |
| 83746301 | RebelStarfighters | | -811367246 | "EnemySmallCap" (but see 1.4) |
| -1107396516/1974306015 | IsMedCap1/2Active | | 1298750726/-775931515 | IsEnemyMedCap1/2Active |
| -2144132285/437060288 | Frigate1/2NormalizedHealth | | 411276784 | CorvetteNormalizedHealth |
| -1617986842/-1692033375 | AddTargets/RemoveTargets | | -95517159 | NewTargetPinged |
| 1642405221/-522485296 | OnEnable/OnDisable | | 93732099 | TargetPrioritisationSettingsIndex |

Still unnamed: -744828767 (the only creep-fed input, feeds FollowWaypoints.FollowFromStart), -268898707
(feeds ProximateAreas.UseApproachPaths), -1817211963 (float, feeds CompareFloat#80.A against the attack
behaviour's timer, toggles tail-attack mode; bots feed 15.0).

Correction to "known" tag list [V]: bit 16 (65536) is not creep-only. pvp_creep_sch IncomRadarTargetTagsData
#6/#10 append 65536, but the frigate logic appends 536936448 (bits 29+16) and the corvette logic 268500992
(bits 28+16). Bit 16 is a "Fleet Battle frontline unit" tag shared by creep, frigates and small caps.

## 1. How a bot squadron picks targets per phase

### 1.1 The phase value [V]
`pf_pvp_frontlinestate` (pvp_frontlinestate_sch#2) output field **965584829** = SyncedInt#1 =
`SelectTransform#22.SelectedIndex` over inputs `[RZone2, RZone1, Zone0, IZone1, IZone2, OvertimeDeprecated,
Transition]`, default 2. So: **0 = Rebel zone 2 (rebel flagship), 1 = Rebel zone 1 (rebel frigates),
2 = Zone0 face-off/kickoff, 3 = Imperial zone 1, 4 = Imperial zone 2, 5 = overtime (unused), 6 = transition.**

The same int reaches both AIs [V]:
- bots: pvp iface -557559407 -> pvp_yavin#5 -> pvp_yavin#8 (PvB SubWorldRef) .965584829 -> pvb iface ->
  `pf_pvb_botlogic_imp/_reb` field PhaseIndex (-1063341324).
- creep: directly `pf_pvp_frontlinestate.965584829 -> pvp_creep_sch#2/#5/#8 (TIE) and #4/#7/#11 (X-wing)
  pf_pvp_creeplogic_exploit_*.862952468` (FrontlineIndex).

The PvB subworld (pvb, pvb_ai, pvb_ai_sch) is streamed only by `mainflow#11 CheckGameMode
GameMode=Frontline_Bots` -> pvp_yavin#8 [V]. **In PvP Fleet Battles the bot logic, its providers, phase configs
and prioritisation settings are not loaded at all.**

### 1.2 Bot logic graph (pf_pvb_botlogic_imp, Team2; _reb mirrors it) [V]
PhaseIndex -> IntChanged#120 -> SelectEvent#147 (Selected = PhaseIndex; also pulsed by Delay#43 1 s autostart and
re-pulsed by Delay#40/#42 150 s and morale checks). Each phase output does two things:
(a) fires an input on **SelectInt#150 -> Controller#106.BehaviourIndex** (picks one of 9 BehaviourProviders,
all using bhv_aisquadron_basic_imperial), and (b) enables one **IncomSquadronAIBotPhaseConfiguration** (#88-#105,
picked by RandomEvents), whose `Squadrons` links are DynamicSquadron configs (#58-#87) that get OnEnable/OnDisable.

| PhaseIndex | BehaviourIndex -> provider -> area | phase config(s) enabled | what the squads prioritise |
|---|---|---|---|
| 0 RZone2 | 1 -> #4 -> pf_pvb_flagship_dogfightarea @(0,508,+4000) (2 spheres r=750) | RandomEvent#140: #101 (80%) / #90 | Siege: subsys 140, dmgsect 100, fighters 0; Flex: subsys 120, players 100, bots 80, turrets 40 |
| 1 RZone1 | SelectEvent#148 on IsEnemyMedCap1/2Active: both alive -> 50/50 BI 2 (#9, medcap area @(500,200,2200)) or 3 (#5, @(-500,200,2200)); one dead -> the other lane | RandomEvent#138: #96 (80%) / #88 | Siege: **cap+medcap 140**, cap+smallcap 100, fighters 0; Flex/Hunter: fighters/players 80-140, turrets 20-60, smallcap 40 |
| 2 Face-off | 4 -> #6 -> SphereData pvb_ai#9 centre (0,0,0) r=1250 (+ waypoints pvb_ai#6) | none from this branch; kickoff configs come from the enemy corvette spawn event (below) | |
| kickoff (event 802424646 = rebelalliance#21 corvette event -1452333337) | - | CompareBool#28 on IsEnemySmallCapActive: true -> #97 (80%)/#104, false -> #93/#94; if both own frigates dead (#17) -> #26 -> #92/#102 or #89/#100 | Flex/Hunter: fighters 120/players 100/bots 80, **cap+smallcap 40-60**; Siege: **cap+smallcap 100**, players 80, bots 60. **No medcap, flagship or generic-capital score anywhere.** |
| 3 IZone1 | SelectEvent#149 on own IsMedCap1/2Active -> BI 5 (#2, own frigate area @(500,200,-2200)) or 6 (#7) | via #28 again (#97/#104/#93/#94) | defensive fighter lists as kickoff |
| 4 IZone2 | 7 -> #1 -> flagship area @(0,500,-4000) (own flagship) | #17 -> #26 chain | same lists, 2000 m variants |

Per-squad settings [V]: all 30 DynSquads have `TargetingRules.EngagementMode=Aggressive`, `MaxSearchRadius`
2000 or 3000, `RequiredTeam=Team1`, `DefaultMaxAttackersPerTarget=10` (override 2), `MinNewTargetDistance=250`,
`SelfDefenseDistance=-1`, `EnableTargetPrioritisation=false`, `TargetPrioritisationSettingsList=[]`, StartEnabled false;
the prioritisation is per **StarshipEntries[class].TargetPrioritisationSettings** (Flex/Siege/Support/Hunter) [V]; that
the per-class entry is what the bot in that slot uses is [I]. 96 settings / 752 scores in total.
Score tag values used: cap+small = 268435458, cap+med = 536870914, subsys = 1048576, dmgsect = 2097152,
AIfig+player = 36, AIfig+bot = 524292, turret = 8 (bit 3).

Morale/tug-of-war [V]: `pf_pvp_frontlinetugofwarlogic.-1504470600` -> CompareFloat#36/#37/#38 (0.4/0.7/0.35)
-> RandomEvent#134/#141 -> #146/#144 -> configs #98/#105/#95/#103 = flex+hunter squads hunting enemy creep (providers
#112-#114/#118), then Delay 150 s back to SelectEvent#147. Own-frigate health < 0.15 (CompareFloat#30/#32) increments
Float#57 -> provider #116.ScoringWeight.

### 1.3 Dynamic target providers / helpers (what each is fed) [V for wiring, I for semantics]
All providers: MaxNumberTargeters -1, PrioritiseTargets true. Linked to squads via DynSquad.TargetProviders (1248225478).

| provider | weight | fed with | used by |
|---|---|---|---|
| #115 | 20000 | helper #108 (DynamicTargetProvider link) <- PingedTargetHelper#119 (Team2): NewTargetPinged -> AddTargets; Delay#41 30 s -> clear | every squad: player pings override everything |
| #116 | Float#57 (1, grows) | FriendlyProtectTargets = own flagship + own frigates (imp: galacticempire#39, #33, #41) | defensive/kickoff 3000 m squads |
| #111 | 10 | FriendlyProtectTargets = same | kickoff 2000 m squads |
| #112 #113 #114 #118 | 10 | FriendlyProtectTargets as above + **TargetVehicles = RebelStarfighters** = pvp iface -2036343240 = rebelalliance#22/#30/#31/#36/#41/#69 (**the X-wing creep spawns**) + pf_pvp_spawnmanager.-87509639 | creep-hunter squads (#71/#81/#85/#58) |
| #117 | 2000, StartEnabled false | helper #107 Targets = iface -811367246 -> pvb `Raider` -> galacticempire#29/#19 (**own** raider for imp; reb gets own corvette) ; enabled+AddTargets when CorvetteNormalizedHealth (= enemy small cap, rebelalliance#21) < 0.1 (10%), disabled/RemoveTargets at 0 | 2000 m kickoff squads |
| #109 #110 | 10 | nothing linked in-prefab | Siege-4 squads #78/#68 (low-frigate-health configs #99/#91) |

So the providers add specific *entities* (own capitals to protect, enemy creep to hunt, pings); they carry no phase
information. Phase awareness comes from (a) which phase config is enabled (different per-class score lists),
(b) the BehaviourIndex keeping the squad inside a per-phase area (face-off sphere r=1250 around the centre; frigate/
flagship dogfight spheres in attack/defence phases).

### 1.4 Oddity
The best name match for -811367246 is "EnemySmallCap", but both bot logics wire it to their **own** small cap
(imp -> Raider, reb -> Corvette) [V]. Either the name collided or the helper means "small cap to escort/protect" [I].

## 2. What the creep lack, and the smallest changes

### 2.1 Creep as shipped (Yavin uses the `exploit` variants) [V]
`pf_pvp_creeplogic_exploit_tiefighters` (squadron #9) and `_exploit_xwings` (#9): Defensive, MaxSearchRadius 500,
DefaultMaxAttackersPerTarget 1 (override 3), MinNewTargetDistance 100, SelfDefenseDistance 400,
EngagementModeOverride Aggressive, EnableTargetPrioritisation **true**, one settings object (#17) with 5 scores:
`262144 (bit 18 standalone turret)=5, 36 (AIfig+player)=3, 524292 (AIfig+bot)=3, 20 (bits 2+4)=2, **2 (any capital)=2**`.
FrontlineIndex(862952468) -> SelectInt#11 (In0..In6 = 1,2,3,4,5,3,3) -> Controller#8.BehaviourIndex.
BehaviourIndex is 1-based over the controller's link order (#3,#1,#2,#5,#4) [I, but every map/phase check fits].
All 5 providers link Areas -> pvp_general#4 OBB (half extents 2500x1250x5000 = the whole battlefield).
The X-wing creep use **bhv_aisquadron_basic_rebel** (file b168c496), not the imperial behaviour; object ids differ.
(The non-exploit `pf_pvp_creeplogic_tiefighters` has 2 providers switched by events, not by FrontlineIndex.)

Creep path distance to capital ships per phase (Yavin, spawn positions; capitals may relocate between zones, the
frigate logic has MoveToNextZone1-3 behaviours, so treat as approximate) [V geometry]:

| creep (side) | phase -> path | enemy frigates | enemy flagship | enemy small cap |
|---|---|---|---|---|
| TIE #2/#5/#8 | 0 & 1 (attack) -> #17/#5/#23 | 143-583 m | 1284-1701 m | 269-1333 m |
| TIE | 2, 5, 6 (face-off) -> #20/#8/#12 | **1300-1687 m** | 3100-3416 m | 2100-3012 m (spawn) |
| TIE | 3 & 4 (defence) -> #19/#1/#24 | >= 3400 m | >= 5247 m | >= 4236 m |
| X-wing #4/#7/#11 | 3 & 4 (attack) -> #22/#21/#3 | 327-745 m | 1231-1399 m | 264-1487 m |
| X-wing | face-off -> #6/#11/#9 | **1312-1687 m** | 3102-3416 m | 2102-3182 m |

Why Aggressive/3000 m creep hit frigates in the opener: their only capital score is the generic tag 2 (matches
frigates and flagship, every phase), the face-off path is only 1.3-1.7 km from the enemy frigates, and their Areas OBB
does not confine them. Bots at 3000 m don't, because their kickoff lists contain only cap+smallcap among capitals and
their face-off area is a 1250 m sphere at the centre [V data; that unlisted tags lose to listed ones is I].
Note: bots list explicit Priority-0 entries (for example Siege: AIfig+player=0), which suggests an unlisted tag is still
targetable at some base priority and a 0 entry is how a tag gets blocked [I].

### 2.2 Options, cheapest first
Runtime offsets are from sws_runtime_types.json (IncomSquadronAISquadronEntityData: TargetingRules @32 [EngagementMode
@32, MaxSearchRadius @40, MinNewTargetDistance @44], TargetPrioritisationSettingsList @104, EngagementModeOverride @112,
TargetPrioritisationSettingsIndex @124, EnableTargetPrioritisation @139; TargetPrioritisationScore TargetTags @40,
Priority @44). Enum ints aren't in the type file (it lists names alphabetically): copy the int that the creep
squadron already holds in EngagementModeOverride (= Aggressive) into TargetingRules.EngagementMode.

**A. Static value writes, geometry-based (no new objects; partly phase-aware).** Per creep squadron (exploit_tie #9,
exploit_xw #9): EngagementMode := Aggressive; MaxSearchRadius ~1000-1200 (face-off path to enemy frigates >= 1300 m,
attack paths <= 750 m); optionally raise score "2" (exploit_tie #15, exploit_xw #14) Priority 2 -> 4 so they commit
to frigates when in range. Result: frigates in the attack phases, small caps/fighters in the opener, nothing
in defence. Gaps: the enemy flagship (phase 0 / 4) sits 1.2-1.7 km from the attack paths (out of reach); search
is measured from the moving squadron leader, so a long dogfight can drift them into frigate range in the opener
[I]; distances differ per map.

**B. Static value writes, prioritisation-based (not phase-aware).** Retag score "2" to 268435458 (cap+smallcap),
or keep tag 2 and set its Priority 0. Then Aggressive/3000 creep only take small caps + fighters, never frigates.
Correct in the opener, wrong on offence.

**C. Phase-timed value writes from the mod (no new objects or connections; fully phase-aware).** The mod already
patches engine memory; on each frontline change (poll pf_pvp_frontlinestate SyncedInt#1, or the live creep
controller's BehaviourIndex, which already follows the phase) rewrite the existing score objects per side:
- TIE creep (score exploit_tie #15): phases 0/1 -> TargetTags 2 (or 536870914 cap+med in phase 1), Priority 4-5;
  phases 2/3/4/5/6 -> TargetTags 268435458 (small cap), or Priority 0.
- X-wing creep (exploit_xw #14): the mirror (attack in 3/4).
- Keep EngagementMode Aggressive and MaxSearchRadius 2000-3000 (the bots' values) constantly.
That reproduces the bots' per-phase capital priorities. Two small writes per phase change. [I] that scores are
re-read at evaluation time (they are plain DataContainer data, the same objects the squadron points at).
Flagship reach in phase 0/4 still needs MaxSearchRadius >= ~1700 m.

**D. Data-graph change (expensive): drive the index.** Add one PropertyConnection
`creep iface 862952468 (FrontlineIndex) -> Squadron#9.TargetPrioritisationSettingsIndex (93732099)` to the prefab's
PropertyConnections array, grow TargetPrioritisationSettingsList (@104) to 7 entries (index = phase 0..6; entries may
share pointers) and create 2-3 new TargetPrioritisationSettings (offence / face-off / defence; they may share
the 5 existing score objects plus new ones). Unverified that the squadron reacts to the index changing after spawn.

**E. Reuse the bots' providers or phase configs: not viable.**
- In PvP mode none of it is loaded (CheckGameMode Frontline_Bots gate).
- BotPhaseConfiguration only fills bot slots by StarshipClass (StarshipEntries Min/Max/Overflow); creep squadrons
  aren't bots, and attaching them means link-array surgery across subworlds.
- The providers carry no phase logic anyway (protect-own-capitals, hunt-enemy-creep, pings).
- In Frontline_Bots mode a pointer swap creep `TargetPrioritisationSettingsList[0]` -> a bot settings object
  (for example the kickoff Flex list) is possible, but it is still static.

Recommended order: test B's "Priority 0" variant once (it shows whether a 0 entry blocks a tag), then implement C.

## 3. Weapon-rule inputs

### 3.1 How AllowFire is computed [V]
Behaviour inputs come from the BehaviourProvider's property connections. Unfed inputs fall back to a
PropertyDefaultValue (PDV) whose DefaultValue is either a Bool node or an interface constant (-141474860 = Boolean
False, -141474864 = Int32 1, and so on, in the behaviour interface #313). That PDV.Value = InValue if set, else
DefaultValue, is [I] from the name `HasValueSet`. Bots feed the 16 inputs on all 9 providers from
interface constants (#121: -141474851 = True, -373703099 = False, -373703100 = 15.0) [V].

**Object ids differ between the two behaviours** (for example imperial Bool#57 = missile default, rebel Bool#57 =
DisableBombs default). Both columns:

| input | bots feed | creep get (default) | imperial PDV / default | rebel PDV / default | gates |
|---|---|---|---|---|---|
| AllowMissileFiring | True | **False** | #386 / Bool#57=F | #382 / Bool#49=F | Missile rule #11 |
| AllowCountermeasures | True | False (see below) | #367 / Bool#49=F | #366 / Bool#56=F | Countermeasure #6 |
| DisableSelfHeal | False | **True** | #366 / Bool#50=T | #381 / Bool#59=T | SelfHeal #13 |
| DisableResupply | False | True | #385 / Bool#63=T | #369 / Bool#58=T | Resupply #12, Loadout #288 |
| DisableAssaultShield | False | True | #374 / Bool#60=T | #368 / Bool#62=T | AssaultShield #2 |
| DisableSensoryArray | False | True | #368 / Bool#52=T | #383 / Bool#52=T | SensoryProbes #14 |
| DisableShieldBuff | False | False | #365 / Bool#62=F | #377 / Bool#60=F | AutoLockOverride #3, Loadout #289 |
| DisableBombs | False | False | #375 / Bool#51=F | #365 / Bool#57=F | Bombs #4, Loadout #289 |
| DisableSupportAbilities | False | False | #376 / const F | #380 / const F | AutoLock #3, Resupply #12, Loadouts |
| DisableAttack | False | False | #383 / const F | #373 / const F | Cannon, Ion, Missile, DogfightingAttack.Enabled |
| DisableWeapons | False | False | #364 / const F | #367 / const F | Cannon, Ion, Missile |
| DisableDogfighting | False | False | #373 / const F | #375 / const F | DogfightingAttack #271.Enabled |
| DisableEvade | False | False | #381 / const F | #384 / const F | Evade #273, Selector #303 |
| TailAttackModeForPlayerStarships | True | False | #377 / none | #386 / none | DogfightingAttack #271 |
| -268898707 (approach paths) | True | (unset) | direct | direct | ProximateAreas #294.UseApproachPaths |
| -1817211963 (float) | 15.0 | (unset, 10.0 in node) | direct | direct | CompareFloat#80.A -> Bool#59 tail-attack gate |

So creep already have cannons, ion, bombs, afterburner and the shield-buff/auto-lock rule. Missiles,
self-heal, resupply, assault shield and sensor probes are off by default. Countermeasures are on for creep only
through the fallback branch: `Or(AllowCountermeasures, And(Not HasValueSet, ShipClass==Siege or ShipClass==Support))`.
So an unfed Siege/Support ship (TIE bomber) is already allowed countermeasures [V graph]; whether creep report a
ShipClass is [I].

### 3.2 Missile rule (#11 in both behaviours) [V]
`AllowFire = And( Or( Or(Bool#61[reb #63]=F, IsBot), Or(Not IsInSquadron, IsSquadronLeader) ),
AllowMissileFiring, Not DisableAttack, Not DisableWeapons )`.
`MinTargetDistance = SelectFloat#395 {AI: 700, Bots: 200}`, InputSelect 0 (= AI); switched to Bots only by
CompareBool#68 (imp) / #70 (reb) when `Or(Bool#61, IsBot)` changes. `UseMinTargetDistance = true`;
`LockOnDelayTime = RandomFloat 1-3 s`; `MaxMissilesPerTarget = PDV default Int32 1` (bots don't feed it either).

Why flipping only imperial Bool#57 didn't produce missiles [I, consistent with data]:
- Only the creep squadron leader passes the Or gate (wingmen are InSquadron, not leader, not bot).
- The creep get the AI minimum of 700 m. With Defensive/500 m targeting, and the creep aiming setting
  RangeMax 1000 (ai_aimingabilitysettings_pvp_creep), the leader rarely holds a lock on a target at 700 m or more.

Torpedoes use the same rule: there is no torpedo rule type. TIE bomber AI (starship_tiebomber_ai) has 2
MissileWeaponControl (WeaponType 2132823915) + 2 BombsWeaponControl (2134491166) + countermeasure + self-heal
[V]. That the torpedo slot carries WeaponType 2132823915 is [I] (VerifyExactWeaponType=true).

### 3.3 Minimal value writes for creep missiles + torpedoes (bots unaffected)
1. AllowMissileFiring default -> true: imperial `Bool#57.DefaultValue` (BoolEntityData @36) and rebel `Bool#49`.
   Bots feed the input, so they are unaffected.
2. Missile minimum distance -> 200 m: `SelectFloat#395.InputSelect` (@48) := 1 ("Bots") in both behaviours. Bots
   already resolve to 200, so they are unaffected. Do not use `UseMinTargetDistance=false` (rule #11 @92): it would
   also remove the 200 m floor for bots.
3. Let wingmen fire (optional; without it only the leader fires): imperial `Bool#61` / rebel `Bool#63` := true.
   Bots are unaffected (IsBot is already true). Side effect on creep: the same Or drives CompareBool#72 (imp,
   TriggerOnStart) -> PropertyGate#15 = tail-attack mode, and And#27 -> DogfightingAttack.Enabled, so creep dogfight
   more like bots. It does not flip #395 by itself (CompareBool#68 is TriggerOnStart=false), so keep write 2.
4. Make them engage at missile range (section 2: Aggressive, radius >= 1000), otherwise targets stay inside 700 m or
   outside 500 m.

Not needed for missiles: AllowCountermeasures (imp Bool#49 / reb Bool#56), self-heal (imp Bool#50 / reb Bool#59 :=
false) and resupply. Listed here only so nobody flips the wrong Bool by id across the two files.

## 4. Open questions / to verify in flight
- Does a Priority-0 score block a tag, and are unlisted tags targetable? (Decides between options B and C.)
- Is TargetPrioritisationScore re-read live (needed for C) and TargetPrioritisationSettingsIndex (needed for D)?
- What exactly the creep squadron's `EngagementModeOverride` (Aggressive) overrides vs `TargetingRules.EngagementMode`.
- Do frigates/flagships move between zones (frigate logic MoveToNextZone1-3)? That changes the distances in 2.1.
- Bit 4 in tag 20 (creep score "AIfig+4") is still unnamed.
- pf_pvb_botlogic_reb was not dumped this session; mirroring of imp is assumed (its pvb wiring was verified).

## 5. Built (2026-10-24, branch creep-rework fcdee56; not flown yet)

**Superseded.** The debug-connection repoint below never switched lists in game: a squadron reads its list once, and the
connection never bound (section 6). The shipped design is in sections 6-8.
Option D, without growing the connection array: both exploit creep prefabs feed FrontlineIndex to a
PropertyDebugEntityData (#6, field 5685821), a debug display with no gameplay role. That one connection is repointed at
the squadron's TargetPrioritisationSettingsIndex (93732099), and the squadron gets a 7-entry settings list (one per phase
value). Evidence the index is re-read live: 16 property connections in the game's own data drive that field, all in
campaign levels and ldtoolbox prefabs (for example spf_c4m8_rep_awing_obattacker: a SelectInt `Out` -> index, Flags 3, two
settings lists); the 198 raw hits for the int were mostly the type layout embedded in each file, so only decoded
PropertyConnections count. Lists: stock creep scores minus generic tag 2, plus medcap (bit 29), flagship (22) and small cap
(28): offense 4/3/2, face-off 0/0/4, defense 0/0/2. Aggressive (the squadron's own override int) within 2000 m.
Missiles: writes 1-3 of 3.3, found by wiring (not object ids), applied on the 2-second scan only once a creep logic
prefab is loaded (the behaviours also drive campaign AI). Frostbite layout from Kyber's SDK (read as a reference only):
DataContainer refcount u32 at +16, flags +20; arrays carry their count at -4; Kyber itself grows arrays by allocating
new ones. The flight decides the open question in 4: whether priority 0 keeps creep off the frigates in the opener.

## 6. Flown and measured (2026-10-24 evening; branch creep-rework 80cdc7b)

- Aggressive creep, missiles and the morale change work in play. In the opener, creep TIE bombers flew straight at the
  enemy frigates in every flight: at 2000 m, at 1200 m, with creep missiles on and off.
- The squadron reads its target list once, when it registers (FUN_1417a2220 from 1417a29e0). It never reads the index
  again, and a property connection binds only to data whose Flags have bits 27-28 set (squadron #9 has 0). So the
  debug-connection repoint of section 5 never switched anything.
- Scores are read live on every evaluation (FUN_141617a00): the FIRST entry whose tags are all on the target wins, and a
  target that matches none scores 0, which drops it. So the capital entries now come first.
- creep_log measured, with the shared list in place: frigate 20010012 -> 0, flagship 00410012 -> 0, smallcap 10010013 ->
  4. NO list anywhere scored a frigate above 0, and only the squadron scorer (return 14182B50B) asked the creep's lists.
- The creep squadron has no TargetProviders, so nothing skips the priority lookup (agent's third report). So it isn't
  targeting that sends the bombers to the frigates. Open candidates: the phase's path (if the opener runs an attack
  path), or chasing other targets while the bomb rule (1417a9670, no tag filter) drops on any hostile body underneath.
- Phase source now: the AI controller's field handler FUN_1416feaf0 (vtable 0x14361c688; BehaviourIndex 875081154 ->
  entity +0xb4). In both creep prefabs SelectInt#11 maps phases 0..6 to behaviours 1,2,3,4,5,3,3. h_ctrlfield logs
  "creep: <faction> creep: behaviour N (phase ...)" and swaps the shared list's scores. The next flight's log shows
  which behaviour the opener runs.

## 7. The bomb raid (2026-10-24 night; creep-rework, after 80cdc7b)

- The frigate runs came from the bombs. bhv_aisquadron_basic_imperial's two IncomSquadronAILoadoutBehaviour nodes
  (#288/#289, aux slots One/Two) load the equipped aux weapon's StarshipWeaponConfig.BehaviourBlueprint (+120). The TIE
  bomber's starshipweaponconfig_bomb_standard_tiebomber imports incomsquadronaibombraidbehavior: one
  IncomSquadronAIBombingFlightBehaviour (RunOnlyForLeaders, EntryDistanceToPath 400, BombingDistance 50).
- Its target gather FUN_14182b910 (verified at 0x14182ba52: squadron entity -> data +0x28 -> cmp [data+0x20], 0) reads
  the squadron DATA's TargetingRules.EngagementMode live. Aggressive (0) with no current target raids the nearest
  bombable enemy within 10 km (SelectTargetSelectorData_Common range). Any other mode raids only the current target.
  The stock creep data is Defensive, which is why stock bombers only bombed frigates their path brought within 500 m.
- Fix: creep_mode_scan (2-second scan) writes data +32 = Aggressive on offense and the stock mode otherwise, as
  h_ctrlfield's phase asks. It writes only while the creep prefab is loaded, and only 5 s after the controller first
  reported a behaviour, so the scorer registers with the Aggressive rules first. Untested in game.

## 8. Shipped (2026-10-26; creep-rework, flown)

Flown by Pseudo on Yavin, Esseles, Sissubo and Fostar Haven; each line says what was seen.

- **Bomb raids by phase** (f27be3a): bombers took appropriate targets in the opener (mostly creep, sometimes the
  Raider) and the frigates on offense.
- **Hull and shields by creep ship type** (1410251): profile values on each type's own creep configs, stock packs
  `*_ai` and changed packs `*_ai_sp`; the DLL's creep defaults fill in only what the profile doesn't set. Raised
  Y-wing hull and shields let them make bombing runs on the cruisers before dying.
- **Creep-kill morale for changed packs** (9f8ffec, also on PR #31): IncomSchematicAttributesConfig holds each rule
  twice, in its registration lists and in typed IncomAttribute objects (Attributes +0x60, parallel AttributeNameHash
  +0x68 = djb2-xor of the CamelCase name, Value +0x20). The match logic's attributes entity binds once, at init: from
  the registered set when it finds it, otherwise from the objects, which held the stock 3/1. Both copies are written
  now; kills of changed-pack creep give the profile's value (15 seen for interceptors in the opener).
- **Torpedoes** (54facd3): Y-wing and TIE bomber packs get a StarshipConfigCollection copy (ClassInfo 0x144a156a0,
  Configs +0x20) holding the creep hull and shields plus the StarshipWeaponSlotConfig (0x1449fc3f0) entries of the
  bots' torpedo collections (`starshipconfigcollection_ywing_ai_pvb_bot_torpedo_bomb`,
  `starshipconfigcollection_tiebomber_ai_pvb_bot_bomb_torpedo_heavyhull`). The sources are found among ALL of a
  level's objects by ClassInfo, then name: the first build searched only partition roots and found nothing. ClassInfos
  found as an exe qword equal to the type description's address (it reproduces the known health 0x144a8ca00 and shield
  0x144a8c980). Y-wings fire one torpedo on the approach, then bombing runs; sometimes at other creep.
- **The bots' handling** (5e24f4f): the same loadout copy, with the bots' movement config (`*AI_BOT`, 0x144ab56a0); for
  stock packs too, keeping their own creep configs. Pseudo: creep "much more active", "I almost got killed".
- **Per ship type**: all creep went for the attacker's new corvette on defense (Esseles), and fighters and
  interceptors swarmed it and got wrecked. The squadron's candidate scorer FUN_14182ade0 (12 integer arguments: the
  second the ship choosing, the eighth the list; the scorer itself type-checks the ship against ServerStarshipEntity,
  ClassInfo 0x1449f5d80) is hooked; a ship that isn't a bomber gets a second list where the corvette or Raider scores 1
  instead of 4, in the opener and on defense. Bombers keep 4. Telling bombers apart took three tries:
  - radar tags read at a candidate's offsets (+0x5d0 | +0x5cc | [+0x30]+0x38): garbage on the ship (ffffffff,
    83ef7678...), so nearly every ship got the bombers' list (Sissubo flight);
  - a data pointer at +0x28: small numbers, not pointers, so every ship got the other list (next flight);
  - the spawn: the server's starship spawn (FUN_1416bcab0, hooked) is handed the spawn entity, whose data (+0x38) is the
    spawn point (StarshipSpawnReferenceObjectData) - measured: it matches the spawn points creep_scan rewrites - and
    returns the ship. Ships spawned from the Y-wing and TIE bomber packs' spawn points are kept; the scorer checks the
    ship against them. Flown on Yavin: all 20 bomber ships scored with the bombers' list and all 50 others with the
    other list, each traced in the log to its spawn point; bombers and no other creep went for the Raider and corvette.
- **Torpedoes for capital ships** (d9a0ce0): the AI torpedo weapons (`starshipweaponconfig_highyieldtorpedo_{ywing,
  tiebomber}_ai_instalock`, StarshipWeaponConfig 0x1449fc370) lock through WeaponFiring (+96), a TargetLockWeaponFiring
  data with TargetTags at +228 (a mask the target must carry all of; 0 = anything). Set to 2, the capital-ship tag, only
  while no pf_pvb_botlogic is loaded (the bots share these weapons). Flown: no torpedoes at creep, and a Y-wing landed
  one on the Raider in the opener.
- **App:** the mouse wheel over a focused dropdown changed its selection, so scrolling down to the hull and shield
  boxes rolled the pack dropdowns to their last ship (U-wing, Reaper) and saved it (30070e7). Reproduced with
  `--test-wheel`; the wheel on a closed dropdown now scrolls the page.

## 9. Torpedoes, wingmen and damage to capital ships (2026-10-27; branch claude/creep-torpedo-runs, flown)

Fifteen flights in one evening, measured with two new diagnostics: `track_log` (R_TRACK 11 records twice a second: each
bomber's position, its assigned target, the distance, the target's tags; `tools/track_report.py`) and
`capital_damage_log` (each capital hull's and shield's hits by attacker, with the drop rule's verdict).

- **Tried and dropped: torpedo runs.** The bomb raid (IncomSquadronAIBombingFlightBehaviour, data ClassInfo 0x144a48340:
  EntryDistanceToPath +40, PointsApproachTolerance +48, DistanceAfterBombing +52, BombingDistance +56,
  AfterburnerThreshold +68, RunOnlyForLeaders +72) loads from any weapon's BehaviourBlueprint (+120), so the creep
  torpedoes got it, retuned. But the run line keeps the target outside the AI torpedo's lock cone, and once their target
  is a capital ship the bombers' own attack makes fair torpedo passes. Also dropped: opening the Raider's and
  corvette's single damage section to runs (DamageSectionComponentData 0x144a245c0, ClearFaces +292..+297; none clear in
  stock, so no raid ever starts on them).
- **The TargetTags = 2 lock filter** (section 8) was dropped here on the reading that it blocked locks on frigate and
  flagship parts (one torpedo a flight while it was in). That reading was wrong: the cause was the 500 m minimum lock
  distance. The filter is back since 2026-10-28 (section 10), and torpedoes lock onto the Star Destroyer with it.
- **Wingmen: one attacker per target.** Each faction's creep logic is ONE squadron for all three packs, and its
  IncomSquadronTargetingRules (the scorer's seventh argument; 56 bytes: EngagementMode +0, DefaultMaxAttackersPerTarget
  +4 = 1 for creep, OtherLeaderTargetScoreWeightMultiplier +32 = 0.2, the mulss at 0x14182b369) allow one ship per
  target: 600 of 600 assigned targets had exactly one bomber. The scorer FUN_14182ade0 loops over all candidates (third
  argument, 24-byte entries) and appends a 0x38-byte entry per candidate to its tenth argument, a vector (begin +0,
  end +8): the ship +0, the candidate's radar target +0x10, the attacker cap +0x1c (the rules' +4, or a per-target
  override; the scorer only tests it for 0). The choice is made from those entries later. Shipped: bombers get a rules
  copy with cap 15 and multiplier 1.0, and after the call the new entries whose candidate isn't a capital ship (tag
  bit 2) get the stock cap back. Flown 2026-10-28: bombers go for capital ships first, then each other.
- **Bombers prefer capital ships**: their list scores the phase's capital ships 10/9/8 on offense and the corvette or
  Raider 10 otherwise, above the stock turrets 5 and starfighters 3 (at 4/3/2 a nearby starfighter tied the flagship).
- **Damage to capital ships: the ignore-AI rule.** Every PvP level connects a level field (default true) into each
  capital ship's IgnoreAIDamage (interface field -1337701026; CapitalShipHealthComponentData 0x144a24a40 +144, only the
  light cruiser sets it in its own data). The component's property handler (vtable 0x14362d880 slot 2, 0x1417d9450)
  stores it at +0x1c8. FUN_1417da0e0(capital health component, hit record) drops a hit when that's set, the record's
  flags (+0xa4) lack 8, and its attacker (+0xb0) is neither a player ([[+8]] - 8) nor a ServerStarshipEntity (cast
  0x1412631b0 from [[+0x40]] - 8) whose +0x338 is set; creep never are, the vs-AI bots and players are. Callers: the
  hull's apply 0x1417dbe90, the shield bubble (vtable 0x14362d508 slot 43, with the ship's hull component [+0x570]), the
  turrets 0x1416ed5b0, the weak points 0x1417dfc50. The mod replaces the check whole (same reads, same order) and,
  with aggressive creep, keeps a creep starship's hit. Flown: hulls and shields take creep damage.
- **`capital_damage`** scales creep hits on hulls (the apply record's +0x88, which the team filter 0x14168ffb0 starts
  from) and shields (the info's +0x84 direct, +0x74 blast); 30% by default. Health getter: vtable +0x108,
  0x140412280, the float at +0x20, for both.
- **`torpedo_lock_min`**: TargetLockWeaponFiringData MinLockDistanceAttribute (+184 -> IncomAttributeFloat value +32,
  id +24 -> +32) and the weapon config's float registration list (+0x40). Bombers close to 200-300 m and couldn't
  lock at the stock 500 m. The stock lock cone (get 10, keep 20 degrees) proved fine once they could.
- **Bomb runs** for aggressive creep that keep their bombs: 800 m in, 75 m tolerance, 425 m out, 100 m above the
  hull, the afterburner beyond 925 m (stock 400/50/250/50/500). Flown: they stop flying into their target.
- **Open, for later:** AI lasers seemed to fire at about 45 degrees off the nose (by eye). The fire cone measured in
  section 10 is 30 degrees full for the creep Y-wings, so the 45 is unexplained.

## 10. Capital-only targeting, the passive damage, and why torpedo bombers stop firing at a flagship (2026-10-28, flown)

- **Settings:** `[creep] bomber_capital_only` (the bombers' list keeps only the phase's capital ships; the app's
  "Bombers attack"); `capital_damage = 0` keeps the stock rule (h_aicheck exempts creep only above 0); the TargetTags 2
  lock filter is back while bombers put capital ships first (bombers that fight starfighters had torpedoed each other),
  not with capital-only, where every target is a capital ship already.
- **The capital ships' passive damage** (DamageOverTimeSettings, FUN_1415b3110) calls the hull apply 0x1417dbe90
  (at 0x1415b3693) with a record at rbp+0x50 whose flags +0xa4 are 0x08 (0x1415b335f; 0x40 is added when it splits
  between ships, and the damage is then read from elsewhere) and whose attacker resolves to an unmarked starship. The
  ignore-AI rule keeps flag 8 before it looks at the attacker, so the exemption never touched it, but `capital_damage`
  scaled it as creep: the corvette's single-target passive damage came through at 30%. Fixed: a flag-8 hit is its own
  class in `capital_damage_log` and is never scaled.
- **Torpedo bombers stop firing at a flagship; bombs don't.** On the Star Destroyer, Y-wings fired well for about a
  minute and then hardly at all, with 15 still holding it as their target to the end. `fire_log` (h_fire on
  FUN_14182a310; see the hook's comment for the node map, the agent map at [0x1441c8a40]+0x26c0, the settings and the
  raycast) showed why: line of sight blocked once in about 100,000 checks; the cone failed. The AI fires only with the
  target's point inside a 30-degree full cone (settings +0x38; range band 1-10000 m). A flagship's point is its centre,
  inside the hull. In the first minute, coming in nose-on, 35-68% of checks passed (point a median 26 degrees off the
  nose when they failed); after that 7-10%, the point a median 80-100 degrees off, the Y-wings 380-500 m from it and
  pressed against the hull below it. Shields and destroyed sections only lined up with the time it takes them to close
  in. Bombs drop on the bomb run's own rule and don't go through this check: flown with capital-only bombers, they hit
  the Star Destroyer through three shield regenerations and took its hull to 696. A wider cone alone wouldn't bring the
  torpedoes back (their lock cone is a separate 10 degrees); the fix would be flying, the way the bomb run does.
  Set aside: bombs are the flagship weapon; torpedoes work on the corvette, the Raider and the cruisers.
