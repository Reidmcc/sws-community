# Inventory, unlocks and loadouts: how the client decides what a player may use

2026-10-20. Static reading only: Ghidra decompiles in `work/decomp/`, capstone, and the game data via `swsdata`.
Nothing was run in the game. Tags:
- **[V]** means verified in code or data.
- **[I]** means inferred. Those are hypotheses to probe.

## 0. Short answer

- **The "inventory" is a vector of 0x38-byte items** in the client's online data model. Each item's ownership key is a
  **numeric u64 at +0x10**, which is compared with `BaseConfig.Identifier` or the config's `ConfigFamily.Identifier`.
  - It is filled from Blaze `EADPMarketplace::GetInventory`.
  - It is cached locally under the key `"__inv_"`.
- **No server-side ownership check was found for starship components** [V for the functions read]. The server accepts
  `IncomCustomizationSetMessage` subject only to:
  - a kill switch, which defaults to *enabled*;
  - the preset index;
  - the kit belonging to the player's team.
- **Networked mode loses the loadouts because the EA "connection lost" path resets the whole data model.** The reset:
  - sets ConnectionState to Offline;
  - wipes the inventory and the online presets.

  Only the inventory comes back, from the `__inv_` cache. Everything that applies the online presets requires
  ConnectionState==Online.
- **Best all-unlocked option:** one hook on the store's SetInventory, `FUN_14123a310`, which appends one item per
  Identifier. This hook alone does not restore presets after the disconnect (see §5).
- **`WSOfflineInventorySettings` has no reader in the readable code** [V for .text1/.data2, see §3].

## 1. Client inventory

### 1.1 Where it lives [V]
- `P = [0x1441c5f28]` is a small struct. `[P+8]` is the online data model ("store"): vtable `0x1433edbe8`, about 0x1a28 bytes.
  - The same store is the online client's `+0xe00` (`param_1[0x1c0]` in `FUN_1412df8a0` and `FUN_1412dd520`).
  - The online client's IInventory sub-object (vtable `0x14358c168`) sits at `+0x10`, so the store is at `+0xdf0` from there.
- `store+0x20` is `ConnectionState`: 0 Offline, 1 Connecting, 2 Online. The getter is vtable[0].
- `store+0xe0` is the inventory state. The getter is vtable[5] or `FUN_14123a150`. Layout:
  - `+0x00` int progress (`InventoryProgress`: 0 NotDownloaded, 1 Downloaded, 2 Failed);
  - `+0x04` int status;
  - `+0x10` / `+0x18` vector of items, begin and end, stride 0x38.
- The setters are non-virtual helpers that fall through to vtable[0x2c]:
  - `FUN_14123a310(store, vec*)` sets the items. It copies `[vec->begin, vec->end)` with `FUN_1401ce440`.
  - `FUN_14123abb0(store, progress, int* status)` sets progress and status.
  - Each setter bumps a dirty counter at `+0xe8`.
- The online presets (ship and pilot loadouts) live in the same store:
  - the getter is vtable[9] (`+0x48`, wrapper `FUN_1413560e0`), and it returns `{int state, int status, set@+8, set@+0x40, set@+0x78}`;
  - the setter is `FUN_1418f58f0(store, data, state)`, which calls vtable `+0x180`.
- The reset is vtable `+0x260` = `FUN_1401b7060`. It reinitialises 0x1a08 bytes from `store+0x20` (connection state,
  inventory, presets and the rest) and keeps only `+0xb98` and `+0xed0`/`+0xed4`.

### 1.2 Item layout (0x38 bytes) [V for the offsets, I for some meanings]
| off | type | meaning |
|---|---|---|
| +0x00 | char* | Blaze item-id string (Blaze item +0x18) |
| +0x08 | char* | second string (Blaze item +0x48, if non-empty) |
| **+0x10** | **u64** | **numeric id (`FUN_1418fbfa0(item)`), the only field the ownership checks read** |
| +0x18 | u64 | value of attribute `"minLevel"` |
| +0x20 | u64 | a second numeric attribute (6-char name at `0x143686d04`) |
| +0x28 | bool | `"isNew"` attribute (drives the "new item" badges in `FUN_1417109d0`) |
| +0x2c, +0x30, +0x34 | u32 | Blaze item +0x60, `FUN_1418fc000(item)`, `FUN_1418fbec0(type)` |

