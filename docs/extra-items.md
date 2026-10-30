# Extra countermeasures and auxiliaries

A profile can let a ship carry another ship's countermeasures and auxiliaries, for example a tractor beam on the TIE Defender. Pick them in the settings app's **Extra countermeasures and auxiliaries** grid on Rules and fixes: one row per item, one column per ship. Click an open cell to give that item to that ship, and click again to take it back. The ship's hangar and briefing room then offer it like any of its own.

In the profile it's an `[extra_items]` section, one line per ship, with items by the game's own names:

```ini
[extra_items]
tiedefender=tractor_beam,missile_torpedo
awing=aux_advancedpowersystem
```

The ships are `xwing`, `ywing`, `awing`, `uwing`, `bwing`, `tiefighter`, `tiebomber`, `tieinterceptor`, `tiereaper` and `tiedefender`. Hovering an item's name in the grid shows its name for the profile file. The setting is part of the fingerprint, so everyone in a match offers the same items.

## How it works

Each ship's hangar offers the items its customization data lists. As that data loads, the mod adds the chosen items to the ship's list. This happens in every player's game, since everyone runs the mod.

- **The item is the other ship's own,** with that ship's values. To tune a borrowed item, change it under the ship it comes from.
- **Where several ships carry an item,** the one used is the first of the same faction to carry it, in the order above. If none of that faction does, it's the first of the other faction. So the Defender's tractor beam is the Reaper's, not the U-wing's.
- **An item the ship already carries** is left as it is.
- **Primary weapons aren't offered.** Lasers fire from the ship's own mount points. Countermeasures and auxiliaries fire forward, or downward for bombs, so they don't need mounts.

## What can't be given

- **The B-wing's wing rotation:** no other ship has the parts it turns.
- **The B-wing's ion beam** (`sec_ionbeam`): its effect is drawn from the B-wing's laser mounts. Flown on an X-wing, it
  worked, but the beam started in front of the cockpit, joined to nothing.
- **The TIE Defender's advanced power system** on the TIE Fighter, Bomber or Interceptor: it needs shields, and those three have none.

The grid shows these as a dash. The mod also leaves them out of a hand-edited profile, with a line in the log.

## Tested

The Reaper's tractor beam on the Defender was flown on 2026-10-30. It showed in the hangar and the briefing room, and worked in flight.

Also flown on an X-wing (2026-10-30): stealth, the missile corrupter countermeasure, and the beam (`beam`, the Y-wing's). All three worked. The beam's effect looked wrong, starting in front of the cockpit and joined to nothing, as the ion beam's did.
