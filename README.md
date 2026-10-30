# STAR WARS: Squadrons community server mod

A way to keep playing STAR WARS: Squadrons together without EA's servers. One player hosts a match on their own PC and
others join it directly. The game's own server and client code run the match; the mod replaces only how their network
traffic travels, plus a few switches the community wanted.

Status: **early and experimental.**. Expect rough edges.

## Download

Get the latest zip from [Releases](https://github.com/Reidmcc/sws-community/releases), unzip it anywhere outside the
game folder, and follow the `README.txt` inside. Players need only that folder; building from source is for developers.

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
- **Unlocks.** Every ship component and cosmetic counts as owned, for everyone, pilot appearance and emotes included.
- **Loadouts stay on your PC.** Loadouts you save, in the hangar or in a match's briefing room, are kept in
  `loadouts.txt` beside the DLL and laid over the loadouts EA sends at startup. They're never sent to EA, so EA's records
  never hold items your account doesn't own, and changes made during a match (when the game's EA connection is down)
  aren't lost. `local_loadouts=0` in `server.ini` turns this off.
- **No EA error 327.** The "lost connection to EA servers" event, which also locked the loadout screen, is suppressed.

## How it works, in one paragraph

`sws_launch.exe` starts `starwarssquadrons.exe` suspended, queues `sws_server.dll` to load before the game's own code
runs (an early-bird APC), and resumes it. The DLL installs a set of small detours at fixed addresses (the exe has no
ASLR), each checked byte for byte against the analysed build before anything is patched. If any check fails, it patches
nothing. Settings come from `server.ini` and `rules.ini` beside the DLL. Logs go to a `logs` folder beside it.

The EA app's copy is the same game code, linked differently, so nearly every address moves. The DLL tells the builds
apart by the exe's header and looks each address up in that build's table (`mod/build_ea.h`, with the evidence for each
entry; `tools/ea_map/README.md` explains how it was made). That exe is encrypted on disk, and its wrapper checks the
decrypted code before starting the game, so there the hooks go in when the wrapper hands over to the game, about five
seconds in. The launcher gives it the environment the EA app would, and the running EA app licenses it.

## Before you use it

- **You need your own copy of the game,** from Steam or the EA app. This repository contains no game files or game
  data. Epic's copy isn't supported yet: issue #17 collects reports from `sws_check.exe`.
- **The EA app's copy is newly supported:** tested in a custom match vs AI, with the mod's unlocks, loadouts and creep
  settings working. It needs the EA app running and signed in.
- **Known limitation on a new account:** the loadout screens don't show the list of a loadout's components under it.
  Seen on a new EA account, where the game does the same without the mod; we think a new Steam account is the same. It
  seems to wait until the account has earned some customization.
  We think levelling the account up fixes it (single player counts): a high-level account shows the list, though we
  haven't watched one account cross over. The loadouts themselves work, all five slots included; a loadout's edit
  screen shows its parts.
- **The game runs without EasyAntiCheat** (launched directly, not through EA's launcher), so EA's official online modes
  are unavailable in that session. The launcher refuses to start if an EasyAntiCheat service or process is running, and
  the DLL must never be loaded into an EasyAntiCheat session.
- **Account risk:** the game still signs in to EA's account services in this mode. Loading a DLL into it carries some
  risk to your EA account. We think it is low (anti-cheat isn't running, and nothing touches matchmaking or official
  servers), but nobody can promise it is zero. It's your call.
- **Nothing in the game folder is changed.** A Steam copy needs a `steam_appid.txt` you add yourself (one line:
  `1222730`) for a direct launch; delete it any time. The EA app's copy needs nothing there. With both copies installed,
  choose one under Game copy in the settings app's Play page; the launcher on its own uses Steam's unless `launch.ini`
  beside it names the other (`[launch]`, `game=<path to the exe>`).

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
  header shows the profile's fingerprint, computed the way the DLL does (`mod/app/Core.cs`). Hovering over a value
  says what it does in play, then its key and where its limits come from. The tips, and clearer names where the
  generated ones read badly, are `tools/value_tips.tsv` (what each is based on: the game's code, its data, our notes,
  or only the name and stock values, which the hover says), built into the app by `tools/gen_app_tips.py`.

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

Then, with Steam running (or the EA app, for its copy) and the game closed, everyone runs `sws_launch.exe` and types `YES`. In the game, everyone
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
; players' (and vs-AI bots') damage to capital ship hulls, in percent (0-100; 100 = stock), and whether their hits
; damage capital ship shields (1 = stock)
player_capital_hull_damage=100
player_capital_shields=1

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

