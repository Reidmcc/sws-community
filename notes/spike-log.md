# Squadrons community-server spike: running log

Started 2026-10-19. Everything below was measured from Pseudo's install, read-only
(`E:\SteamLibrary\steamapps\common\STAR WARS Squadrons`, never written to), or from
cloned reference code in `refs/`.

## The install
- Frostbite. Uses the newer "manifest" layout: `Data/layout.toc`, `initfs_Win32`,
  and CAS/cat install chunks (`initialexperience`, `mp`, `sp`, `misc`), plus a `Patch/` overlay. 32 GB.
- EasyAntiCheat. `starwarssquadrons_launcher.exe` is the EAC bootstrapper, and
  `EasyAntiCheat/Launcher/Settings.json` points at `starwarssquadrons.exe`.
- Backend is EA Blaze. The game's internal codename is **Maverick** (`Blaze::Maverick`,
  `MavSeason`, `MavChallenge`, `MavEvent`). There are also leftover `PvzGwStats` components.
- The exe is 515 MB, built 2021-10-21 (the final patch).

## The client exe carries the server half
- 434 `Server*` entity/component types in the exe's strings (vs 448 `Client*`).
  These include `ServerAIVehicleBehaviorEntity`, the capital-ship subsystems and the weapons.
- Strings found: `CLIENT_TYPE_DEDICATED_SERVER`, `GameNetworkTopology_DedicatedServer`,
  `IsDedicatedServerLevel`, `IsDedicatedServerBundle`, `DedicatedServerCpu`.
- `NullRendererEnable` is present (a render setting), which is promising for headless.
- Online backends registered in the type info:
  - `OnlineBackend_Blaze`, `_BlazeInProc`, `_Lan`, `_Local`, `_Peer`, `_DirtySock`, ...
  - `LanServerBackendData`, `LocalServerBackendData`, `PeerServerBackendData`,
    `BlazeInProcServerBackendData`, `LanPresenceBackendData`.
  - **Open question:** are the LAN/Local backends' implementations compiled into the
    shipping exe, or only their type info? If they are, the engine may already have a
    no-Blaze path to a server.

## Engine settings that matter (from Frosty's SWSSDK type dump)
- `ClientSettings.ServerIp`, `SecondaryServerIp`, `QuitGameOnServerDisconnect`
- `NetworkSettings.ServerPort`, `ClientPort`, `ServerAddress`, `MaxClientCount`, `ProtocolVersion`, `TitleId`
- `ServerSettings.InstancePath`, `ServerName`, `ServerPassword`, `IsAiEnabled`, `Playlist`,
  `ForcePlaylist`, `IsRanked`, `DedicatedServerCpu`
- `OnlineSettings.Environment`, `Provider`, `ServerLoginEmail/Password/PersonaName`,
  `TrustedLoginPath/CertFilename/KeyFilename`, `IsSecure`
- `GameSettings.Level`, `StartPoint`, `MaxPlayerCount`, `DefaultLayerInclusion`
- `WSGameSettings.SkipLobby`, `ForcePrivateMatchLobby`, `DisableStartupFlow`
  ("WS" is the Squadrons/Motive game-specific prefix)
- `SyncedGameSettings.EnableFriendlyFire`, `BulletDamageModifier`, `AllowClientSideDamageArbitration`

## Balancing knobs: all data (EBX), not code
Frosty's `SWSSDK.dll` holds 12,418 types (dumped to `notes/sws_sdk_types.json`).
The ones that matter for league balance:
- **`StarshipMovementConfig`**: the flight model.
  - MaxSpeeds, the acceleration and deceleration rates, turn rates, TurnRateBySpeedCurves.
  - **Drift\***: DriftActivationCost, DriftAxisTurnRate, DriftDecelerationFactor, DriftTransitionTime, DriftFullMomentumAngle.
  - **Afterburner\***: MaxSpeed, ActivationCost, ChargeRate, DecayRates, RechargeCooldown, AfterburnerDriftBoostAccelerationForce(+Time).
  - EngineEnergyRegen, and the mouse/joystick input configs.
- **`StarshipPowerManagement`**: Engine/Weapon/ShieldPowerData (min/max/default levels),
  WeaponToEngineConversionFactor, EngineToWeaponConversionFactor, SupportEmergencyPowerTransfer.
- **`StarshipShieldData`**: max/initial shield, the regen rates and delays by power level,
  the supercharge decay, and the second-layer threshold.
- **`StarshipHealthDamageData`**: MaxHullHealth, the collision damage, and the shield/hull
  multipliers for laser vs non-laser damage.
- **`IncomBulletConfig`**: Start/EndDamage, the falloff distances, ion %, InitialSpeed, TimeToLive.
- **`IncomMissileConfig`**: damage, speeds, arm time, evade parameters, lose-target distances.
- **`LaserRegenWeaponFiringData`** / **`StarshipWeaponFiringData`**: the energy pools, cost per shot,
  the cooldowns, burst/auto timing, ammo, and aim assist.