- Empty strings point to the sentinel `0x143f7b968`, a zeroed .rodata tail that the string code never frees [V].
- Item types come from `Blaze::Maverick::OnlineItemType` (string table built in `FUN_1419d9920`). The relevant ones are:
  - `ShipComponent`;
  - `Ship`;
  - `ShipLoadout`, `PilotLoadout`;
  - `Auxiliary`, `Countermeasure`, `Hull`, `Primary_Weapon`, `Secondary_Weapon`, `ShipVisual`, `ShipDecal`, `Pilot`.

  Currencies are named `Credits`, `Crystals`, `Ship_token` and `Ship_component_token`, the last being the requisition
  unlock tokens; the parser is `FUN_1418f9460`.

### 1.3 How it is populated [V]
The online inventory manager's `+0x10` is the store, and `+0x18` is the online event sink.

- **Request:** `FUN_1418fa950(mgr, full)` sends a Blaze request (component `0x80c`, query `"view=full&limit=10000"`) with
  the callback `FUN_1418fd3d0`.
  - Caller: `FUN_1412df760`, the online client's vtable slot at `0x14358c158`.
- **Response:** `FUN_1418fd3d0` parses the items and the loadouts. It then:
  1. calls `FUN_1418ff580(mgr, &items, status=1)`;
  2. saves the cache with `FUN_141270710(&items)`;
  3. sets the presets with `FUN_1418f58f0(store, …, 1|2)`;
  4. posts `ClientOnlineLoadoutUpdatedMessage` (`0x416f9105`).
- **Errors:**
  - `FUN_1418ff180` and `FUN_1418fe780` (GetInventoryError/TimeOut) set progress to 2 (Failed).
  - `FUN_1418ff110` (UncertainInventory) sets progress to 0.
- **`FUN_1418ff580(mgr, vec<src>*, int* status)`:**
  1. dedupes with `FUN_1418ff330`;
  2. calls `FUN_14123a310(store, &v)`;
  3. calls `FUN_14123abb0(store, 1 /*Downloaded*/, status)`;
  4. posts `MarketplaceInventoryUpdatedMessage` (`0x3d50f9ae`; category `0xdb9834da`).

  Callers: `FUN_1418fd3d0`, `FUN_1418fa280`, `FUN_1418f74f0` (purchase and grant flows) and `FUN_1418ff260`.
- **Local cache:** the key `"__inv_"` in a key-value blob store at `[0x14419b990]` (+0x10 or vtable) `+0x250`.
  - Writer: `FUN_141270710`, which calls `FUN_140ce7610`.
  - Reader: `FUN_141270aa0`, which calls `FUN_140ce7bc0` and requires value type 5 (blob).
  - Blob format:
    ```
    u32 0x0e0c010d, u8 1, u16 count,
    count × { "solo", u32 n, itemId[n], u32 m, str2[m],
              u32 (+0x2c), u32 (+0x30), u32 (+0x34), u32 id (+0x10) }
    ```
  - Where this store is persisted on disk was **not determined**.
- **Cache loaders:**
  - `FUN_1418ff260(mgr)` loads the cache and calls `FUN_1418ff580` with status 0. It is called from the
    connection-lost path (§2).
  - `FUN_141903df0(mgr)` loads the cache and calls `FUN_14123a310` and `FUN_14123abb0(…,1,0)` directly. It is reached
    via the thunk `0x1412db2d0` (this+0x1108), which sits in the online-client vtable slot `0x14358c7b8`.

### 1.4 Who decides "selectable/owned" [V]
All of these scan `store+0xe0` items for `item+0x10 == cfg.Identifier` or `item+0x10 == cfg.ConfigFamily.Identifier`.
In the asset, `BaseConfig.Identifier` is `+0x20`, `ConfigFamily` is `+0x28`, and `ConfigFamily.Identifier` is `+0x20`.

