# Test 1: static map of starwarssquadrons.exe

2026-10-19. Everything here was measured on a copy (`work/starwarssquadrons.exe`, sha256 prefix
5172a17f…), byte-identical to the install's exe.
Tools: `tools/pemap.py`, `tools/disasm.py` (capstone, function bounds from .pdata),
`tools/xref.py` (RIP-relative refs, numpy), `tools/dispscan.py`, `tools/fbsdk.py`.
Ghidra 12.1.4 headless analysis is running in `work/ghidra_proj` (Denuvo block removed first).

## Code and Denuvo
.pdata lists 194,838 functions:

| section | functions | bytes | |
|---|---|---|---|
| `.text1` | 180,946 | 40.6 MB | plain x64 |
| `.data2` | 11,071 | 2.4 MB | plain x64 (executable despite the name) |
| `.idata` | 2,821 | 2.3 MB | **Denuvo VM** |

About 98.6% of the functions are readable. The base is 0x140000000 with no ASLR, so addresses are fixed.

## Runtime type info, recovered (`notes/sws_runtime_types.json`)
- `TypeInfoData` layout, measured:
  - `+0` name; `+8` flags (bits 5..9 = type code, 3 = class); `+0xA` size;
  - `+0x10` ModuleInfo*; `+0x18` array type; `+0x20` align, fieldCount;
  - `+0x28` super; `+0x30` fields.
  - The fields are 0x18 bytes each: name, flags, offset, ?, TypeInfo*. This is close to BF2's layout as Kyber documents it.
- 23,658 records across 128 modules: 8,490 classes, 2,667 value types, 1,203 enums, 11,276 arrays.
  Squadrons' game module is **"Incom"**. A **`GameServer`** module exists with 401 types.
- Class field offsets are verified: `StarshipMovementConfig` matches all 88 fields of the data dump, and
  `ClientSettings.ServerIp` is at +0x48. Enum values don't decode yet (a layout difference, still to fix).
- **Message/type IDs = a djb2-xor hash of the class name** (h=5381; h = (h*33) ^ c). Verified on 2
  constants in a message handler, and it decodes the backend enum: OnlineBackend_Local 0xB8566ABC,
  Lan 0xF73C8052, Peer 0xDEBD4193, Blaze 0xB7F53FE1, Playground 0xB3FE9490.
- Attribute values are *not* code constants: `AttributeId_AfterburnerMaxSpeed.Identifier` = 1456443907,
  which appears nowhere in the code. Attribute ids resolve at runtime from data.

## Movement (for the exploit patches)
- Runtime classes: `ClientStarshipMovementComponent` (0xE40 bytes) and `ServerStarshipMovementComponent` (0xB00).
  `getType()` is at 0x14169cef0 for the client and 0x1416a2630 for the server. The server vtable has getType at
  0x143615850, and slot +21 is the message handler 0x1416a32e0.
- Message ids: `StarshipMovementComponentDriftInputOptionsMessage` 0x7a2fbcff (handled in 0x1416a3260,
  built in 0x142e9ec70); `...AfterburnerActivatedMessage` 0x5cd1d391; `...DeactivatedMessage` 0xd6dd3710;
  `StarshipMovementSetThrottleMessage` 0xf63a4971 (handled in 0x14169dfb0).
- Boost on/off edge detectors (they broadcast the messages and are not the physics): client 0x1416a0230
  (flag at comp+0x278, previous value at +0xd51), server 0x1416a13a0 (+0xdb1).
- The physics routine itself is **not located yet**. Config-offset scans are too ambiguous (the offsets
  collide with other structs), and attribute ids aren't in code. Next is Ghidra's decompiler with the
  recovered struct types applied, plus a dynamic method in Test 2: a hardware watchpoint on the
  live `AfterburnerMaxSpeed` attribute value. Whatever reads it is the boost physics, and that is where
  pinballing's comparison lives.

## Server boot path: first anchors
- **Backend chooser at 0x1418f2930** (BF2's equivalent, `CreatePresenceBackend`, is at 0x1418D3380 per
  Kyber, so the layouts are close). It reads byte flags at +0x24..+0x29 of a spawn-info struct:
  - +0x29 → Playground
  - +0x24 or +0x25 → Local
  - +0x28 → Peer
  - +0x26 && `OnlineSettings.ServerIsPresenceEnabled` (+0x102) → Blaze
  - otherwise → **Lan**
- ~~So a server spawned with presence disabled lands on the built-in LAN backend.~~ **CORRECTED after
  decompiling the factory:** `FUN_1412e5be0` builds only Playground and Local (both via `FUN_1412ee990`)
  and Peer (`FUN_1412ee8d0`). For **Lan and Blaze it returns null**. Those backends are cooked out of this
  build, which matches Kyber's BF2 comment. So the fix is Kyber's: force Local. Simplest form: patch the
  chooser 0x1418f2930 to `mov eax, 0xB8566ABC; ret`, or set a +0x24/+0x25 flag.

## Ghidra decompile pass (2026-10-19, after the 24-minute analysis)
Scripts: `ghidra_scripts/SwsDecomp.java` (C + callers + callees → `work/decomp/<addr>.c`) and
`SwsCallers.java` (caller tree with strings). Runners: `work/decomp.sh`, `work/callers.sh`, ~9 s per batch.

The server spawn chain, bottom-up:
- `0x1418f2930` backend chooser (spawn-info flags → backend hash), called from
- `0x1412e5d40` (sets up the backend from settings and calls the factory `0x1412e5be0`), called from the
- **Server ctor `0x1412e5e50`** (object size 0x5410, allocated in `0x1412eed90`). It passes spawn-info
  `+0x26` on as a mode flag to `0x141272920` and `0x1417803b0`. Called via
- `0x1401bd610` (builds the server-side game object, vtable 0x1433ee4b8, and calls the factory above),
  from `0x1401bdc80`, from
- **`0x1401bfa70` = virtual "create server(spawnInfo)"**, slot 224 of the vtable at 0x1433ee038
  (267 slots). If spawnInfo+0x26 is set it calls `FUN_141b7b660(0)` first.
- Spawn-info flags, as read across these functions:
  - +0x24 / +0x25 → Local backend (single player / local host)
  - **+0x26 = isDedicated**
  - +0x28 → Peer
  - +0x29 → Playground
  - +0x40 → passed to `0x1401be830`

Main loop and clock:
- `0x1401aa760` and `0x1401bf4d0` select the simulation timing mode **by name**: "RenderFrames",
  "Dedicated", "Networked", "Sleeping". This is the clock Kyber renames to "Dedicated" on BF2.
- `0x1401b57b0` creates the **"MainLoop"** thread (callers 0x1401b4fe0, 0x1401b5010). Kyber's
  `MainLoop->isDedicatedServer` byte should be found in the object these build. Next step: decompile them.

What this means for Test 3: the engine's own dedicated path exists end to end (a spawn flag, a timing
mode, and the server ctor branching on it), as in BF2. We need the same three levers Kyber uses: the
MainLoop dedicated flag, a spawn call with isDedicated=1, and the backend forced to Local.
- Still to find:
  - MainLoop init and its isDedicatedServer flag;
  - GameSimulation init / spawnServer;
  - the settings manager getter (0x1401c3f50 is a likely candidate, called with a settings TypeInfo);
  - the Server ctor;
  - the client connect path (`ClientSettings.ServerIp`).

