# Online guard: research record (2026-11-06)

Static research for `[server] online_guard` (sws_probe.cpp, "Online guard"), done by a research agent and spot-checked:
the five hook sites' bytes match the Steam exe, the party group's join call is at 0x141ba5795, and the matchmaking
start's reject branch is as described. What was built differs from the recommended design below in three ways:

- join by player is blocked outright (Pseudo's call: the mod's matches never use it);
- the backend chooser is left alone: the tripwire refuses the createGame a Peer-hosted game would need;
- h_connect also refuses any connect that is neither this PC nor the join address;
- the custom-match reroute turns every mode but 3 into 3, with no type check (the second reader showed a missed check
  would let a createGameTemplate out, which the tripwire only logs).

Correction from the second reader: sendRequest takes 8 arguments (the 8th a dword), not the 9 written below.

Steam addresses throughout; the EA ones are in mod/build_ea.h (three mapped by hand, see tools/gen_build_ea.py).

---

## 0. The layers, bottom-up

1. **Blaze ComponentManager::sendRequest `0x1422676a0`** [V] `(mgr, JobId* out, u16 component, u16 command, request, RpcJob*, ...)`:
   looks up the sender for `component` and calls `sender->vtbl[+8]`. If no sender: `*out = 0` and returns (no callback ever).
   All Blaze RPCs pass here (inventory 0x80c too, `FUN_1418f6090`).
2. **GameManagerComponent** (component 4; name fn `0x142269210`; command-name switch `0x1422693d0`) [V]. Command ids decoded
   (cmdids.py): 0x01 createGame, 0x09 joinGame, 0x0a createGameTemplate, 0x10 startMatchmakingScenario,
   0x11 cancelMatchmakingScenario, 0x1e joinGameByUserList, 0x2d createOrJoinGame, 0x5a createTournamentGame,
   0x64/0x65 getGameListSnapshot/Subscription (server browser lists), 0xab reportTelemetry. Thin RPC stubs [V]:
   createGame `0x14223d030`, createGameTemplate `0x14223d160`, createOrJoinGame `0x14223d290`, joinGame `0x14223d620`,
   joinGameByUserList `0x14223d750`, startMatchmakingScenario `0x14223dfb0` (each has exactly one caller).
3. **Blaze GameManagerAPI** (SDK, 0x14222xxxx) [V]: createGame `0x14222a430`/`0x14222a710`; createGameTemplate `0x14222a9d0`;
   createOrJoinGame `0x14222ae80`/`0x14222b180`; joinGame core `0x14222cb20` (all join variants funnel here: `c4a0`<-`c660`,
   `c7f0`<-`c9a0` joinGameById, `c160`<-`c310`); joinGameByUserList `0x14222d250`/`0x14222d6b0`; startMatchmakingScenario
   `0x1422307f0`. **Every one starts with the same early-out** [V at 0x14222aa18, 0x14222cb6f, 0x142230838, 0x14222a468]:
   `if (!api->[+0x1e8]->vtbl[+0x28]())` (fast path: byte `adapter+0x299`) → schedule the caller's callback with BlazeError
   **0x40040000** via `FUN_142229f30(api, JobId* out, -, cb, err)` (createGame uses `FUN_142229e70`; matchmaking inlines it via
   `0x14223de20`), then a 2nd check (`[hub+0x768]->vtbl[+0xe8]`) → 0x80190000. So the SDK already has a "fail cleanly,
   callback fires later with an error, nothing sent" path.