| function | role | gates |
|---|---|---|
| `FUN_141644fb0` | `ClientMarketplaceItemInfoEntity` update. Inputs: ItemId `+0x20`, DefaultItemRarity `+0x24`, DebugEnableBuyingOwnedItem `+0x28`. Outputs owned, isNew and price; the "owned/free" output is `FUN_141644dc0`, which sets output +0x58 = true. This is what the hangar/marketplace UI shows. | owned if (a) mission whitelist `FUN_14170c300()` ≠ 0; or (b) GameMode=="Practice" (`FUN_141740ed0`) and ConnectionState≠Online; or (c) the id is a faction DefaultConfig (`FUN_141744600`); or (d) the inventory has the id (needs progress==1, but **not** Online) |
| `FUN_141744860(cfg)` | "is config available". Callers: `14163b1e0`, `14163ca20`, `141641b50`, `14164bfa0` (UI and customization entities) | only for cosmetic classes (BodyPartVisualConfig, StarshipVisualBaseConfig, FlairSlot, EmoteConfig); **every other config type returns 1**. Free flags: `cfg+0x3c`/`+0x68` (body part), `+0x70` (visual). Then the same (a)–(d) as above |
| `FUN_141743f60(id, faction, inv)` | "must reset this slot" (1 = not owned), used by the loadout sanitizer `FUN_141707850` | default config → keep. Otherwise scans the inventory |
| `FUN_141707850` | sanitizes the online presets. Each non-owned component in ship/pilot presets is reset to default with `FUN_141709df0`/`FUN_14170a090`, then the fixed presets are pushed back to the backend | runs only if ConnectionState==Online **and** presets {1,0} **and** inventory progress==1 |
| `FUN_141743e90(cfg)` | owned check used by `FUN_14164cff0` (random pilot customization; `cfg+0x3c` = BodyPartBaseConfig.AvailableOffline also allows) | requires Online **and** progress==1 |
| `FUN_1412def60(this, u32 id)` | IInventory::HasItem (vtable `0x14358c168`[0]); `[1]` = `FUN_1412df000` GetItem | progress==1 |

- Class ranges in these checks come from `*(u16*)(typeinfo+0x48)` against `DAT+0x48/+0x4a` of the class objects. The
  class objects are:

  | class object | class |
  |---|---|
  | `0x1449fbe00` | BodyPartVisualConfig |
  | `0x144a53dc0` | StarshipVisualBaseConfig |
  | `0x144a01be0` | FlairSlot |
  | `0x144a39200` | EmoteConfig |
  | `0x1449fd2b0` | StarshipVisualConfig |
  | `0x1449fd230` | StarshipDecalConfig |
  | `0x1449fc3f0` | StarshipWeaponSlotConfig |

- GameMode helpers. Each reads level `"GameMode"` and compares its djb2 hash:

  | function | true when GameMode is |
  |---|---|
  | `FUN_141740ed0` | Practice |
  | `FUN_141740e40` | Missions |
  | `FUN_141740db0` | Tutorial |
  | `FUN_141740d10` | Dogfight, Frontline or Frontline_Bots |

### 1.5 What has to be owned (game data) [V]
`StarshipCustomizationAsset` (one per kit: X-wing, A-wing, U-wing, Y-wing, B-wing, TIE fighter, TIE bomber, TIE
interceptor, TIE reaper, TIE defender) has:
- `AdditionalConfigs`: the ownable items;
- `DefaultConfigs`: 9 per kit, always free;
- `SPOnlyConfigs` and `SPDataConfigs`: single-player only.

Totals over all kits plus the pilot, flair and emote assets:
- 1,367 unique `AdditionalConfigs` Identifiers in 258 ConfigFamilies.
- Gameplay components: 224 Identifiers in 92 families:
  - WeaponSlot 125;
  - Engine 37;
  - Hull 36;
  - Shield 26.
