# Balancing knobs: X-wing (T-65) sample

These values were read from the local install (STAR WARS Squadrons, Steam, build of 2021-10-21)
with `tools/swsdata.py`. Nothing in the install was modified. The full decoded assets are in `out/`:

- `out/<asset>.json` is the faithful object graph (`objects[]`, with `{"$ref": n}` for internal pointers and `{"$import": "fileGuid/classGuid"}` for pointers into other assets).
- `out/<asset>.inline.json` is the same data as one readable tree (`root`), with imports annotated by asset name.

To regenerate one: `E:\sws_community\.venv\Scripts\python.exe -I E:\sws_community\tools\swsdata.py dump <asset> [--inline]`

All five assets live in bundle `win32/game/gameplay/starships/starships_sharedbundle`
(catalog `initialexperience`, base `Data` CAS, not patched).

## How the values are stored

Most tunables appear twice in each config:

1. As a **typed field** on the config class (e.g. `StarshipMovementConfig.AfterburnerMaxSpeed`). For "Incom attribute" knobs this field is a pointer to an `IncomAttributeFloat` / `IncomAttributeVec3` object inside the same file. That object carries `Value` plus `AttributeId`, which imports a shared `attributeid_*` asset under `game/gameplay/starships/common/attribute/ids/...`.
2. In a **`*IncomAttributesToRegister` list** on the same root object: `{AttributeId (u32 hash), DebugName, Value}`.

In every sample checked, the two copies hold the same value. Which copy the runtime reads (or whether both must change together) is **not yet known**; a mod should change both until that is tested.

## 1. Flight model: `StarshipMovementConfig`

Asset: `game/gameplay/starships/xwing/config/starshipmovementconfig_xwing` (37 objects, 52 imports)

The per-level arrays have 9 entries. Power runs from 0 to 4 (see Power below), so these are most likely **engine power in 0.5 steps** (index 0 = 0 power, 4 = default 2.0, 8 = max 4.0). This is an inference and has not been verified in-game.

| Field | X-wing | (TIE/ln for comparison) |
|---|---|---|
| MaxSpeeds[0 / 4 / 8] | 120 / 132.5 / 145 | 110 / 130 / 150 |
| AfterburnerMaxSpeed (boost speed) | 240 | 250 |
| AfterburnerEngineAccelerationRate | 200 | |
| AfterburnerDecelerationFactor | 4.0 | |
| AfterburnerActivationCost | 0.05 | 0.05 |
| AfterburnerChargeRate | 0.05 | 0.05 |
| ActivatedAfterburnerDecayRate / LoseAfterburnerDecayRate | -0.1 / -0.05 | |
| AfterburnerRechargeCooldown | 1.0 | |
| AfterburnerAxisTurnRate (x,y,z) | 10, 5, 90 | |
| DriftActivationCost | 0.0 | 0.0 |
| DriftDecelerationFactor | 1.5 | 1.0 |
| DriftTransitionTime | 2.5 | 2.0 |
| DriftAxisTurnRate (x,y,z) | 112.5, 90, 180 | 120, 90, 187.5 |
| AfterburnerDriftBoostAccelerationForce / ...Time | 10000 / 3.0 | 10000 |
| AfterburnerDriftBoostDriftTransitionTime | 1.5 | |
| DriftFullMomentumAngle | 45 | |
| DriftInputDoubleClickDelay / DriftInputHoldDelay | 0.25 / 0.1 | |
| EngineAccelerationRates[0..8] | 120 to 160 (step 5) | |
| EngineDecelerationRates[0..8] | 55 to 105 (step 6.25) | |
| EngineAccelerationDriftForces[0..8] | 4000 to 8000 (step 500) | |
| EngineDecelerationDriftForce | 2000 | |
| EngineEnergyRegen[0..8] | 0.1 x5, then 0.12, 0.14, 0.16, 0.18 | |
| EngineDeadDecelerationRate | 10 | |
| AxisTurnRates[0] (pitch?, yaw?, roll?) | 62.5, 42.5, 95 (+1.5625 / +1.5625 / +3.125 per step) | 60, 40, 85 |
| TurnTorques | 2500, 2500, -31500 | |
| SteerRates / SteerRateRoll / SteerRateThrottle | (2,3,4) / (2,3,2) / (4,2,20) | |
| AxisRotationVelocityLimits | 200, 200, 200 | |
| ControlDeadAxisTurnRates | 20, 25, 50 | |
| InitialThrottleRatio | 0.625 | |
| TurningAccelerationDriftForce | (8000, 8000) | |

The curves (accel/decel by speed, turn rate by speed per power level, afterburner turn rate, throttle friction) are separate `FloatCurve`-style assets under `game/gameplay/starships/common/config/curve/...`. Their names are listed in the `imports` section of the dump.