4. **Frostbite Blaze presence backend** (0x141b9xxxx-0x141bbxxxx), the callers of the API [V]:
   - `FUN_141ba5670` = handler of **PresenceGameRequestMessage** (id 0xc5f1645d [V hash]); listener ctor `FUN_141ba5360`
     registers it (`FUN_141b81560(mm, 0xc5f1645d, ...)`). Switch on msg+0x40 (request type), msg+0x38 = request id:
     0 `FUN_141b9fc40`, 1 createGameTemplate `FUN_141b9ff70`(msg+0x7f0; template name at msg+0x7f8), 2 `FUN_141ba0620`,
     3 `FUN_141ba5ac0`, 4/6/8 join by id `FUN_141ba15f0`, 5/7 join by player `FUN_141ba1a30`, 9 leave `FUN_141ba2010`, 0xa-0xd misc.
   - The message is built only by ctor `FUN_1412cf570(msg, type)` (0x11b8 bytes; +0x8 category 0x9c775c5c, +0xc id) and
     0xc5f1645d appears at only 3 code sites (ctor, listener ctor, handler) [V imm.py]. Ctor call sites and types [V]:
     `0x14174d6e5` type 1 (custom-match create), `0x1412d4918` type 1 (party group "incomCrossplayGroup"/"incomNoCrossplayGroup",
     `FUN_1412d4800` <- `FUN_1412d6120`, online-client slot 0x14358ca28), `0x1412cf4de` type 4 (`FUN_1412cf4a0` <-
     `FUN_1412d4a40` <- `FUN_1412d62b0`, slot 0x14358ca38: leaves the current group then joins one = party/group join [I]),
     `0x1418f0602`/`0x1418f129f` type 9 (leave). **So types 0, 2, 3, 5-8 are never sent as messages** [V by census].
   - Direct (non-message) game joins [V]: `FUN_141ba15f0` <- `0x141750381` (custom-match browser join `FUN_1417502b0`),
     `0x1412df17a` (IOnlineClient::JoinGameById `FUN_1412df0f0`, slot 0x14358c1f8), `0x14194bb2f`/`0x14194bc7b` (group member
     follows the group into a game, `FUN_14194b940`/`FUN_14194bb60`); `FUN_141ba1a30` <- `0x1412df332` (IOnlineClient::
     JoinGameByPlayer `FUN_1412df1c0`, slot 0x14358c238), `0x141ba5d78` (`FUN_141ba5ca0`, type-3 completion).
   - Matchmaking [V]: `FUN_141baad30(mmObj, params)` is the one door to startMatchmakingScenario (`FUN_141baae40` -> API
     `0x1422307f0`). Callers: `FUN_1419217c0` (<- IOnlineClient::StartMatchmaking `FUN_1412dee30` slot 0x14358c1d0, and group
     MM `FUN_141922560`), `FUN_141921e40` (<- `FUN_141921c70`), and the jmp at `0x141babbca`.
   - createGame / createOrJoinGame: `FUN_141b9e1c0` (vtable slot 0x1436cabd0) and `FUN_141bb0960` (slots 0x143689390,
     0x1436cbcb0; builds a create with the local user name or "__MISSING_USER__"): no client UI route found to them;
     `FUN_141bb0960`'s class also posts ClientJoinMultiplayerMessage (`FUN_141bb13e0`) → likely the peer/hosted-game
     creation used with the Peer server backend [I].
5. **Incom online client** (big object; vtables 0x14358c0xx-0x14358cbxx) turns presence result messages into UI events:
   `FUN_1412d2180` (presence message dispatcher, slot 0x14358c9d8), `FUN_1412d43d0` (matchmaker result → event type 0x14/0x15/
   0x16 with reason), then the error-event consumer `FUN_1412df8a0` (already hooked by the mod) shows popups.
6. **After Blaze succeeds**: the presence layer posts **ClientJoinMultiplayerMessage 0x4551edba** ("join server" in
   client_msg_name) with the EA server address from the Blaze game object (`FUN_14194b670`, `FUN_141bb13e0`) [V]; the client
   (`FUN_140a338d0`) then connects with `FUN_140cb0500`. Other 0x4551edba posters: `0x1401bd1e0`, `0x140a30330` [V, purpose not read].