- The remainder are cosmetics:
  - decals 546, ship visuals 210;
  - pilot parts 250, voices 18, body types 2, poses 31;
  - flair 72, emotes 19.

The items match on the Identifier *or* the family id, so the 258 family ids plus the Identifiers of configs that have no
family cover everything. The list can be regenerated with the `AssetIndex`/`EbxFile` API:
1. Load the roots of all EBX under starships, customization, pilot, flair and emote paths (about 11.5k assets, a few seconds).
2. Resolve each `*CustomizationAsset.AdditionalConfigs` `$import` by file GUID.
3. Read `Identifier` and `ConfigFamily→Identifier`.

## 2. Single-player vs networked: why networked ends on the DEFAULT loadout

- The client customization manager's message listener is `FUN_1417072b0`. Its vtable slot is `0x14361d588`, and it
  receives `this = listener-8`.
  - On `IncomCustomizationInitMessage` (sent by the server on ServerPlayerEnteredLevel; see §4) or
    `ClientOnlineLoadoutUpdatedMessage`, it calls `FUN_141708350(mgr, player, state, useOnline = !Missions && !Tutorial, init)`.
- `FUN_141708350` applies the online presets only if **ConnectionState==Online and presets state=={1,0}** [V]. It then:
  - writes each slot with `FUN_14170a090`/`FUN_141709df0`, which update the local state through `FUN_14170cf60`/`FUN_14170ce10`;
  - sends `IncomCustomizationSetMessage` (category `0xd605985e` "IncomCustomization", type `0xeb153a97`).

  Otherwise the state stays at the kit defaults.
- **Connection lost** comes from online event 0x1e, status 1, handled by `FUN_1412dd520` case 1 [V]. In order it:
  1. shows the popup;
  2. calls `FUN_141924850`;
  3. calls **store vtable+0x260 = `FUN_1401b7060` (full data-model reset)**;
  4. calls `FUN_1412dd4c0`;
  5. calls **`FUN_1418ff260(this+0x2058)`**, which reloads only the inventory from `__inv_`, with progress Downloaded.

  The result is ConnectionState Offline, presets gone and inventory cache-only. That gives default loadouts [I for the
  causal link to the symptom; each step is V].
- **Solo vs AI works [I]:** that match never takes the 0x1e/status-1 path, so the store stays Online with the presets.
- **No separate SP loadout manager was found in readable code.** `WSClientSPLoadoutManagerInitializedMessage`
  (`0xac700bc7`) exists only as a prototype in .rodata (`0x143ebe3b0`, vtable `0x143585158`). Its vtable is referenced
  only by that prototype; nothing constructs or compares it in .text1/.data2. It is probably used by protected (.idata)
  code [I].
- The single-player campaign uses `SPOnlyConfigs`/`SPDataConfigs` and the mission whitelist (`FUN_14170c300` reads
  `MissionCustomizationWhitelistEntity.WhitelistAsset`) instead of the inventory [V for the whitelist bypass].

## 3. WSOfflineInventorySettings / WSEventOfflineInventorySettings [V: no consumer found]

- **Code references:** the only references to the class objects (`0x144ae3fa0`, `0x144ae3f20`) and to the names
  `"WSOfflineInventory"` (`0x143583308`) and `"WSEventOfflineInventory"` (`0x1435831d8`) are the registration at
  `0x142e2acf3`/`0x142e2add3`.
- **Settings getters:** every getter that caches a settings instance from `DAT_143f7d798` was listed (63 of them; 55 map
  by their `lea` to a class).
  - None is either offline-inventory class.
  - Eight are Denuvo thunks: `0x1401c3eb0`, `0x1401c3ff0`, `0x1401c40b0`, `0x1401c4150`, `0x1401c4290`, `0x1401c4330`,
    `0x1401c4410`, `0x1401c45f0`.
  - Their cached instances are read at offsets that do not fit a 48-byte {bool@+0x20, array@+0x28} object.
