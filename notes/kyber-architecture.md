# Kyber (SWBF2 2017) architecture notes, for the Squadrons feasibility assessment

Source: `E:\sws_community\refs\Kyber` (commit 4627e3e5, 2026-10-16). Read-only review; nothing was built or run.
All paths below are relative to that root unless absolute. "Not found" means I searched and found nothing.

---

## 1. Top-level layout

Monorepo, described in `README.md` and `BUILDING.md`.

| Dir | What it is | Language / build |
|---|---|---|
| `Module/` | **Kyber.dll**: the C++ module injected into `starwarsbattlefrontii.exe`. It holds everything that runs in the game: hooks, client/server logic, mod loader, networking replacement, Lua plugins, VoIP. | C++20, MSVC only, **Bazel** (`Module/MODULE.bazel`, `Module/BUILD.bazel`, `Module/build.bat`). Output `bazel-bin/Kyber.dll`, which must be built with `--config=release` (`BUILDING.md`). |
| `Launcher/` | Desktop launcher GUI: login, server browser, mod collections, starting and injecting the game. | Flutter/Dart plus Rust via `flutter_rust_bridge` (`Launcher/rust/src/api/maxima.rs`, `downloader.rs`, `archive.rs`). Inno Setup installer `Launcher/installer/installer.iss`. |
| `CLI/` | Headless CLI (`kyber_cli`): `start_server`, `start_game`, `download_game`, `get_token`, `get_ea_token` (`CLI/lib/commands/*`). Also holds the **dedicated-server Docker image** (`CLI/docker/Dockerfile`, `CLI/docker/entrypoint.sh`). | Dart (`dart build cli`) plus Rust bridge (`CLI/rust/src/api/maxima.rs`). |
| `Proxy/` | UDP-over-WebSocket relay ("server proxy"), JWT-authenticated (`Proxy/src/main.rs`, `jwks.rs`, `user_manager.rs`). | Rust, cargo, Dockerfile. |
| `API/` | Kyber's own backend: auth, server browser, join tokens, server management, stats, parties, queue, VoIP creds, reports (`API/internal/rpc/*.go`). | Go 1.24+, gRPC (protos in `Module/Proto/*.proto`), Redis, DB, MQ. Dockerfile. |
| `Packages/` | Shared Dart packages: `kyber` (proto/client bindings), `kyber_collection` (mod collections), `nexus_bridge` / `nexus_gql` (Nexus Mods GraphQL). | Dart, Melos workspace (`pubspec.yaml` at root). |
| `ThirdParty/Maxima`, `CLI/ThirdParty/Maxima` | Git submodules (`.gitmodules`) pointing to `github.com/ArmchairDevelopers/Maxima`: an open-source EA app / Origin replacement in Rust. **Both are empty in this checkout**, so its source was not reviewed. | Rust |
| `Module/Proto/` | gRPC contracts: `kyber_api.proto` (module/launcher to Kyber API), `kyber_interface.proto` (launcher to module), `kyber_ea_bridge.proto`, `mod_bridge.proto`, `kyber_common.proto`. | protobuf |

