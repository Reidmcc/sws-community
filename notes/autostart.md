# Auto-start: research record (2026-11-06)

Static research for `[server] autostart` (sws_probe.cpp, "auto-start"), done by a research agent; the start handler's
calls and their order were checked in the disassembly. What was built follows its recommended implementation, with
these differences:

- "The main menu is up" is client state 12 after the Frontend level's start message (107569eb); the windows (1.5 s after
  EA's loadouts, the 30 s fallback, the 60 s give-up) count from it. EA's loadouts are the presets set whose caller
  returns to 0x1418fddfd (EA: 0x1419015ce, mapped by hand in tools/gen_build_ea.py).
- No `autostart=always`: once per launch, and not at all if the player starts a level from the main menu first.
- Done when it got there: a host when the waiting room's bf585a6d arrives. A joiner's own in-process server sends that
  message too, before the join hooks redirect the connection (probe_logs/server_20261020_193343: bf585a6d at 46.469, the
  connect rewritten at 46.872, the connection dropped at 53.886, the Frontend again at 53.903), so a joiner is done when
  its client reaches state 12 as a joined client (game type 3; inferred, no successful joiner log yet), or 45 s after
  the start with no return to the main menu. A joiner back at the main menu tries again 10 s after it's up again, three
  starts in all; a host doesn't retry.
- Every read is checked, and both functions' first bytes are compared before calling them.
- Not handled: the player busy in the menus when it fires (the custom match screen's edits aren't copied, since
  FUN_14174b700 is skipped); the docs say to leave the menus alone. A player's own start in the ~1 s before its lobby
  message could meet a second start from the mod; closing that needs a hook on FUN_1417544b0.

Found by the second reader (review on the PR): the joiner case above, the timing from state 12, the EA-reply caller.

Not flown yet.

---

Steam build addresses. [V] = verified in code or game data (address / asset given); [I] = inferred.
The research agent's helper scripts and dumps aren't in the repo.

## Corrections to what we "already knew"
- `FUN_14174d7e0` does **not** post a load-level message. It sends a client->server **network message**
  (category Network `0xc67d277b`, id `0x3aff49d1`, vtable `0x1434d1658`) through the client connection
  `[[DAT_14419b5a8]+0x60]+0x48`, sink at `conn+0x7a8`, vtable slot 0 [V 0x14174d82b..0x14174dbb6]. The in-process
  Frontend server handles it in `FUN_140ba0d30` -> `FUN_140ba0d60` [V], which calls `FUN_140ba0c20(msg+0x58)` = the
  server "level change" `0xf8429a0e` the mod already sees in h_smsg [V], then the client gets "host level" `0xbf585a6d`.
- The map in that LevelSetup (+0x30 = `PVP_Yavin`) is a **hard-coded placeholder** [V: only code ref to the Yavin
  string is in this function, 0x14174d970/0x14174db12]. The real map is chosen later, in the waiting room, from
  `BasicSettings.MapIndex` and streamed as a detached sub-level [V data, see Q2]. Log proof: run
  `probe_logs\server_20261023_131452` has LevelSetup map `PVP_Yavin` at 13:15:42 but queues
  `Game/Levels/PvP/PvP_Esseles/PVP_Esseles` at 13:16:21 after the waiting room [V]. Every logged custom match
  shows `PVP_Yavin` at +0x30 (284 + 143 lines across probe_logs) although Pseudo flew Esseles, Sissubo, Fostar Haven.
  => **For the main session:** `maps_selection.md`'s plan ("swap where LevelSetup+0x30 names the replaced map") will not
  see the picked map in custom matches; +0x30 is always Yavin there. (It does carry the real map in the main menu's
  Fleet-Battles-vs-AI LevelControlEntity route, see Q2 note.)
- LevelSetup field names (from `LevelControlEntityData` fields matched against both producers) [I, strong]:
  +0x08 LevelToLoad, +0x18 StartPoint, +0x20 LevelManagerInitialLevel (initial detached sub-level: the waiting room,
  or Lobby_MP), +0x30 MultiplayerGameplayLevel.

## Q1. FUN_14174d7e0: signature, inputs, how to call it
- **Signature: effectively `void FUN_14174d7e0(void)`** [V]. rcx is only spilled to its home slot; the arena lookup
  keyed by that slot's stack address (`DAT_143f7d7a0` map) falls back to the default arena `0x143f5e2e0` when no entry
  exists, and the spilled value is then unused (0x14174d8d0..0x14174d92d). No `this`.
- **Inputs:** only `DAT_14419b5a8` (ClientGameContext, created in FUN_1401aa070) -> `+0x60` -> `+0x48` (the client
  connection; returns silently if either is null) [V 0x14174d80a..0x14174d825]. Everything else is constant:
  root `Game/Levels/RootLevel/RootLevel`, +0x20 `CustomMatchWaitingRoom`, +0x30 `PVP_Yavin`, options
  `GameMode=Frontline_Bots; SquadronComposition=Solo` (FUN_141191750) [V].
