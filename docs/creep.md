# Creep

In Fleet Battles each faction has three packs of five AI "creep" ships: X-wings for the Rebels, TIE fighters for the
Empire. As shipped they fly set paths, engage only what comes within 500 m of their path, and never fire anything but
lasers, so they matter mostly as morale to farm. These settings make them part of the fight. They're all in the
profile, under **Creep packs** on the app's Rules and fixes page, all off unless turned on, and all part of the
fingerprint: everyone in a match needs the same creep.

The vs-AI bots of co-op and solo vs AI aren't changed: the creep settings borrow some of their configs, but never write
to them.

## Settings

```ini
[creep]
; each pack's ship, numbered along the line the packs spawn in (missing = the stock ship)
; rebel:    xwing ywing awing uwing
; imperial: tiefighter tiebomber tieinterceptor tiereaper
rebel_pack1=ywing
rebel_pack2=awing
imperial_pack1=tiebomber
imperial_pack2=tieinterceptor
; 1 = creep engage like the vs-AI bots, with targets that follow the phase
aggressive=1
; how far aggressive creep look for targets, in metres (500 to 3000; 2000 is the bots' value)
radius=2000
; how much of their damage aggressive creep do to capital ships, in percent (0 to 100; 0 = none, as in stock)
capital_damage=30
; 1 = aggressive bomber creep attack capital ships only; 0 = capital ships first, starfighters when none is in reach
bomber_capital_only=0
; 1 = creep fire missiles (and torpedoes, when they carry them)
missiles=1
; 1 = Y-wing and TIE bomber packs carry torpedoes instead of bombs
torpedoes=1
; how close the bombers' torpedoes may lock on, in metres (0 to 1000; 500 is stock)
torpedo_lock_min=500
; 1 = creep wingmen attack and fly the bombers' runs like their squadron's leader, and bombers may all attack one capital ship
wingmen=1
; 1 = every pack flies with the vs-AI bots' handling for its ship
bot_handling=1
```

**Ship per pack.** Any of the faction's AI ships, in any combination; dropships too. The B-wing and TIE Defender have
no AI version, so they can't be creep.

**What each creep ship carries.** Creep fly the AI ship's own weapons: the stock creep loadout changes only hull and
shields, and the mod's loadouts change no weapons except the bombers' torpedoes (below). Carrying an aux isn't the
same as using it, though: the creep's squad behaviour switches some aux off, and the mod only ever switches missiles on.

| Ship | Primary | Aux 1 | Aux 2 | Countermeasure |
|---|---|---|---|---|
| X-wing | lasers | homing missile (**missiles**) | squad self-heal (never) | flares |
| Y-wing | lasers | standard bombs (always) | assault shield (never) | chaff |
| A-wing | rapid-fire lasers | anti-fighter homing missile (**missiles**) | sensor array (never) | flares |
| U-wing | ion cannons | shield buff for allies (always) | resupply droid (never) | sensor scrambler |
| TIE fighter | lasers | homing missile (**missiles**) | squad self-heal (never) | flares |
| TIE bomber | lasers | standard bombs (always) | assault shield (never) | chaff |
| TIE interceptor | rapid-fire lasers | anti-fighter homing missile (**missiles**) | sensor array (never) | flares |
| TIE Reaper | ion cannons | shield buff for allies (always) | resupply droid (never) | sensor scrambler |

