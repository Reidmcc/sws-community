# STAR WARS: Squadrons community server mod

A way to keep playing STAR WARS: Squadrons together without EA's servers. One player hosts a match on their own PC and
others join it directly. The game's own server and client code run the match; the mod replaces only how their network
traffic travels, plus a few switches the community wanted.

Status: **early and experimental.** On 2026-10-20 two players finished a Fleet Battles match on a self-hosted server
over Tailscale. Co-op vs AI also runs. Expect rough edges.

## What it does

- **Self-hosting, Kyber's way.** The game ships its server half inside the client. The mod makes the host's own match
  server networked and carries the engine's connection over real UDP, by patching the engine's in-memory socket manager
  and socket in place. The approach follows the one [Kyber](https://github.com/ArmchairDevelopers/Kyber) took for
  Battlefront II; none of Kyber's code is used.
- **Joining.** A joining player's game connects to the host's address as the engine's own "Joined" client type, which
  loads levels itself and needs no local server.
- **Modes.** The host picks what the match really is, whatever was chosen in the menu: Fleet Battles (PvP), co-op vs
  AI, solo vs AI, or Dogfight.
- **Rulesets.** Optional "as intended" movement rules: no zero-throttle pinballing, no multidrifting. The chosen rules are
  folded into the network protocol version, so players with different rules are refused at connect instead of landing
  in the same match.
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

Everyone needs the same build of the mod, the same `rules.ini`, and a network path between them. The tested setup is
[Tailscale](https://tailscale.com): every player installs it, and the host shares their PC with the others.

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

**Rules** (`rules.ini`, must match across players; `1` = the exploit is removed):

```ini
[rules]
no_zero_throttle_pinball=0
no_multidrift=1
```

All-unlocked reads `unlock_ids.txt` beside the DLL. It's generated from the game's data and ships with release builds.

## Building

Windows, with Visual Studio 2022 Build Tools (C++). From `mod/`:

- `build_server.bat` builds `sws_server.dll`, `sws_launch.exe`, the server test DLL and the stand-in test target.
- `build.bat` / `build_rules.bat` build the earlier probe and rules-only variants.

`sws_probe.cpp` is one source file with several builds, selected by defines: `/DRULES` (movement rules), `/DSERVER`
(everything above), `/DPROBE_TEST` (test builds that hook stand-in functions instead of the game).

**Testing without the game:** `test_server_target.exe` maps the hook sites at their real addresses, plants the real
function prologues with stand-in bodies, and checks each hook's effect through the real launcher, including a UDP round
trip. Run it with the game closed:

```bash
sws_launch.exe --target <full path to test_server_target.exe> --dll sws_server_test.dll --yes
```

## Repository layout

- `mod/`: the mod's source, the launcher, the test targets and the build scripts.
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