Message-id names resolved with hashname.py [V]: 0xc5f1645d PresenceGameRequestMessage, 0x11cd888c PresenceGameMessage (results),
0x81635301 PresenceMatchmakerMessage, 0xc07e960d PresencePrivateMatchRequestMessage, 0x70611e4c
CustomMatchMultiplayerPrivilegeCheckedMessage, 0x06596000 CustomMatchGameCreatedMessage, 0x9aa72c48 ClientConnectedMessage,
0x4551edba ClientJoinMultiplayerMessage, 0x107569eb ClientStartSingleplayerMessage, 0xbf585a6d ClientStartMultiplayerMessage.
Error reasons: 0x1e4595bc OnlineErrorReasonMatchmaking_Offline, 0xa3b0ef71 ..._NonLeaderError, 0x870ee29d ..._Failure,
0xe154c306 ..._Timeout, 0xb0da8ad0 ..._Qos, 0xb44bd622 ..._AlreadyInSession, 0x1d0b39cf ..._QueryError, 0xa99b61a3 OnlineErrorReason_None.

## Q1. Every UI path to an EA-hosted game, decision point and earliest interception

| path | decision made in | earliest safe intercept (before any Blaze request) |
|---|---|---|
| Custom match create, mode 0/1/2 | `FUN_14174d540` router [V]: reads BasicSettings.GameMode (`[[self+8]+0x18]+0x18`); 0/1/2 → PresenceGameRequestMessage type 1, template incomCustomDogfight/Frontline/PvB, PERMISSION_SYSTEM customGamePrivate/Public, ADMIN_ONLY_INVITES; stores request id at self+0x26c; posts via `FUN_140234bd0([DAT_14419b5a8]+0x10, msg, 0.0)`. 3 → `FUN_14174d7e0` local. Called only from `FUN_14174b1f0` (custom-match listener) on CustomMatchMultiplayerPrivilegeCheckedMessage when the create-pending flag (self+0x268) is set [V] | hook `FUN_14174d540` (reroute, Q2). Backstop: `FUN_141b9ff70` (template name at param_3+8 starts with "incomCustom") |
| Server browser join (custom matches) | `FUN_14174b1f0` → `FUN_1417502b0(self, gameId=self+0x278, spectator=self+0x280)` after the privilege check → `FUN_141ba15f0` [V] | `FUN_141ba15f0` (or `FUN_1417502b0`) |
| Join by game id (invite to a game, join in progress) | IOnlineClient::JoinGameById `FUN_1412df0f0` → `FUN_141ba15f0` [V]; who in the UI calls the slot is not traced [I] | `FUN_141ba15f0` |
| Join friend / by player | IOnlineClient::JoinGameByPlayer `FUN_1412df1c0` → `FUN_141ba1a30` [V] | `FUN_141ba1a30` (see risk: may also be used for party) |
| Party member follows the party into a game | `FUN_14194b940` / `FUN_14194bb60` → `FUN_141ba15f0` [V] | `FUN_141ba15f0` |
| Matchmaking / quick play (Fleet Battles, Dogfight, PvB), solo and party leader | IOnlineClient::StartMatchmaking `FUN_1412dee30` → `FUN_1419217c0`; party `FUN_141922560` → `FUN_1419217c0`; `FUN_141921e40`; all → `FUN_141baad30` [V] | `FUN_141baad30` |
| End-of-round re-matchmaking | ServerEndOfRoundMatchmakingEntity / EorMatchmaking* strings exist; route not traced [I] → would also reach `FUN_141baad30` or the API | `FUN_141baad30` + tripwire |
| createGame / createOrJoinGame (peer-hosted game) | `FUN_141b9e1c0`, `FUN_141bb0960` [V callers of the API]; trigger not found from the client UI [I] | tripwire (sendRequest) and keep the Peer backend unreachable (chooser `0x1418f2930`) |
| Party group create/join (no game server) | `FUN_1412d4800` (boot, template incom[No]CrossplayGroup) / type-4 message from `FUN_1412d4a40` [V] | **must stay allowed** |

Invites: game invites end in JoinGameById/ByPlayer; party invites end in the type-4 group join [I].

## Q2. Custom-match create → Solo route