- **Type hashes:** `0xde34e6ef` (WSOfflineInventorySettings) and `0x519de583` (WSEventOfflineInventorySettings) appear
  nowhere in .text1.
- **Conclusion:** most likely dead or compiled-out data, or read only from protected code [I].
- **Expected format:** `InventoryItems` would hold item-id strings (field +0x00), not numbers [I]. The loaders above never
  read them, so setting `Enabled` cannot be expected to do anything [I; cheap to test by setting it].
- **The working "offline inventory" is the `__inv_` cache** (§1.3).

## 4. Server side: does it validate loadouts?

### 4.1 The server customization manager
Message handler `FUN_14170f5e0` (vtable slot `0x14361d558`, `this = listener-8`; the manager vtable is `0x14361d568`, whose [1] `FUN_14170ffd0` returns `serverPlayer+0x1b8`) [V]:

- `IncomCustomizationSetMessage` → `FUN_14170fa40`. Fields:
  - +0x58 category, +0x5c player index, +0x60 faction;
  - +0x64 kit, +0x68 preset, +0x70 config, +0x7c/+0x7d flags.
- **Ship slots** (categories 1–5, 0x10, 0x13):
  1. requires kill switch `"KS_Loadouts"` = `FUN_1417444b0(1, player+0x68)`;
  2. checks the preset with `FUN_141744200`, skipped when Practice and +0x7d is set;
  3. stores the config with `FUN_1417103f0` → `FUN_14170ce10` into `serverPlayer+0x1b8`.
- **Pilot cosmetics** (7–0xd, 0x11, 0x12, 0x15) and **emotes** (0xf) require `"KS_CharacterCustomization"`, then
  `FUN_141710680`.
- **No inventory or entitlement check.**
- **Kill switches:** `FUN_141781730` returns 1 (enabled) unless the switch is found in the list. The server list is at
  `[0x1441c5f20]+0x5280` and the client's at online client `+0xf30`. An empty list means everything is allowed.
- **`FUN_141744200(player, faction, presetIdx)`:**
  - valid if `presetIdx < min(kitData+0x58, (Dogfight|Frontline|Frontline_Bots|Practice) ? 5 : 1)`;
  - otherwise the preset is rank-gated: table `[0x1441c5f00+0xf8]+0x30` against the player rank at `player+0x4c`.
- **`IncomCustomizationSetDefaultStarshipInfoMessage`** applies the kit/preset with `FUN_14170d730` and `FUN_1415a07a0`.
- **On ServerPlayerEnteredLevel** the server sends `IncomCustomizationInitMessage` with its stored state to the client.

### 4.2 Other server paths
- **`NetworkAssignKitMessage`** (`FUN_14159fd70`, server player; +0x58 kit, +0x5c preset, +0x60 pilot kit) calls
  `FUN_1415a07a0`.
  - It checks the kit against the team's StarshipKits and the custom-match ship restrictions (`FUN_1415a00a0`, using
    `[0x1441c5f00]+0x4189`/`+0x3df8`).
  - It requires preset < 5.
  - It then calls `FUN_1415a0940`, which fills empty required slots (weapon slots, visual, decal) with
    `DefaultConfigs` via `FUN_14170ce10`.
  - **No inventory check** [V].
- **`NetworkPlayerSelectedUnlockAssetsMessage`** (`FUN_140bf5a90`) filters `VisualUnlockAsset`s through the player's
  unlock provider (virtual `+0x20`) before applying them to the ServerCharacterEntity. This is a generic Frostbite
  pilot-visual filter, not starship components [V for the code, I for its source of truth].
- **Not located in readable code:** `NetworkPlayerSpawnWithLoadoutMessage` (`0x266dbdd2`, category Network
  `0xc67d277b`) and `PersistenceNetworkLoadoutInfoMessage` (`0x003ae2a1`).
  - Only reflection constructors exist (`0x142e284a0`, `0x142e413a0`).
  - No type compares were found.
  - These are likely handled in protected code [I].
- **`ServerGateway_GetInventoryError`** (`0xf95f2370`) is never compared in .text1, so no server-side inventory fetch
  is visible.