## Server boot map, completed (2026-10-19 evening), Kyber's levers mapped onto Squadrons
- **MainLoop:** thread setup `0x1401b57b0` → thread fn `0x1401b56f0` → `0x1401b5580` allocates MainLoop (0x698 bytes),
  ctor **`0x1401ae7d0(obj, flag)` stores `flag` at +0x8 = isDedicatedServer** (Kyber's offset). The flag comes from a
  stack byte that `0x1401b57b0` sets to 0. MainLoop vtable 0x1433ece00, and init = vtable+0x18. `0x1401b5580` returns `MainLoop+0x66C`.
- **Instance mutex:** `0x1401b57b0` creates `Global\{5C009BF0-B353-4fc3-A37D-CC14511238AC}_Instance_Mutex` and exits
  if it already exists (0xB7). This needs patching to run a server and a client on one PC.
- **GameSimulation:** base vtable 0x1433ee038, game subclass vtable **0x1433ee890**.
  - slot 9 = **`spawnServer(ServerSpawnInfo&)` `0x1401aac90`**: reads info+0x24/+0x25, writes +0x20 (tickFrequency) and +0x10 (playerManager),
    creates the server via `(*playerMgrHost+0x50)(…, info)`.
  - slot 31 = module registration (called unconditionally in setup, so NOT Kyber's initDedicatedServer here).
  - slot 47 = clock selector (`0x1401bf4d0` in the subclass), picking "RenderFrames"/"Dedicated"/"Networked"/"Sleeping" by name.
  - thread setup `0x1401aa070` ("GameSimulationLoop"); asserts in `0x1401aa5e3`.
- **ServerSpawnInfo** matches Kyber's BF2 struct: +0x10 playerManager, +0x18 LevelSetup&, +0x20 tickFrequency, +0x24 isSinglePlayer,
  +0x25 isLocalHost, +0x26 isDedicated, +0x27 isEncrypted, +0x28 isCoop (→ Peer backend), +0x29 isMenu (→ Playground backend).
- **The dedicated path is not yet known in Squadrons:** what the engine does with MainLoop+0x8 = 1 (does anything call spawnServer
  with isDedicated, or must we, as Kyber does from its slot-31 replacement?). Stage 3a measures this.
- **Injection timing:** MainLoop is constructed at startup, so the server needs a launcher that creates the process and loads the DLL
  before the main loop starts. Attaching to a running game (inject.exe) is too late for the flag.
- **Confirmed 2026-10-19:** the MainLoop dedicated flag is a compiled-in constant: `0x1401b5946: mov byte ptr [rbp-0x80], 0`
  (bytes C6 45 80 00), the struct passed to the main-loop thread. No command line, setting or config reaches it, so running
  this exe as a dedicated server needs a code change (in memory via the DLL, or 1 byte on disk in a patched copy).

## Test 3a on the real game, and the observation run (2026-10-19, 23:05-23:19)
- **3a (dedicated=1):** the early-bird APC load works on the real game (hooked synchronously at load; Denuvo fine), and
  MainLoop+0x8 = 1. Then there's no window, one core busy, and no spawn. The stack sampler shows the main loop ticking GameSimulation
  slot 35 (`0x1401bfe60`), which tests gameSim+0xF8 (server, where spawnServer stores it) and +0xE0 (client). Both are empty, so
  it idles. In dedicated mode something must spawn the server; that's the Kyber gap.
- **Observation run (dedicated=0), Pseudo started an AI match.** Every spawnServer comes from one caller, the client's
  start-local-server routine **`0x140a33160`** (return address 0x140a33408):
  - the front-end menu is a local server: `Game/Levels/Menu/Frontend/Frontend`, sp=1 localhost=1 **menu=1**;
  - the AI match: `Game/Levels/RootLevel/RootLevel`, options **GameMode=Frontline_Bots, SquadronComposition=Solo**,
    lobby sub-level `Game/Levels/Lobby/Lobby_MP`, map `Game/Levels/PvP/PVP_DestroyedMoon/PVP_DestroyedMoon`,
    sp=1 localhost=1 dedicated=0 menu=0.
- **LevelSetup (Squadrons):** +0x00 options array ({char* criterion, char* value} pairs), +0x08 root level name,
  +0x20 lobby sub-level, +0x30 map level; the empty strings point at 0x143f7b968. The client holds one at client+0x408.
  Engine functions: copy ctor `0x142e1db30(dst, src)`, assign `0x142d2e150(dst, src)`, dtor `0x140a26b30(ls)`.
- **The spawn-info build in 0x140a33160** (stack locals): +0x18 = &LevelSetup, +0x20 tick, +0x24 sp = (client+0x4ac & ~2)==0,
  +0x25 localhost = client+0x4b3, +0x26/27 = 0 (dedicated!), +0x28 coop = client+0x4b4, +0x29 menu (from a level lookup),
  +0x30 = 0x14317a6f0 (empty), +0x5c = a setting at +0x90. The spawn call is through `client+0x4b8` (the GameSimulation).
- **Client direct connect:** after spawning, `FUN_140cb0500(client+0x4e0, "localhost:%u" | "127.0.0.1:%u", password)`. That's the
  connect-to-address function for Test 4.
- **Plan for 3b:** in dedicated mode, capture the GameSimulation from the slot-35 tick. Build a LevelSetup in engine memory by
  copy-constructing from a template matching the AI match (or by assigning fields with the engine's own setters, still to find).
  Then call spawnServer on the main thread with sp=0, localhost=0, dedicated=1, menu=0. The backend is already forced to Local.

## Custom matches (2026-10-19 late), Pseudo: the server mode needed is custom lobbies, not Fleet Battles vs AI
- **Model** (runtime types): `CustomMatchSettings` { `BasicSettings` (GameMode enum Dogfight/Frontline/PVBCoop/PVBSolo @0x18,
  DogfightPointsValue, KickoffTime, MatchTime, MapIndex, Password, IsPublic, IsInviteOnly, BotDifficulty, PVBTeamId,
  AllowJoinInProgress, CreatorJoinAsSpectator), `Restrictions` (Ban X/Y/A/U/B-wing, TIE F/B/I/Reaper/Defender, aux,
  countermeasures), `Modifications` (CollisionDamage, RespawnTimeSeconds, per-faction max health/shield, aux and countermeasure
  cooldown and ammo, player damage taken, flagship shields, AllowDogfightLDResupplies) }.
  Asset `game/gameplay/custommatch/incomcustommatchconfigdata` maps each modification to a ship attribute id;
  `CustomMatchValidationData`: kickoff 60-3600 (120), match 60-3600 (1800), respawn 5-60 (20), dogfight points 1-100 (30).
- **Server side:** ServerCustomMatchSetupEntity / DataEntity / TeamAssignmentEntity; messages for settings updated or finalized,
  start match, admin, kick, assign team. Level: `LiveService/Levels/PvP/CustomGames/CustomMatchWaitingRoom/CustomMatchWaitingRoom`.
- **Mode routing** (`FUN_14174d540`, reads BasicSettings.GameMode): 0 Dogfight → Blaze game type `incomCustomDogfight`,
  1 Frontline → `incomCustomFrontline`, 2 PvB Coop → `incomCustomPvB` (all hosted on EA's servers; plus customGamePrivate/Public),
  **3 PvB Solo → local, via `FUN_14174d7e0`**: LevelSetup root=RootLevel, +0x20=CustomMatchWaitingRoom, +0x30=PVP_Yavin,
  GameMode=Frontline_Bots, SquadronComposition=Solo, sent as a load-level message through `(DAT_14419b5a8+0x60)->+0x48 +0x7a8`.
- On EA's infrastructure the multiplayer custom settings very likely travel as Blaze game attributes (strings custom_match, gameAttribs,
  SetGameAttributesRequest). Without Blaze, our server will need to supply CustomMatchSettings itself; how the server reads them is next.
- **3b status:** the spawn path passed end to end on stand-ins. A strcat_s overflow in the spawn dump (an abort on overflow) was found and fixed.
- **Next:** observe a local PvB Solo custom match (direct launch, dedicated=0) to see the real custom-match LevelSetup and flow.

## Test 3b on the real game (2026-10-19 23:47): the server is created, then deadlocks in startup
- The first launch with Steam off failed with an auth error (Steam must be running). The second launch worked.
- At 23:47:36 the DLL spawned with the custom-match setup (waiting room + Yavin, Frontline_Bots/Solo), dedicated=1.
  `spawnServer` never returned. Main thread 72292: user CPU frozen at 1781 ms and the identical stack for 3 minutes, so
  this is a deadlock, not loading. Memory went 586 → 635 MB (the server object and some resources were allocated).
- Real chain: spawnServer → create-server `0x1401bdc80` → `0x1401bd610` (Server ctor done; `FUN_1412e6670` after it)
  → **`FUN_140b9dad0`** (large server setup) → at 0x140b9dde9 **`FUN_14021bb10(DAT_1442f7c68, jobHandle)` = wait for a job**,
  queued just before by `FUN_14021b2b0(&h, DAT_1442f7c68, 0, &DAT_1441e2938, 1)`, right after an "ImpulseResponse" check (audio?).
- `FUN_14021bb10`: if the scheduler has workers (sched+0x20 != 0) and no help callback, it blocks on a lock without helping run
  jobs. All other threads were idle the whole time.
- **Hypotheses for next session:**
  (a) the job is bound to a thread or queue that we are blocking, because we spawn from inside the GameSimulation tick on the main thread;
  (b) the job belongs to a subsystem a dedicated build doesn't start (audio "ImpulseResponse", or render).
  Next: identify the job at DAT_1441e2938 (what it runs, which queue), compare with the context of the client's spawn path
  (what thread and phase calls `0x140a33160`), and try spawning from that same phase; or skip/neutralise that one job if it's audio-only.

### 3b digging, 2026-10-20 ~00:10 ET
- Timing ruled out. The game's own spawn runs at the END of the same GameSimulation tick we hook (FUN_1401bfe60 → 0x1401c027e
  → FUN_1401bc8c0(client+0x528), the client update → 0x140a33160 start-local-server). In dedicated mode client+0xE0 is null, so
  that call never happens; ours runs right after the tick returns, the same phase.
- Server::init's dedicated-only block (Server+0x8c), in order: flags on DAT_14419be98; pins the thread to the settings core
  (param_1[0x12]+0xa0); FUN_1401fd1c0(platform) maps "Win32"→0, "DedicatedServer"→1, and if not DedicatedServer it adds
  hash("ImpulseResponse") to a list (FUN_1401f82f0); queues job DAT_1441e2938 on scheduler DAT_1442f7c68 and spin-waits
  (the loop looks up thread "Main"); then FUN_140cd55e0 preloads every level in the LevelListReport asset
  (bundle win32/game/incomgameconfiguration; 3 BuiltLevels). If the report lookup fails, DAT_1444bac10 = 0 and the next
  read is a null dereference: a crash candidate for the 00:00 run.
- Probe additions: vectored crash catcher (skips the probe's own guarded reads, bounded to the stack region, re-entry
  guard); the job object's first 6 qwords and what they point at are logged at spawn.
- RUN 00:10 (crash catcher on): the crash is the engine's OWN fatal check, not a null bug. FUN_140218aa0 reads a CAS block
  header (big-endian size, codec byte < 0x1b, '70' marker, size ≤ 0x10000) and on a bad one calls FUN_140217390 and writes
  0xdeadbabe to address 0. Two threads hit it in the same millisecond: the main thread inside the dedicated block's job wait
  (14021bb10 → help callback 140a2c500 → resource-manager pump FUN_1401f7960(&DAT_1442d18c0, 1) → FUN_14021a360 → decoder),
  and a worker through 141a3e450 → FUN_1401f7960 → same decoder. The job object at DAT_1441e2938 begins with the inline
  string "game/incomgameconfiguration" (a bundle). DAT_14419be98 (flagged by the block) is used by the VFX/render code.
  Two different failures from the same call (deadlock 23:47, crash 00:10) fit a race in that data path.
- Next experiment: spawn_dedicated=0 (MainLoop still dedicated, server object non-dedicated), which skips the block entirely.
- RUN 00:14 (spawn_dedicated=0): crashed 22 ms in, at 0x1401bfae5 in createServer FUN_1401bfa70: when spawnInfo+0x26 is 0 it
  reads [[gameSim+0xD0]+0x2308] (a client-side object, null in a dedicated MainLoop) and passes +0xa50 of it to the Server
  ctor. So the non-dedicated spawn needs a client: dead end.
- Next: spawn_dedicated=1 + skip_server_job=1, which forces the 0x140b9dd89 → 0x140b9decb jmp (bytes verified in the exe),
  skipping the bundle job and its wait; setup continues at the LevelListReport preload FUN_140cd55e0.
- RUN 12:17 (skip_server_job=1): skip worked; crashed at 0x140cd5678 in FUN_140cd55e0 (read 0x20): LevelListReport not
  found, exactly the predicted null read. The skipped job was what loads its bundle.
- ROOT CAUSE CANDIDATE: bundle "Systems/FrostbiteStartupData" (string 0x1434ad998, ONE code reference: 0x140a2c0c5) is loaded
  only by the CLIENT constructor FUN_140a2b4a0 (called from createClient FUN_1401bf8b0 via FUN_1401bce80). A dedicated
  process has no client, so it is never loaded, and Server::init's incomgameconfiguration load runs without it (deadlock
  once, corrupt-block crash once). Client sequence: queue FUN_14021b2b0(&h, [0x1442f7c68], 0, &name, 1) with a lock-inc of
  the dword at 0x1442d18c0; wait FUN_14021bb10(sched, h, 0, 0x140a2c500) (blocks, pumping via the callback + Sleep(1));
  release FUN_14021cb70(sched+0x22600+((h&0xffffff)>>16&0xf)*0x1180, h&0xffff, (h&0xffffff)>>20) under that critical
  section; decrement; pump FUN_1401f7960(&0x1442d18c0,1,0); then a virtual on DAT_14419b9b0 (+0x10), not replicated yet.
- Next run: load_startup_data=1 (the DLL replicates that sequence before spawning), skip_server_job=0.
- RUN 12:21: my startup-data call crashed: FUN_14021b2b0 takes 9 args (9th = uint at caller [rsp+0x40], decompiler's
  in_stack_00000048), I passed 5. Fixed.
- RUN 12:23: FrostbiteStartupData queued (handle 01000002) and loaded in 35 ms. No crash. spawnServer never returned: main
  thread in Server::init's job wait; worker 59940 idle until the spawn (12:23:41), then 100% CPU (~30 s by 12:24:13) in
  FUN_14026edb0, a streaming EBX parser state machine (checks magic 0x0fb4d1ce / byte-swapped). The 23:47 run's 165 s
  spin was the same. So the server's incomgameconfiguration load gets past CAS decode now and loops in EBX parsing.
- Next run: load_game_config=1 loads DAT_1441e2938's bundle ourselves BEFORE the spawn (before the dedicated block adds
  "ImpulseResponse" to the hash list at DAT_1442f8008). Spins too → loading context; loads → the exclusion list.
