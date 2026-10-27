STAR WARS: Squadrons community server mod
=========================================

Play Squadrons with other people without EA's servers: one player hosts a match on their own PC, the others join it.
Project page, source and issues: https://github.com/Reidmcc/sws-community

This is early and experimental. Two-player Fleet Battles and co-op vs AI have been played; expect rough edges.


Before you start (please read)
------------------------------
- You need your own copy of the game, from Steam or the EA app. Have it from Epic instead? Run sws_check.exe (it only
  reads the game's exe) and attach its report to issue #17 on the project page: that tells us whether your copy is the
  same build, the first step to supporting it.
- The EA app's copy is new in this version. It has been tested as far as the game's menu, with the mod working there,
  but not yet in a match. Please tell us how it goes.
- The game runs without EasyAntiCheat (started directly, not through EA's launcher), so EA's official online modes are
  not available in that session. The launcher refuses to start if EasyAntiCheat is running.
- The game still signs in to EA's account services. Loading a mod into it carries some risk to your EA account. We think
  it is low (anti-cheat isn't running, and nothing touches matchmaking or official servers), but nobody can promise it
  is zero. It's your call.
- Nothing in your game folder is changed. A Steam copy needs a steam_appid.txt you add yourself (below), which you can
  delete any time; the EA app's copy needs nothing there.
- To remove the mod: delete this folder (and, for a Steam copy, steam_appid.txt from the game folder).
- Antivirus: Windows Defender may flag the mod as Trojan:Win32/Wacatac.H!ml and remove sws_server.dll. It's a false
  positive (a machine-learning guess at what the launcher does: start your game and load the mod's DLL into it), and
  it's being reported to Microsoft. The source is public, and the release page lists each file's SHA-256 hash. If
  Defender removes the files, you can choose to allow just this folder (Windows Security > Virus & threat protection >
  Manage settings > Exclusions). Please don't turn your antivirus off.


One-time setup
--------------
1. Steam copy only: in your game folder (right-click Squadrons > Manage > Browse local files), create a text file
   named steam_appid.txt containing just:  1222730
   Have both a Steam copy and an EA app copy? The launcher uses Steam's. To use the other, create launch.ini beside
   sws_launch.exe with these two lines (your path may differ):
       [launch]
       game=C:\Program Files\EA Games\STAR WARS Squadrons\starwarssquadrons.exe
2. Put this folder anywhere outside the game folder, for example on your Desktop.
3. A network path between the players. The tested way is Tailscale (tailscale.com, free): everyone installs it, and the
   host shares their PC with the others in Tailscale's admin page. The host's address is their Tailscale IP (100.x.y.z).


Settings: sws_settings.exe
--------------------------
Double-click sws_settings.exe for everything below without editing files:
- Play: host (pick the match type) or join (type the host's address, keep favorites), then Launch.
- Rules and fixes: turn the profile's exploit rules (zero-throttle pinballing, multidrifting) and map fixes (Zavian
  Abyss's middle X-wing creep pack spawning 3 ships instead of 5) on or off. Each is off unless you turn it on.
  Creep packs: what each Fleet Battles creep pack flies, and whether creep engage aggressively (following the
  phases), fire missiles, carry torpedoes on the bombers and fly with the vs-AI bots' handling; and creep hull and
  shields by ship type. docs/creep.md on the project page explains each.
- Profile and values: pick, make, copy, rename, import or delete profiles; and change
  any of the game's values (speeds, shields, hull, weapons, components...) for every ship, each with its default and the
  range it's allowed. Reset one value or all of them to default. The fingerprint at the top must match everyone else's.
  Each ship's Size makes it smaller or bigger, model and hitbox together (0.5 to 1.5). docs/ship-size.md on the
  project page lists the sizes closest to canon.
The values list fills in as you play: the mod records each part of the game the first time it loads it.

Or by hand (server.ini, open it in Notepad):
- HOST: leave "join=" empty, and pick the mode on the "match_mode=" line: fleet (Fleet Battles), coop (co-op vs AI),
  solo, or dogfight.
- JOIN: put the host's address after "join=", for example  join=100.101.102.103
- PROFILE: "profile=" picks the rules, fixes and values. Everyone in a match must use the same profile, or the host's game refuses
  the connection on purpose.
    Default        the game as shipped
    NoMultidrift   pinballing allowed, multidrifting removed
    AsIntended     both exploits removed
  Profiles are files in the profiles folder; you can copy one and edit it, or use sws_settings.exe.


Playing
-------
1. Steam running (for the EA app's copy: the EA app running and signed in), Squadrons closed.
2. Click Launch in sws_settings.exe and confirm in the window it opens. The game starts.
   (Or double-click sws_launch.exe, read what it says, type YES and press Enter.)
3. In the game, start a SOLO CUSTOM MATCH VS AI, on the host and on every joiner. The host's becomes the real match
   (per match_mode); a joiner's connects to the host instead. Joiners: start once the host's lobby is up.
4. Every ship component and cosmetic is unlocked, and EA's error 327 shouldn't appear.
5. Loadouts you save, in the hangar or in a match's briefing room, are kept in loadouts.txt beside this file and
   aren't sent to EA. When you update the mod, keep loadouts.txt along with server.ini and your profiles.


If something goes wrong
-----------------------
The logs folder beside this file has a server_<date>_<time>.status.txt for each launch. It records what the mod and the
game's connection did (addresses, packet counts, timings), nothing else from your PC. Attach the newest one to an issue
on the project page, with what you saw on screen.


License: CC BY-NC-SA 4.0 (non-commercial). Not affiliated with or endorsed by Electronic Arts, Motive Studio or Lucasfilm.