- **`StarshipSubSystemData`**, **`StarshipAttributeModifiersConfig`**, **`IncomAttributeModifier*`**:
  how components modify the base ship.
- The capital ship / Fleet Battles settings: health, subsystems, weak points, turrets, engine wash.

The consequence: a mod that edits these values changes balance while the game's own code
(and so its flight-model feel, exploits included) runs unchanged.

## Precedents
- **Frosty Toolsuite** has a Squadrons profile: ManifestAssetLoader, NullDeobfuscator,
  and the same code paths as Battlefield V almost everywhere.
- **Kyber** (Battlefront II 2017): open source since Jan 2026. Its architecture is being
  read in `notes/kyber-architecture.md`.

## Data reader results (2026-10-19, `tools/swsdata.py`, details in `data-reader.md`)
- Everything comes from `Data/layout.toc`; there's no patch layout. The manifest has 1,112 bundles,
  and **all 97,444 EBX assets parse with 0 failures**. ZStd compression, EBX v4, self-describing.
  No Oodle and no encryption.
- X-wing, spot-checked by me from the dump:
  - MaxSpeeds run 120 → 145 in 9 steps (my reading: engine power in half-pip steps).
  - AfterburnerMaxSpeed 240, AfterburnerActivationCost 0.05.
  - DriftActivationCost 0.0, DriftDecelerationFactor 1.5.
  - The rest is in `balancing-knobs-sample.md`.
- The values are `IncomAttributeFloat` objects with an `AttributeId` asset
  (`game/gameplay/starships/common/attribute/ids/movement/attributeid_*`). So the attribute
  system is the hub that components modify, and a mod can target these ids.
- Open: tunables appear both as typed fields and in `*IncomAttributesToRegister` lists, and we
  don't know which one the game reads. Mods should edit both until measured.

## Online backends in the shipping data
- `legacy/online/onlineprovider` (`OnlineProviderAsset`) has a Win32 config for
  `Client: "maverick server"` with `IsServer: true` (ServiceName `maverick-1-pc`), alongside the client one.
- `online/onlinebackends/localserverbackend` → `LocalServerBackendData`, with
  **BackendType 0xB8566ABC**. That is the exact constant Kyber forces the BF2 server's
  presence backend to (per the Kyber read, `Server.cpp:538-558`). The backend-id scheme is
  shared across the engine branch, so Kyber's server recipe is a map that applies here, not
  just an analogy.
- `lanserverbackend` and `peerserverbackend` assets also ship in `win32/default_settings`.

## Exploit notes (Pseudo, 2026-10-19)
- **Zero-throttle pinballing.** Hitting boost while current speed is above the *throttle* speed jumps
  straight to boost max (240 on the X-wing) instead of ramping (AfterburnerEngineAccelerationRate 200).
  The intended case is current speed above *cruise max* (MaxSpeeds[power]). At throttle 0, every speed counts.
  A community member's earlier RE found it's a single value in code (Pseudo is asking her for notes).
- **Multidrifting.** With boost and drift on separate inputs, releasing and re-pressing drift during a drift
  changes the movement vector instantly. Hypothesis: the re-press re-runs drift *start*, skipping
  DriftTransitionTime (2.5 s; 1.5 s from boost) / DriftFullMomentumAngle 45. Drift registers after
  DriftInputHoldDelay 0.1 s.
- **Combined mode for comparison (Pseudo):** pressing boost/drift again while drifting starts a new boost,
  which ends the drift. So the "as intended" ruleset should make a separate-input re-press do one of:
  end the drift, or be ignored. The community picks; it's not an instant redirect either way.
- Verification: log speed and velocity vector per sim tick from the probe DLL. Both exploits show as
  one-tick discontinuities.

## Milestone, 2026-10-19 ~21:20: "as intended" rules work in flight
Pseudo, flying sws_rules.dll v2: "You got it." Zero-throttle boosts ramp, and multidrifting is impossible. The only visible
artifact: a small post-release display rise (~30 → ~50) at low speed. That's actual velocity closing the lag behind commanded speed
(measured, e.g. 49.9 → 62.3), the same as on any boost. Rules: probe/sws_probe.cpp built with /DRULES. Design and evidence in
notes/exploits-code-map.md; run instructions in probe/RUN.md.

Where the project stands:
- **Done:**
  - data reader (97,444 assets) and runtime type SDK (23,658 types);
  - the exploit mechanisms, mapped to code and confirmed in flight;
  - a hook/log toolchain with offline test hosts;
  - both rules, stateless and replay-consistent.
- **Mapped but not run:** the server spawn chain (createServer 0x1401bfa70, isDedicated = spawn-info +0x26, Server ctor
  0x1412e5e50), the backend factory (force Local), and the "Dedicated" timing mode. Still to find: MainLoop's dedicated flag,
  the spawnServer call site, and the client connect path.
- **Next (Test 3):** boot the game's own server side as a dedicated server, using the same hook infrastructure.