Module third-party code (`Module/ThirdParty/`): **safetyhook** (inline hooks, uses zydis), **MinHook** (inside ToolLib; initialized but effectively unused), EASTL/EABase (to match the game's containers), **FBRuntimeTweaker** (Frosty `.fbmod` parser, MIT, same author), IXWebSocket, gRPC, Lua, Vivox SDK, Sentry, FastDelegate, glm, nlohmann json, zip, openssl, ToolLib (spdlog/fmt/imgui/minhook).

Module sub-areas (`Module/Source/*`, `Module/Public/*`), with approximate LOC excluding the generated SDK: Core 4.9k, ModLoader 6.3k, SDK (hand-written) 6.6k, Script/Lua 4.5k, Entity 2.6k, RPC 1.8k, Network 1.5k, Misc 1.2k, Voip 1.2k, Persistence 0.4k, Hook 0.3k. Total is about 32k lines, plus 13k lines of generated `Public/SDK/Fb/*.h`.

Build CI: `.github/workflows/build-module.yaml`, `build-cli.yaml`, `build-launcher.yml`, `build-api.yaml`, `build-proxy.yaml`, `build-maxima.yaml`, `deploy-cli.yaml`.

---

## 2. Injection and hooking

### How the DLL gets into the process
- The game is **launched through Maxima** (their EA app replacement), which also acts as the **LSX server** (the Origin/EA Desktop local SDK protocol) that the game talks to for auth and licensing.
  - Launcher: `Launcher/lib/features/maxima/helper/maxima_helper.dart:197-230`. It calls `maxima.startGame(...)`, waits on `lsxGetEventStream(pid)` until the game sends the LSX request **`RequestLicense`**, then calls `maxima.injectKyber(pid, <ModuleDir>/Kyber.dll)`.
  - CLI/server: `CLI/lib/commands/start_server_command.dart:473-498`. `startGame(...)` returns the PID once the LSX **`ChallengeResponse`** request is seen (`CLI/rust/src/api/maxima.rs:138-158`), and then `injectKyber(pid, path)` runs.
  - The injection call is `maxima::core::background_service::request_library_injection(pid, &path)` (`CLI/rust/src/api/maxima.rs:455-458`, `Launcher/rust/src/api/maxima.rs:70-72`). The injection technique itself is inside Maxima's background service, which is the empty submodule, so it is **not found here**. Given the API (a PID and a DLL path) it is almost certainly remote `LoadLibrary`. Nothing indicates a manual map or a proxy DLL.
  - **The process is not created suspended by Kyber.** It is injected after the game has started and done its LSX handshake. To avoid racing the engine, the first thing the DLL does is hook the engine's main init (`0x1401898A0`, `MainInitHk`), which **blocks the game's init on a condition variable** until Kyber has fetched its config from the launcher over gRPC (`Module/Source/Core/Program.cpp:118-135`; it is released in `Module/Source/RPC/API/Launcher.cpp:117-119`).
- `DllMain` creates `Program`, which spawns an init thread (`Module/Source/Core/Main.cpp:22-34`, `Program.cpp:67-87`).
- Configuration reaches the DLL via **environment variables** set by the launcher/CLI before launch, which the child process inherits: `KYBER_API_TOKEN`, `KYBER_DEDICATED_SERVER`, `KYBER_INTERFACE_PORT`, `KYBER_API_HOSTNAME`, `KYBER_HIDE_CONSOLE`, `KYBER_LOG_LEVEL`, and so on (`maxima_helper.dart:153-189`, `start_server_command.dart:279-298`). The DLL then calls back to the launcher's gRPC `LauncherCommon.Initialize` to get an `InitializeRequest` (start-server or join-server, plus mod list) (`Module/Proto/kyber_interface.proto:7-12,78`, `Module/Source/RPC/API/Launcher.cpp:20-120`).

### Hooking library
- **safetyhook** inline hooks: `safetyhook::create_inline(target, replacement)` in `Module/Source/Hook/Hook.cpp:31-38`, which notes "Adapted from ReShade". MinHook is `MH_Initialize()`d (`Program.cpp:81`), but `Hook::ApplyQueuedActions()` returns `true` before calling MinHook (`Hook.cpp:52-54`).
- Hook registry: `Module/Source/Hook/HookManager.cpp`. Hooks are declared as tables of `{ address, replacementFn }` (`HookTemplate`, `Public/Hook/Hook.h:13-17`).
- **VTable hooks** for some objects: `PlatformUtils::HookVTableFunction(inst, fn, index)` and `DuplicateVTable`. Examples: GameSimulation vtable index 31 (`Program.cpp:647`), entity vtables in `Source/Entity/NativeEntityManager.cpp:665-748`.
- **Byte patches / NOPs** via `MemoryUtils::Nop/Patch` (31 call sites).

### How game functions are located: hardcoded absolute addresses only
- **No signature or pattern scanning is used.** `PlatformUtils::MaskCompare` exists (`Source/Utilities/PlatformUtils.cpp:57`) but has no callers. No FindPattern of any kind was found.
- Every function, global and TypeInfo is an **absolute VA assuming image base 0x140000000**, with no rebasing. `HOOK_OFFSET(addr)` is just `reinterpret_cast<void*>(addr)` (`Public/Hook/Hook.h:9-11`), and `TL_DECLARE_FUNC(addr, ret, name, args...)` makes a typed function pointer to a fixed address (`Module/ThirdParty/ToolLib/Public/ToolLib/Func.h`). This works because BF2's exe is a single frozen build at its preferred base.
- Examples:
  - `Program.cpp:37-57`: `OFFSET_GET_SETTINGS_OBJECT 0x1401F7BD0`, `OFFSET_MESSAGEMANAGER_DISPATCH_MESSAGE 0x1401F6CA0`, `OFFSET_ORIGINSDK_INITIALIZE 0x14138D070`, `OFFSET_MAINLOOP_INIT 0x140186B90`, `OFFSET_GAMESIMULATION_INIT 0x145315930`, `OFFSET_GAMESIMULATION_SPAWNSERVER 0x14018EE70`, `OFFSET_FILESUPERBUNDLEMANAGER_UPDATECONFIG 0x14024CA10`.
  - `Public/SDK/Funcs.h:9-52`: `ServerLoadLevelMessage_post 0x140BCF350`, `VirtualFileSystem_mount 0x14023A210`, `Win32FileSystem_ctor 0x140241B30`, `ResourceManager_lookupDataContainer 0x1454B82B0`, `DataContainerClassInfo_createInstance 0x1453D5AB0`, `ScriptContext_Impl_executeString 0x140208C30`.
  - `Public/Core/Memory.h:33-41`: arena globals `FB_STATIC_ARENA 0x143CF74E0`, `FB_SERVER_ARENA 0x143CFA7C0`, `MemoryArena_alloc 0x14541CD00`.
  - Globals: `ServerGameContext*` at `0x143EC7238`, `ClientGameContext*` at `0x143EE7858` (`Public/SDK/SDK.h:768,1033`).
- Rough counts (grep over `Module/Source` and `Module/Public`):
  - **379 unique code/global addresses** outside the generated SDK. The largest users are `SDK.h` (70), `Program.cpp` (48), `Server.cpp` (32), `ModLoader.cpp` (29), `DebugHooks.cpp` (29), `Funcs.h` (29).
  - About **127 inline-hook table entries**.
  - **101 `TL_DECLARE_FUNC`** called functions.
  - **74 `KB_DECLARE_GAMEMEMBERFUNC`** member-function wrappers.
  - **31 byte patches/NOPs**.
  - **12,178 `_KB_DECLARE_TYPEINFO(Name, addr)`** TypeInfo addresses in 129 generated headers under `Public/SDK/Fb/`.
  - Hand-written struct layouts with fixed field offsets: `Public/SDK/TypeInfo.h` (3,091 lines) and `Public/SDK/SDK.h` (2,352 lines).
- **Porting cost:** every one of the roughly 380 code/global addresses, plus all struct field offsets, has to be found again by reverse engineering in the Squadrons exe. The TypeInfo addresses would be regenerated by a dumper (see Q7).

---

## 3. Headless dedicated server

**It is the same retail client exe** (`starwarsbattlefrontii.exe`) with the same Kyber.dll. There is no separate server binary. Switching modes is done with the environment variable **`KYBER_DEDICATED_SERVER`** (present means dedicated), which the CLI sets in `start_server_command.dart:287-291`. **No game command-line args are used:** both launcher and CLI pass `gameArgs: []` (`start_server_command.dart:478,486`, `maxima.rs:132-136`). I found no use of `-server`, `-dedicated`, `Game.Platform` or similar. A `DataPlatform = "DedicatedServer"` override exists but is **commented out** (`Program.cpp:545-555,749`).

How it is done (all in the DLL):
1. **Flip the engine's own dedicated flag.** `MainLoopInitHk` sets `MainLoop->isDedicatedServer` (byte at `+0x8`) to `true` (`Module/Source/Core/Program.cpp:504-543`, hook at `0x140186B90`). The retail exe already contains Frostbite's dedicated-server main-loop path; Kyber just enables it.
2. **Supply server creation.** `GameSimulationInitHk` (`0x145315930`) sets the game clock name to `"Dedicated"` (`*0x143AEBA18`) and **hooks GameSimulation vtable slot 31** with `GameSimulationInitDedicatedServerHk` (`Program.cpp:633-664`). That replacement (`Program.cpp:564-622`):
   - allocates and constructs the engine's own **DirtySock socket manager** (`DirtySockSocketManager_ctor 0x14131AB20`) if not using Kyber's;
   - sets settings `Network.MaxClientCount=64`, `Game.MaxSpectatorCount=4`, `NetObjectSystem.MaxServerConnectionCount=64`, and `Server.ServerName`;
   - registers with the Kyber API (`Server::Register`);
   - builds a `LevelSetup` (level plus `GameMode` inclusion option), and calls the original `GameSimulation::spawnServer` with `ServerSpawnInfo{isSinglePlayer=false, isLocalHost=false, isDedicated=true}`.
3. **Server-side hooks for dedicated mode**, in `Module/Source/Core/Server.cpp:843-858` (`dedicatedServerHookOffsets`):
   - `Server` ctor `0x140BC6370`, which forces `info.isLocalHost=false` and `ServerSettings.ThreadingEnable=false` (`Server.cpp:354-382`);
   - `GetSocketManager` `0x1484213F0`, which returns Kyber's own socket manager;
   - **`CreatePresenceBackend` `0x1418D3380`**, which forces `OnlineBackend_Local (0xB8566ABC)` because "the game crashes without this, presumably trying to use a Blaze presence backend for dedicated servers ... which has been cooked out" (`Server.cpp:538-558`);
   - `ServerConnection::onCreatePlayerMessage` `0x140BF33B0` for join-token auth;
   - `Server::updatePassPreFrame` `0x146860470` as the per-tick hook;
   - level load/complete hooks, chat, AFK timeout.
4. **Dedicated-only byte patches** with no comments explaining them (`Server.cpp:912-927`): NOP `0x146860582`(3), `0x1418E3F3C`(16), `0x1453B2163`(3); patch `0x14183D533 = E9 EF 00`; patch `0x140249606 = EB`; and `0x1454DCC9D = EB` for both modes. Global patches (`Program.cpp:782-805`): NOP `0x14018B133` ("Allow Multiple Game Instances"), NOP `0x140235C2E` ("Enable All Console Commands"), and `0x1418B64CC = EB`.
5. **Rendering and window: not explicitly disabled.** I found no headless, null-renderer or no-window code (searched for headless/NullRender/CreateWindow/Render settings). The DLL forces DX11 and disables DX12/DXR/HDR for all modes via `SettingsManagerApplyHk` (`Server.cpp:416-472`). In dedicated mode `SettingsManagerApplyHk` is not hooked (it is commented out in the dedicated table, `Server.cpp:852`). What the engine skips once `MainLoop.isDedicatedServer` is set cannot be seen from source. Evidence that a display is still needed: the **Docker image runs Xvfb** (see below), and Kyber allocates a console window for the stdin command console (`Program.cpp:141-165,247-256`).
6. **Docker:** **Linux container plus Wine, not a Windows container.**
   - `CLI/docker/Dockerfile`: `FROM debian:13`, installs `winehq-devel`, `xvfb`, `winbind`, and downloads **GE-Proton8-26 (wine-lutris build)** into `/home/kyber/wine`.
   - Env: `MAXIMA_WINE_COMMAND=/home/kyber/wine/bin/wine64`, `MAXIMA_DISABLE_WINE_VERIFICATION=1`, `MAXIMA_DISABLE_QRC=1`, `MAXIMA_CREDENTIALS=persona:password`, `KYBER_TOKEN`. `EXPOSE 25200`.
   - It copies in `Kyber.dll`, `vivoxsdk.dll`, `VanillaBundleAggregation.kb`, `ca_root.pem`, `maxima-bootstrap`, `wine-helper.exe`, `kyber_cli`.
   - The game install is mounted at `/mnt/battlefront`.
   - `CLI/docker/entrypoint.sh` runs `winecfg` on the Maxima wine prefix, sets `KYBER_BYPASS_DOCKER_I_REALLY_KNOW_WHAT_I_AM_DOING=1`, then:
     ```
     ./kyber_cli start_server --show-console --credentials="${MAXIMA_CREDENTIALS}" --token "${KYBER_TOKEN}" --game-path /mnt/battlefront/starwarsbattlefrontii.exe --verbose
     ```
   - `Module/Docker/entrypoint.sh` (an older variant) explicitly starts **`Xvfb :1 -screen 0 1024x768x16`** and `export DISPLAY=:1`, then runs `kyber_cli start_server --server-name Test --show-console ...`.
   - `start_server` refuses to run outside Docker unless that bypass env var is set (`start_server_command.dart:116-122`).
   - A Wine-specific hook: `ReadObfuscatedHk` (`0x1454DC150`) strips a `0x22C`-byte header, commented "Need to bypass signature verification to run under wine" (`Program.cpp:324-329`).
7. Other server details:
   - Map rotation and `startup-commands` are passed via `InitializeRequest.startServer` (`start_server_command.dart:438-471`).
   - Default level is `S5_1/Levels/MP/Geonosis_01/Geonosis_01` with mode `HeroesVersusVillains`.
   - Next level on `ServerLevelCompletedMessage` (`Program.cpp:383-394`).
   - Console commands are read from stdin and run with `Console_enqueueCommand` (`Program.cpp:262-284`).
   - A **"hosted" (listen-server) mode** also exists, where a player's client runs the server in-process: `Server::Start`, `clientServerHookOffsets` (`Server.cpp:188-240,823-841`), and `--no-dedicated` in the CLI.

---

## 4. Backend: Blaze is bypassed, not emulated

- **No Blaze or TDF emulation exists anywhere.** I grepped for Blaze/TDF/redirector/gosredirector. The hits are only TypeInfo enums (`OnlineBackend_Blaze`, `BlazeServerTimeout`, ...) and these:
  - `Server.cpp:538-558`: forces the presence backend to `OnlineBackend_Local` on the dedicated server.
  - `Misc/SquadManager/ServerSquadManager.cpp:307-309`: `// Disable blaze communication`, patches byte `0x143FEB970 = 0`.
  - `Server.cpp:830-833`: commented-out ProtoHttp/ProtoSSL hooks (DirtySDK HTTP).
- **Matchmaking is replaced by Frostbite's built-in direct-connect path.**
  - To join, the client sets **`ClientSettings.ServerIp`** and **`NetworkSettings.ServerPort`**, then forces the client state machine to `ClientState_Startup` (`Module/Source/Core/Client.cpp:80-124`).
  - The server sets `NetworkSettings.ServerPort = 25200` (hosted mode, `Server.cpp:192-195`).
  - Hooks involved: `ClientStateChange 0x140A8C7A0`, `ClientInitNetwork 0x140A8DE80`, `ClientConnectToAddress 0x140CB3990`, `OnlineManagerConnect 0x140CB3640` (`Client.cpp:416-430`).
- **Server discovery** goes through Kyber's own gRPC API, not EA:
  - The server calls `ServerBrowser.RegisterServer` and sends keepalives every 10 s (`Server.cpp:724-808`, `Proto/kyber_api.proto:72-84`).
  - The client gets the server from `ServerBrowser.GetServer(id)` and optionally overrides the IP with a pinned proxy (`Client.cpp:82-106`).
- **Transport: Kyber replaces the engine's socket layer with its own.**
  - Code: `Module/Source/Network/UDPSocket.cpp`, `SocketManager.cpp`, `WebSocket.cpp`.
  - Two paths: (a) plain UDP `sendto/recvfrom` for direct IP; (b) **proxied**, where game packets are tunneled over **WebSockets** to the Kyber **Proxy** (`ws://<proxy>/server` or `/client`, `WebSocket.cpp:35-60`). Proxied clients appear to the server as fake addresses `0.1.1.<proxyIndex>`, with a 2-byte port prefix (`UDPSocket.cpp:62-121,165-176`).
  - This acts as NAT traversal by relay. **No NAT punch-through was found.**
  - Set `KYBER_USE_DIRTY_SOCK` to fall back to the engine's DirtySock socket manager, commented as "makes KYBER servers compatible with non-kyber battlefront clients" (`Server.cpp:148-158`).
- **Player auth is done by Kyber, through the player name field.**
  - The client rewrites `NetworkCreatePlayerMessage.playerName` to `"KyberAuthentication:<joinToken>"` (`Client.cpp:37-57,271-280`).
  - The server strips it, calls API `ClientServer.ConsumeJoinToken`, then restores the real name and sets `OnlineId.m_nativeData = userId` (`Server.cpp:627-715`).
  - `Environment::getHostId/getHostIdentifier` are hooked to return the env var `EALaunchEAID` (`Program.cpp:666-669,744-745`).
- **What is still needed from EA:**
  - An **EA account login via Maxima** (Nucleus OAuth with the JUNO PC client ID, or `persona:password` manual login: `CLI/rust/src/api/maxima.rs:165-208`).
  - **Game ownership and license**, through the LSX flow (`RequestLicense`, `ChallengeResponse`) that Maxima serves in place of the EA app. For the server the CLI uses `LaunchMode::OnlineOffline("1035052", user, pass)` (`maxima.rs:110-111`). It also fetches and uploads an EA license file `.../ProgramData/Electronic Arts/EA Services/License/1035052.dlf` and a **Denuvo token** (`MAXIMA_DENUVO_TOKEN`, log prefix `Got Denuvo Token:`) to and from an optional license server (`start_server_command.dart:25,269-308,526-649`).
  - `OriginSDK::initialize` is hooked only to override the LSX port from `EALsxPort` (`Program.cpp:331-342`).
  - The Kyber API's own login takes the EA access token and checks EA entitlements, e.g. bans via `gateway.ea.com/proxy/identity/pids/me/entitlements?groupName=SWBF2PC&entitlementTag=ONLINE_ACCESS&status=BANNED` (`API/pkg/ea/api.go`, `API/internal/rpc/authentication.go:386`).
  - So: **EA identity, ownership and DRM are still used; EA game servers and matchmaking (Blaze) are not.**
- I could not determine from source whether the BF2 client still contacts Blaze in the frontend menus. Nothing hooks it; on the server side Blaze presence is replaced with Local.

---

## 5. Anti-cheat

- **There is no EasyAntiCheat or EA Javelin handling anywhere.** Grep for EasyAntiCheat/EAC/anticheat/Javelin found nothing in Kyber's own code. BF2 (2017) shipped with server-side **FairFight**, which only appears as kick-reason enums and strings: Kyber relabels "You've been kicked by Fairfight" to "KYBER" (`Program.cpp:671-683`) and reuses `SecureReason_KickedViaFairFight` for custom kick messages (`Client.cpp:237-269`).
- BF2's real client-side obstacle is **Denuvo DRM plus EA licensing**, and Kyber handles it by **running the game through Maxima legitimately**. Maxima provides LSX, the license `.dlf`, and the Denuvo token. Kyber does not crack it. There is also the Wine signature-check bypass `ReadObfuscatedHk` (`Program.cpp:324-329`).
- Kyber's own anti-cheat is server-authoritative plus a mod: the BF+ collection lists `"KYBER - Anticheat.fbmod"` (`Launcher/lib/features/frosty/helper/frosty_collection_writer.dart:16`). The server also has an admin-chat spoof check, AFK kick and a chat filter.
- **For Squadrons this is a gap:** Kyber offers **no precedent for getting past an EAC launcher**. The relevant fact is that BF2's exe is launched directly by Maxima with no AC bootstrapper in between.

---

## 6. Mod loading: in-memory, nothing written to the game directory

- **Format:** Frosty Mod Manager **`.fbmod`** files, parsed with `RT_FrostyLoadMod` / `RT_FrostyReadModResource` from **FBRuntimeTweaker** (`Module/ThirdParty/FBRuntimeTweaker/FBRuntimeTweaker/`, MIT, (c) 2023 Sean Kahler). Mod lists arrive as collections (`.kbcollection` / `.kmodcollection`) or a raw JSON list, are filtered to gameplay mods by the CLI/launcher, and are sent to the DLL in `InitializeRequest.modData` (`start_server_command.dart:310-376`, `RPC/API/Launcher.cpp:91-115`).
- **Entry point:** a hook on `FileSuperBundleManager::updateConfig` (`0x14024CA10`) constructs `ModLoader` before the engine reads its superbundle config (`Program.cpp:691-703`). `GAME_DATA_DIR` is cleared, commented "Kyber Mod Loader requires vanilla game data" (`Program.cpp:209-210`), which disables Frosty's ModData redirect.
- **Steps** (`Module/Source/ModLoader/ModLoader.cpp:1314-1395`, `ManifestMerger.cpp:308-391`):
  1. **Mount the mods directory into the engine's VFS.** Construct the engine's own `Win32FileSystem(basePath)` (`0x140241B30`) and call `VirtualFileSystem::mount(vfs, backend, "/kyber_mods")` (`0x14023A210`). There is also a NOP at `0x1402487AC`.
  2. **Treat each .fbmod file as an extra CAS file.** `GetNextModFile()` assigns each mod a synthetic CAS file index (`ModLoader.cpp:1761+`). `LoadCatEntriesHk` (`0x1402491B0`) registers the vanilla `/native_data/{Data|Patch}/<catalog>/cas_NN.cas` files and then each `/kyber_mods/<mod>.fbmod` into the `CasFileMap`.
  3. **Rewrite the layout manifest in memory.** `ProcessManifestHk` (`0x1454DB6D0`) parses the layout manifest buffer, merges mod entries, re-serializes it into an arena buffer, and calls the original with the swapped pointer. Per-bundle manifests are rewritten in `MergeBundleManifest` (`ModLoader.cpp:716+`) so EBX/RES/chunk entries point at `(fbFile, offset, size)` inside the .fbmod.
  4. To know where vanilla assets live, a precomputed index **`VanillaBundleAggregation.kb`** (magic `0x77778888`) shipped next to Kyber.dll is used (`BundleMerger.cpp:499-531`).
  5. **Chunk overrides:** hooks on `ResourceManager::beginChunkRead / pollChunkOperation / endChunkRead(Ex)` serve modified chunks from memory (`ModLoader.cpp:204-300`).
  6. **EBX:** hooks `EbxPartitionReader::process` (`0x14549B690`), `RuntimeDatabaseDomain::readEbxPartitionFromStream` (`0x14020F070`) and `fetchNewlyLoadedPartitions`. Custom "handlers" merge, rather than replace, specific asset types: `UITextDatabase`, `MeshVariationDatabase`, `NetworkRegistryAsset`, `ProfileOptionsAsset`, `UIMetaDataAsset`, `WSTeamData` (`Source/ModLoader/Handlers/*`, registered at `ModLoader.cpp:1325-1336`).
  7. About 21 ModLoader hooks in total (`ModLoader.cpp:1369-1395`, `ManifestMerger.cpp:377-383`).
- There is also live EBX editing (`Source/ModLoader/LiveEditManager.cpp`) and a Lua plugin system (`Source/Script/*`; its README says it is undocumented and meant for server tools).
- **Takeaway:** the approach is engine-generic Frostbite (VFS mount, CAS file map, layout manifest, EBX partition reader). The **BF2-specific parts** are the addresses, struct offsets, catalog names and sizes (`kCatalogSizes` = initialexperience, frontend, sp, sp_a1..., `ModLoader.cpp:1763-1771`), the asset paths used as duplicate sources, and the `.kb` aggregation file.

---

## 7. Knowledge of game data and type info

- **Generated SDK:** `Module/Public/SDK/Fb/*.h` has 129 headers named after Frostbite modules (`GameServer.h`, `WS.h`, `WSShared.h`, `Entity.h`, `DiceShooter.h`, `RvmDx12.h`, ...). Each contains `#define _KB_DECLARE_TYPEINFO(type, addr) inline const TypeInfo* typeInfo_##type = (const TypeInfo*)addr` and then thousands of `_KB_DECLARE_TYPEINFO(ServerBangerEntity, 0x1444EEFF0);` lines, **12,178 in total**. Only 128 class/struct definitions are in that folder.
- **Hand-written or dumped layouts:** `Public/SDK/TypeInfo.h` and `SDK.h` hold settings classes with offsets, e.g. `NetworkSettings`, `ServerSettings`, `ClientSettings`, `GameSettings`, `MeshStreamingSettings` (`TypeInfo.h:603+,684+,893+`), plus enums like `SecureReason` hashes and `OnlineBackend`.
- **Runtime reflection use:**
  - `TypeInfo::getName()` reads `typeInfoData->name` (`SDK.h:1063-1175`, `Source/SDK/SDK.cpp:61-114`).
  - `isKindOf` uses `ClassInfo_isKindOf 0x145440530`.
  - Instances are created through `DataContainerClassInfo_createInstance`.
  - Settings are looked up by name with `SettingsManager_getSettingsById(inst, "Network")` (`0x1401F7BD0`) or by TypeInfo (`0x1401F7C70`) (`Public/Core/Program.h:29-30,105-125`).
  - Messages are dispatched by type-name string compare in `MessageManagerDispatchMessageHk` (`Program.cpp:346-502`).
- **The dumper or SDK generator itself is not in the repo.** I searched for dump/TypeInfo-walk code and found nothing besides engine settings fields. The `Fb/*.h` files look tool-generated, but the tool is absent. `DebugHooks.cpp` hooks `ClassInfo ctor` (`0x1453BBCC0`) and `NativeTypeRegistry::init` (`0x1453DB400`) (`Source/Core/DebugHooks.cpp:29-30`), which hints at how types are discovered (catching TypeInfo registration at startup), but these are debug hooks, not a generator.
- The existing `E:\sws_community\notes\sws_sdk_types.json` (already present, not touched) would be the Squadrons counterpart of the `Fb/*.h` data.

---

## 8. Game-specific vs reusable

**Reusable almost as-is (no game addresses):**
- Launcher, CLI and packages (Dart/Flutter/Rust), apart from BF2 strings: game slug `star-wars-battlefront-2`, offer/content `1035052`, default map/mode, mod categories, `starwarsbattlefrontii.exe`.
- The Go API (auth, server browser, join tokens, parties, queue) and the Rust Proxy (generic UDP-over-WS relay with JWT). The EA entitlement check is BF2-specific (`groupName=SWBF2PC`).
- Docker / Wine / Xvfb / Maxima pipeline. Maxima itself is generic EA-app emulation, but **EAC is a new blocker for Squadrons**.
- Module infrastructure: hook manager (safetyhook), logging, console, gRPC client, event system, thread executor, WebSocket/UDP socket classes, Lua host, VoIP wrapper, FBRuntimeTweaker `.fbmod` parsing, LayoutManifest/BundleManifest (de)serializers. The manifest format may differ across Frostbite versions; I did not verify this.

**Requires reverse engineering again for each game build (the bulk of the effort):**
- About 380 function and global addresses, about 127 hook targets, 31 byte patches whose intent is mostly undocumented (`Server.cpp:912-933`, `Program.cpp:782-805`), and vtable indices (GameSimulation slot 31, entity vtable sizes 106/148).
- All struct field offsets in `SDK.h` / `TypeInfo.h`: settings classes, `ServerSpawnInfo`, `LevelSetup`, `ServerPlayer`, `MainLoop.isDedicatedServer@+0x8`, `EngineConnection` reason fields at `+0x608/+0x628`, `loadInfo+0x6E8` load state, and so on.
- The 12k TypeInfo addresses, which a type dumper can regenerate.
- **Game-logic code is BF2-specific and would need rewriting:** `WSGameSettings` ("Whiteshark" is BF2's internal codename), squads, battlepoints, abilities, kits, team data, persistence/stats, player extents, map rotation level names, mod handlers such as `WSTeamData`.
- **Dedicated-server bring-up logic** (MainLoop flag, GameSimulation vtable 31 dedicated init, forcing the presence backend to Local, DirtySock socket manager ctor) is the conceptual template for Squadrons. Every address, and the exact crash fixes, would need rediscovering. Squadrons may also have ASLR or relocation, unlike Kyber's fixed `0x140000000` assumption; Kyber does no rebasing (`PlatformUtils.cpp:70` gets the module handle but `HOOK_OFFSET` ignores it).

**Mentions of other games or Frostbite versions:** only one, a comment in `Program.cpp:650-657` saying the tick rate "works properly" "In Battlefield" with `Server.OutgoingHighFrequency`, but not in Battlefront. No mentions of Squadrons, Anthem, BF1/BFV or Frostbite version numbers in code or docs (`CHANGELOG.md` included). `ReleaseVersion` enum values in `TypeInfo.h` are BF2 internal builds.

---

## 9. License

- Root `LICENSE`: **GNU GPL v3** (29 June 2007), also stated in `README.md` ("GPL-3.0").
- `Module/LICENSE`: GPL v3. Source headers say "Copyright Armchair Developers / Sean Kahler. Licensed under GPLv3."
- `Module/ThirdParty/FBRuntimeTweaker/FBRuntimeTweaker/LICENSE`: **MIT**, (c) 2023 Sean Kahler.
- Other third-party code keeps its own licenses (EABase, zydis, sentry/crashpad/breakpad, spdlog, fmt, IXWebSocket, ...).
- Implication: any derivative of the Module or other Kyber code must be GPLv3. FBRuntimeTweaker can be reused under MIT.
