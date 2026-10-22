STAR WARS: Squadrons community server mod
=========================================

Play Squadrons with other people without EA's servers: one player hosts a match on their own PC, the others join it.
Project page, source and issues: https://github.com/Reidmcc/sws-community

This is early and experimental. Two-player Fleet Battles and co-op vs AI have been played; expect rough edges.


Before you start (please read)
------------------------------
- You need your own copy of the game, installed through Steam.
- The game runs without EasyAntiCheat (started directly, not through EA's launcher), so EA's official online modes are
  not available in that session. The launcher refuses to start if EasyAntiCheat is running.
- The game still signs in to EA's account services. Loading a mod into it carries some risk to your EA account. We think
  it is low (anti-cheat isn't running, and nothing touches matchmaking or official servers), but nobody can promise it
  is zero. It's your call.
- Nothing in your game folder is changed, except a steam_appid.txt you add yourself (below), which you can delete any time.
- To remove the mod: delete this folder, and delete steam_appid.txt from the game folder.


One-time setup
--------------
1. In your game folder (Steam: right-click Squadrons > Manage > Browse local files), create a text file named
   steam_appid.txt containing just:  1222730
2. Put this folder anywhere outside the game folder, for example on your Desktop.
3. A network path between the players. The tested way is Tailscale (tailscale.com, free): everyone installs it, and the
   host shares their PC with the others in Tailscale's admin page. The host's address is their Tailscale IP (100.x.y.z).


Settings: sws_settings.exe
--------------------------
Double-click sws_settings.exe for everything below without editing files:
- Play: host (pick the match type) or join (type the host's address, keep favorites), then Launch.
- Profile and values: pick, make, copy, rename, import or delete profiles; turn the movement rules on or off; and change
  any of the game's values (speeds, shields, hull, weapons, components...) for every ship, each with its default and the
  range it's allowed. Reset one value or all of them to default. The fingerprint at the top must match everyone else's.
The values list fills in as you play: the mod records each part of the game the first time it loads it.

Or by hand (server.ini, open it in Notepad):
- HOST: leave "join=" empty, and pick the mode on the "match_mode=" line: fleet (Fleet Battles), coop (co-op vs AI),
  solo, or dogfight.
- JOIN: put the host's address after "join=", for example  join=100.101.102.103
- PROFILE: "profile=" picks the movement rules. Everyone in a match must use the same profile, or the host's game refuses
  the connection on purpose.
    Default        the game as shipped
    NoMultidrift   pinballing allowed, multidrifting removed
    AsIntended     both exploits removed
  Profiles are files in the profiles folder; you can copy one and edit it, or use sws_settings.exe.


Playing
-------
1. Steam running, Squadrons closed.
2. Click Launch in sws_settings.exe and confirm in the window it opens. The game starts.
   (Or double-click sws_launch.exe, read what it says, type YES and press Enter.)
3. In the game, start a SOLO CUSTOM MATCH VS AI, on the host and on every joiner. The host's becomes the real match
   (per match_mode); a joiner's connects to the host instead. Joiners: start once the host's lobby is up.
4. Every ship component is unlocked, and EA's error 327 shouldn't appear.


If something goes wrong
-----------------------
The logs folder beside this file has a server_<date>_<time>.status.txt for each launch. It records what the mod and the
game's connection did (addresses, packet counts, timings), nothing else from your PC. Attach the newest one to an issue
on the project page, with what you saw on screen.


License: CC BY-NC-SA 4.0 (non-commercial). Not affiliated with or endorsed by Electronic Arts, Motive Studio or Lucasfilm.
