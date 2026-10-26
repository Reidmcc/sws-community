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
; 1 = creep fire missiles (and torpedoes, when they carry them)
missiles=1
; 1 = Y-wing and TIE bomber packs carry torpedoes as well as bombs
torpedoes=1
; 1 = every pack flies with the vs-AI bots' handling for its ship
bot_handling=1
```

**Ship per pack.** Any of the faction's AI ships, in any combination; dropships too. The B-wing and TIE Defender have
no AI version, so they can't be creep. A changed pack flies its AI ship with lasers, and bombs on the bombers.

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

Players and other creep are targets in every phase. Bombers' bomb runs follow the same phases: on offense they raid the
nearest enemy capital ship; otherwise only their current target, which can be the corvette or Raider but never a frigate
or the flagship.

**Missiles and torpedoes.** Creep fire their missiles, and the bombers their torpedoes, as the vs-AI bots do (stock creep
never do). This also gives creep the bots' tail-chasing dogfighting. With **torpedoes**, Y-wing and TIE bomber packs carry
the weapons of the vs-AI bots' torpedo loadout (torpedoes and bombs; the TIE bomber also gets flares) with the creep's
own hull and shields. In flight, Y-wings tend to fire one torpedo on the approach and then switch to bombing runs.

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
