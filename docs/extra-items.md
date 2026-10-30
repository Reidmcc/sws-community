# Extra countermeasures and auxiliaries

A profile can let a ship carry another ship's countermeasures and auxiliaries, for example a tractor beam on the TIE Defender. Pick them in the settings app's **Extra countermeasures and auxiliaries** grid on Rules and fixes: one row per item, one column per ship. Click an open cell to give that item to that ship, and click again to take it back. The ship's hangar and briefing room then offer it like any of its own.

The grid also takes a ship's own items away: click a "stock" cell and it reads "removed". The ship's hangar no longer offers that item.

In the profile it's an `[extra_items]` section, one line per ship, with items by the game's own names. An item taken away has a `-` in front:

```ini
[extra_items]
tiedefender=tractor_beam,missile_torpedo
awing=aux_advancedpowersystem,-stealth
```

The grid shows each item by its in-game name, with both names where the factions differ (Quick-Lock / Anti-Starfighter Missile). The profile file uses the game's internal names, which the grid shows on hover. The ships are `xwing`, `ywing`, `awing`, `uwing`, `bwing`, `tiefighter`, `tiebomber`, `tieinterceptor`, `tiereaper` and `tiedefender`. Hovering an item's name in the grid shows its name for the profile file. The setting is part of the fingerprint, so everyone in a match offers the same items.

## How it works

Each ship's hangar offers the items its customization data lists. As that data loads, the mod adds the chosen items to the ship's list. This happens in every player's game, since everyone runs the mod.

- **The item is the other ship's own,** with that ship's values. To tune a borrowed item, change it under the ship it comes from.
- **Where several ships carry an item,** the one used is the first of the same faction to carry it, in the order above. If none of that faction does, it's the first of the other faction. So the Defender's tractor beam is the Reaper's, not the U-wing's.
- **An item the ship already carries** is left as it is.
- **Ownership:** a borrowed item is owned through the unlock list, so it needs `all_unlocked=1` in `server.ini`, which release builds have. The list includes the countermeasures and auxiliaries that are free defaults on their own ships, like the laser mines or the resupply droid. Without them a borrowed default showed the unlock icon, and picking it sent a purchase request that the game's backend refused.
- **Primary weapons aren't offered.** Lasers fire from the ship's own mount points. Countermeasures and auxiliaries fire forward, or downward for bombs, so they don't need mounts.

## Taking items away

- **Every ship keeps at least two of its stock auxiliaries and one of its stock countermeasures,** so the game can always fill its countermeasure slot and both aux slots. In the grid, the last ones a ship keeps are greyed, and clicking one says why. The mod ignores a hand-edited profile that asks for more, with a line in the log.
- **A default taken away** (each ship has one default countermeasure and two default auxiliaries, which fill empty slots) is replaced by the first stock item of the same kind the ship still has. That item becomes the default.

## Loadouts that hold an item the ship doesn't offer

A loadout keeps each item by its id. A loadout made where a ship could carry another ship's item, or before one of its own items was taken away, can hold an item that this match's profile doesn't offer that ship. The game would leave that slot empty. The mod gives the slot the ship's default instead, for every player and with any profile. The loadout itself keeps the item, so it comes back in a match that allows it.

## What can't be given

- **The B-wing's Gyro/Aux Control Module** (`aux_bwing_rotation`): no other ship has the parts it turns.
- **The two beams,** the B-wing's Ion Beam (`sec_ionbeam`) and the Beam Cannon the Y-wing, TIE Bomber and B-wing carry (`beam`):
  their effects are drawn from their own ship's laser mounts. Flown on an X-wing, both worked, but the beam started in
  front of the cockpit, joined to nothing. They can be revisited if mount points are ever worked out for primary weapons.
- **The Y-wing's Automatic Ion Cannon** (`laser_turret`): it sits on the Y-wing's turret mount, on top of the ship. On a TIE Fighter it didn't work.
- **The TIE Defender's APS** (`aux_advancedpowersystem`) on the TIE Fighter, Bomber or Interceptor: it needs shields, and those three have none.

The grid shows these as a dash. The mod also leaves them out of a hand-edited profile, with a line in the log.

**Clear the grid**, under it, takes every extra item out of the profile at once, after asking.

## Tested

The Reaper's tractor beam on the Defender was flown on 2026-10-30. It showed in the hangar and the briefing room, and worked in flight.

Also flown on an X-wing (2026-10-30): stealth and the missile corrupter countermeasure, which worked, and the two beams, which worked but looked wrong (above). Later the same day: the Defender's power system on the A-wing, across factions, worked; the laser turret on a TIE Fighter didn't.