- RUN 12:27 (our own blocking load of incomgameconfiguration BEFORE the spawn): hangs too, with no CPU. Main thread in our
  wait → pump FUN_1401f7960 (holds mgr CS +0x268e0) → FUN_1401facb0 → wait; worker blocked entering the "resourceMgrJob"
  lock at mgr+0x26950 in FUN_1401f7ba0. So the ImpulseResponse exclusion is NOT the cause: a blocking wait inside the
  GameSimulation tick deadlocks the resource manager. (The client queues this bundle without waiting.)
- Non-blocking check: FUN_14021ba60(sched, handle) (takes the slot lock itself). FUN_14021ca90 is a timed (11 s) wait.
- Next run: per-tick state machine in the tick hook: queue startup data, poll, release; queue game config, poll, release;
  then spawn. Pumps the resource manager each tick only if a bundle hasn't finished after 5 s.
- RUN 12:30 (non-blocking loads): FrostbiteStartupData done in 0-3 ms, incomgameconfiguration in 63 ms. Spawn then stuck in
  Server::init's own job wait (re-queues the same bundle, waits inside the tick; idle CPU). Next: skip_server_job=1 again,
  now that the bundle (and LevelListReport) is loaded before the spawn.
- RUN 12:32 — MILESTONE: spawnServer RETURNED with a dedicated server object. Recipe: MainLoop dedicated + Local backend;
  per-tick non-blocking loads of Systems/FrostbiteStartupData then game/incomgameconfiguration (the client's two bundles);
  skip Server::init's duplicate job (0x140b9dd89 jmp); spawn with the custom-match LevelSetup, isDedicated=1. gameSim+0xF8 =
  server, port(info+0x58) = 25200. No crash for ~55 s. Main thread then idles in the frame limiter FUN_140cde6d0 (waitable
  timer) via 0x140ca3487 → 0x1401abe81 → MainLoop. Little CPU after the spawn: the level probably did not start loading.
- Next run: post-spawn heartbeat from the tick hook; the watcher records netstat for the game pid 15 s after the spawn.
- RUN 12:35: heartbeat 300 ticks / 10 s (30 Hz) for the whole run; 715 MB; NO sockets for the pid 15 s after the spawn.
  The server exists and simulates but loads no level and does not listen.
- Level load: Squadrons has no ServerLoadLevelMessage; it has ServerLoadLevelMessageBase (0xf8429a0e, category 0xc6e40ba0)
  and ServerPeerLoadLevelMessage (0xcc048f59). FUN_140ba0c20(LevelSetup*, -, flag, -) = Kyber's ServerLoadLevelMessage_post:
  allocates the 0x70 message, copy-constructs the LevelSetup at +0x30 (0x142e1db30), +0x6a = flag (callers pass 0), posts to
  the message manager at [DAT_144199b60+0x10]. Prologue 40 57 48 83 ec 30.
- Next run: post_load_level=1 posts it with the spawn LevelSetup on the 2nd tick after the spawn.
- RUN 12:43 (post_load_level): the message was handled; 2.2 s later assert 0xdeadcccc in the server level-load state
  machine FUN_140b950a0 (state at loadState+0x6e8), state 2: bundle jobs report done but server+0x110 (loaded level) is null.
  Cause chain: handler FUN_140b9fe10 sets loadState+0x694 = (server+0xc8 == 0) (0xc8 = "same level already loaded",
  set by FUN_140ba0340); FUN_140cd6150 then looks up the level's bundle list via FUN_140cd57f0(name), which searches the
  vector at 0x143ea0768/0770 built by FUN_140cd55e0 from LevelListReport.BuiltLevels: these are the LevelDescriptionAssets
  of RootLevel_SP, RootLevel (b7dde75c..., LevelName 'Game/Levels/RootLevel/RootLevel', +0x30 name, +0x48 Bundles,
  +0x58 SuperBundles) and Frontend, all in bundle win32/game/incomgameconfiguration. Not found → no bundle job → assert.
  Suspect: the preload ran (inside our spawn) before that bundle's objects were registered.
- Next run: post only once FUN_140cd57f0(root) succeeds; otherwise log the list and re-run FUN_140cd55e0 each second (30 s).
- RUN 12:50: lookup found RootLevel at once (list already had 3 entries), so the "empty list" theory for the 12:43 assert was
  WRONG; the 12:43 assert's real cause is unexplained. Posting on the first tick after the spawn, the load got further:
  state machine → FUN_140b960b0/FUN_140b96890 → entity creation; 1.7 s later AV at 0x14115910b in FUN_1411590d0
  (r9 = [rsi] = 0: the data object passed as param_5 = *(param_4+8) has a null first qword, i.e. unresolved data).
- Next run: crash catcher logs all GPRs, and at that rip dumps 8 qwords of rsi (data) and rdi (owner).
- RUN 12:53: posted on the first tick again; 0.66 s later a different AV (0x14023ebbc, read -4 through null rsi: an array
  count on a null array) in core code. Three level-load runs, three different failures (deadcccc / unloaded entity data /
  null array) from identical inputs → a race. ServerSettings (DAT_144331c50, 264 bytes): JobEnable +0xee, ThreadingEnable
  +0xef, DedicatedServerCpu +0xa0. ThreadingEnable is read at runtime at 0x1401af7ae (MainLoop), 0x1401bf269
  (GameSimulation) and 0x140b95b50 (level-load state 3). Kyber forces it false on BF2.
- Next run: server_threading=0 clears ThreadingEnable right after the spawn (logs JobEnable/ThreadingEnable/CPU first).
- RUNS 12:55 (threading off) and 12:59: same 0xdeadcccc. Full registers + dump: level manager 0x42400ae0 name
  'Game/Levels/RootLevel/RootLevel', +0x110 (LevelData) = 0, loadState+0x62c = 0, +0x694 = 1. The assert block is a
  consistency dump: r13 = 1 says a level-bundle job WAS queued (+0x62c cleared on completion by FUN_140cd6420). My
  "waiting room not in list" guess was wrong (the name is RootLevel, which is in the list).
- FUN_140cd6420: after both jobs (+0x628 shared bundles into compartment 3, +0x62c the level's bundles into server+0x90)
  are done, levelmgr+0x20 (= +0x110 from the manager base) = FUN_1401f9860(server+0x90, &LevelData type 0x144a17ea0),
  a type lookup walking compartments via +0x62. It proceeds even when that is null. RootLevel's description Bundles include
  'Game/Levels/RootLevel/RootLevel'. The 12:50 run got past this, so: timing again (job done before objects registered).
- Next run: hook FUN_140cd6420 (15-byte prologue): when both jobs are done but LevelData isn't findable, return 0 and
  retry next tick (log every 2 s, 30 s cap).
- RUN 13:03 (LevelData wait): found at once and passed state 2; then AV at 0x140b9537a in state 4: [[levelmgr+0x110]+0x100]
  = LevelData.AutoLoadBundles (CString-Array) is NULL; reading its count at -4 faults. Loaded arrays are never null, so the
  LevelData was found while still being filled in. LevelData fields near: +0xd0 VoiceOverLogic, +0xe8 CameraModes,
  +0x100 AutoLoadBundles, +0x110 AutoloadedDetachedSubWorldDatas, +0x118 GameConfigurationName.
- Next run: the wait requires +0xd0/+0xe8/+0x100 non-null for two consecutive checks.
- RUN 13:07: my hook crashed: FUN_1401f9860 locks through (&DAT_1442d18c8)[server+0x90] (compartment table) which was still null; fixed by checking the table entry first.
- RUN 13:09 (guarded wait): the server compartment existed at first, but LevelData never appeared in 30 s (heartbeat steady
  at 30 Hz throughout, 675 MB, still no sockets). On fall-through the ENGINE's own FUN_1401f9860 crashed on a null
  compartment-table entry in the walk (rsi=0xbbe), i.e. a compartment was torn down meanwhile. Reading: the level's bundle
  load into the server compartment FAILS and is unwound; 13:03's "found" LevelData was the half-built object of a load in
  progress. Candidate causes: the description's SuperBundles (Flairs, Game/Levels/RootLevel/RootLevel, Globals, Pilots,
  Starships, StarshipsCine, StarshipsGarage, UIStateGroups) not mounted on the dedicated path (the client mounts per level),
  or another client-side prerequisite. Next: static work: who reads LevelDescriptionAsset.SuperBundles (+0x58), the
  non-dedicated path FUN_140a2f990, and whether the engine has a reachable error/log channel to hook.

### Listen-server route (2026-10-20, Pseudo: "ideally the user would be able to self-host from within the game")
- Server::init: server+0x8b = spawnInfo+0x24 (isSinglePlayer). If 0, it starts the network object at [DAT_144199b60-derived
  param_1[0x18]+0x78] (vtable +8). Backend chooser FUN_1418f2930: menu(+0x29) → Playground; localhost(+0x25) or sp(+0x24)
  → Local; else coop(+0x28) → Peer; dedicated with [DAT_143f5e1e0+0x102] → Blaze; else Lan. So sp=0 + localhost=1 → Local.
- Experiment: server.ini dedicated=0, force_local_backend=0, listen=1: the client's own non-menu spawn (solo custom match)
  gets isSinglePlayer cleared. Watch for UDP 25200 while in the match. Dedicated config kept as build/server_3b.ini.
- RUN 13:14 (listen): match spawn went networked and returned; ~8 s later the client respawned the Frontend (kicked to menu).
  The client's start-local-server FUN_140a33160 builds the spawn info from client+0x4ac (sp), +0x4b3 (localhost),
  +0x4b4 (coop), spawns, then connects its own client with FUN_140cb0500(client+0x4e0, "%s:%u", pw): "127.0.0.1" if
  localhost, else "localhost" ("" if coop), port = spawnInfo+0x58 as written by the server. Note: netstat also shows the
  game's normal TCP sessions to EA services (login), true of every direct launch since Test 0.
- Next run: in-process UDP watcher logs every port the game opens for 20 s after the listen spawn, plus the server's port.
- RUN 13:19 (listen + in-process UDP watch): server reported port 25200 but bound NOTHING in the 3.5 s before the client
  gave up (kick at +3.5 s; the only new port, 54131, came after). So with sp=0 the client tries real UDP to 127.0.0.1:25200
  and nobody listens yet.
- Import slots: the exe's real IAT is at the start of .xtls (on disk each slot = RVA of hint/name; ordinals for most ws2_32
  calls). ws2_32: connect 0x14307fed8, socket 0x14307fee0, bind 0x14307fef0, listen 0x14307fef8, sendto 0x14307ff10,
  recvfrom 0x14307ff18, WSARecvFrom 0x14307ff30, WSASocketW 0x14307ff70, closesocket 0x14307ffd0. Code never references
  the import thunks in .idata directly. The DLL now swaps bind/socket/WSASocketW/sendto for logging pass-throughs
  (stand-in tested: swapped on the first retry, calls behave normally).