- [V] Mode 3 does exactly: `FUN_14174d7e0()` (its argument is unused: only spilled to the home slot), which builds LevelSetup
  RootLevel / CustomMatchWaitingRoom / **PVP_Yavin (hard-coded; MapIndex not read)**, GameMode=Frontline_Bots,
  SquadronComposition=Solo, and sends it through `([DAT_14419b5a8+0x60]+0x48)+0x7a8 → vtbl[0]`. It does not touch self+0x26c
  or self+0x268, same as the native Solo pick. Precondition: `[DAT_14419b5a8+0x60]` and its `+0x48` non-null (else it does nothing).
- Two ways, both safe at the router level:
  - **A (minimal):** in the hook, if `[self+0x26c]==0` and mode∈{0,1,2}: call `FUN_14174d7e0(0)` and return. Settings keep the
    user's mode.
  - **B (recommended):** write 3 to BasicSettings+0x18 and call the original. The whole session then runs the tested Solo flow.
- What reads the mode later: `FUN_14174b1f0` sends the whole "CustomMatchSettings" to the server on ClientConnectedMessage
  (`FUN_140da4f50(..."CustomMatchSettings"...)`) [V]; the server-side ServerCustomMatchSetupEntity/TeamAssignmentEntity then reads
  GameMode, MapIndex, PVBTeamId etc. [I]. With A the local server gets GameMode Dogfight/Frontline/PvBCoop together with a
  Solo LevelSetup: an untested combination (team assignment may wait for human teams). With B the server sees PvB Solo, which
  is exactly the configuration the mod already runs; match_mode then rewrites GameMode/SquadronComposition on the LevelSetups
  (apply_match_mode). B's visible side effect: the lobby/settings UI shows PvB Solo afterwards [I].
- Other readers not enumerated (the settings object is reached through generic data-container pointers) [I].

## Q3. Clean failures the game already handles

- **Matchmaking:** `FUN_141baad30`'s own failure branch [V]: `msg = FUN_141baac60(reason, 0)` (PresenceMatchmakerMessage
  0x81635301, +0x30 kind 0, +0x34 reason) and `FUN_140234bd0([DAT_1441c2910]+8, msg, 0.0)`. Reasons it uses: 3 (backend state
  not 6), 4, 6. The online client maps +0x34 in `FUN_1412d43d0` [V]: 3 → OnlineErrorReasonMatchmaking_Offline, 4 → _Qos,
  6 → _QueryError, and queues event type 0x14 for the UI. Use reason 3 ("offline"): it is the same event the game raises when
  matchmaking is started while offline (`FUN_1412dee30` raises {1, 0x1e4595bc, 1} itself when `[this+0x1c28]->vtbl[0x18]()` is false) [V].