## 2. Shields: `StarshipShieldData`

Asset: `game/gameplay/starships/xwing/config/starshipshielddata_xwing`

| Field | Value |
|---|---|
| InitialShieldHealth | 400 |
| MaxShieldHealth | 800 |
| SecondLayerShieldPercentThreshold | 0.5 |
| MaxShieldRegenRate / MinShieldRegenRate | 0.15 / 0.0 |
| MaxRegenLevelRegenDelay / MinRegenLevelRegenDelay | 2.0 / 6.0 s |
| SuperChargedShieldRegenLevel | 1.0 |
| LoseSuperChargeDecayRate | -0.006 |
| ShieldRegenByPowerLevel[0..8] (rate, delay) | (0, 0), (0.02, 4.5), (0.04, 4.125), (0.06, 3.75), (0.08, 3.375), (0.10, 3.0), (0.12, 2.625), (0.14, 2.25), (0.20, 1.5) |
| ShieldRegenRateFactor / ShieldRegenDelayFactor | 1.0 / 1.0 |
| UseBulletHitAngleThreshold | 45 |
| ShieldRegenRateFactorCurve | import `.../starshipdatacurve_regenfactorboost` |

## 3. Hull and damage: `StarshipHealthDamageData`

Asset: `game/gameplay/starships/xwing/config/starshiphealthdamagedata_xwing` (there is also an `_sp` variant for the campaign)

| Field | Value |
|---|---|
| MaxHullHealth | 1200 |
| CollisionMaxDamageStaticObjectAttribute | 1080 |
| CollisionMaxDamageDynamicObjectAttribute | 1080 |
| CollisionDamageFriendly / Hostile / Neutral TeamModifier | 0.75 / 1.0 / 1.0 |
| Shield/Hull DamageMultiplierFromLaser / FromNonLaser | 1.0 (all four) |

## 4. Laser cannon projectile: `IncomBulletConfig`

Asset: `game/gameplay/starships/xwing/config/tuning/bulletconfig_laser_xwing` (a `_overcharged` variant also exists)

| Field | Value |
|---|---|
| StartDamage | 60 |
| EndDamage | 30 |
| DamageFalloffStartDistance / EndDistance | 100 / 1000 |
| InitialSpeed | 2000 |
| TimeToLive | 0.5 s (so range is about 1000 m) |
| PercentOfRegularDamageFromIonDamage | 0 |
| ImpactImpulse | 0 |

In this asset the `DebugName` entries in `FloatIncomAttributesToRegister` are empty. The values match the typed fields in the order shown.

The weapon itself (rate of fire, energy pool) is a separate asset, `.../config/starshipweaponconfig_las_reb_xwing` (`StarshipWeaponConfig` / `LaserRegenWeaponFiringData`):

| Field | Value |
|---|---|
| TimeBetweenAutomaticShots | 0.1333 s (about 7.5 shots/s) |
| MaxLaserEnergyPool | 182 |
| LaserRegenPips[0..8] CostPerShot | 4.0 down to 2.0 |
| LaserRegenPips[0..8] BulletRegenRate | 0 up to 18 |
| LaserRechargeDelay | 0.25 |

A decoded copy is in `out/starshipweaponconfig_las_reb_xwing.inline.json`.

## 5. Power: `StarshipPowerManagement`

Asset: `game/gameplay/starships/xwing/config/starshippowermanagement_xwing`

| Field | Value |
|---|---|
| Engine / Weapon / Shield PowerData (min, max, default) | (0, 4, 2) each |
| SimpleManagementSystemPowerFloor | 1.0 |
| WeaponToEngineConversionFactor / EngineToWeaponConversionFactor | 1.0 / 1.0 |
| SupportEmergencyPowerTransfer | false |
| StickReleaseDelay | 0.2 |
| PowerTransferMenuEntryList | Rebel: StarshipShieldPriority_Front / _Back; Empire: EmergencyPowerTransfer_ToPrimaryWeapon / _ToEngine |

## Where to look next

The same set of types exists for every ship. Counts across the install: 45 StarshipMovementConfig, 37 StarshipShieldData, 56 StarshipHealthDamageData, 112 IncomBulletConfig, 28 StarshipPowerManagement and 347 StarshipWeaponConfig. Each ship has `_ai`, `_ai_bot`, `_ai_squadmate` and `_sp` variants.

Components are under each ship's `config/` folder:

- engine: `enginecustom_*`
- hull: `hullcustom_*`
- shield: `shieldcustom_*`

These are `IncomAttributeModifierFloat` lists that scale the attributes above.
