# Can Squadrons' own exe boot as a dedicated server? The test plan

Written 2026-10-19 from the Kyber read (`kyber-architecture.md`), the exe header and strings,
and PCGamingWiki. Nothing here has been run yet.

## What Kyber does for BF2, in one paragraph
The server is the retail client exe. An injected DLL sets the engine's own
`MainLoop->isDedicatedServer` flag at init. It then hooks GameSimulation init, sets the
Network/Game/Server settings, and calls the engine's own spawn-server with `isDedicated=true`.
It forces the presence backend to Local, because Blaze crashes on a server, and patches out
Blaze communication. Clients connect through Frostbite's built-in direct-connect path
(`ClientSettings.ServerIp` + `NetworkSettings.ServerPort`, then force `ClientState_Startup`).
There is no Blaze emulation at all. The account login, ownership and the Denuvo token still
come from EA, via Maxima (their open-source EA-app replacement). All hooks are hardcoded
absolute addresses for one BF2 build: about 380 functions and globals.

## How Squadrons compares (measured)
| | BF2 (Kyber) | Squadrons |
|---|---|---|
| Image base / ASLR | 0x140000000, never rebased | 0x140000000, **no DYNAMIC_BASE, no reloc dir**, so fixed addresses work |
| Code readable on disk? | yes | `.text1` (45 MB) has an entropy of about 6.4, i.e. plain x64 code. The 440 MB RWX `.idata` is the Denuvo-protected part |
| DRM | Denuvo + EA licensing | the same (PCGamingWiki: "All versions require an EA account and Denuvo") |
| Anti-cheat | none on the client | EAC, but launching `starwarssquadrons.exe` directly skips it (PCGamingWiki), at the cost of the official online play we don't want anyway |
| Server code in the client | yes | yes: 434 `Server*` types, `MainLoop`/`DedicatedServer` strings, `NullRendererEnable` |
| Direct-connect settings | `ClientSettings.ServerIp`, `NetworkSettings.ServerPort` | **same fields exist** (SWSSDK) |
| Local/LAN backends | Local used | `OnlineBackend_Local`, `_Lan`, `_Peer`, `LocalServerBackendData`, `LanServerBackendData` in the type info |

The Steam build needs `SteamAppId` to start without the Steam launcher. PCGamingWiki says to
drop a `steam_appid.txt` in the game folder. **We won't**, because the install stays
untouched. `steam_api64` also reads the `SteamAppId` / `SteamGameId` environment variables,
so our launcher sets `SteamAppId=1222730` instead. To be verified in Test 0.

## Tests, in order. Each one gates the next.

### Test 0: does the exe run without EAC? (no code, about 10 minutes)
**Pseudo, 2026-10-19:** a `steam_appid.txt` in the game folder is fine. The install rule was
"discuss before changing", not "never". Pseudo plays through Steam and doesn't use the EA app.
This machine has only a 2023 Origin install (EALink/EASteamProxy) and no EA Desktop, so the
Steam copy is licensed through Steam plus a linked EA account. That differs from BF2's
Maxima/LSX path and is simpler if it holds.
Pseudo runs it: put `steam_appid.txt` (contents `1222730`) in the game folder, Steam running,
then start `starwarssquadrons.exe` directly (not the launcher).
- Pass: the game reaches the main menu and Practice or a story mission loads.
- Learn: whether the env-var trick works or Steam insists on its own launch path; whether the
  EA app/Denuvo handshake happens normally; what the online menu says without EAC.

### Test 1: static map of the exe (agent work, no running game)
Copy the exe out of the install into `work/` (a read; nothing written to the install) and
load it into Ghidra. That needs Java + Ghidra installed, which is a download, so Pseudo's OK first.
1. **Type-info SDK**: walk Frostbite's TypeInfo registrations in `.data2`/`.rdata` (the class
   names are already in the strings) to get class layouts and vtables. Kyber's 129 generated
   headers show what the output should look like. Cross-check against Frosty's SWSSDK field lists.
2. **Find the boot path** using string cross-references and Kyber's BF2 structure as a map:
   MainLoop init and its `isDedicatedServer` flag, GameSimulation init, the spawn-server call,
   ServerSettings/NetworkSettings/ClientSettings access, the presence-backend choice, and the
   Blaze calls to stub. Probably 15-30 addresses for a first boot, not Kyber's full 380.
3. Note which of these sit in the plain `.text1` and which have been pulled into Denuvo's
   section. Functions there are much harder to read, and we'd hook around them instead.

### Test 2: probe DLL, read-only (first time anything is injected)
A small C++ DLL with safetyhook (as Kyber uses), loaded by our own launcher
(CreateProcess + remote LoadLibrary). Started with EAC off, from outside the install.
It only **logs**: settings values at boot, which online backend is chosen, and the MainLoop
state. This confirms the addresses from Test 1 against the running process before anything
is changed.

### Test 3: boot as server
Set the dedicated flag, force the Local backend, stub Blaze, and load an MP level
(Fleet Battles vs AI is a good first target, since it exercises the server-side AI, which
`ServerAIVehicleBehaviorEntity` etc. say exists). Window and rendering are allowed for now:
Kyber runs its servers with a window too, under Xvfb.
- Pass: the server ticks the level, the AI fly, and nothing crashes for 10 minutes.

### Test 4: a client joins
A second instance sets `ClientSettings.ServerIp`. Two instances on one EA account may not be
allowed, so this probably needs a second machine (the cloud VM?) or a second account that
owns the game. Kyber's server logs in with its own EA persona for the same reason.
- Pass: a pilot spawns in the server's match and flies.

If Test 4 passes, the project is real. Everything after it (a server browser, mods via a VFS
mount of `.fbmod`s the way Kyber does, the league tooling) is building on a working core.

## Risks to name honestly
- **Denuvo** may have pulled exactly the functions we need into its VM section. Kyber
  managed with BF2 under Denuvo, so it's survivable, but it's the biggest unknown in Test 1.
- **EA account**: the game runs logged in, with EAC off and a DLL injected. Without EAC the
  official servers are unreachable, so nothing official ever sees the modified process.
  Kyber's users have done the equivalent for BF2 for years. Still, it's Pseudo's account,
  so it's Pseudo's call.
- **The EULA forbids reverse engineering.** Kyber has existed openly for years, and EA has
  even fixed exploits its team reported. That's tolerance, not permission. The community
  should decide with eyes open, and the project should never distribute game files: players
  bring their own install.
- **Future EA action**: if EA shuts down the EA account/licensing for Squadrons entirely,
  Denuvo tokens stop. That kills Kyber-style projects too, and only a full rebuild survives it.

## Results
- **Test 0 PASSED (Pseudo, 2026-10-19):** with `steam_appid.txt` in place and Steam running, `starwarssquadrons.exe` launched directly (no EAC), reached Practice, and flew. Pseudo's own account. Next is Test 1, and Pseudo has approved downloading Java + Ghidra.