- **Joins:** `FUN_141ba15f0` and `FUN_141ba1a30` have native reject branches [V]:
  `msg = FUN_141ba4f90(backend, reqId, params, gameId, err)` then `FUN_140234bd0([DAT_1441c2910]+8, msg, 0.0)`.
  15f0 uses err 3 (game id 0), 0x19 (same join already pending), 0x1b; 1a30 uses 0x19. `params` (param_3) is the same JoinParams
  type in both (15f0's local copy is made by the copy ctor `FUN_141ba1950`) [V], gameId = `*(u64*)(params+0x30)` [V].
  `FUN_141ba4f90` builds a PresenceGameMessage (0x11cd888c) with success=0 (via `FUN_141b9ed80` or `FUN_14194a510(…,5/6)`
  depending on params+0x48) [V]; the UI's text for each code is unknown [I] (at worst a generic "failed to join").
- **Custom create (only if not rerouted):** the custom-match module waits for PresenceGameMessage type 1/2 with its request id
  (`FUN_14174b4e0` [V]) and turns success=0 into CustomMatchGameCreatedMessage(false) [V], which the UI shows as a failed
  creation [I]. A failure can be produced with `FUN_141ba4f90` and a zeroed fake JoinParams {+0x40 = 1 (type), +0x48 = 0} [I].
- **SDK level:** API early-out with 0x40040000 (Section 0.3) [V], i.e. the same callback path as a server-side error.
- **Avoid:** returning from sendRequest with JobId 0 (no callback ever: the requester waits forever) [V for the code path,
  I for the UI hang].

## Q4. Single chokepoint vs per-path

| candidate | covers | before the request? | clean failure? | context to tell game from party group? |
|---|---|---|---|---|
| sendRequest `0x1422676a0` (component 4) | everything | yes | **no** (JobId 0, no callback) | no (would need TDF request parsing) |
| GameManagerAPI 7 entry points | everything | yes | yes (replicate the 0x40040000 early-out; per-function callback-arg slot: e.g. createGameTemplate arg6, joinGame core arg8, createGame internal arg5) | no: party create (createGameTemplate) and party join (joinGame) use the same functions |
| presence backend (`141baad30`, `141ba15f0`, `141ba1a30`) + router | all known UI paths | yes | yes (native reject messages) | **yes**: caller/return address and template name |
| client "join server" 0x4551edba / connect `0x140cb0500` | game sessions only | **no** (Blaze game already joined/created) | partial | n/a |
| client game type (client+0x4ac Hosted) | game sessions | no | no | n/a |

**Verdict:** no single point is both early and context-aware. Use per-path hooks at the presence layer (four hooks), plus
sendRequest as a log-and-block tripwire for anything unmapped, plus the 0x4551edba drop as a last backstop.

## Q5. What still talks to EA after the guard

Harmless account/session services that must keep working (untouched by the design): Nucleus/Origin sign-in (NucleusResponseLoginUI),
Blaze login and session, redirector, marketplace inventory (`FUN_1418fa950`), loadouts (`FUN_1418f61b0` and related), stats and
PIN telemetry (WSTelemetryEvent streams; GameManager reportTelemetry 0xab), presence and friends, QoS pings (DirtySDK), the
**party group** (createGameTemplate incom[No]CrossplayGroup at boot, `FUN_1412d6120` → `FUN_1412d4800`, and group joins via the
type-4 message), leave-game requests, and **server-browser list queries** (getGameListSnapshot/Subscription 0x64/0x65: they read
EA's game list, so the browser still shows live EA games; joining is what the guard blocks). Leaving these alone means EA still
sees the account online, as it does today [I]. The Peer server backend (`FUN_1412ee8d0`, spawnInfo+0x28 coop) would register a
Blaze game for a peer-hosted local server: keep it unreachable (backend chooser `0x1418f2930` forced to Local; the notes say the mod
already has force_local_backend) [I].

## Recommended design (SERVER build of sws_probe.cpp, `[server] online_guard=1` default on)

Globals used below [V]: `G(0x1441c2910)` holds the presence message-manager owner (`mm = rd<u64>(rd<u64>(G(0x1441c2910),0),8)`),
post = `FUN_140234bd0(u64 mm, u64 msg, float delay)` (3rd arg is xmm2; pass 0.0f).

1. **custom-match-route `0x14174d540`**, steal 14: `48 8b c4 55 57 41 56 48 8d a8 d8 f6 ff ff`
   (`mov rax,rsp; push rbp; push rdi; push r14; lea rbp,[rax-0x928]`).
   `void h(u64 self)`: if guard and `rd<int>(self,0x26c)==0`: `bs = rd<u64>(rd<u64>(self,8),0x18)`; mode = `rd<int>(bs,0x18)`;
   if mode ∈ {0,1,2}: log "guard: custom match mode %d (EA-hosted) → local Solo route", write 3 to bs+0x18 (option B). Call the original.
2. **matchmaking-start `0x141baad30`**, steal 16: `48 89 5c 24 08 57 48 83 ec 20 48 8b d9 48 8b fa`.
   `void h(u64 mm, u64 params)`: if guard: log; `post(mmgr, FUN_141baac60(3, 0), 0.0f)`; return. Else the original.
3. **presence-join-by-id `0x141ba15f0`**, steal 15: `40 55 53 56 57 41 56 48 8d ac 24 a0 fd ff ff`.
   `void h(u64 backend, u32 reqId, u64 params)`: allow when `_ReturnAddress() == 0x141ba579a` (type-4 group join from
   `FUN_141ba5670`; the call is at 0x141ba5795) [V call site; I that type 4 is party-only]. Otherwise, if guard: log gameId
   (`params+0x30`) + caller; `post(mmgr, FUN_141ba4f90(backend, reqId, params, rd<u64>(params,0x30), 3), 0.0f)`; return.
   Set a thread-local `g_allowGM` while calling the original on the allowed path (for the tripwire).
4. **presence-join-by-player `0x141ba1a30`**, steal 15: `40 55 53 56 57 41 56 48 8d ac 24 40 fe ff ff`.
   Same as 3 with `FUN_141ba4f90(backend, reqId, params, 0, 0x19)` (its own code). Open item: whether this is also the
   "join a friend's party" route (params+0x48 = 1/2 there; select response types 6/5). First launch: log params +0x00 (player id),
   +0x30, +0x40, +0x44, +0x48, +0x4c, +0x4d, +0x50, +0x54, +0x55 and the caller, then decide.
5. **Tripwire: Blaze sendRequest `0x1422676a0`**, steal 17: `40 53 48 83 ec 50 4c 8b 59 38 48 8b da 4c 8b 51 40`.
   `u32* h(u64 mgr, u32* out, u16 comp, u16 cmd, u64 req, u64 job, u64 a7, u64 a8, u32 a9)`: if comp==4, log the command name
   (`((const char*(*)(u64,u16))0x1422693d0)(0, cmd)`). If guard and !g_allowGM and cmd ∈ {0x01, 0x09, 0x10, 0x1e, 0x2d, 0x5a}:
   log "guard: BLOCKED unmapped GameManager %s" and return `*out=0` (no send; caller may wait: acceptable for a should-never-fire
   tripwire). Leave 0x0a createGameTemplate log-only (party groups use it; custom ones are handled by hook 1, optionally also by a
   `FUN_141b9ff70` hook that checks the "incomCustom" prefix at param_3+8).
6. **Backstop, existing hook client-message `0x140a338d0` (h_cmsg):** if guard and id == 0x4551edba and the message did not come
   from the mod's own flow: log the address (msg+0x50 = game/host field written by `FUN_14194b670`: local_88[10]) and drop it.
   Optionally, in h_connect, refuse any connect address that is not 127.0.0.1/localhost/the configured join address.
7. Keep the Peer backend unreachable (force Local in the chooser or refuse spawnInfo+0x28 when guard is on).

## Risks

- Hook 3's caller allow-list assumes the type-4 message is the party/group join only (one sender, `FUN_1412d4a40` via online-client slot
  0x14358ca38, which leaves the current group first) [I strong].
- Hook 4 could block "join friend's party" if JoinGameByPlayer also serves parties [I]. Parties are not in the must-keep list,
  but launch logging should settle it.
- Error codes 3 / 0x19 / reason 3 are the game's own, but the UI text each produces is unverified [I].
- Option B changes the visible custom-match mode to PvB Solo [I]; option A leaves an untested server-side combination [I].
- The tripwire's block leaves that requester without a callback (possible stuck "joining" UI) [I]; it should never fire.
- Any EA path not reaching these functions (e.g. end-of-round matchmaking driven server-side, tournament games) is caught only by
  the tripwire [I].

## What only a launch can settle

1. Which UI actions hit which hook (log callers): browser join, Fleet Battles/Dogfight quick play, invites, friend join, party follow.
2. The on-screen message for each blocked path (matchmaking "offline" popup; join codes 3/0x19).
3. Whether party create/join still works with the guard on (no tripwire hits for createGameTemplate/joinGame from party flows).
4. Option A vs B for the rerouted custom match (server-side waiting room/team setup with a non-Solo GameMode).
5. That `FUN_14174d7e0` with a mode-0..2 lobby state behaves like a Solo pick (map stays Yavin unless match_mode/map code changes it).
6. Whether hooks 3/4 leave the backend's pending-join bookkeeping consistent for a second attempt (native reject paths do not insert
   into the pending map, so it should).