- **Message layout** (on the stack, sent synchronously): +0 vtable 0x1434d1658, +8 0xc67d277b, +0xc 0x3aff49d1,
  +0x10 0xff, +0x2c 0, +0x30/+0x38 0 (server reads +0x30 as the sending connection: filled on receipt [I]),
  +0x40 16 bytes from 0x1437cf150, +0x50 0, **+0x58 LevelSetup** (0x40 bytes) [V]. Send = `(*(conn+0x7a8))[0](conn+0x7a8, &msg)`;
  cleanup FUN_140a26b30 on the LevelSetups [V].
- **Server acceptance** (FUN_140ba0d60) [V]: requires `*( *(senderConn+0x7b8) + 0x4690) == 0` and
  `senderConn+0x5f98 != 0` (meaning [I]: the local host's own connection); then posts 0x387e3581 and the level change.
- **Who calls it in the game** [V]:
  - `FUN_1417544b0` = ClientCustomMatchSetupEntity's "create" event (event 0x1d8418cf in FUN_1417521d0, vtable data
    0x143621588): `FUN_14174b700(mgr, entity+0x1b8)` (copy the UI's edited settings into the manager),
    `FUN_14174c240(mgr)` (serialize settings, send them as the "CustomMatchSettings" blob over the same client
    connection, conn+0x800), then if `mgr+0x268 == 0`: GameMode 3 -> `FUN_14174d7e0()`; else ask the online store
    for the multiplayer privilege (`[DAT_1441c5f28]`, vfunc +0x18 of its +0x88 member) and set `mgr+0x268 = 1`.
  - `FUN_14174d540(mgr)` (the router; reached from the manager's listener `FUN_14174b1f0` on message 0x70611e4c
    = privilege-check result, when `mgr+0x268` is set): returns if `mgr+0x26c != 0`; GameMode 3 -> FUN_14174d7e0;
    0/1/2 -> Blaze job.
- **The custom-match manager** (the object FUN_14174d540/FUN_1417544b0 use) [V unless marked]:
  - ctor FUN_14174af30 (vtable `0x1436217f0` at +0; listener vtable 0x1436217f8 at +0x1b0), reset FUN_14174d200
    which allocates fresh `CustomMatchSettings` at **mgr+8** (Basic at settings+0x18, Restrictions +0x20,
    Modifications +0x28), and sets +0x268/+0x26c to 0.
  - **Found as `mgr = *(uint64_t*)0x1441c5f00 + 0x3df8`**: ServerCustomMatchDataEntity's ctor (0x1415e8430) stores
    `[0x1441c5f00]+0x3df8` and binds `[0x1441c5f00]+0x3e00` as a `CustomMatchSettings` (type 0x144a05890) [V];
    FUN_1415ea1d0(`[0x1441c5f00]+0x3df8`, i) reads `+8 -> +0x20 -> ban bytes` [V]; 0x141731956 reads
    `[0x1441c5f00]+0x3e00 -> +0x20` [V]. That `+8 = settings` layout is the manager's. That it is *the same* object the
    UI uses (rather than a twin) is [I, strong]; the mod can check it at runtime: `rd(mgr,0) == 0x1436217f0`.
  - `[0x1441c5f00]+0x4189` (= mgr+0x391) is the "custom match active" flag read by ship-ban / loadout code [V reads];
    writers at 0x141264f9e/0x141264fa6/0x1412b8a49 not traced.
  - Default BasicSettings (FUN_14173c680) [V]: GameMode 0 (Dogfight), DogfightPoints 30, Kickoff 120, MatchTime 1800,
    **MapIndex 0 (= random, see Q2)**, PVBTeamId 1, BotDifficulty 1, IsPublic 1, AllowJoinInProgress 1.
    Basic object: +8 = 0x144a38f00 (type pointer; usable as a sanity check).
- **Can the mod call it directly? Yes.** Recommended form (mirrors FUN_1417544b0's solo branch without the UI copy):
  1. `mgr = rd(G(0x1441c5f00),0) + 0x3df8`; require `rd(mgr,0) == G(0x1436217f0)`, `settings = rd(mgr,8)`,
     `basic = rd(settings,0x18)`, `rd(basic,8) == G(0x144a38f00)`, `rd<u8>(mgr,0x268) == 0`, `rd<int>(mgr,0x26c) == 0`,
     and `rd(rd(G(0x14419b5a8),0)+0x60)` and its `+0x48` non-null.
  2. Write `basic+0x18 = 3` (PVBSolo) and `basic+0x28 = MapIndex` (Q2).
  3. Call `FUN_14174c240(mgr)` (as the UI does; sends the settings blob over the local connection) [I: possibly not
     required for a local server, which reads the same object, but it is the game's own sequence].
  4. Call `FUN_14174d7e0()`. Do **not** route through FUN_14174d540 with an unchecked GameMode: modes 0-2 create a
     Blaze game.
- **Building the message ourselves:** possible (layout above) but no gain: the only thing we would change, the map,
  is ignored by the custom-match flow. Not recommended.
- **Thread:** the game calls this from entity event handling inside the client update, which runs in the
  GameSimulation tick (FUN_1401bfe60 -> client update) [V from notes 3b; I for the UI entity]. The mod's
  `h_gstick` (after `o_gstick` returns, before its `if (!g_dedicated) return;`) is the safe place; the dedicated 3b
  code already calls FUN_140ba0c20 there.

## Q2. MapIndex -> level path; naming the map
- Chain [V, game data]:
  waiting room `liveservice/levels/pvp/customgames/custommatchwaitingroom` (default_schematic): server-realm
  `CustomMatchDataEntity` (#26) -> DataSourceQuery `BasicSettings` -> DataSourceQuery `MapIndex` ->
  LogicPrefab `game/gameplay/profiles/battlemapselection` `.MapIndex`; its `.MultiplayerGameplayLevel` ->
  SchematicChannel `sc_pvpmapselect` field -1170950784. Root level `game/levels/rootlevel/default_schematic`:
  `lpf_pvpmapselect_sc` field -1170950784 -> `IncomNextMultiplayerLevelEntity.DSubNameOverride`; the waiting room's
  `lpf_ui_custommatchoptions` fires the channel event that drives IncomNextMultiplayerLevelEntity, whose `DSubName`
  goes back out through `lpf_pvpmapselect_sc_writer`. Maps are detached sub-levels of RootLevel
  (`rootlevel_dsublevelmanager_genprefab`).
- `battlemapselection` [V data]: MathEntity `select(MapIndex == 0, RandomInt(0..5), MapIndex - 1)` (instructions
  InputI/ConstI 0/EqI/InputI random/ConstI 1/SubI/Func; the typed-register reading is [I]) into SelectString inputs
  in this order. **MapIndex table:**

  | MapIndex | name | DSub level |
  |---|---|---|
  | 0 | random of the first six (never Abyss) | - |
  | 1 | Esseles | Game/Levels/PvP/PvP_Esseles/PVP_Esseles |
  | 2 | Fostar Haven | LiveService/Levels/PvP/PVP_FostarHaven/PVP_FostarHaven |
  | 3 | Destroyed Moon (Galitan) | Game/Levels/PvP/PVP_DestroyedMoon/PVP_DestroyedMoon |
  | 4 | Nadiri Dockyards | Game/Levels/PvP/PvP_NadiriDockyards/PVP_NadiriDockyards |
  | 5 | Sissubo | Game/Levels/PvP/PVP_Sissubo/PVP_Sissubo |
  | 6 | Yavin | Game/Levels/PvP/PVP_Yavin/PVP_Yavin |
  | 7 | (Zavian) Abyss | Game/Levels/PvP/PVP_Abyss/PVP_Abyss |

  Setting proposal: `[server] autostart_map = random|esseles|fostar|moon|nadiri|sissubo|yavin|abyss` -> index.
  Unset MapIndex stays 0 = random.
- The CustomMatchDataEntity binds the settings object by pointer [V 0x1415e857c..0x1415e85a0], so the waiting room
  reads live values [I]; writing MapIndex before the start is enough. The host can still change it in the waiting
  room's options screen.
- Note [V data]: the main menu (`game/ui/lpf_ui_mainmenu`) also uses battlemapselection for its own Fleet Battles vs AI
  launch: LevelControlEntity LevelToLoad RootLevel, `GameMode=Frontline_Bots;SquadronComposition=Solo`,
  LevelManagerInitialLevel `Game/Levels/Lobby/Lobby_MP` (straight to the briefing room, no waiting room), map from a
  profile option via ClientIntToServer. There LevelSetup+0x30 is the real map.

## Q3. "Main menu reached, online ready": the signal
- Already observable in the mod, consistent in 30 logged launches (probe_logs 10-28 .. 10-31) [V logs]:
  1. client state 11 -> 12 with level `Game/Levels/Menu/Frontend/Frontend` (h_cstate; client+0x380 == 12 and
     `client+0x408+8` contains "Frontend") - the Frontend is loaded;
  2. **~10.5-12.5 s later: `loadouts: set by 1418fddfd, state 1, status 0` (h_presetset, caller 0x1418fddfd = EA's
     inventory reply)** - EA's loadouts have arrived (and local_loadouts laid over them). `inventory: set` (h_invset)
     fires 3-4 times, the first already at the Frontend spawn (+0 s), so it is not a usable "ready" signal.
- **Recommended trigger:** a flag set at the first EA loadouts set (caller 0x1418fddfd, state 1, status 0) while the
  client is at Frontend state 12; fire on the next h_gstick tick (h_presetset's thread is not established [I]).
  Fallback: 30 s after Frontend state 12 with no EA reply (offline / error) -> fire anyway and log it.
- What breaks if it fires too early:
  - before the frontend's client connection exists: FUN_14174d7e0 silently does nothing [V] (pre-check it, or the
    one-shot is wasted);
  - before EA's loadouts arrive: the EA backend connection is lost ~6 s into a match (`backend-lost` suppressed at
    20:47:56 after the 20:47:50 start [V logs]) and no "from EA" set was ever logged after a return [V logs], so the
    reply would likely never arrive: in-match loadouts/presets would be the store's defaults and local_loadouts'
    overlay would not run [I];
  - unlocks: unlock_presets needs the rank asset `[[0x1441c5f00]+0xf8]` [V mod code]; normally loaded by then [I].
- The game itself gates only the non-solo modes on the online privilege check (FUN_1417544b0) [V]; the solo branch
  has no online precondition in code.

## Q4. After the match (return to Frontend)
- The return is a new `start local game` 0x107569eb with the Frontend level and client state 12 again (e.g.
  20:55:54-20:55:59) [V logs]. EA's loadouts do not arrive again (no second "from EA" in any log) [V logs], so a
  trigger keyed on the EA reply would not re-fire; a trigger keyed on "Frontend state 12" would, every time.
- Use a process-wide one-shot flag anyway (set when the start is sent; optional `autostart=always` re-arms on each
  Frontend state 12). Confirm success by watching h_smsg for `0xf8429a0e` with lobby = CustomMatchWaitingRoom within
  ~5 s; retry once if absent.
- Manager state persists across matches: MapIndex/GameMode written by the mod stay in `mgr+8`. A later manual start
  through the UI first copies the UI entity's settings over them (FUN_14174b700) [V], so no interference. Nothing in
  the existing hooks (apply_match_mode, h_cinit, h_connect, h_cset) re-fires a start [V source].

## Q5. EA servers on this route
- Fully local in the start path [V]: GameMode 3 skips the privilege check (FUN_1417544b0) and the Blaze game job
  (FUN_14174d540 allocates the 0x11b8 Blaze job only for 0-2: `incomCustomDogfight/Frontline/PvB`); FUN_14174c240 and
  FUN_14174d7e0 send only over the client connection to the in-process Frontend server (127.0.0.1:25200 in logs).
- Not local, around it [I]: EA sign-in, inventory and loadout fetch at boot (which we wait for), telemetry, the
  online data model reacting to the match (the backend-lost event the mod suppresses), and server-realm entities in
  the waiting room (`PersistenceControlEntity`, `ServerEndOfRoundMatchmakingEntity`) that may talk to the backend.

## Recommended implementation (HOST and JOIN)
1. Settings: `[server] autostart = 0|1|always`, `autostart_map = random|esseles|...|abyss` (host; joiner may omit).
2. Arm: first EA loadouts set (h_presetset caller 0x1418fddfd, state 1, status 0) while at Frontend state 12; or 30 s
   after Frontend state 12.
3. Fire in h_gstick, once: the checks and steps 1-4 of Q1 (vtable/type checks, GameMode 3, MapIndex,
   FUN_14174c240(mgr), FUN_14174d7e0()). Log every check.
4. Verify: f8429a0e with the waiting room lobby within 5 s; else log and retry once.
5. HOST: match_mode keeps rewriting the options as today. JOIN: same call; the existing join hooks redirect the
   connection as in the manual flow [I]. The joiner should fire only when the host is up (connect failure returns
   it to the Frontend; consider `autostart_join_delay` or one retry).
- Risks: manager identity is [I, strong] (vtable check guards it); FUN_14174c240's need is [I]; the MapIndex math
  semantics are [I] (table order from data is [V]); joiner timing; UI elements of the main menu may still be mid-
  animation (level switch tears them down [I]); a non-default first-run state (EULA, first-login popups, title
  screen) may delay the client's Frontend state or block the connection's +0x48 [I].

## Only a launch can settle
- That `[0x1441c5f00]+0x3df8` is the UI's manager (vtable check) and that the call from h_gstick lands in the waiting
  room (f8429a0e + bf585a6d logged, then SoloPvB client type 2).
- The MapIndex -> map table order (fire with each index once, watch the `bundle: queue` DSub).
- Whether skipping FUN_14174c240 matters; whether calling it is harmless.
- That loadouts in the auto-started match are the EA+local ones (fire after the EA set) and what happens offline.
- Re-fire behaviour on return to the Frontend; joiner flow end to end with autostart on both sides.
- Whether title screen / first-run popups delay Frontend state 12 or the EA reply on a fresh install.