- RUN 13:25 (listen + socket wrappers): wrappers installed at 13:25:27. All logged sockets are WSASocketW af 23 (IPv6), all
  via the helper FUN_1406914b0 (EA DirtySock socket layer: callers include protoupnp; 0x1424xxxxx = DirtySDK/Blaze libs).
  At the listen match spawn only two TCP sockets were created, no UDP; kick at +7 s. NO bind calls were seen at all,
  although UDP ports 9999/50747 opened: some code binds through a second route (likely the .idata import copy at
  0x15f143a30 used by Denuvo-side code). Bind logging is therefore incomplete.
- Backends: factory FUN_1412e5be0 builds Playground and Local with the SAME ctor FUN_1412ee990 (LocalServerBackendData,
  no fields) and Peer with FUN_1412ee8d0 (PeerServerBackendData : PresenceBackendData, CreateParameters
  PeerCreateGameParameters = an EA matchmaking game). So Peer = EA-hosted, ruled out; Local = in-process.
- spawnInfo+0x08 (an external network object for Server::init's context+0x78) is 0 in the client's own match spawns too.
- Plan for in-game hosting: keep the host's client single-player (memory socket), and open the server's UDP listener for
  remote players once its level is loaded. Needed: the game-server transport among FUN_1406914b0's callers and its trigger.

### Kyber's transport, mapped onto Squadrons (2026-10-20, Pseudo chose "Kyber's way")
- Kyber source (sparse clone, read-only): ext/src/kyber @ 4627e3e. Engine interfaces it implements (BF2):
  ISocketManager {Destroy, Connect(addr, blocking), Listen(addr, blocking), CreateSocket}; ISocket {dtor, Send,
  ReceiveFrom, ReceiveFromWhen, SetBroadcast, SetPeerAddress, PeerAddress, SetDefaultPacketInfo, BlockingMode,
  SetBlockingMode, ReceivePulse, Pulse, SendPulse, NativeSocket, Port, Address} + IUDPSocket {Connect, Listen, Create};
  SocketAddr = 64-byte sockaddr + length byte. Hosted mode: substitutes its manager in Server::start (BF2 0x140BD17E0)
  and sets info.isLocalHost = false; client: ClientInitNetwork(inst, sp, localhost, coop, hosted) gets a
  SocketManagerCreator at inst+0xA8 (vtable: createSocketManager); NetworkSettings.ServerPort = 25200.
- Squadrons: createServer passes [[gameSim+0xD0]+0x2308]+0xa50 (non-dedicated) or 0 (dedicated) through
  FUN_1401bdc80 → FUN_1401bd610 (6th arg) → FUN_1412eed90 → FUN_1412e5e50 (7th arg; stored at +0xa30; vtable
  0x143687c18 gets a fast path) → FUN_141272920 (9th arg; stored at +0x3fe0). Its users call vtable +0x20 and +0x38 with
  byte vectors, so it may be a session/messaging channel rather than the socket manager: unconfirmed.
- Client: FUN_140a33160 (local host) and FUN_140a338d0 (other path) write client+0x4b3/+0x4b4; client+0xa8 holds an
  object called at vtable +0x48 (BF2's creator lives at +0xA8). Connect = FUN_140cb0500(client+0x4e0, "host:port", pw).
- Next run (capture only, listen=0): hooks on FUN_140cb0500 and FUN_1412e5e50 log the objects and vtables involved.
- CAPTURE RUN 13:41 (normal solo custom match): every server spawn (Frontend, match, Frontend) gets the SAME a7 object
  0x3f8fa330, vtable 0x143687c18 with 11 entries (then string data). Entries are thin pimpl wrappers over [obj+8]:
  +0x00 attach(owner) = impl+0x10 = owner; +0x08 detach; +0x10 FUN_1401e4ad0(impl+0x38); +0x18 FUN_14191b530;
  +0x20 send(byte vector, moved) → FUN_14191b5d0(impl, &vec, 0, this); +0x28 FUN_14191b6a0; +0x30 FUN_14191b740;
  +0x38 sendTo(a, b, c, byte vector) ; +0x40 FUN_14191bad0; +0x48 FUN_14191bb70; +0x50 FUN_14191bc10.
  FUN_1412e5e50 attaches the online server's callback interface (server+0x20, vtable 0x14358bb78) as owner.
  Reading: this is the in-process server<->client message transport (this branch's analogue of the memory socket
  manager). Kyber-style plan: our own implementation of this interface on host and clients, carrying remote traffic
  over UDP. Needed: exact method semantics and the owner callback interface (vtable 0x14358bb78); then a logging
  pass-through on live traffic before writing the transport.
- Client connect obj (client+0x4e0) vtable 0x143509488; members +0x18 vt 0x1433edb68, +0x20 vt 0x143580958,
  +0x28 vt 0x1434ae128, +0x38 vt 0x1433fa218. Connect target is always 127.0.0.1:25200 with a 9-char password.
- CORRECTION: the 11-method a7 object is a report/session channel (FUN_1412752b0 sends mission-progress blobs via +0x20;
  FUN_1412754d0 serializes a 4000-byte stats bitstream from +0x3cd8 and sends via +0x38 with an id). Needed later for
  results/progression; NOT the game connection transport.
- Game connection path (client), matching Kyber's BF2 map: FUN_140cb0500 (= ClientConnectToAddress) parses "host:port"
  (FUN_140296090) → FUN_140cb05c0 (= OnlineManagerConnect(inst, SocketAddr&)) builds the connect request (protocol from
  settings +0x90) → FUN_140d76240(peer = connobj+0x40, conn = connobj+0x48, addr, packet) → FUN_140d74ca0(peer+8) →
  FUN_140d7c280(transport = peer+0x88, addr) copies a 64-byte address + length at +0x40 (= Kyber SocketAddr) →
  FUN_140d7be30 allocates a 0x740-byte connection. In FUN_140d76240 the transport's [+0x740] object is called at vtable
  +0x38 returning {minSize, maxSize, recommendedSize, overhead, alignment} = Kyber ISocket slot 7 PacketInfo. So the
  socket is transport+0x740 and the ISocket layout looks like BF2's.
- Next capture: after connect, dump the socket's 20-entry vtable, the transport's, and client+0xa8 (creator candidate).
- CAPTURE RUN 14:01: client socket at transport+0x740 = vtable 0x1433fa240 (20 slots); the socket MANAGER vtable is
  0x1433fa218 (5 slots: 0x140295bb0, 0x140295dd0, 0x140295c40, 0x140295f90, 0x140295b50 = Destroy, Connect, Listen,
  CreateSocket, Close; same as Kyber ISocketManager + SocketCreator::Close), directly before the socket's vtable.
  Socket slots: 0 dtor 0x140294ff0, 1 Send 0x1402951c0, 2 ReceiveFrom 0x1402933a0 (→ slot 3), 3 ReceiveFromWhen
  0x140295480, 4 SetBroadcast (ret 0), 5 SetPeerAddress 0x140295800, 6 PeerAddress 0x140295830, 7 PacketInfo 0x140295890
  (max 1600), 8 BlockingMode (ret 0), 9 SetBlockingMode 0x1401ae7c0, 10 ReceivePulse (nop), 11 Pulse 0x1402958e0, 12
  SendPulse (nop), 13 NativeSocket (ret 0), 14 Port (+0x54), 15 Address 0x1401b9bc0, 16 ? (nop, extra vs BF2),
  17 Connect 0x140295700, 18 Listen 0x1402957a0 (records addr, ntohs port), 19 Create 0x140295760.
  This is an IN-MEMORY loopback socket: Send copies into a pooled 0x6cc buffer and delivers via a global port-keyed table
  (DAT_1441e5cc8 under lock DAT_1441ef030). No OS socket ever opens (explains no 25200 in netstat).
- HOOK POINTS (Kyber's): server: Server::init 3rd arg → FUN_140b9eaa0(server, info, overrides, socketManager): if
  null and overrides+8 null, it creates the in-memory manager (stored server+0x110). Caller FUN_1401bdc80 passes
  *(*(onlineServer+0x7e8)+0x48) (EA backends supply a network manager; Local supplies none).
  Client: FUN_140cb0080(connobj, mode, creator, ., .): skips creation if connobj+0x38 already set; mode 0/2 → in-memory
  manager; mode 1 → creator->vtable[0]() (Kyber's SocketManagerCreator); result stored at connobj+0x38.
- Plan: (1) document all 25 slots from the in-memory implementation; (2) logging pass-through wrapper over the engine's
  manager/sockets in a normal solo match; (3) our UDP implementation (host listens 25200; host client + remote clients
  connect over UDP); (4) remote join, rulesets via protocol version, report channel.
- Socket spec written by a subagent: notes/socket-interface.md (VERIFIED/INFERRED tags). Spot-checked: receive is socket
  slot 2 (buf, 1600, &when) at 0x140d7b710, result < 1 stops, 0 would spin; slot 3 returns -1 when empty; manager slot 4
  is the deleting destructor (Close lives on the second base 0x1433fa210, called only by the socket destructor).
- Pass-through (socket_log=1): 12 vtable entries patched in place (manager 0-4; socket 0, 1, 2, 5, 17, 18, 19), all or
  nothing after checking every slot holds the analysed function; Send/receive counted per socket, summaries every 5 s.
  Stand-in test: all pass (values, timestamp and -1 pass through).
- RUN 14:24 (pass-through, normal solo match): the whole game connection runs through these sockets. Per spawn the
  server's manager does Listen('127.0.0.1:25200') and the client's Listen('127.0.0.1:25100') + peer 127.0.0.1:25200; the
  server's socket learns the client as peer from receives. ~90 packets/s each way, ~1.25 MB per 2-minute match, counts
  mirror exactly. The manager's Listen does NOT call the socket's own Listen slot.
- UDP transport (transport=udp, udp_bind): the engine sockets keep their fields; manager Listen also binds a non-blocking
  OS UDP socket on udp_bind:port (SIO_UDP_CONNRESET off, 1 MB buffers); Send → sendto(peer at +0x58, len +0x98);
  receive → recvfrom, peer = sender, when = timeGetTime, -1 when empty; destructor closes it. Stand-in: real loopback
  round trip both ways passes. Next run: a solo match entirely over real UDP on 127.0.0.1.
- RUN 14:30 — MILESTONE: transport=udp on 127.0.0.1. netstat during the run: the game holds UDP 127.0.0.1:25100 and
  127.0.0.1:25200 (our sockets). Front end → 2-minute solo match → front end, all over real UDP; match ~6,400 packets
  each way (client→server 463 KB, server→client 1.09 MB); sent/received counts mirror (6253/6253, 6438/6432 = in flight
  at teardown); no sendto/recvfrom errors. Pseudo's feel report pending.
- Remote join needs: host udp_bind=0.0.0.0 (firewall prompt; router forward or VPN); joining client connects to the host's
  address instead of 127.0.0.1 and does not spawn its own server; the 9-char connect password (client+0x458, probably a
  per-session token) must be understood; ruleset via protocol version. Test needs a second game instance (second PC +
  account, or two instances on one PC with the single-instance mutex patched).

### Join path (2026-10-20)
- ClientSettings (size 0xb0, type va 0x144999670): ServerIp +0x48, SecondaryServerIp +0x50.
- Client state machine FUN_140a30d10: one state (0x140a31282) checks no local server exists, calls
  FUN_140a31d50(client, sp 0, localhost 0, coop 0, 0) (networked init), then if client+0x460 (address string) is non-empty
  calls FUN_140cb0500(connobj, client+0x460, client+0x458 password) — the engine's own remote-join flow (Kyber drives
  the BF2 equivalent via ServerIp + client state Startup). Writers of +0x460/+0x458: ctor FUN_140a2b4a0 (clears) and
  FUN_140a338d0 (from a message: string at msg+0x30 → +0x460; 64-byte SocketAddr msg+0x50 → +0x468, len → +0x4a8).
  Other connect path: connobj update FUN_140cb0440 connects to connobj+0x7c when connobj+0xc0 == 1 (deferred).
- Next host test (one PC): listen=1 + transport=udp: server networked (sp 0, localhost 1, Local backend), host client over
  real UDP loopback. Join trigger still to be worked out (state number / message).
- RUN 14:40 (listen=1 + udp): match server networked (sp 0, localhost 1). Host client connected over real UDP: 7 packets
  client→server (299 B), 15 server→client (206 B), then the CLIENT destroyed its socket 83 ms after the connect; server
  torn down 7 s later; back to menu. Handshake reached both ways; the client quits. Suspect a configuration mismatch
  (client initialized single-player via FUN_140a33160's flags vs networked server; cf. SecureReason_ConfigurationNotAllowed).
  Next: log the client disconnect reason (Kyber hooks ClientConnectionOnDisconnected on BF2), and drive the host's own
  client through the engine's networked join path (FUN_140a31d50(client, 0, 0, 0) + connect) to 127.0.0.1.
- SecureReason values = djb2-xor of the SecureReason_* names (all present in the exe; Ok = 0x102cbecd, stored at
  connobj+0xc4 on connect; ConfigurationNotAllowed 0x345f5d54, WrongProtocolVersion 0xb0d2286, WrongPassword 0x18237655).
- FUN_140a31d50 = ClientInitNetwork(client, sp, localhost, coop, hosted); FUN_140a33160 calls it with
  sp = (client+0x4ac & ~2) == 0, localhost +0x4b3, coop +0x4b4, hosted = (+0x4ac == 1), after assigning the pending
  LevelSetup at client+0x408. Mode inside: 0 if sp or localhost (in-memory manager), else 1 (networked; needs a
  socket-manager creator from FUN_140cb0080's param 3, else fails) or 2 (coop). The 14:40 run had a sp client vs a
  networked server.
- Next run: hook FUN_140a31d50 (15-byte prologue): listen + non-Frontend level → sp 0, localhost kept (mode 0, our UDP
  sockets); connection watcher logs connobj state/reason changes by name.
- RUN 14:48 — HOST WORKS: listen=1 + udp + client-init hook. Server and host client both networked (client conn states
  2 → 3, reason Ok throughout); ~3.5-minute match, ~2 MB server→client over real UDP. Pseudo: "the game played just
  fine ... played through the whole opening and saw no problems". Downsides: at "start match" an EA error dialog
  "You have lost connection to EA Servers ... Error code: 327" (dismissable, then the match starts); ship loadout
  selection disabled (default loadout only), presumably because networked play takes unlocks/loadouts from EA's
  backend. Kyber runs its own persistence layer server-side; for community servers: supply loadouts (e.g. all unlocked).
- Password check (server): FUN_140d7c400 asks a validator (transport+0x788, vtable +0x28) per request: 1 accept,
  4 WrongPassword (0x18237655), 3/other reject. Next run logs whether the client's 9-char password equals
  ServerSettings.ServerPassword (+0xb0), comparing in memory only. Host now udp_bind=0.0.0.0 (firewall prompt).
- Join mode built: [server] join=<host> rewrites the match connect from 127.0.0.1:25200 to the host (uses the latest
  client-init level to tell match from menu). Bug fixed before use: in h_connect, obj is the connection manager itself,
  not &client->+0x4e0.
- RUN 14:56 (host, udp_bind=0.0.0.0): game held UDP 0.0.0.0:25100/25200; networked match fine again; no firewall prompt
  (existing inbound-allow rules for starwarssquadrons.exe, TCP+UDP, Private profile at least; game install rules).
- CORRECTION: the "9-character connect password" was my hook's bug: the password is a plain char* (client+0x458's value);
  I dereferenced it as a pointer and logged my own "<bad ptr>" placeholder (9 chars). Fixed; next run logs the real
  comparison with ServerSettings.ServerPassword (expected: both empty).
- Portability: launcher finds the game via launch.ini, else Steam's libraryfolders.vdf (verified on this PC), warns if
  steam_appid.txt is missing; the DLL logs to E:\sws_community\probe_logs if present, else <dll dir>\logs.
- Package for the first two-player test: E:\sws_community\release\sws_community_test_v0 (launcher, DLL, join-mode
  server.ini with join=HOST_ADDRESS_HERE, rules.ini = host's, README.txt with risks/setup/steps/log return).

### Joiner, error 327, rulesets (2026-10-20 afternoon)
- join_local_server (default 1): 0 = in join mode a match spawn is skipped. Safe-looking: FUN_140a31ff0 (client network
  teardown) sets client+0x100 = 0 and destroys the local server, so "no local server" is a normal client state; the
  connect port is then 0, so the join rewrite now matches any "127.0.0.1:" address. Untested until a real join.
- Error 327 = UIErrorSettings ErrorCodes {OnlineError 0x8ec30718 → 327}; 0x8ec30718 = djb2("OnlineErrorReason_Generic").
  Raised by FUN_1412d43d0, which maps a backend operation's result (+0x34: 0..6 specific, else Generic) and queues it.
  Logging hook on FUN_1412d43d0 (15 bytes; the 16th byte starts a call to the stack probe) records the result fields and
  callers next time a networked match starts. Loadouts likely share the cause (inventory/unlocks via EA's backend).
- Rulesets: transports are set up by FUN_140d7bb70(transport, socket, title → +0x748, protocol → +0x74c); the server's
  FUN_140d798a0 compares the client's values (WrongProtocolVersion 0x0b0d2286, then title). Hook: protocol ^=
  0x53570000 | id<<8 | id, id = bit0 pinball rule + bit1 multidrift rule; id 0 (classic) = stock. Stand-in: id 2,
  00a0b0c0 → 53f7b2c2. Package README says rules.ini must match the host's.
- RUN 16:05 (solo host, ruleset hook live): stock NetworkSettings values seen at transport setup: title 5, protocol
  0x00098b74; ruleset id 2 → 0x535e8976 on every transport (frontend and match, server and client). Host client connected
  to the networked server (2 → 3, Ok); 3-minute match, ~2.1 MB server→client. The FUN_1412d43d0 error hook did NOT fire;
  asking Pseudo whether 327 appeared this time.
- Error 327 did appear in the 16:05 run (Pseudo) and loadouts were locked, but the FUN_1412d43d0 hook never fired → 327
  is raised elsewhere. New hook on FUN_1412df8a0 (20-byte prologue): the error-event handler that branches on
  event+4 == OnlineErrorReason_Generic; logs each event (32 bytes) + callers. Other Generic sites: 0x14133a8xx
  translation tables (leaf code, no pdata).
- Unlock-all lead: WSOfflineInventorySettings (SystemSettings, 48 B; type record 0x144979ef0, class object 0x144ae3fa0,
  short name "WSOfflineInventory"): +0x20 Enabled (bool), +0x28 InventoryItems (CString-Array); twin
  WSEventOfflineInventorySettings (class 0x144ae3f20, "WSEventOfflineInventory"). Settings instances are looked up by
  class pointer in the settings manager's hash map (pattern in FUN_1401c44b0: buckets at mgr+0xa8, count mgr+0xb0,
  entries {class, instance, next}; mgr = DAT_143f7d798). Only registration code references the class objects; reader
  not found yet (by-name lookup, or compiled out). Pseudo wants ALL components unlocked for all players.
- RUN 16:17 (error-event hook): FUN_1412df8a0 is the online layer's general event DISPATCHER (all online events; byte 0
  = event type, +4 = reason). ~5 s after the networked match started (16:18:13.736) a type-0x1e event with reason
  OnlineErrorReason_Generic, +8 status 1, +0xc -1. Type 0x1e → FUN_1412dd520 (string "PreferredPingSite"); its status-1
  branch is the "online backend connection lost" path: shows popup FUN_1419233f0(this+0x74, 0x14a6c1ef, reason) and
  tears down online services (FUN_141924850, FUN_1412dd4c0, online-player cleanup, FUN_1418ff260 ...). So 327 = the
  client's EA online connection declared lost in networked mode; the teardown plausibly drops the online inventory
  (→ default loadouts). Type-0x1e Generic events also occurred in the menus (16:17:22/27/28) with other statuses.
  Next: what declares it lost in networked mode; and our own inventory source for all-unlocked (WSOfflineInventory).

### Customization catalog (data side of all-unlocked, 2026-10-20)
- Per ship: kit asset (StarshipKitAsset, e.g. kit_starship_xwingt65, Identifier 662443148) → CustomizationOptions =
  StarshipCustomizationAsset (starshipcustomization_<ship>) with DefaultConfigs (9), AdditionalConfigs (unlockables),
  SPOnlyConfigs, SPDataConfigs; each entry imports an item asset with root fields Identifier (u32, data-assigned, not a
  name hash), DebugName/TelemetryName (e.g. ion_cannon_standard), Name.
- tools/customization_catalog.py → notes/customization-catalog.json: 10 ships (incl. liveservice B-wing/TIE Defender),
  1,166 items. Unlockable gameplay components: 111 weapon/aux slots, 37 engines, 36 hulls, 26 shields (210); cosmetics
  ~700 (+74 unresolved, likely shared cosmetics outside starship folders). Defaults per ship: primary laser,
  countermeasure, 2 aux, shield, hull, engine (common *_default_reb/imp), visual, decal. SP-only: 79 weapon slots.
- Inventory key format (Identifier vs DebugName vs GUID) pending the code map (subagent → notes/inventory-unlocks.md).

### Product notes (Pseudo, 2026-10-20)
- UI: players should pick servers from a list, not edit server.ini. Plan: a launcher window first (Host / Join, server
  list + favorites, ruleset shown, rule options as checkboxes); in-game menu integration maybe later.
- Server list source is a community decision: a community-maintained list file (GitHub / league site) + favorites
  (simplest), or a small registration/browser service like Kyber's (needs hosting). Public hosting needs reachability
  without a shared VPN: port forwarding or a relay (Kyber runs a relay proxy). Show each server's ruleset; grey out
  mismatches (rules are folded into the protocol version).
- rules.ini as written by us: no_zero_throttle_pinball=0 (Pseudo's standing preference).
- FIRST TWO-PLAYER ATTEMPT (16:48, friend's log E:\downloads\server_20261020_164545.status.txt): friend's client reached
  the host over Tailscale (join <host Tailscale IP>:25200; 6 packets out, 2 back) and was refused with WrongProtocolVersion:
  the friend ran the pre-ruleset DLL (no "ruleset:" lines, no transport-setup hook) while the host's ruleset id 2 changed
  the protocol to 0x535e8976. Networking path proven end to end. Friend then saw EA's "confirm your identity" prompt in
  Squadrons (EA account security; not in our logs). Fix: friend gets the current package. Logs now start with the build
  stamp (__DATE__ __TIME__); launcher stamp built once the host's launcher window is closed (exe was locked).

### Joiner stall at the match load (second two-player run, 2026-10-20 17:00, analysed 17:15-17:30)
- What happened: the friend (<joiner Tailscale IP>) connected to the host's networked match server at 17:00:03 (state 3, Ok) and
  reached the custom-match lobby; when the host launched the match the friend stuck on the loading screen before the
  briefing room. The connection stayed healthy throughout: both logs show a steady ~30 packets/s each way until the
  friend quit at ~17:04:50 (host server socket serving both clients at 30 Hz; SetPeerAddress alternating per send is the
  engine's own one-socket-many-peers pattern). No new spawnServer on the host at the launch: the waiting room -> map
  step happens inside the same RootLevel server. So: not the network, not the transport.
- Cause (static, strong but not yet measured): the friend's client ran the HOSTED flow (FUN_140a33160: client init
  network with sp/localhost set, spawn a local server, connect 127.0.0.1) with only the address rewritten. The engine
  treats such a client as the local host of an in-process server (connection mode 0 at conn+0x78, localhost at +0x31).
  The friend's own local match server (join_local_server=1) had loaded the same RootLevel + waiting room, which is
  probably why the lobby worked; it never moved on to the map, so the client waited for content only an in-process
  server provides.
- The engine's genuine remote join exists and is separate:
  - client+0x4ac game type: 0 SinglePlayer, 1 Hosted, 2 SoloPvB, 3 Joined (names from FUN_140a30330's telemetry strings).
  - Client message 0x4551edba "join server" (0x98 bytes, vtable 0x1434ae610; +0x30 address CString, or a 64-byte sockaddr
    at +0x50 with length at +0x90; +0x40 byte) -> stores the address at client+0x460, type 3, state 14.
  - Client state table at 0x140a31990 (16 entries). State 14 (0x140a31282): wait until gameSim+0xf8 (local server) is
    gone, then FUN_140a31d50(client, 0,0,0,0) -> connection mode 1, then FUN_140cb0500(conn, client+0x460, client+0x458).
  - Mode 1 makes FUN_140cb0080 take the socket manager from a creator (client+0xb8) and apply the remote-client network
    settings; FUN_140a31d50 then tells the game simulation's modules "remote" (FUN_1401ab610 -> each module's vfunc +0xb8).
  - Joined clients use a connection subclass (vtable 0x1436cbd40, built by factory FUN_141bb1750 when client+0xc0 holds
    a factory) whose slots 21/22 report a server level held by an online-side holder (conn+0xd0 -> +0x20 -> {+8 flag,
    +0x10 LevelSetup}); fed by client message 0xde75b306 and the online (Blaze) category 0x9c775c5c. The base class
    (0x143509488) returns false/empty there. In a direct launch client+0xc0 is null, so we get the base class.
  - Client message handler FUN_140a338d0 (ids: 0x107569eb start local game, 0xbf585a6d host level, 0x4551edba join,
    0x0036a95b server level info). Server-side level messages FUN_140ba0340 (0xab12c7a3 load level, 0xf8429a0e level
    change -> pushes FUN_140a2f990 to the in-process client).
- Build 17:28 (stand-in ALL PASS, 22 hooks):
  - join_remote=1: in join mode a match's client init becomes sp=localhost=coop=hosted=0 (mode 1); hook FUN_140cb0080
    (15 bytes) builds the socket manager the mode-0 way first (in-memory -> our UDP) so mode 1 needs no creator.
  - Diagnostics: client state requests FUN_140a30230 (17 bytes), client messages FUN_140a338d0 (15), server level
    messages FUN_140ba0340 (16), bundle queue FUN_14021b2b0 (20; bundle_log=1, names, cap 3000).
  - level-step2 hook now passes straight through unless dedicated.
  - all_unlocked + suppress_backend_lost now built (stand-in: 1 item in, 1254 out).
- Next test: host as before (now all_unlocked=1); friend with join_remote=1 + join_local_server=0 (try 1), falling back to
  join_remote=0 + join_local_server=0 (try 2). Both logs will show whether the joiner queues the map's bundles.

### Build 2 two-player tries (2026-10-20 17:36-17:41)
- Both tries ran build 17:28 with join_local_server=0. Host side: 327 event suppressed (17:39:12, "online: suppressed"),
  inventory 320 owned -> 1356 with all_unlocked.
- Try 1, join_remote=1: client init became sp/localhost 0; FUN_140cb0080 built our UDP manager then ran mode 1;
  connected (state 3, Ok, 17:37:33.250). The client then RECEIVED the host's level and went state 6 -> 7: state 6
  (0x140a3104b) waits on a queue of server-sent level entries at client+0x110 (+0x120 count; entry +0x10 LevelSetup,
  +0x18 name, +0x4a flag), copies the setup to client+0x408 and moves to 7; state 7 calls FUN_140a32d50, which creates
  the client level via client vtable +0x40 (FUN_1401bca20, allocator 0x143f633a0) and tail-calls FUN_140cca8f0(level,
  client+0xf8 load info, client+0x408). Crash 17:37:33.608 inside its LevelSetup copy, destination 0x100 = null level
  + 0x100: the create returned null (its only null path is the allocation). Reading: that allocator/arena exists only
  alongside a local server; untested.
- Try 2, join_remote=0: connected (17:39:22.475), then the client tore its own connection down within 0.05 s (socket
  destroyed, message 6f8b48d5 from the state-6 disconnect check) and returned to the frontend. Host saw ~0.33 s of
  traffic. The earlier lobby run (join_local_server=1) did not do this.
- Common factor of both failures: no local server. Next try (ini only, no rebuild): join_remote=1 + join_local_server=1.

### Joined game type and match modes (2026-10-20 ~18:00-18:15, build 3)
- Correction to the "background server" retry idea: FUN_140cca8f0 ends with: if loadInfo+0x695 (the game simulation has
  a local server) -> FUN_140cd6580 (share the in-process server's loaded level) else FUN_140cd6150 (load it yourself).
  So a joiner with a local server waits on that server for the map: the first session's stall, from the code.
- Why the remote try crashed: client state 6 (0x140a3104b) sets r12 = "keep" for every game type except 3 (Joined)
  unless the level and GameMode are unchanged; client+0x4b2 = r12; FUN_140a32a10 calls FUN_140b29e80 only when
  +0x4b2 == 0. FUN_140b29e80 creates the "Level" memory arena (FUN_1401def80, DAT_143f633a0, 512 MB) and mounts the
  level's superbundles. Our joiner was type 2 (SoloPvB), so no arena, so the level create (FUN_1401bca20) returned null.
- Build 3: join_remote also sets client+0x4ac = 3 (Joined) at the match's client init.
- Modes: GameMode is the only WorldPartInclusion criterion (Frontline, Frontline_Bots, Missions, Dogfight, Practice,
  Tutorial). All 13 code reads of SquadronComposition compare it with "Solo" only. EA custom matches asked Blaze for
  Dogfight / Frontline / Frontline_Bots (FUN_1417503e0, gameModeMatchRule), with no Solo flag. Solo vs AI = Frontline_Bots
  + Solo: the server gives the one human 4 AI wingmen (the joiner appeared as a 6th on the team).
- Build 3 host setting match_mode = solo | coop | fleet | dogfight: rewrites GameMode/SquadronComposition (via
  FUN_141191750; reads via FUN_141191d50) on client start messages (0x107569eb, 0xbf585a6d) and server level messages
  (0xf8429a0e, 0xab12c7a3), host only, non-Frontend levels. Unverified in game. Open: team assignment and AI fill for
  PvP without Blaze (MaximumBotCountImperial/Rebel, TeamSizesSufficient exist).
- RUN 18:49 (host, match_mode=coop, first version): removing Solo from the start message left the client "Hosted"
  (type 1: sp 0, localhost 0, hosted 1 = EA's peer-hosted path); the spawn was not local host, the backend chooser found
  no constructible backend, crash at 0x141b8348a under FUN_1412e5d40. The custom-match module (vfunc +0x160 in
  FUN_140a33160's loop; reads GameMode + SquadronComposition==Solo) is what turns a start into the local SoloPvB flow.
  Fix: host client init with match_mode set and type 1 -> type 2, client+0x4b3 = 1, localhost 1, hosted 0.
  Also: safe_str has one buffer per thread, so multi-string log lines printed the last value; safe_copy added.
- RUN 18:52 (host, match_mode=coop, fixed): options Frontline_Bots/Solo -> Frontline_Bots/None on the server level
  change and the client start; client kept local; spawn, connect, ~2.5 min, back to frontend, no crash, 327 suppressed.
  PSEUDO: the lobby had four "add friend" slots (new; correct for co-op), no AI teammates, played as normal vs 5 AI
  opponents. CO-OP MODE CONFIRMED on a self-hosted match.
- RUN 19:00 (host, match_mode=fleet): options Frontline_Bots/Solo -> Frontline/None; Yavin loaded its Frontline parts
  (bundles PVP_Yavin/PvP, CineIntro_Frontline, Music_Frontline, via the waiting room, CustomMatch_Lobby_Transition,
  Lobby/PVP_Battlemap_M00, PvP_Default_Cine_Intro). PSEUDO: identical to an EA custom Fleet Battles match played alone:
  faction choice in the lobby, match started with only them (no teammates or opponents); they were Empire, and quitting
  mid-game gave the Rebel victory outro (19:03:47). FLEET BATTLES PVP CONFIRMED on a self-hosted match. Team choice is
  the lobby's own faction selection (no Blaze needed for that, at least for the host).

### FIRST TWO-PLAYER FLEET BATTLES (2026-10-20 19:13-19:21, build 3) and the rubberbanding
- Pseudo hosted (match_mode=fleet), the teammate joined with build 3 (join_remote=1, join_local_server=0): client type 3
  Joined, connected 19:13:11, states 9-12 as Joined, played to 19:21:27. PSEUDO: "It works! The fleet mode loaded with
  all its features." The joiner saw rubberbanding; the host played smoothly.
- Measured (host + joiner logs, 19:13:44-19:21:14, 450 s): server -> joiner ~10548 packets (23.4/s, 450 B average; the
  engine rate-limits a remote client below the host client's 30/s), joiner received ~10261 => ~2.7% loss; joiner ->
  server 30/s, no loss.
- Suspects fixed in build 4: (1) packet size: the in-memory socket's GetPacketInfo (slot 7) reports max 1600; Kyber
  reports 1264 (+28 overhead). Over Tailscale (inner MTU 1280) packets over ~1252 B are IP-fragmented, and losing any
  fragment loses the packet: big server->client state packets are the lossy direction. Build 4 patches slot 7:
  {1, udp_max_packet (default 1200), same, 28, 1}. (2) receive stamp: the engine stores it per packet (FUN_140d73a60,
  record +0x10); Kyber stamps with NetTick. Squadrons' DirtySock NetTick = FUN_140696cb0 (QPC*1000/freq); build 4 uses
  it instead of timeGetTime (both count from boot, so probably minor). Summaries now log the largest packet sent and the
  count over 1252 B.
- RUN 19:37-19:43 (build 4, both sides): Pseudo: "much better"; the joiner felt "a little bit of lag, but that may be no
  worse than people get on the EA servers". Measured: the server's largest packet is now exactly 1200 B (0 over 1252), so
  the engine was filling packets to the advertised maximum before and big ones were fragmenting. Server -> joiner 22.7/s,
  378 B avg; loss estimate ~4% (+/-1.5: 5-second summaries on two clocks), not lower than build 3's 2.7%. So the remaining
  loss is the path itself; next check is whether Tailscale is direct or relayed (tailscale ping).