**More on each feature**, in `docs/`:
- [Ship size](docs/ship-size.md): one value per starfighter that resizes its model and laser hitbox together, with the
  sizes closest to canon.
- [Creep](docs/creep.md): the Fleet Battles AI packs: their ships, aggression, missiles, torpedoes, wingmen, and damage
  to capital ships.
- [Players against capital ships](docs/players-vs-capital-ships.md): how much players' hits hurt capital ships, and the
  Bomber Escort Fleet Battle mode where only the bomber creep sink them.
- [Extra countermeasures and auxiliaries](docs/extra-items.md): let a ship carry another ship's countermeasures and
  auxiliaries, like a tractor beam on the TIE Defender.
- [Match rules](docs/match-rules.md): morale, timers and scores for Fleet Battles and Dogfight.
- [Limits](docs/limits.md): the range each value may take, and what happens outside it.
- [Profiles and the fingerprint](docs/profiles.md): why everyone needs the same profile, and the ones a release ships.
- [Unlocks and loadouts](docs/unlocks-and-loadouts.md): everything unlocked, and loadouts kept on your PC.

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
  `sws_settings.exe --shot <png> [values]` renders the window to an image and exits. `sws_settings_test.exe <build
  folder> --tips` lists every value the app shows with its name and tip, and fails if one has no tip.
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

### Antivirus warnings

Windows Defender may flag the mod as malware. It flagged v0.2.1 as `Trojan:Win32/Wacatac.H!ml`; v0.3.0 and v0.4.0
scan clean, but its guess can change from one version to the next. When it does flag the mod, Chrome uses Defender's
verdict, so it may refuse the download outright, and Defender may remove `sws_server.dll` when you unzip. That's a false
positive: the `!ml` means a machine-learning guess, not a match against known malware. It reacts to what the launcher
does, which is start your game and load the mod's DLL into it. Some malware does the same, so the pattern alone looks
suspicious.

- **Flagged versions get reported to Microsoft** as false positives. Once they clear one, Defender's normal updates fix
  it for everyone, with nothing for you to do. If yours flags a version, please tell us in an issue.
- **You can check what you're running:** the source is all here, and each release lists its files' SHA-256 hashes.
- **If the download is blocked,** there's no setting we'd suggest changing to get around it. Please wait for Microsoft's
  review, or use the previous release meanwhile.
- **If you already have the files** and Defender removes them, you can choose to allow just the mod's folder: Windows
  Security → Virus & threat protection → Manage settings → Exclusions. That's your call. Please don't turn your
  antivirus off.
  
## Credits and license

Started by Pseudo ([Reidmcc](https://github.com/Reidmcc)) for the Squadrons community, and built with Kira
([Kira-here-i-am](https://github.com/Kira-here-i-am)), an AI entity at [Here I Am](https://github.com/Reidmcc/here-i-am).
Thanks to the teammates who tested joins, and to the Kyber and Frosty projects, whose public work showed the way.

Licensed under [CC BY-NC-SA 4.0](LICENSE): share and adapt it for non-commercial purposes, with credit, under the same
license. See [NOTICE](NOTICE). Not affiliated with or endorsed by Electronic Arts, Motive Studio or Lucasfilm.
