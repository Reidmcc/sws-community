# Players against capital ships

These settings control how much players' hits, and the vs-AI bots', hurt capital ships. They're in the app's section of that name, on Rules and fixes, and under `[rules]` in the profile:

```ini
[rules]
; players' (and vs-AI bots') damage to capital ship hulls, in percent (0-100; 100 = stock)
player_capital_hull_damage=100
; whether their hits damage capital ship shields (1 = stock)
player_capital_shields=1
```

Each setting is in the fingerprint when it isn't stock.

## What each covers

- **`player_capital_hull_damage`** (percent) scales everything that reaches a capital ship's hull: lasers, ion and ordnance alike. On the Star Destroyer and MC75 that includes the power, shield and targeting subsystems, which are sections of the hull.
- **`player_capital_shields`** (0 or 1) covers the capital ships' shield bubbles. Off means the shields ignore players' hits.

Creep are scaled by their own setting ([creep.md](creep.md)).

Turrets and weak points aren't covered:
- **Weak points:** a destroyed weak point's damage to the hull is the value `capitalshipweakpointssettings.damagedealtondestructionattribute`, 5000 as shipped.
- **Turrets:** a destroyed turret takes about 200 off its ship's hull, whoever destroys it. That was measured in flight; it isn't a value.

## A match where only the creep sink capital ships

Set:
- the hull damage to 0%;
- the shields off;
- the weak-point value above to 0;
- the capital ships' passive damage off: `damageovertimesettings.targethealthratiotostopattribute` at 1 stops it. At 1 on every config, the corvette's and Raider's attack stops too.

Then the bomber creep do the capital ships' damage, apart from those turrets, and the players fight over them.

The release ships this mode as the profile **Bomber Escort Fleet Battle** (`mod/release/profiles`):
- players at 0%, with shields off;
- Y-wing and TIE bomber packs bombing capital ships only, at 40% damage;
- passive and weak-point damage off;
- sturdier bomber creep.

It's a starting point for tuning, not a balanced final word.
