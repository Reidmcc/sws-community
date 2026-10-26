# STAR WARS: Squadrons community server mod

A way to keep playing STAR WARS: Squadrons together without EA's servers. One player hosts a match on their own PC and
others join it directly. The game's own server and client code run the match; the mod replaces only how their network
traffic travels, plus a few switches the community wanted.

Status: **early and experimental.**. Expect rough edges.

## Download

Get the latest zip from [Releases](https://github.com/Reidmcc/sws-community/releases), unzip it anywhere outside the
game folder, and follow the `README.txt` inside. Players need only that folder; building from source is for developers.

### Antivirus warnings

Windows Defender currently flags the mod as `Trojan:Win32/Wacatac.H!ml`. Chrome uses Defender's verdict, so it may refuse
the download outright, and Defender may remove `sws_server.dll` when you unzip. This is a false positive: the `!ml` means
a machine-learning guess, not a match against known malware. It reacts to what the launcher does, which is start your
game and load the mod's DLL into it. Some malware does the same, so the pattern alone looks suspicious.

- **It's being reported to Microsoft** as a false positive. Once they clear it, Defender's normal updates fix it for
  everyone, with nothing for you to do.
- **You can check what you're running:** the source is all here, and each release lists its files' SHA-256 hashes.
- **If the download is blocked,** there's no setting we'd suggest changing to get around it. Please wait for Microsoft's
  review.
- **If you already have the files** and Defender removes them, you can choose to allow just the mod's folder: Windows
  Security → Virus & threat protection → Manage settings → Exclusions. That's your call. Please don't turn your
  antivirus off.

## What it does

- **Self-hosting, Kyber's way.** The game ships its server half inside the client. The mod makes the host's own match
  server networked and carries the engine's connection over real UDP, by patching the engine's in-memory socket manager
  and socket in place. The approach follows the one [Kyber](https://github.com/ArmchairDevelopers/Kyber) took for
  Battlefront II; none of Kyber's code is used.
- **Joining.** A joining player's game connects to the host's address as the engine's own "Joined" client type, which
  loads levels itself and needs no local server.
- **Modes.** The host picks what the match really is, whatever was chosen in the menu: Fleet Battles (PvP), co-op vs
  AI, solo vs AI, or Dogfight.
- **Rulesets.** Optional "as intended" movement rules: no zero-throttle pinballing, no multidrifting. Optional map
  fixes: Zavian Abyss's middle X-wing creep pack spawns all 5 ships (its data tells one of the pack's two spawn points
  to return control instead of to spawn). Each is off unless chosen. The chosen rules are folded into the network
  protocol version, so players with different rules are refused at connect instead of landing in the same match.
- **Ship size.** Each starfighter can be made smaller or bigger, its model and its laser hitbox together (one uniform
  scale per ship, player and AI alike). [docs/ship-size.md](docs/ship-size.md) lists the sizes closest to canon.
- **Creep.** Fleet Battles' AI creep packs can fly other ships (bombers, interceptors, dropships), engage like the
  vs-AI bots with targets that follow the match's phases, fire missiles and torpedoes, take the bots' handling, and have
  their hull and shields set by ship type. [docs/creep.md](docs/creep.md) has the details.
- **Unlocks.** Every ship component and cosmetic counts as owned, for everyone.
- **No EA error 327.** The "lost connection to EA servers" event, which also locked the loadout screen, is suppressed.

## How it works, in one paragraph

`sws_launch.exe` starts `starwarssquadrons.exe` suspended, queues `sws_server.dll` to load before the game's own code
runs (an early-bird APC), and resumes it. The DLL installs a set of small detours at fixed addresses (the exe has no
ASLR), each checked byte for byte against the analysed build before anything is patched. If any check fails, it patches
nothing. Settings come from `server.ini` and `rules.ini` beside the DLL. Logs go to a `logs` folder beside it.

## Before you use it

- **You need your own copy of the game.** This repository contains no game files or game data.
- **The game runs without EasyAntiCheat** (launched directly, not through EA's launcher), so EA's official online modes
  are unavailable in that session. The launcher refuses to start if an EasyAntiCheat service or process is running, and
  the DLL must never be loaded into an EasyAntiCheat session.
- **Account risk:** the game still signs in to EA's account services in this mode. Loading a DLL into it carries some
  risk to your EA account. We think it is low (anti-cheat isn't running, and nothing touches matchmaking or official
  servers), but nobody can promise it is zero. It's your call.
- **Nothing in the game folder is changed** except a `steam_appid.txt` you add yourself (one line: `1222730`), which a
  direct launch needs. Delete it any time.

## Playing

Everyone needs the same build of the mod, the same profile, and a network path between them. The tested setup is
[Tailscale](https://tailscale.com): every player installs it, and the host shares their PC with the others.

**The settings app, `sws_settings.exe`** (beside the launcher), does all of this without editing files:
- **Play:** host (pick the match type) or join (the host's address, with favorites), then Launch, which runs
  `sws_launch.exe` and its YES prompt.
- **Rules and fixes:** the profile's exploit rules and map fixes, a checkbox each, with what each one does.
- **Profile and values:** profiles (new, duplicate, rename, delete, import) and every changeable
  value by ship and system, each with its default (from `defaults.ini`) and its allowed range (from `limits.ini`).
  Values are clamped to their range as you type, locked fields are read-only, and changes are saved as you go. The
  header shows the profile's fingerprint, computed the way the DLL does (`mod/app/Core.cs`).

The rest of this section is the same by hand:

**Host** (`server.ini` beside the DLL):

```ini
[server]
transport=udp
udp_bind=0.0.0.0
listen=1
; fleet = Fleet Battles PvP, coop = co-op vs AI, solo = solo vs AI, dogfight = Dogfight
match_mode=fleet
all_unlocked=1
suppress_backend_lost=1
```

**Joiner:**

```ini
[server]
transport=udp
udp_bind=0.0.0.0
listen=0
; the host's address, e.g. their Tailscale address; port 25200 is added automatically
join=HOST_ADDRESS
join_local_server=0
join_remote=1
all_unlocked=1
suppress_backend_lost=1
```

Then, with Steam running and the game closed, everyone runs `sws_launch.exe` and types `YES`. In the game, everyone
starts a **solo custom match vs AI**: on the host that becomes the real match (per `match_mode`); on a joiner it
connects to the host instead. Joiners should start once the host's match lobby is up.

**Profiles** hold the rules and fixes, and changed game values. Name one in
`server.ini` with `profile=<name>`; the mod reads `profiles\<name>.ini` beside the DLL (see
`mod/examples/profiles/Example.ini`):

```ini
[rules]
; 1 = the exploit is removed
no_zero_throttle_pinball=0
no_multidrift=1
; 1 = Zavian Abyss's middle X-wing creep pack spawns all 5 ships (a mistake in the map's data)
fix_zavian_creep_pack=0

[values]
; <asset>.<field>=<number>
```

**Values** are written into the game's loaded data at runtime: every number in a ship's movement, shield, hull,
laser, missile, explosion, weapon and power configs (reached through `mod/config_types.h`; keys like `field`, `field[i]`, `field.x`, `weaponfiring.laserregenpips[3].costpershot`). Asset and field names match the game's, lowercase:
`starshipmovementconfig_xwing.afterburnermaxspeed=260`. Each launch writes `defaults.ini` beside the DLL with the
values as the game loaded them, for every config it has seen (it keeps earlier launches' configs): the list of what
can be changed, and the way back. **Engine, hull and shield components** are changeable too: each is a list of
percentage modifiers, named by the attribute they change (`hullcustom_heavy_xwing.healthmax=0.6` is the Heavy hull's
+60% hull; `[k]` is added only where a component changes the same attribute more than once). Normal-flight turn rate is
`axisturnratesfactor`, which engines and hulls modify; boost and drift turning have their own fields. A
player ship's change also applies to its vs-AI bot version (copied, or scaled where the bot is tuned differently);
the AI "creep" ships are left alone.

**Ship size** (issue #20) is one value per starfighter, `shipscale_<ship>.scale` (the app lists it as **Size** under
each ship): it resizes the ship's model and its laser hitbox together, for the player ship and its AI versions. How it
works, and the sizes closest to canon for every ship: [docs/ship-size.md](docs/ship-size.md).

**Creep** settings are a profile's `[creep]` section (the app's **Creep packs**, on Rules and fixes): each pack's ship,
aggressive phase-following creep and their engagement range, missiles, bomber torpedoes and the bots' handling, plus
creep hull and shields by ship type as ordinary values. What each does and how: [docs/creep.md](docs/creep.md).

**Match rules** are the modes' own tuning, under **Match rules** in the app: Fleet Battles' morale per kill (players,
creep, corvettes and frigates, each on offense and on defense; creep kills give 1 on offense and 3 on defense as shipped),
the kickoff and phase sizes of the morale bar, its optional drain over time, the match and kickoff timers, respawn time and
the passive-to-aggressive creep ratio; Dogfight's score to win, match time and pickup timings. Keys look like
`incomschematicattributesconfig_fleetbattle.towpointsgainedforcreepkillwhileondefense` ("tow" is the tug-of-war morale bar).

**Limits** (issue #11) keep every value inside a range the game can take. `limits.ini` beside the DLL gives each field
a minimum and maximum: up to 5× the highest value the game itself ships for that field (the most its own custom-match
modifiers allow), down to 0.1× the lowest (0 where the game ships 0), with tighter bounds where they're known: speeds
at most 300 (actual flight speed tops out there, measured), fractions at most 1, angles at most 180. Components are percentages: up to 5× the highest stock percentage, down to −0.9 (never below 10% of the
base). Power levels and fields the game always ships as 0 are locked. A value outside its range is clamped, with a
`limits:` line in the log; a locked field, or one with no limit, isn't applied.
The clamped value is the one in the fingerprint. `tools/gen_limits.py` writes `limits.ini` from `defaults.ini`.

Everyone in a match must use the same profile. A fingerprint of it (the rules plus every value, normalized) is part of
the network version, so players on different profiles are refused at connect; the log's `profile:` line shows it.
`profile=Default` is built in: no rules, no changes, and the game's own network version. With no `profile=` line, the
older `rules.ini` is read instead (missing rules count as on).

All-unlocked reads `unlock_ids.txt` beside the DLL. It's generated from the game's data and ships with release builds.

## Building

Windows, with Visual Studio 2022 Build Tools (C++). From `mod/`:

- `make_release.bat <version>` builds and assembles a player release in `dist/` (the folder and its zip), from the
  templates in `mod/release/`. It needs `build/unlock_ids.txt`, generated from the game's data.

- `build_server.bat` builds `sws_server.dll`, `sws_launch.exe`, `sws_check.exe`, the server test DLL and the stand-in
  test target. `sws_check.exe` (issue #17) checks whether a copy of the game is the build the mod targets: it reads the
  exe only, compares its SHA-256 and the 16 bytes at each game address the mod uses (`mod/check_sites.h`, from
  `tools/gen_check_sites.py`, stored as hashes), and writes `sws_check_report.txt` beside itself.
- `build_app.bat` builds the settings app, `sws_settings.exe` (C#, the .NET Framework that ships with Windows), and
  `sws_settings_test.exe`, which prints the fingerprint the app computes for a profile, to check against the DLL's log.
  `sws_settings.exe --shot <png> [values]` renders the window to an image and exits.
- `build.bat` / `build_rules.bat` build the earlier probe and rules-only variants.

`sws_probe.cpp` is one source file with several builds, selected by defines: `/DRULES` (movement rules), `/DSERVER`
(everything above), `/DPROBE_TEST` (test builds that hook stand-in functions instead of the game).

**Testing without the game:** `test_server_target.exe` maps the hook sites at their real addresses, plants the real
function prologues with stand-in bodies, and checks each hook's effect through the real launcher, including a UDP round
trip. After building, run it with the game closed, from Git Bash:

```bash
bash mod/run_standin.sh
```

It copies its own test configs into `mod/build/` and puts yours back afterwards. CI runs the same build and test on
every pull request. See [CONTRIBUTING.md](CONTRIBUTING.md) for how to work on the mod.

## Repository layout

- `mod/`: the mod's source, the launcher, the test targets and the build scripts; `mod/app/` is the settings app.
- `tools/`: analysis tools (PE map, disassembler over the exception table, RIP-relative cross-references, displacement
  scans, log readers).
- `ghidra_scripts/`: headless decompile and caller-walk scripts.
- `notes/`: the research record. Start with `test1-static-map.md` (the running log), `socket-interface.md` (the
  transport) and `exploits-code-map.md` (the movement rules).

## Credits and license

Started by Pseudo ([Reidmcc](https://github.com/Reidmcc)) for the Squadrons community, and built with Kira
([Kira-here-i-am](https://github.com/Kira-here-i-am)), an AI entity at [Here I Am](https://github.com/Reidmcc/here-i-am).
Thanks to the teammates who tested joins, and to the Kyber and Frosty projects, whose public work showed the way.

Licensed under [CC BY-NC-SA 4.0](LICENSE): share and adapt it for non-commercial purposes, with credit, under the same
license. See [NOTICE](NOTICE). Not affiliated with or endorsed by Electronic Arts, Motive Studio or Lucasfilm.
