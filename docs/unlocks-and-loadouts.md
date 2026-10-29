# Unlocks and loadouts

**All-unlocked** (`all_unlocked=1` in `server.ini`) reads `unlock_ids.txt` beside the DLL. That file is generated from the game's data, and it ships with release builds.
- **What it unlocks:** every ship component and cosmetic, pilot cosmetics and emotes included.
- **Loadout slots:** it also opens all five loadout slots on every ship. The game would otherwise open slots 2 to 5 by player rank.

**Local loadouts** keep `loadouts.txt` beside the DLL. Each line is one saved loadout (a pilot, a ship, or the profile's default ships): its name, then its item ids. Loadouts you save aren't sent to EA. To start again from the loadouts EA has, delete the file.