- **always:** creep use it as shipped (stock creep never fly these ships, so you won't have seen it before).
- **missiles:** only with **missiles** on; stock creep never fire it.
- **never:** switched off in the creep's behaviour, with any settings.
- **With torpedoes on,** Y-wing and TIE bomber packs carry torpedoes instead of bombs and their assault shield (fired
  only with **missiles** on), and the TIE bomber's chaff becomes flares.
- **Countermeasures:** creep aren't allowed them, except possibly the bombers and dropships, which the rule lets through
  when nothing else decides; nobody has seen creep use one.

The names are read from the game's weapon configs (`starshipweaponconfig_homingmissile_xwing_ai` and so on), so they
may differ from the in-game names.

**Hull and shields, by ship type.** The **Creep hull and shields** boxes set each creep ship type's hull and shields,
for every pack flying that ship (the same ship with different hull in one match would feel strange). The defaults are
the stock creep's share of the player ship's: about a quarter of the hull and half the shields (the creep X-wing is
300 / 400 against the player's 1200 / 800). They're ordinary profile values on the configs that ship's creep use:
`starshiphealthdamagedata_xwing_ai.maxhullhealth` for a stock pack, `starshiphealthdamagedata_ywing_ai_sp.maxhullhealth`
for a changed one (the single-player AI configs, which only the campaign uses otherwise), and the same for
`starshipshielddata_*`.

**Aggressive creep that keep to the phases.** Creep engage everything within the engagement range, the way the vs-AI
bots do, and their targets follow the match's phase:

| Phase | Capital ships the creep go for |
|---|---|
| Opener, transitions | the enemy corvette or Raider: bombers go for it first; fighters, interceptors and dropships only once no starfighter is in reach |
| Their side on offense | the enemy frigates, then the flagship once the frigates are gone |
| Their side on defense | the enemy corvette or Raider (the attacker brings a new one each offense phase): bombers go for it first; fighters, interceptors and dropships only once no starfighter is in reach |

Players and other creep are targets in every phase. Bombers rank the phase's capital ships above everything else and
fight starfighters when none is in reach. With **Bombers attack: Capital ships only** (`bomber_capital_only`) they attack
nothing else, and have no target while the phase offers no capital ship. Bombers' bomb runs follow the same phases: on offense they raid the
nearest enemy capital ship; otherwise only their current target, which can be the corvette or Raider but never a frigate
or the flagship. Bombers that keep their bombs start their runs further out than stock (800 m instead of 400) and fly
them higher over the hull (100 m instead of 50), so they don't fly into their target.

**Damage to capital ships.** In stock Fleet Battles, creep can't hurt capital ships at all: every PvP level sets its
capital ships to ignore damage from AI starships that aren't the vs-AI bots, hull, shields, turrets and weak points alike.
With aggressive creep their hits count, and on hulls and shields they're scaled by **Damage to capital ships**
(`capital_damage`, beside the engagement range in the app), 30% by default: at 100% a creep torpedo hits as hard as a
player's, and fifteen bombers take a capital ship down very fast. At 0% the stock rule stays, and creep do capital ships
no harm at all. Their hits on turrets and weak points count in full. Other AI
damage, such as capital ships' turrets firing on each other, keeps the stock rule.

**Missiles and torpedoes.** Creep fire their missiles, and the bombers their torpedoes, as the vs-AI bots do (stock creep
never do). This also gives creep the bots' tail-chasing dogfighting. Bombs need no setting: bomber creep always drop them.
**Minimum lock distance** (`torpedo_lock_min`) sets how close the bombers' torpedoes may lock on. Stock is 500 m, but
bombers attacking a capital ship close in to 200-300 m and then can't lock, so a shorter distance lets them fire far more
often. With **torpedoes**, Y-wing and TIE bomber packs carry
the vs-AI bots' torpedoes instead of bombs (the TIE bomber also gets the bots' flares), with the creep's own hull and
shields. They fire them as they attack, and the torpedoes lock only onto capital ships, so bombers that also fight
starfighters save them for capital ships: on a capital ship, the bombers' own attack makes a fair torpedo pass, firing,
pulling out and coming back. The torpedo weapons are the vs-AI bots' too, so the lock filter and distance are only set
while no bots are loaded, which is to say in Fleet Battles. (Until v0.5.0 bombers carried
torpedoes as well as bombs: Y-wings fired one torpedo on the approach and then switched to bombing runs.)

**Wingmen like their leader.** In stock, two things are a creep squadron leader's alone: attacking on its own, and the
weapon behaviours the squad AI runs for its aux weapons, which is where the bombers' runs come from. The wingmen follow
the leader in formation. With **wingmen**, they attack on their own, and bomber wingmen plan and fly their own runs
(each picks its own line across the target). And in stock a creep squadron (one per faction, all three packs) lets only
one ship attack each target, so wingmen were left with nothing; with **wingmen**, bombers may all attack the same capital
ship, while every other target keeps the stock rule of one attacker each, so a squadron doesn't pile onto a player. The
run behaviour is the vs-AI bots' too, so its part is only set while no bots are loaded; the bots' own gates don't change
(they feed the inputs the creep-only switch reads).

**The bots' handling.** Every pack flies the vs-AI bots' movement for its ship (their drift, roll and catch-up tuning)
instead of the AI ship's own, which makes creep noticeably sharper in a dogfight. Hull and shields stay the creep's.
Bots follow the player ship's movement values, so with this on, creep follow a profile's movement changes too.

**Morale.** What a creep kill is worth is a match rule: under **Match rules** in the app,
`incomschematicattributesconfig_fleetbattle.towpointsgainedforcreepkillwhileondefense` (3 as shipped) and
`...whileonoffense` (1). It counts for every creep, changed packs included.

## How it works

The game's own data does nearly all of it; the mod only rewires and copies.

- **Packs** are found through the level's link connections, from each creep logic placement to its two spawn points; a
  changed pack's spawn points get the chosen ship's AI blueprint, and a loadout when one is asked for.
- **Loadouts** (torpedoes, the bots' handling) are copies of the stock creep collection whose configs are the creep's
  hull and shields plus the bots' torpedo weapon slots and/or the bots' movement config. The game's own collections are
  left as they are.
- **Phase-following targets.** A creep squadron reads its target list once, when it spawns, but reads the scores
  inside the list every time it chooses a target. The mod gives each faction's squadron one list and swaps its scores
  as the phase changes. The phase comes from the creep logic itself: the game switches the creep's flight path by
  phase through the controller's BehaviourIndex, and the mod watches that switch.
- **Bomb runs** come with the bombs: the bomb weapon carries its own AI behaviour, a bomb raid, which raids the
  nearest enemy capital ship when the squadron is Aggressive and has no target, and only the current target otherwise.
  The mod sets the squadron to Aggressive on offense and back to its stock Defensive mode otherwise.

The research behind it, with the addresses, offsets and what was ruled out, is in
[notes/creep-ai.md](../notes/creep-ai.md).

## Diagnostics

`creep_log=1` under `[server]` in `server.ini` logs the creep squadrons' target-score lookups, and any target list
that scores a frigate above zero, with the code that asked. The log always says what each pack flies, its loadout,
the phase the creep are in, and when bomb raids switch.