## 5. Options for "everything unlocked", ranked

1. **Synthetic inventory, one hook. Recommended.**
   - Hook `FUN_14123a310(void* store, Vec* v)`, where `Vec = {Item* begin, Item* end, …}` and `Item` is 0x38 bytes.
   - It is the only writer of the inventory vector; it is reached from `FUN_1418ff580` and `FUN_141903df0` [V].
   - The hook passes a private `{begin, end}` over a static array: the original items plus one Item per id
     (`+0x10 = id`, both strings = `0x143f7b968`, everything else 0).
   - `FUN_1401ce440` only reads begin and end, so the array can be ours [V].
   - The callers always follow with `FUN_14123abb0(store, 1, …)`, which sets progress to Downloaded.
   - **What it unlocks** (they all need progress==1 only):
     - the UI owned flag (`FUN_141644fb0`);
     - cosmetics availability (`FUN_141744860`);
     - the sanitizer (`FUN_141743f60`);
     - `HasItem`.

     `FUN_141743e90` also needs Online.
   - **Supply:** the 1,367 Identifiers, or the 258 family ids plus family-less ids (§1.5).
   - **With no backend at all:** nothing calls the setter, so call `FUN_14123a310([P+8], &v)` and then
     `FUN_14123abb0([P+8], 1, &zero)` once ourselves, with `P = [0x1441c5f28]`.
   - **Limitation:** it does not bring back presets lost in the §2 reset.
2. **Forge the `__inv_` cache, no code patch.**
   - Write the blob from §1.3 with the same ids. The stock code loads it on disconnect (`FUN_1418ff260`) and via `FUN_141903df0`.
   - Drawbacks:
     - the next real GetInventory overwrites it (`FUN_141270710`);
     - the on-disk location of the `[0x14419b990]` store is unknown;
     - loading is not under our control.
3. **Keep the store "Online" and the presets alive.** Stop or neutralise the `FUN_1412dd520` case-1 teardown in
   networked mode: at minimum skip the `+0x260` reset, or re-run the preset apply afterwards. Without this, players keep
   default loadouts even when everything is owned (§2). This is a separate fix from unlocking and needs the
   0x1e-event root cause (see test1-static-map.md).
4. **Patch the individual checks.** Five functions return "owned":
   - `FUN_141744860` → 1;
   - `FUN_141743f60` → 0;
   - `FUN_141743e90` → 1;
   - `FUN_1412def60` → 1;
   - plus the inline scan in `FUN_141644fb0`.

   Drawbacks: more hooks, and inline copies may be missed.
5. **Force "Practice" (`FUN_141740ed0` → 1) while Offline.** This makes `FUN_141744860` and `FUN_141644fb0` report
   everything owned. It has 14 callers, including the server preset rules and FUN_14170fa40's preset-check skip, so the
   side effects are broad. Not recommended.
6. **Force the SP loadout manager, or enable WSOfflineInventory:** no readable code path (§2, §3). Not viable without
   reaching into protected code.

The server needs nothing for components: it does not validate ownership [V for the readable paths]. Kill switches
default to allowed, and only presets 0–4 are valid in Dogfight/Frontline.

## 6. Open points
- Where the `[0x14419b990]` key-value store (holding `__inv_`) is persisted.
- What raises the 0x1e/status-1 event in networked mode. This is the real reason the presets vanish.
- Handlers of `NetworkPlayerSpawnWithLoadoutMessage` and `PersistenceNetworkLoadoutInfoMessage`, and the
  `WSClientSPLoadoutManager`. These are probably in protected code.
- The exact semantics of `FUN_1418fbfa0` (Blaze item → numeric id). It is assumed to equal the config Identifier,
  because every reader compares it with `BaseConfig.Identifier`.
- Probes worth adding to the DLL:
  - log `store+0x20`, `+0xe0` (progress and count) and the preset state (`FUN_1413560e0`) before and after the 0x1e event;
  - try option 1 with the 224 component ids first.
