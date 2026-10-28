// sws_probe.dll: a read-only observation probe for STAR WARS: Squadrons (build 2020-12-18, exe sha256 5172a17f...).
//
// What it does: detours three movement functions, records their inputs/state every call into a ring buffer,
// and a writer thread streams it to E:\sws_community\probe_logs\probe_<time>.bin. It never changes game state:
// every detour calls the original with the same arguments and returns its result.
//
// Safety: before patching, each hook site's bytes are compared with the bytes from the analysed exe. On any
// mismatch nothing is patched and the reason is written to the status log. Other threads are suspended while
// the 14-byte jumps are written, and a thread whose instruction pointer is inside a patch range aborts the install.
//
// Markers: F8 / F9 / F10 write a MARK record (1/2/3) so a pilot can label moments ("multidrifting now").

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#include <ws2tcpip.h>
#include <timeapi.h>
#include <stdlib.h>
#include <mstcpip.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>
#include <malloc.h>
#include <math.h>
#include <share.h>
#include <intrin.h>
#include "game_build.h"

// ---------------------------------------------------------------- records
enum : uint32_t { R_TICK = 1, R_DRIFT_PRE = 2, R_DRIFT_POST = 3, R_DRIVE = 4, R_MARK = 5, R_INFO = 6, R_BOUNDS = 7, R_INTEG = 8, R_OUTPUT = 10 };   // 9 is R_RULE (the movement rules)

#pragma pack(push, 1)
struct Rec {
    uint32_t type;
    uint32_t tid;
    int64_t qpc;
    uint64_t state;
    volatile uint32_t seq;   // (index + 1) truncated; written last, so the writer never reads a half-filled record
    float f[27];
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 136, "record size");

static const uint32_t RING = 1u << 16;  // 65536 records (~8.9 MB)
static Rec* g_ring;
static volatile LONG64 g_head;  // next slot to claim
static volatile LONG64 g_done;  // records fully written (approx; writer tolerates lag)
static volatile LONG64 g_dropped;

// [server] record: 1 = write the flight record (*.bin, the hooks' records) and run the observation sampler. Off by
// default in the server build (Pseudo, 2026-10-22: playing, hosting or joining doesn't need it; it costs disk and CPU),
// set from the settings app's checkbox. The probe and rules builds always record. Off, claim() hands back a scratch
// record that goes nowhere: the hooks' code is unchanged, and nothing is queued or written.
#ifdef SERVER
static bool g_record = false;
#else
static bool g_record = true;
#endif
static __declspec(thread) Rec t_scratch;
static __declspec(thread) LONG64 t_idx;
static Rec* claim() {
    if (!g_record) return &t_scratch;
    t_idx = InterlockedIncrement64(&g_head) - 1;
    return &g_ring[t_idx & (RING - 1)];
}
static void commit() {
    if (!g_record) return;
    Rec* r = &g_ring[t_idx & (RING - 1)];
    MemoryBarrier();
    r->seq = (uint32_t)(t_idx + 1);
    InterlockedIncrement64(&g_done);
}

template <class T> static T rd(uint64_t p, uint32_t off) {
    T v{};
    __try { v = *(T*)(p + off); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
}
static float rdf(uint64_t p, uint32_t off) { return rd<float>(p, off); }
static float rdb(uint64_t p, uint32_t off) { return (float)rd<uint8_t>(p, off); }

// Fill the shared state snapshot (movement state object; offsets from notes/exploits-code-map.md).
static void snap(Rec* r, uint64_t s) {
    r->f[0] = rdf(s, 0x258);   // commanded forward speed
    r->f[1] = rdf(s, 0x260);   // its scale
    r->f[2] = rdb(s, 0x278);   // afterburner active
    r->f[3] = rdb(s, 0x279);   // above-cruise flag
    r->f[4] = rdf(s, 0x27C);   // afterburner energy
    r->f[5] = rdf(s, 0x280);
    r->f[6] = rdf(s, 0x284);   // drift-boost window timer
    r->f[7] = rdb(s, 0x289);   // drifting
    r->f[8] = rdb(s, 0x28A);
    r->f[9] = rdb(s, 0x28B);
    r->f[10] = rdf(s, 0x290);  // drift frame quaternion
    r->f[11] = rdf(s, 0x294);
    r->f[12] = rdf(s, 0x298);
    r->f[13] = rdf(s, 0x29C);
    r->f[14] = rdf(s, 0x2A0);
    r->f[15] = rdf(s, 0x1F8);  // power ratio (inferred)
    r->f[16] = rdf(s, 0x7C8);  // MaxSpeeds factor (inferred)
    uint64_t cfg = rd<uint64_t>(s, 0x28);
    r->f[17] = cfg ? rdf(cfg, 0x2EC) : -1.f;  // AfterburnerDriftBoostAccelerationForce
    r->f[18] = cfg ? rdf(cfg, 0x2F0) : -1.f;  // ...ForceTime
    // MaxSpeeds[last] for this ship (Float32 array at cfg+0x168; count at ptr-4)
    float top = -1.f;
    if (cfg) {
        uint64_t arr = rd<uint64_t>(cfg, 0x168);
        __try {
            if (arr) { uint32_t n = *(uint32_t*)(arr - 4); if (n && n < 64) top = *(float*)(arr + 4ull * (n - 1)); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    r->f[19] = top;
    *(uint64_t*)&r->f[20] = cfg;  // f[20..21]
}

static void stamp(Rec* r, uint32_t type, uint64_t state) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    r->type = type; r->tid = GetCurrentThreadId(); r->qpc = q.QuadPart; r->state = state;
}

// Non-boost top ("cruise") speed, replicating the non-afterburner branch of FUN_1416ab660, which reads the live
// factor at +0x7C8 (post-component). Returns -2 when the +0x630 override branch would apply (it needs a virtual
// call we don't replicate); the probe logs the raw fields so that case is visible.
static float cruise_replica(uint64_t s) {
    __try {
        if (*(uint8_t*)(s + 0x630)) return -2.f;
        uint64_t cfg = *(uint64_t*)(s + 0x28);
        uint64_t arr = *(uint64_t*)(cfg + 0x168);
        int n = *(int*)(arr - 4);
        float v = 0.f;
        if (n) {
            if (!*(uint8_t*)(s + 0x3E0) || !*(uint8_t*)(s + 0x6B9)) {
                unsigned idx = (unsigned)((float)(n - 1) * *(float*)(s + 0x1F8));
                if ((int)idx < n) v = *(float*)(arr + 4ull * idx);
            } else {
                v = *(float*)(arr + 4ull * (n - 1)) + *(float*)(cfg + 0x2D8);
            }
        }
        float k = *(float*)(s + 0x7C8);
        if (k <= 0.f) k = 0.f;
        return k * v;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1.f; }
}

static float flagbits(uint64_t s) {
    static const uint32_t offs[] = { 0x630, 0x638, 0x3E0, 0x6B9, 0x335, 0x2F1, 0x332, 0x254, 0x278, 0x279, 0x289 };
    uint32_t b = 0;
    for (int i = 0; i < (int)(sizeof offs / sizeof offs[0]); i++) if (rd<uint8_t>(s, offs[i])) b |= 1u << i;
    return (float)b;
}

#ifdef RULES
// ---------------------------------------------------------------- rules (sws_rules.dll only)
// Two optional "as intended" rules, each switchable in rules.ini beside the DLL:
//   no_zero_throttle_pinball: while boosting, a "slow down" throttle request is presented as "hold" (+0x04 = 0) when
//     commanded speed is at or below live cruise + 1.0 (cruise = the game's own non-boost top speed: current engine
//     power and the fitted engine's table). So the instant jump to boost speed happens only from above cruise, and
//     otherwise the boost ramps at the boost rate, as a boost from settled full throttle does.
//   no_multidrift: a drift may start only while the afterburner is on (one drift per boost, which is how the combined
//     boost/drift input already behaves).
// Both rules are stateless: they read only this tick's state and input, so client prediction replays agree with the
// server. (v1 kept a latch outside the game state and the client diverged; see rules_hold.)
enum : uint32_t { R_RULE = 9 };
static bool g_rulePinball, g_ruleMultidrift;
static bool g_ruleZavianCreep;   // fix_zavian_creep_pack (see map_fix_partition); in the fingerprint when on
// [creep] <faction>_pack<1-3> = <ship> (creep_scan): each creep pack's ship, an index into kCreepShips; 0 = the stock ship
static const char* kCreepShips[2][4] = { { "xwing", "ywing", "awing", "uwing" }, { "tiefighter", "tiebomber", "tieinterceptor", "tiereaper" } };
static const char* kCreepFaction[2] = { "rebel", "imperial" };
static int g_creepPack[2][3];
static bool g_creepOn;
// [creep] aggressive = 1 (creep_ai_partition): creep engage like the vs-AI bots, with target priorities by phase;
// missiles = 1 (creep_behaviour_scan): creep fire missiles and torpedoes. Each in the fingerprint when on.
static bool g_creepAggressive, g_creepMissiles;
// [creep] torpedoes = 1 (creep_loadout, creep_torpedo_runs): Y-wing and TIE bomber packs carry torpedoes instead of
// bombs, and fly torpedo runs. In the fingerprint when on, as "creep.torpedo_runs=1" since v0.5.0's torpedoes (carried
// as well as bombs, no runs) were a different thing.
static bool g_creepTorpedoes;
// [creep] wingmen = 1 (creep_behaviour_scan, creep_wingmen_raid): creep wingmen do what only a squadron's leader does in
// stock: attack on their own (the bot test, also opened by missiles) and fly the bombers' runs. In the fingerprint when on.
static bool g_creepWingmen;
// [creep] bot_handling = 1 (creep_loadout): every creep pack flies with the vs-AI bots' movement config for its ship. In the
// fingerprint when on.
static bool g_creepBotHandling;
// [creep] radius: how far aggressive creep look for targets, in metres (500..3000; 2000 is the bots' value). In the
// fingerprint only when aggressive is on and it isn't 2000, so profiles from before it keep theirs.
static int g_creepRadius = 2000;
static int g_creepCapDamage = 100;   // [creep] capital_damage: percent of their damage creep hits do to capital ships (aggressive creep)
static int g_creepTorpMinLock = 500;   // [creep] torpedo_lock_min: metres, the creep torpedoes' minimum lock distance (500 = stock)
// the per-faction phase switch (creep_ai_partition, h_ctrlfield): the creep squadron's and controller's data, the shared
// target list, and the score arrays for offense, face-off and defense; last = the last behaviour seen (-1 none)
// Bomb raids: a bomber's aux bombs load a raid behaviour (IncomSquadronAIBombingFlightBehaviour, from the bomb weapon
// config's BehaviourBlueprint) whose target gather (FUN_14182b910) reads the squadron DATA's TargetingRules.EngagementMode
// (+32) live: Aggressive (0) and no current target = raid the nearest bombable enemy within 10 km, a frigate in the
// opener; any other mode = raid only the current target, which the creep's scores make the corvette or Raider at most.
// So the mode follows the phase: Aggressive on offense, the stock mode (Defensive) otherwise. The squadron's own scorer
// copied the rules (Aggressive) when it registered and keeps them. stockMode/aggMode: the two values; wantMode: what
// the phase asks for; seenAt: when the controller first reported a behaviour (the squadron registers about then).
// shared: the bombers' list (and the squadron's own); sharedF and arrF: the other creep's, which score the enemy corvette
// or Raider lower on defense (h_scorer hands it to any creep ship that isn't bomber-class)
struct CreepPhase { uint64_t squad, ctrl, shared, sharedF; uint64_t arr[3], arrF[3]; volatile LONG last; int32_t stockMode, aggMode;
                    volatile LONG wantMode; volatile LONG64 seenAt; };
static CreepPhase g_creepPh[2];
static bool g_creepLog;   // [server] creep_log: log the creep squadrons' target-score lookups (diagnostic)
static bool g_trackLog;   // [server] track_log: record each bomber creep's position and its target's, twice a second (creep_track)
static bool g_bomberCapitalOnly;   // [server] bomber_capital_only: test switch, bomber creep target capital ships only
static bool g_torpCapitalLock;     // [server] torpedo_capital_lock: test switch, the creep torpedoes lock onto capital ships only (TargetTags 2)
static bool g_torpRuns = true;     // [server] torpedo_runs: test switch (default on), 0 = the creep torpedoes carry no run behaviour
// 0 offense, 1 face-off (and transition, overtime), 2 defense, for a faction (0 Rebel, 1 Imperial) in a FrontlineIndex
// phase: 0/1 the Rebel zones (Imperial offense), 3/4 the Imperial zones (Rebel offense)
static int creep_role(int fac, int p) {
    bool offense = fac ? p == 0 || p == 1 : p == 3 || p == 4, defense = fac ? p == 3 || p == 4 : p == 0 || p == 1;
    return offense ? 0 : defense ? 2 : 1;
}
static uint8_t* g_creepPool;   // the creep's own target lists (creep_alloc), made when aggressive is on

// Profiles (issue #2). server.ini [server] profile=<name> selects profiles\<name>.ini beside the DLL:
//   [rules]   no_zero_throttle_pinball, no_multidrift, fix_zavian_creep_pack (missing = off; the map fix is in the
//             fingerprint only when on, so profiles without it keep theirs)
//   [values]  <asset>.<field>=<number> overrides (applied by the runtime value engine, issue #3; the pseudo-configs
//             materialgrid.* and shipscale_<ship>.scale are applied by their own hooks)
// "Default" is built in: no rules, no overrides, never read from disk. With no profile set, rules.ini is read as before
// (missing = on), so older setups keep working. The fingerprint is FNV-1a over a canonical text of the rules and the
// sorted, normalized overrides; it is mixed into the network protocol version (h_tinit), so players on different profiles
// are refused at connect. Default rules with no overrides give fingerprint 0: the stock protocol.
static void status(const char* fmt, ...);
struct ValueOverride { char key[128]; double value; };
static ValueOverride* g_overrides; static int g_overrideCount;
static char g_profileName[64] = "(rules.ini)";
static uint32_t g_profileFp;
static uint32_t fnv1a(uint32_t h, const char* t) { for (; *t; t++) { h ^= (uint8_t)*t; h *= 16777619u; } return h; }
static uint32_t profile_fingerprint() {
    if (!g_rulePinball && !g_ruleMultidrift && !g_ruleZavianCreep && !g_overrideCount && !g_creepOn && !g_creepAggressive && !g_creepMissiles && !g_creepTorpedoes && !g_creepWingmen && !g_creepBotHandling) return 0;
    int* order = (int*)_alloca(sizeof(int) * (g_overrideCount + 1));
    for (int i = 0; i < g_overrideCount; i++) order[i] = i;
    for (int i = 1; i < g_overrideCount; i++)   // insertion sort by key (lists are small)
        for (int j = i; j > 0 && strcmp(g_overrides[order[j - 1]].key, g_overrides[order[j]].key) > 0; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    char b[192]; uint32_t h = 2166136261u;
    sprintf_s(b, "pinball=%d;multidrift=%d;", (int)g_rulePinball, (int)g_ruleMultidrift); h = fnv1a(h, b);
    if (g_ruleZavianCreep) h = fnv1a(h, "zaviancreep=1;");   // only when on, so profiles without it keep their fingerprint
    for (int f = 0; f < 2; f++)   // creep packs: only the ones changed from the stock ship
        for (int p = 0; p < 3; p++)
            if (g_creepPack[f][p]) { sprintf_s(b, "creep.%s_pack%d=%s;", kCreepFaction[f], p + 1, kCreepShips[f][g_creepPack[f][p]]); h = fnv1a(h, b); }
    if (g_creepAggressive) h = fnv1a(h, "creep.aggressive=1;");
    if (g_creepAggressive && g_creepRadius != 2000) { sprintf_s(b, "creep.radius=%d;", g_creepRadius); h = fnv1a(h, b); }
    if (g_creepAggressive && g_creepCapDamage != 100) { sprintf_s(b, "creep.capital_damage=%d;", g_creepCapDamage); h = fnv1a(h, b); }
    if (g_creepMissiles) h = fnv1a(h, "creep.missiles=1;");
    if (g_creepTorpedoes) h = fnv1a(h, "creep.torpedo_runs=1;");
    if (g_creepTorpedoes && g_creepTorpMinLock != 500) { sprintf_s(b, "creep.torpedo_lock_min=%d;", g_creepTorpMinLock); h = fnv1a(h, b); }
    if (g_creepWingmen) h = fnv1a(h, "creep.wingmen=1;");
    if (g_creepBotHandling) h = fnv1a(h, "creep.bothandling=1;");
    for (int i = 0; i < g_overrideCount; i++) { sprintf_s(b, "%s=%.9g;", g_overrides[order[i]].key, g_overrides[order[i]].value); h = fnv1a(h, b); }
    return h ? h : 1;
}
static bool profile_name_ok(const wchar_t* n) {
    if (!n[0] || wcsstr(n, L"..")) return false;
    for (const wchar_t* c = n; *c; c++)
        if (!(iswalnum(*c) || *c == L' ' || *c == L'_' || *c == L'-' || *c == L'.')) return false;
    return true;
}
// Limits (issue #11). limits.ini beside the DLL (tools/gen_limits.py, from defaults.ini; shipped in releases) gives
// every changeable field a range: <type>.<field path without indices> = min,max,flags (i = integer, n = -1 allowed).
// A profile value is clamped to it before anything else sees it, so the log, the fingerprint and every config write
// (the vs-AI bots' scaled values included) use the clamped value; two players asking 320 and 300 for a speed capped
// at 300 match. A value whose field has no limit is not applied, nor is one whose field is locked (flag x: power
// levels, and fields every config ships as 0). Weapons are limited as primaries or aux: limits.ini's [primary] section
// names the primary weapon configs. Without limits.ini nothing is clamped (logged).
struct ValueLimit { char key[160]; double lo, hi; bool isInt, neg1, locked; };
static ValueLimit* g_limits; static int g_limitCount = -1;   // -1: limits.ini not loaded
static char (*g_primaryWeapons)[96]; static int g_primaryCount;
static char (*g_capitalConfigs)[96]; static int g_capitalCount;   // [capital]: limited as <type>_capital
static const char* kLimitTypes[] = { "starshipmovementconfig", "starshipshielddata", "starshiphealthdamagedata",
    "starshippowermanagement", "starshipweaponconfig", "bulletconfig", "missileconfig", "explosionconfig",
    "enginecustom", "hullcustom", "shieldcustom",   // the components share one category: starshipcustomconfig
    "capitalshiphealthsettings", "damagesectionsettings", "capitalshipsubsystemsettings",
    "capitalshipweakpointssettings", "capitalshipweaponsubsystemsettings", "capitalshipenginewashdamageareasettings",
    "damageovertimesettings", "shieldhealthsettings", "capitalshipresupplysubsystemsettings",
    "turrethealthsettings", "turretaimingsettings", "turrettargetcomputersettings",
    "turretfirepatternsettingsasset",   // capital ships and turrets (tools/gen_limits.py CAPITAL)
    "materialgrid",   // laser and ion damage against capital ships (grid_apply)
    "shipscale",   // a starfighter's size (model_scale_partition)
    "incomschematicattributesconfig" };   // match rules: Fleet Battles' morale per kill, phase sizes, timers; Dogfight's
static void load_limits(const wchar_t* dir) {
    wchar_t li[MAX_PATH]; swprintf_s(li, L"%ls\\limits.ini", dir);
    if (GetFileAttributesW(li) == INVALID_FILE_ATTRIBUTES) { status("limits: %ls not found; values are NOT clamped", li); return; }
    const DWORD cap = 1 << 18; wchar_t* sec = (wchar_t*)calloc(cap, sizeof(wchar_t));
    DWORD n = sec ? GetPrivateProfileSectionW(L"limits", sec, cap, li) : 0;
    g_limits = (ValueLimit*)calloc(2048, sizeof(ValueLimit)); g_limitCount = 0;
    for (wchar_t* e = sec; g_limits && n && *e && g_limitCount < 2048; e += wcslen(e) + 1) {
        wchar_t* eq = wcschr(e, L'='); if (!eq || e[0] == L';') continue;
        ValueLimit& L = g_limits[g_limitCount]; int k = 0;
        for (wchar_t* c = e; c < eq && k < 159; c++) if (!iswspace(*c)) L.key[k++] = (char)towlower(*c);
        L.key[k] = 0;
        wchar_t flags[8] = L"";
        if (swscanf_s(eq + 1, L"%lf,%lf,%7[inx]", &L.lo, &L.hi, flags, (unsigned)_countof(flags)) < 2 || !k) continue;
        L.isInt = wcschr(flags, L'i') != nullptr; L.neg1 = wcschr(flags, L'n') != nullptr; L.locked = wcschr(flags, L'x') != nullptr;
        g_limitCount++;
    }
    auto read_names = [&](const wchar_t* section, char (*&out)[96], int& count) {   // the name is everything before '='
        n = sec ? GetPrivateProfileSectionW(section, sec, cap, li) : 0;
        out = (char(*)[96])calloc(1024, 96);
        for (wchar_t* e = sec; out && n && *e && count < 1024; e += wcslen(e) + 1) {
            if (e[0] == L';') continue;
            int k = 0; char* d = out[count];
            for (wchar_t* c = e; *c && *c != L'=' && k < 95; c++) if (!iswspace(*c)) d[k++] = (char)towlower(*c);
            d[k] = 0; if (k) count++;
        }
    };
    read_names(L"primary", g_primaryWeapons, g_primaryCount);
    read_names(L"capital", g_capitalConfigs, g_capitalCount);
    free(sec);
    status("limits: %d field limits, %d primary weapon configs, %d capital-ship configs from %ls", g_limitCount,
           g_primaryCount, g_capitalCount, li);
}
// the limit for asset.field: <type of the asset>.<field without [n]>
static const ValueLimit* find_limit(const char* asset, const char* field) {
    if (g_limitCount <= 0) return nullptr;
    const char* type = nullptr;
    for (const char* t : kLimitTypes) if (!strncmp(asset, t, strlen(t))) { type = t; break; }
    if (!type) return nullptr;
    if (!strcmp(type, "enginecustom") || !strcmp(type, "hullcustom") || !strcmp(type, "shieldcustom")) type = "starshipcustomconfig";
    char key[160]; int k = sprintf_s(key, "%s.", type);
    bool capital = false;
    for (int i = 0; i < g_capitalCount && !capital; i++) capital = !strcmp(g_capitalConfigs[i], asset);
    if (capital) k = sprintf_s(key, "%s_capital.", type);
    else if (!strcmp(type, "starshipweaponconfig")) {
        bool primary = false;
        for (int i = 0; i < g_primaryCount && !primary; i++) primary = !strcmp(g_primaryWeapons[i], asset);
        k = sprintf_s(key, "%s_%s.", type, primary ? "primary" : "aux");
    }
    for (const char* c = field; *c && k < 159; c++) {
        if (*c == '[') { while (*c && *c != ']') c++; if (!*c) break; continue; }
        key[k++] = *c;
    }
    key[k] = 0;
    for (int i = 0; i < g_limitCount; i++) if (!strcmp(g_limits[i].key, key)) return &g_limits[i];
    return nullptr;
}
static double clamp_to(const ValueLimit* L, double v) {
    if (!L) return v;
    if (L->neg1 && v == -1.0) return v;
    if (L->isInt) v = (double)llround(v);
    return v < L->lo ? L->lo : v > L->hi ? L->hi : v;
}
// clamp the profile's overrides (before the fingerprint); drop the ones whose field has no limit
static void clamp_overrides() {
    if (g_limitCount < 0) return;
    int kept = 0;
    for (int i = 0; i < g_overrideCount; i++) {
        ValueOverride& o = g_overrides[i];
        const char* dot = strchr(o.key, '.');
        char asset[96] = "";
        if (dot && dot - o.key < 96) strncpy_s(asset, o.key, dot - o.key);
        const ValueLimit* L = dot ? find_limit(asset, dot + 1) : nullptr;
        if (!L) { status("limits: %s has no limit (unknown field, or not in limits.ini); not applied", o.key); continue; }
        if (L->locked) { status("limits: %s is locked (power levels, and fields the game always ships as 0); not applied", o.key); continue; }
        double c = clamp_to(L, o.value);
        if (c != o.value) status("limits: %s = %.9g is outside %.9g..%.9g; clamped to %.9g", o.key, o.value, L->lo, L->hi, c);
        o.value = c;
        g_overrides[kept++] = o;
    }
    g_overrideCount = kept;
}
// Creep durability (step 1 of the creep rework): a pack set to a ship other than its stock X-wing or TIE fighter spawns
// with no loadout, so the ship flies its AI blueprint's defaults (lasers; bombs on the bombers, which creep behaviour
// allows) and takes its hull and shields from that ship's single-player AI configs (*_ai_sp: only the campaign uses
// them, not co-op's or vs-AI's bots). Those get the stock creep's share of the player ship's: Rebel hull 1/4 and shields
// 1/2 (creep X-wing 300/400 against the player's 1200/800), Imperial hull 0.32 (creep TIE fighter 500 against 1550) and
// shields 1/2. The *_ai_sp configs already carry the creep's collision caps (10000). Player values from the game data.
// These are only defaults: a profile value for the same key (the settings app's creep hull and shields, by ship type)
// is kept, and is part of the fingerprint like any other value.
struct CreepDurability { const char* ship; double hull, shields; };
static const CreepDurability kCreepDurability[] = {
    { "ywing", 300, 600 },   // player 1200 / 1200
    { "awing", 125, 250 },   // 500 / 500
    { "uwing", 250, 500 },   // 1000 / 1000
    { "tiebomber", 710, 0 },   // 2200
    { "tieinterceptor", 250, 0 },   // 775
    { "tiereaper", 323, 500 },   // 1000 / 1000
};
static void add_override(const char* key, double v) {
    if (!g_overrides || g_overrideCount >= 1024) return;
    for (int i = 0; i < g_overrideCount; i++) if (!strcmp(g_overrides[i].key, key)) { g_overrides[i].value = v; return; }
    ValueOverride& o = g_overrides[g_overrideCount++]; strcpy_s(o.key, key); o.value = v;
}
static void creep_durability() {
    if (!g_creepOn) return;
    for (const CreepDurability& d : kCreepDurability) {
        bool used = false;
        for (int f = 0; f < 2; f++) for (int p = 0; p < 3; p++) if (g_creepPack[f][p] && !strcmp(kCreepShips[f][g_creepPack[f][p]], d.ship)) used = true;
        if (!used) continue;
        char key[128]; double hull = d.hull, shields = d.shields; bool hullSet = false, shieldsSet = false;
        auto keep = [&](const char* k, double& v) {   // the profile's own value wins over the creep default
            for (int i = 0; i < g_overrideCount; i++) if (!strcmp(g_overrides[i].key, k)) { v = g_overrides[i].value; return true; }
            add_override(k, v); return false;
        };
        sprintf_s(key, "starshiphealthdamagedata_%s_ai_sp.maxhullhealth", d.ship); hullSet = keep(key, hull);
        if (d.shields > 0) { sprintf_s(key, "starshipshielddata_%s_ai_sp.maxshieldhealth", d.ship); shieldsSet = keep(key, shields); }
        if (d.shields > 0) status("creep: %s creep: hull %g (%s), shields %g (%s)", d.ship, hull, hullSet ? "the profile's" : "the creep default",
                                  shields, shieldsSet ? "the profile's" : "the creep default");
        else status("creep: %s creep: hull %g (%s)", d.ship, hull, hullSet ? "the profile's" : "the creep default");
    }
}
static wchar_t g_dllDir[MAX_PATH];
static bool g_shipScaleOn;
static void load_profile(const wchar_t* dir) {
    wcscpy_s(g_dllDir, dir);
    wchar_t sini[MAX_PATH]; swprintf_s(sini, L"%ls\\server.ini", dir);
    wchar_t name[64] = L""; GetPrivateProfileStringW(L"server", L"profile", L"", name, 64, sini);
    if (!name[0]) {
        wchar_t rini[MAX_PATH]; swprintf_s(rini, L"%ls\\rules.ini", dir);
        g_rulePinball = GetPrivateProfileIntW(L"rules", L"no_zero_throttle_pinball", 1, rini) != 0;
        g_ruleMultidrift = GetPrivateProfileIntW(L"rules", L"no_multidrift", 1, rini) != 0;
    } else if (!_wcsicmp(name, L"Default") || !profile_name_ok(name)) {
        if (_wcsicmp(name, L"Default")) status("profile: name '%ls' is not allowed (letters, digits, space, _ - .); using Default", name);
        strcpy_s(g_profileName, "Default");
        g_rulePinball = g_ruleMultidrift = false;
    } else {
        WideCharToMultiByte(CP_UTF8, 0, name, -1, g_profileName, sizeof g_profileName, nullptr, nullptr);
        wchar_t pf[MAX_PATH]; swprintf_s(pf, L"%ls\\profiles\\%ls.ini", dir, name);
        if (GetFileAttributesW(pf) == INVALID_FILE_ATTRIBUTES) {
            status("profile: %ls not found; using Default", pf);
            strcpy_s(g_profileName, "Default");
            g_rulePinball = g_ruleMultidrift = false;
        } else {
            g_rulePinball = GetPrivateProfileIntW(L"rules", L"no_zero_throttle_pinball", 0, pf) != 0;
            g_ruleMultidrift = GetPrivateProfileIntW(L"rules", L"no_multidrift", 0, pf) != 0;
            g_ruleZavianCreep = GetPrivateProfileIntW(L"rules", L"fix_zavian_creep_pack", 0, pf) != 0;
            for (int f = 0; f < 2; f++)
                for (int p = 0; p < 3; p++) {
                    wchar_t key[32], v[32] = L""; swprintf_s(key, L"%hs_pack%d", kCreepFaction[f], p + 1);
                    GetPrivateProfileStringW(L"creep", key, L"", v, 32, pf);
                    char a[32]; int k = 0; for (wchar_t* c = v; *c && k < 31; c++) if (!iswspace(*c) && *c != L'-' && *c != L'_') a[k++] = (char)towlower(*c);
                    a[k] = 0; if (!k) continue;
                    int found = -1; for (int t = 0; t < 4; t++) if (!strcmp(a, kCreepShips[f][t])) found = t;
                    if (found < 0) { status("profile: [creep] %ls = '%ls' isn't one of the %s ships; the stock ship stays", key, v, kCreepFaction[f]); continue; }
                    g_creepPack[f][p] = found; if (found) g_creepOn = true;
                }
            g_creepAggressive = GetPrivateProfileIntW(L"creep", L"aggressive", 0, pf) != 0;
            g_creepMissiles = GetPrivateProfileIntW(L"creep", L"missiles", 0, pf) != 0;
            g_creepTorpedoes = GetPrivateProfileIntW(L"creep", L"torpedoes", 0, pf) != 0;
            g_creepWingmen = GetPrivateProfileIntW(L"creep", L"wingmen", 0, pf) != 0;
            g_creepBotHandling = GetPrivateProfileIntW(L"creep", L"bot_handling", 0, pf) != 0;
            g_creepRadius = (int)GetPrivateProfileIntW(L"creep", L"radius", 2000, pf);
            if (g_creepRadius < 500 || g_creepRadius > 3000) {
                int was = g_creepRadius; g_creepRadius = g_creepRadius < 500 ? 500 : 3000;
                status("profile: [creep] radius = %d is outside 500..3000; clamped to %d", was, g_creepRadius);
            }
            g_creepCapDamage = (int)GetPrivateProfileIntW(L"creep", L"capital_damage", 100, pf);
            if (g_creepCapDamage < 0 || g_creepCapDamage > 100) {
                int was = g_creepCapDamage; g_creepCapDamage = g_creepCapDamage < 0 ? 0 : 100;
                status("profile: [creep] capital_damage = %d is outside 0..100; clamped to %d", was, g_creepCapDamage);
            }
            g_creepTorpMinLock = (int)GetPrivateProfileIntW(L"creep", L"torpedo_lock_min", 500, pf);
            if (g_creepTorpMinLock < 0 || g_creepTorpMinLock > 1000) {
                int was = g_creepTorpMinLock; g_creepTorpMinLock = g_creepTorpMinLock < 0 ? 0 : 1000;
                status("profile: [creep] torpedo_lock_min = %d is outside 0..1000; clamped to %d", was, g_creepTorpMinLock);
            }
            if ((g_creepAggressive || g_creepTorpedoes || g_creepBotHandling) && !g_creepPool) g_creepPool = (uint8_t*)VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            const DWORD cap = 65536; wchar_t* sec = (wchar_t*)calloc(cap, sizeof(wchar_t));
            DWORD n = sec ? GetPrivateProfileSectionW(L"values", sec, cap, pf) : 0;
            g_overrides = (ValueOverride*)calloc(1024, sizeof(ValueOverride));
            for (wchar_t* e = sec; g_overrides && n && *e && g_overrideCount < 1024; e += wcslen(e) + 1) {
                wchar_t* eq = wcschr(e, L'=');
                if (!eq || e[0] == L';' || e[0] == L'#') continue;
                ValueOverride& o = g_overrides[g_overrideCount];
                int k = 0;
                for (wchar_t* c = e; c < eq && k < 127; c++) if (!iswspace(*c)) o.key[k++] = (char)towlower(*c);
                o.key[k] = 0;
                wchar_t* end = nullptr; o.value = wcstod(eq + 1, &end);
                while (end && iswspace(*end)) end++;
                if (!k || !end || end == eq + 1 || *end) { status("profile: ignoring '%ls' (expected asset.field=number)", e); continue; }
                bool dup = false;
                for (int i = 0; i < g_overrideCount; i++) if (!strcmp(g_overrides[i].key, o.key)) { g_overrides[i].value = o.value; dup = true; }
                if (!dup) g_overrideCount++;
            }
            free(sec);
        }
    }
    load_limits(dir);
    clamp_overrides();
    for (int i = 0; i < g_overrideCount; i++) if (!strncmp(g_overrides[i].key, "shipscale_", 10)) g_shipScaleOn = true;
    g_profileFp = profile_fingerprint();
    creep_durability();   // derived from [creep], so after the fingerprint (which has [creep] itself)
    status("profile: '%s': no_zero_throttle_pinball=%d no_multidrift=%d fix_zavian_creep_pack=%d, %d value override(s), fingerprint %08x%s",
           g_profileName, (int)g_rulePinball, (int)g_ruleMultidrift, (int)g_ruleZavianCreep, g_overrideCount, g_profileFp,
           g_profileFp ? "" : " (stock protocol)");
    for (int f = 0; f < 2; f++)
        for (int p = 0; p < 3; p++)
            if (g_creepPack[f][p]) status("  creep: %s pack %d flies %s", kCreepFaction[f], p + 1, kCreepShips[f][g_creepPack[f][p]]);
    if (g_creepAggressive) status("  creep: aggressive within %d m, with targets by phase; their hits do %d%% damage to capital ships", g_creepRadius, g_creepCapDamage);
    if (g_creepMissiles) status("  creep: fire missiles and torpedoes");
    if (g_creepTorpedoes) status("  creep: Y-wing and TIE bomber packs carry torpedoes instead of bombs, and fly torpedo runs; they lock from %d m%s",
                                 g_creepTorpMinLock, g_creepTorpMinLock == 500 ? " (stock)" : "");
    if (g_creepWingmen) status("  creep: wingmen attack and fly runs like their leader");
    if (g_creepBotHandling) status("  creep: every pack flies with the vs-AI bots' handling");
    for (int i = 0; i < g_overrideCount; i++) status("  value %s = %.9g (%s)", g_overrides[i].key, g_overrides[i].value,
        !strncmp(g_overrides[i].key, "shipscale_", 10) ? "the ship's size, applied as its blueprint loads, issue #20"
        : !strncmp(g_overrides[i].key, "materialgrid.", 13) ? "laser and ion damage against capital ships, applied at each hit"
        : "applied by the value engine, issue #3");
}
static const float kCruiseMargin = 1.0f;

struct ShipRules { volatile uint64_t key; uint8_t prevAb, allowInstant, boostedSinceDrift, blockLogged; };
static ShipRules g_ships[256];

static ShipRules* ship(uint64_t s) {
    uint32_t h = (uint32_t)((s >> 4) * 2654435761u) & 255;
    for (int i = 0; i < 256; i++) {
        ShipRules* e = &g_ships[(h + i) & 255];
        if (e->key == s) return e;
        if (e->key == 0 && InterlockedCompareExchange64((volatile LONG64*)&e->key, (LONG64)s, 0) == 0) return e;
        if (e->key == s) return e;
    }
    return nullptr;  // table full: rules don't apply to this state (logged as unchanged behaviour)
}

static void rule_rec(uint64_t s, float rule, float a, float b, float c) {
    Rec* r = claim(); stamp(r, R_RULE, s); r->f[0] = rule; r->f[1] = a; r->f[2] = b; r->f[3] = c; commit();
}

// Diagnostics only (no effect on the rules): logs afterburner engagements as seen by each state.
static void rules_edge(uint64_t s, ShipRules* e) {
    uint8_t ab = rd<uint8_t>(s, 0x278);
    if (ab && !e->prevAb) {
        float cmd = rdf(s, 0x258), cru = cruise_replica(s);
        rule_rec(s, 1.f, cmd, cru, (float)(cmd > cru + kCruiseMargin));
    }
    e->prevAb = ab;
}

// True when this tick's throttle request should be presented as "hold" (0) instead of "slow down" (-1).
// STATELESS on purpose (v2, 2026-10-19): the decision depends only on this tick's state and input, so the client's
// prediction replays reach the same answer as the server. The v1 rule latched a decision at boost engagement in
// memory outside the game state; replays re-triggered the latch and the client diverged from the server (measured:
// 68 engagement decisions for ~17 boosts, and 28 visible speed swings vs 0 without the rules).
// This is equivalent to the latch in practice: once a ramped boost is under way the game's own request is "speed up"
// (+1) toward boost speed, so a "slow down" during a boost only occurs on the engaging tick.
static bool rules_hold(uint64_t s, ShipRules* e, uint8_t* input) {
    (void)e;
    if (!g_rulePinball || !input) return false;
    if (!rd<uint8_t>(s, 0x278) || *(float*)(input + 4) >= 0.f) return false;
    float cru = cruise_replica(s);
    if (cru < 0.f) return false;  // unknown cruise: leave the game alone
    return rdf(s, 0x258) <= cru + kCruiseMargin;
}
#endif

// ---------------------------------------------------------------- hooks
typedef void(__fastcall* bounds_t)(uint64_t state, uint8_t* input, float* mn, float* mx, float dt);
typedef void(__fastcall* integ_t)(uint64_t state, uint8_t* input, float mn, float mx, float dt);
static bounds_t o_bounds;
static integ_t o_integ;

static void __fastcall h_bounds(uint64_t state, uint8_t* input, float* mn, float* mx, float dt) {
#ifdef RULES
    ShipRules* e = ship(state);
    if (e) rules_edge(state, e);
    if (rules_hold(state, e, input)) {
        float saved = *(float*)(input + 4);
        *(float*)(input + 4) = 0.f;
        o_bounds(state, input, mn, mx, dt);
        *(float*)(input + 4) = saved;
    } else
#endif
    o_bounds(state, input, mn, mx, dt);
    Rec* r = claim(); stamp(r, R_BOUNDS, state);
    __try {
        r->f[0] = mn ? *mn : 0.f; r->f[1] = mx ? *mx : 0.f;
        for (int i = 0; i < 10; i++) r->f[2 + i] = input ? ((float*)input)[i] : 0.f;  // input +0x00..+0x24
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    r->f[12] = rdf(state, 0x258); r->f[13] = rdf(state, 0x25C); r->f[14] = dt;
    r->f[15] = rdf(state, 0x6D0);           // min speed
    r->f[16] = cruise_replica(state);       // our cruise, for checking against the game's
    r->f[17] = rdf(state, 0x780);           // afterburner top (cached attribute)
    r->f[18] = rdf(state, 0x63C); r->f[19] = rdf(state, 0x634);
    r->f[20] = flagbits(state);
    r->f[21] = rdf(state, 0x1F8); r->f[22] = rdf(state, 0x7C8); r->f[23] = rdf(state, 0x270);
    commit();
}

static void __fastcall h_integ(uint64_t state, uint8_t* input, float mn, float mx, float dt) {
    float before = rdf(state, 0x258);
#ifdef RULES
    ShipRules* e = ship(state);
    if (rules_hold(state, e, input)) {
        float saved = *(float*)(input + 4);
        *(float*)(input + 4) = 0.f;
        o_integ(state, input, mn, mx, dt);
        *(float*)(input + 4) = saved;
    } else
#endif
    o_integ(state, input, mn, mx, dt);
    Rec* r = claim(); stamp(r, R_INTEG, state);
    r->f[0] = before; r->f[1] = rdf(state, 0x258); r->f[2] = mn; r->f[3] = mx; r->f[4] = dt;
    __try { for (int i = 0; i < 10; i++) r->f[5 + i] = input ? ((float*)input)[i] : 0.f; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    r->f[15] = rdb(state, 0x278); r->f[16] = flagbits(state);
    commit();
}

typedef void(__fastcall* tick_t)(uint64_t state, float dt);
typedef void(__fastcall* drift_t)(uint64_t state, char pressed);
typedef float*(__fastcall* drive_t)(uint64_t state, float* out, float* p3, float* quat, float* target, float* vel, float dt);

static tick_t o_tick;
static drift_t o_drift;
static drive_t o_drive;

static void __fastcall h_tick(uint64_t state, float dt) {
    Rec* r = claim(); stamp(r, R_TICK, state); snap(r, state); r->f[22] = dt; commit();
    o_tick(state, dt);
}

static void __fastcall h_drift(uint64_t state, char pressed) {
    Rec* r = claim(); stamp(r, R_DRIFT_PRE, state); snap(r, state); r->f[22] = (float)(uint8_t)pressed; commit();
#ifdef RULES
    // STATELESS (v2): a drift may start only while the afterburner is on ("the boost state that allows an initial
    // drift", Pseudo's wording). The game switches the afterburner off when a drift starts, so a re-press needs a new
    // boost. Depends only on this tick's state, so prediction replays agree with the server.
    ShipRules* e = g_ruleMultidrift ? ship(state) : nullptr;  // used for diagnostics logging only
    uint8_t wasDrifting = rd<uint8_t>(state, 0x289);
    char give = pressed;
    if (g_ruleMultidrift && pressed && !wasDrifting && !rd<uint8_t>(state, 0x278)) {
        give = 0;  // not boosting: this press doesn't start a drift
        if (e && !e->blockLogged) { rule_rec(state, 2.f, rdf(state, 0x258), 0.f, 0.f); e->blockLogged = 1; }  // once per held press
    }
    o_drift(state, give);
    if (e && !pressed) e->blockLogged = 0;
#else
    o_drift(state, pressed);
#endif
    r = claim(); stamp(r, R_DRIFT_POST, state); snap(r, state); r->f[22] = (float)(uint8_t)pressed; commit();
}

static float* __fastcall h_drive(uint64_t state, float* out, float* p3, float* quat, float* target, float* vel, float dt) {
    float* res = o_drive(state, out, p3, quat, target, vel, dt);
    Rec* r = claim(); stamp(r, R_DRIVE, state);
    __try {
        for (int i = 0; i < 4; i++) r->f[i] = quat ? quat[i] : 0.f;
        for (int i = 0; i < 3; i++) r->f[4 + i] = target ? target[i] : 0.f;
        for (int i = 0; i < 3; i++) r->f[7 + i] = vel ? vel[i] : 0.f;
        for (int i = 0; i < 4; i++) r->f[10 + i] = res ? res[i] : 0.f;
        for (int i = 0; i < 4; i++) r->f[14 + i] = p3 ? p3[i] : 0.f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    r->f[18] = dt;
    r->f[19] = rdf(state, 0x670); r->f[20] = rdf(state, 0x674); r->f[21] = rdf(state, 0x678);  // chosen gains
    r->f[22] = rdf(state, 0x284); r->f[23] = rdb(state, 0x289);
    commit();
    return res;
}

static void status(const char* fmt, ...);

#ifdef SERVER
// ---------------------------------------------------------------- dedicated server (sws_server.dll only)
// Stage 3a: make the engine believe it is a dedicated server from startup, force the Local online backend (the Blaze and
// LAN server backends are cooked out of this build), and log what the engine then does. server.ini beside the DLL:
//   [server] dedicated=1 force_local_backend=1
static bool g_dedicated = true, g_forceLocal = true, g_listen = false, g_sockLog = false;
static bool g_udpTransport;
static int g_udpMaxPacket = 1200;       // [server] udp_max_packet: the packet size the engine is told (fits Tailscale's 1280 MTU)
static char g_joinAddr[96];   // [server] join = host address (join mode); empty = host/normal
static bool g_joinLocalServer = true;   // [server] join_local_server: 0 = a joiner does not spawn its own match server
static bool g_joinRemote = false;       // [server] join_remote: 1 = a joiner's match connection is a remote client, not a local host
static bool g_bundleLog = true;         // [server] bundle_log: log bundle queue requests (diagnostic)
static char g_matchMode[16];
static bool g_observe = true;           // [server] observe: 0 = no output sampling (issue #11)
static bool g_stackSample = false;      // [server] stack_sample: 1 = the stack sampler runs (dedicated-mode debugging)
static bool g_localLoadouts = true;     // [server] local_loadouts: 1 = keep loadouts in loadouts.txt and never send them to EA
static wchar_t g_ldPath[MAX_PATH];
static void ld_load();
static bool g_loadoutLog = false;       // [server] loadout_log: 1 = log the loadouts the game loads from EA and saves (2026-10-26 probe)
static bool g_spawnLog = false;         // [server] spawn_log: 1 = log every starship the server spawns, and whether it worked
static bool g_materialLog = false;      // [server] material_log: 1 = log the material pairs the grid is asked about (diagnostic)
static bool g_hitLog = false;   // [server] hit_log: 1 = log every direct hit on a starfighter in the ship's own frame (#20)
static bool g_capDmgLog = false;   // [server] capital_damage_log: 1 = log what reaches capital ships' hulls and shields, and from whom
static bool g_valuesOn = true;           // [server] values: 0 = the runtime value engine (issue #3) does nothing            // [server] match_mode (host): "" = as started | solo | coop | fleet | dogfight
static bool g_allUnlocked = false;       // [server] all_unlocked: add every id in unlock_ids.txt to the inventory
static bool g_suppressBackendLost = true; // [server] suppress_backend_lost: in listen/join mode, drop the online "backend lost" event
static uint64_t* g_unlockIds; static int g_unlockCount;
static char g_udpBind[64] = "127.0.0.1";
// [spawn] section: what the dedicated server loads (defaults = the game's own custom-match setup, observed 2026-10-19)
static char g_rootLevel[256] = "Game/Levels/RootLevel/RootLevel";
static char g_lobbyLevel[256] = "LiveService/Levels/PvP/CustomGames/CustomMatchWaitingRoom/CustomMatchWaitingRoom";
static char g_mapLevel[256] = "Game/Levels/PvP/PVP_Yavin/PVP_Yavin";
static char g_options[512] = "GameMode=Frontline_Bots;SquadronComposition=Solo";
static int g_spawnDelayMs = 3000;
static int g_lsInt10 = 0, g_lsInt14 = 0;
static bool g_spawnDedicated = true, g_skipNullJob = true, g_skipJobAlways = false, g_loadStartupData = true, g_loadGameConfig = true, g_postLoadLevel = true, g_serverThreading = false;

static void read_server_ini() {
    wchar_t ini[MAX_PATH]; HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&read_server_ini, &self);
    GetModuleFileNameW(self, ini, MAX_PATH);
    wchar_t* sl = wcsrchr(ini, L'\\'); if (sl) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - ini), L"server.ini");
    g_dedicated = GetPrivateProfileIntW(L"server", L"dedicated", 1, ini) != 0;
    g_forceLocal = GetPrivateProfileIntW(L"server", L"force_local_backend", 1, ini) != 0;
    // listen=1 (with dedicated=0): the player's own game hosts. A match spawn (not the menu) is made networked by clearing
    // isSinglePlayer (+0x24); isLocalHost (+0x25) stays set, so the backend chooser picks Local by itself (2026-10-20)
    g_listen = GetPrivateProfileIntW(L"server", L"listen", 0, ini) != 0;
    // socket_log=1: logging pass-through over the engine's socket manager and socket vtables (notes/socket-interface.md)
    g_sockLog = GetPrivateProfileIntW(L"server", L"socket_log", 0, ini) != 0;
    // transport=udp: the engine's sockets carry their traffic over real UDP, bound to udp_bind (default 127.0.0.1)
    { wchar_t t[32]; GetPrivateProfileStringW(L"server", L"transport", L"memory", t, 32, ini); g_udpTransport = _wcsicmp(t, L"udp") == 0;
      wchar_t ub[64]; GetPrivateProfileStringW(L"server", L"udp_bind", L"127.0.0.1", ub, 64, ini); WideCharToMultiByte(CP_UTF8, 0, ub, -1, g_udpBind, 64, nullptr, nullptr);
      if (g_udpTransport) g_sockLog = true;
      g_udpMaxPacket = GetPrivateProfileIntW(L"server", L"udp_max_packet", 1200, ini);
      if (g_udpMaxPacket < 512 || g_udpMaxPacket > 1600) g_udpMaxPacket = 1200;
      wchar_t jw[96]; GetPrivateProfileStringW(L"server", L"join", L"", jw, 96, ini); WideCharToMultiByte(CP_UTF8, 0, jw, -1, g_joinAddr, 96, nullptr, nullptr);
      if (g_joinAddr[0] && !strchr(g_joinAddr, ':')) strcat_s(g_joinAddr, ":25200");
      g_joinLocalServer = GetPrivateProfileIntW(L"server", L"join_local_server", 1, ini) != 0;
      g_joinRemote = GetPrivateProfileIntW(L"server", L"join_remote", 0, ini) != 0;
      g_bundleLog = GetPrivateProfileIntW(L"server", L"bundle_log", 1, ini) != 0;
      g_valuesOn = GetPrivateProfileIntW(L"server", L"values", 1, ini) != 0;
      g_record = GetPrivateProfileIntW(L"server", L"record", 0, ini) != 0;
      g_observe = g_record && GetPrivateProfileIntW(L"server", L"observe", 1, ini) != 0;   // observation writes records
      g_materialLog = GetPrivateProfileIntW(L"server", L"material_log", 0, ini) != 0;
      g_creepLog = GetPrivateProfileIntW(L"server", L"creep_log", 0, ini) != 0;
      g_spawnLog = GetPrivateProfileIntW(L"server", L"spawn_log", 0, ini) != 0;
      g_trackLog = GetPrivateProfileIntW(L"server", L"track_log", 0, ini) != 0;
      g_bomberCapitalOnly = GetPrivateProfileIntW(L"server", L"bomber_capital_only", 0, ini) != 0;
      if (g_bomberCapitalOnly) status("server: bomber_capital_only=1 (test): bomber creep target capital ships only, and their torpedoes lock as stock");
      g_torpCapitalLock = GetPrivateProfileIntW(L"server", L"torpedo_capital_lock", 0, ini) != 0;
      status("server: torpedo_capital_lock=%d (test: 1 = creep torpedoes lock onto capital ships only)", (int)g_torpCapitalLock);
      g_torpRuns = GetPrivateProfileIntW(L"server", L"torpedo_runs", 1, ini) != 0;
      if (!g_torpRuns) status("server: torpedo_runs=0 (test): the creep torpedoes carry no run behaviour");
      g_loadoutLog = GetPrivateProfileIntW(L"server", L"loadout_log", 0, ini) != 0;
      g_localLoadouts = GetPrivateProfileIntW(L"server", L"local_loadouts", 1, ini) != 0;
      { wcscpy_s(g_ldPath, ini); wchar_t* s3 = wcsrchr(g_ldPath, L'\\'); if (s3) wcscpy_s(s3 + 1, MAX_PATH - (s3 + 1 - g_ldPath), L"loadouts.txt");
        status("server: local_loadouts=%d", (int)g_localLoadouts);
        if (g_localLoadouts) ld_load(); }
      g_hitLog = GetPrivateProfileIntW(L"server", L"hit_log", 0, ini) != 0;
      g_capDmgLog = GetPrivateProfileIntW(L"server", L"capital_damage_log", 0, ini) != 0;
      if (g_capDmgLog) status("server: capital_damage_log=1: what reaches capital ships' hulls and shields is logged, by attacker");
      g_stackSample = GetPrivateProfileIntW(L"server", L"stack_sample", 0, ini) != 0;
      { wchar_t mw[16]; GetPrivateProfileStringW(L"server", L"match_mode", L"", mw, 16, ini);
        WideCharToMultiByte(CP_UTF8, 0, mw, -1, g_matchMode, sizeof g_matchMode, nullptr, nullptr); }
      g_allUnlocked = GetPrivateProfileIntW(L"server", L"all_unlocked", 0, ini) != 0;
      g_suppressBackendLost = GetPrivateProfileIntW(L"server", L"suppress_backend_lost", 1, ini) != 0;
      if (g_allUnlocked) {   // unlock_ids.txt beside this DLL: one decimal id per line, the rest of the line is a comment
          wchar_t idf[MAX_PATH]; wcscpy_s(idf, ini); wchar_t* sl2 = wcsrchr(idf, L'\\');
          if (sl2) wcscpy_s(sl2 + 1, MAX_PATH - (sl2 + 1 - idf), L"unlock_ids.txt");
          FILE* f = nullptr;
          if (_wfopen_s(&f, idf, L"r") == 0 && f) {
              g_unlockIds = (uint64_t*)calloc(8192, sizeof(uint64_t));
              char line[512];
              while (g_unlockIds && g_unlockCount < 8192 && fgets(line, sizeof line, f)) {
                  if (line[0] < '0' || line[0] > '9') continue;
                  uint64_t v = _strtoui64(line, nullptr, 10); if (v) g_unlockIds[g_unlockCount++] = v;
              }
              fclose(f);
          }
          status("server: all_unlocked=1, %d ids from unlock_ids.txt", g_unlockCount);
      }
      status("server: suppress_backend_lost=%d", (int)g_suppressBackendLost);
      status("server: join=%s join_local_server=%d join_remote=%d bundle_log=%d", g_joinAddr[0] ? g_joinAddr : "(none: host/normal)",
             (int)g_joinLocalServer, (int)g_joinRemote, (int)g_bundleLog);
      status("server: match_mode=%s", g_matchMode[0] ? g_matchMode : "(as started)");
      status("server: socket_log=%d transport=%s udp_bind=%s udp_max_packet=%d", g_sockLog, g_udpTransport ? "udp" : "memory", g_udpBind, g_udpMaxPacket); }
    status("server: dedicated=%d force_local_backend=%d listen=%d", g_dedicated, g_forceLocal, g_listen);
    // 3b spawn settings (narrow strings: read wide, convert)
    wchar_t w[512];
    auto rs = [&](const wchar_t* key, char* out, size_t n) {
        if (GetPrivateProfileStringW(L"spawn", key, L"\x01", w, 512, ini) && w[0] != 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, nullptr, nullptr);
    };
    rs(L"root_level", g_rootLevel, sizeof g_rootLevel);
    rs(L"lobby_level", g_lobbyLevel, sizeof g_lobbyLevel);
    rs(L"map_level", g_mapLevel, sizeof g_mapLevel);
    rs(L"options", g_options, sizeof g_options);
    g_spawnDelayMs = GetPrivateProfileIntW(L"spawn", L"delay_ms", 3000, ini);
    g_lsInt10 = GetPrivateProfileIntW(L"spawn", L"levelsetup_int10", 0, ini);
    g_lsInt14 = GetPrivateProfileIntW(L"spawn", L"levelsetup_int14", 0, ini);
    g_spawnDedicated = GetPrivateProfileIntW(L"spawn", L"spawn_dedicated", 1, ini) != 0;
    g_skipNullJob = GetPrivateProfileIntW(L"spawn", L"skip_null_server_job", 1, ini) != 0;
    // 1 = always skip Server::init's dedicated job queue+wait (a load of bundle game/incomgameconfiguration, which the
    // client already loaded at startup); its data decode hit the engine's corrupt-block check on 2026-10-20
    g_skipJobAlways = GetPrivateProfileIntW(L"spawn", L"skip_server_job", 0, ini) != 0;
    // 1 = load bundle Systems/FrostbiteStartupData before spawning, as the client's constructor does (0x140a2c0c2);
    // only the client loads it, and a dedicated process has no client (2026-10-20)
    g_loadStartupData = GetPrivateProfileIntW(L"spawn", L"load_startup_data", 1, ini) != 0;
    // 1 = also load the client's second bundle (DAT_1441e2938: game/incomgameconfiguration) before spawning
    g_loadGameConfig = GetPrivateProfileIntW(L"spawn", L"load_game_config", 1, ini) != 0;
    // 1 = after the spawn, post ServerLoadLevelMessageBase with the LevelSetup (FUN_140ba0c20; Kyber's
    // ServerLoadLevelMessage_post): creating a dedicated server does not load a level by itself (2026-10-20)
    g_postLoadLevel = GetPrivateProfileIntW(L"spawn", L"post_load_level", 1, ini) != 0;
    // 0 = force ServerSettings.ThreadingEnable (+0xef) off after the spawn, as Kyber's Server ctor hook does on BF2; the
    // engine reads it at runtime (0x1401af7ae, 0x1401bf269, level load 0x140b95b50). Level loads raced with it on (2026-10-20)
    g_serverThreading = GetPrivateProfileIntW(L"spawn", L"server_threading", 0, ini) != 0;
    status("spawn: spawn_dedicated=%d skip_null_server_job=%d skip_server_job=%d", g_spawnDedicated, g_skipNullJob, g_skipJobAlways);
    status("spawn: root='%s' lobby='%s' map='%s' options='%s' delay=%dms", g_rootLevel, g_lobbyLevel, g_mapLevel, g_options, g_spawnDelayMs);
}

static const char* safe_str(uint64_t p) {
    static __declspec(thread) char buf[160];
    buf[0] = 0;
    __try { if (p) { strncpy_s(buf, (const char*)p, _TRUNCATE); } } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(buf, "<bad ptr>"); }
    return buf;
}

static const char* safe_copy(uint64_t p, char* out, size_t n) { strncpy_s(out, n, safe_str(p), _TRUNCATE); return out; }

// MainLoop constructor 0x1401ae7d0(obj, isDedicatedServer, ...): stores the flag at obj+0x8.
typedef uint64_t(__fastcall* mlctor_t)(uint64_t obj, char dedicated, uint64_t a3, uint64_t a4);
static mlctor_t o_mlctor;
static uint64_t __fastcall h_mlctor(uint64_t obj, char dedicated, uint64_t a3, uint64_t a4) {
    char use = g_dedicated ? 1 : dedicated;
    status("MainLoop ctor obj=%p isDedicatedServer %d -> %d", (void*)obj, (int)dedicated, (int)use);
    uint64_t r = o_mlctor(obj, use, a3, a4);
    status("MainLoop+0x8 now %d", (int)rd<uint8_t>(obj, 8));
    return r;
}

// GameSimulation::spawnServer 0x1401aac90(gameSim, ServerSpawnInfo&)
typedef void(__fastcall* spawn_t)(uint64_t gs, uint8_t* info);
static spawn_t o_spawn;
// Socket wrappers (diagnostics, SERVER build): the exe calls ws2_32 through its import table in .xtls; these slots are
// swapped for pass-through wrappers that log binds, socket creation and the first sends to 127.0.0.1, with the game code
// that called them. Every wrapper calls the real function unchanged.
static bool committed(uint64_t a);
static uint64_t stack_end(uint64_t sp);
static bool g_netWrapped;
#define SLOT_BIND G(0x14307fef0ull)
#define SLOT_SOCKET G(0x14307fee0ull)
#define SLOT_WSASOCKETW G(0x14307ff70ull)
#define SLOT_SENDTO G(0x14307ff10ull)
typedef int (WSAAPI* bind_t)(SOCKET, const sockaddr*, int);
typedef SOCKET (WSAAPI* socket_t)(int, int, int);
typedef SOCKET (WSAAPI* wsasocketw_t)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
typedef int (WSAAPI* sendto_t)(SOCKET, const char*, int, int, const sockaddr*, int);
static bind_t r_bind; static socket_t r_socket; static wsasocketw_t r_wsasocketw; static sendto_t r_sendto;
static void caller_frames(void* retAddrSlot, char* out, size_t n) {
    uint64_t* sp = (uint64_t*)retAddrSlot; uint64_t top = stack_end((uint64_t)sp); int found = 0; size_t k = 0; out[0] = 0;
    __try {
        for (int i = 0; i < 512 && found < 10 && (uint64_t)(sp + i + 1) <= top; i++) {
            uint64_t v = sp[i];
            if (v >= G(0x140001000ull) && v < G(0x14307e400ull)) { k += sprintf_s(out + k, n - k, " %llx", v); found++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void fmt_addr(const sockaddr* a, char* out, size_t n) {
    out[0] = 0;
    __try {
        if (a && a->sa_family == AF_INET) { const sockaddr_in* i = (const sockaddr_in*)a; const uint8_t* b = (const uint8_t*)&i->sin_addr;
            sprintf_s(out, n, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(i->sin_port)); }
        else if (a) sprintf_s(out, n, "family %u", a->sa_family);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "<bad>"); }
}
static int WSAAPI w_bind(SOCKET s, const sockaddr* a, int len) {
    int r = r_bind(s, a, len); int err = r ? WSAGetLastError() : 0;
    char ad[64], fr[220]; fmt_addr(a, ad, sizeof ad); caller_frames(_AddressOfReturnAddress(), fr, sizeof fr);
    status("net: bind(socket %llu, %s) = %d%s err=%d frames:%s", (unsigned long long)s, ad, r, r ? " FAILED" : "", err, fr);
    if (r) WSASetLastError(err);
    return r;
}
static SOCKET WSAAPI w_socket(int af, int type, int proto) {
    SOCKET r = r_socket(af, type, proto); char fr[220]; caller_frames(_AddressOfReturnAddress(), fr, sizeof fr);
    status("net: socket(af %d, type %d, proto %d) = %llu frames:%s", af, type, proto, (unsigned long long)r, fr);
    return r;
}
static SOCKET WSAAPI w_wsasocketw(int af, int type, int proto, LPWSAPROTOCOL_INFOW pi, GROUP g, DWORD fl) {
    SOCKET r = r_wsasocketw(af, type, proto, pi, g, fl); char fr[220]; caller_frames(_AddressOfReturnAddress(), fr, sizeof fr);
    status("net: WSASocketW(af %d, type %d, proto %d) = %llu frames:%s", af, type, proto, (unsigned long long)r, fr);
    return r;
}
static volatile LONG g_loopSends;
static int WSAAPI w_sendto(SOCKET s, const char* buf, int len, int fl, const sockaddr* to, int tolen) {
    int r = r_sendto(s, buf, len, fl, to, tolen);
    __try {
        if (to && to->sa_family == AF_INET && ((const sockaddr_in*)to)->sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
            InterlockedIncrement(&g_loopSends) <= 20) {
            char ad[64], fr[220]; fmt_addr(to, ad, sizeof ad); caller_frames(_AddressOfReturnAddress(), fr, sizeof fr);
            status("net: sendto(socket %llu, %s, %d bytes) = %d frames:%s", (unsigned long long)s, ad, len, r, fr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static bool swap_slot(uint64_t slot, void* expect, void* wrapper, void** realOut) {
    uint64_t cur = rd<uint64_t>(slot, 0);
    if (!expect || cur != (uint64_t)expect) return false;
    DWORD old; if (!VirtualProtect((void*)slot, 8, PAGE_READWRITE, &old)) return false;
    *realOut = (void*)cur; InterlockedExchange64((volatile LONG64*)slot, (LONG64)wrapper);
    VirtualProtect((void*)slot, 8, old, &old);
    return true;
}
static void install_socket_wrappers() {
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    if (!ws || !committed(SLOT_BIND)) { status("net: ws2_32 not loaded or slots not mapped; socket wrappers not installed"); return; }
    bool a = swap_slot(SLOT_BIND, (void*)GetProcAddress(ws, "bind"), (void*)w_bind, (void**)&r_bind);
    bool b = swap_slot(SLOT_SOCKET, (void*)GetProcAddress(ws, "socket"), (void*)w_socket, (void**)&r_socket);
    bool c = swap_slot(SLOT_WSASOCKETW, (void*)GetProcAddress(ws, "WSASocketW"), (void*)w_wsasocketw, (void**)&r_wsasocketw);
    bool d = swap_slot(SLOT_SENDTO, (void*)GetProcAddress(ws, "sendto"), (void*)w_sendto, (void**)&r_sendto);
    status("net: socket wrappers bind=%d socket=%d WSASocketW=%d sendto=%d", a, b, c, d);
    g_netWrapped = a || b || c || d;
}
// ---- Socket pass-through (2026-10-20): patch entries of the engine's in-memory socket manager vtable (0x1433fa218) and
// socket vtable (0x1433fa240) in place with logging thunks that call the originals. Object layouts are untouched. Spec:
// notes/socket-interface.md. Per-packet calls (Send, receive) are counted per socket and summarized every 5 s.
#define VT_SOCKMGR G(0x1433fa218ull)
#define VT_SOCKET  G(0x1433fa240ull)
struct SockStat { volatile LONG64 sock; volatile LONG sends, sbytes, recvs, rbytes, empty, smax, big; int port; char peer[48]; };
static SockStat g_sockStats[48];
static SockStat* sock_stat(uint64_t s) {
    for (int i = 0; i < 48; i++) if ((uint64_t)g_sockStats[i].sock == s) return &g_sockStats[i];
    for (int i = 0; i < 48; i++) if (InterlockedCompareExchange64(&g_sockStats[i].sock, (LONG64)s, 0) == 0) return &g_sockStats[i];
    return nullptr;
}
static void fmt_sockaddr(uint64_t sa, char* out, size_t n) {   // engine SocketAddr: 64 bytes of sockaddr + length at +0x40
    out[0] = 0; uint8_t len = rd<uint8_t>(sa, 0x40);
    if (len == 0x10 && rd<uint16_t>(sa, 0) == AF_INET) {
        uint8_t b[4]; for (int i = 0; i < 4; i++) b[i] = rd<uint8_t>(sa, 4 + i);
        sprintf_s(out, n, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(rd<uint16_t>(sa, 2)));
    } else sprintf_s(out, n, "(len %u family %u)", len, rd<uint16_t>(sa, 0));
}
// ---- UDP transport (2026-10-20, Kyber's way, our own code): with transport=udp the engine's socket objects keep their
// fields, but Listen also binds a real non-blocking OS UDP socket, Send becomes sendto(peer), and receive becomes
// recvfrom that sets the peer to the sender (as the in-memory class does), returns -1 when empty and stamps the time
// with timeGetTime (the engine's NetTick default). The socket destructor closes the OS socket. Semantics from
// notes/socket-interface.md; slots patched in place by install_socket_passthrough.
struct UdpBinding { volatile LONG64 sock; SOCKET os; };
static UdpBinding g_udp[48];
static SOCKET udp_of(uint64_t s) {
    for (int i = 0; i < 48; i++) if ((uint64_t)g_udp[i].sock == s) return g_udp[i].os;
    return INVALID_SOCKET;
}
static void udp_bind_for(uint64_t s, int port) {
    if (udp_of(s) != INVALID_SOCKET) return;
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET os = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (os == INVALID_SOCKET) { status("udp: socket() failed %d", WSAGetLastError()); return; }
    u_long nb = 1; ioctlsocket(os, FIONBIO, &nb);
    int big = 1 << 20; setsockopt(os, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big); setsockopt(os, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof big);
    // a UDP socket on Windows reports an ICMP "port unreachable" as WSAECONNRESET on the next recvfrom; turn that off
    BOOL nf = FALSE; DWORD br = 0; WSAIoctl(os, _WSAIOW(IOC_VENDOR, 12), &nf, sizeof nf, nullptr, 0, &br, nullptr, nullptr);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); inet_pton(AF_INET, g_udpBind, &a.sin_addr);
    if (::bind(os, (sockaddr*)&a, sizeof a) != 0) { status("udp: bind(%s:%d) failed %d", g_udpBind, port, WSAGetLastError()); closesocket(os); return; }
    for (int i = 0; i < 48; i++)
        if (InterlockedCompareExchange64(&g_udp[i].sock, (LONG64)s, 0) == 0) { g_udp[i].os = os; status("udp: engine socket %llx bound to %s:%d", s, g_udpBind, port); return; }
    closesocket(os);
}
static void udp_close_for(uint64_t s) {
    for (int i = 0; i < 48; i++) if ((uint64_t)g_udp[i].sock == s) {
        closesocket(g_udp[i].os); g_udp[i].os = INVALID_SOCKET; InterlockedExchange64(&g_udp[i].sock, 0);
        status("udp: engine socket %llx closed", s); return; }
}
static volatile LONG g_udpErrs;
static bool udp_send(uint64_t s, const uint8_t* buf, int n) {
    SOCKET os = udp_of(s); if (os == INVALID_SOCKET) return false;
    uint8_t len = rd<uint8_t>(s, 0x98);
    int r = ::sendto(os, (const char*)buf, n, 0, (const sockaddr*)(s + 0x58), len ? len : (int)sizeof(sockaddr_in));
    if (r < 0 && InterlockedIncrement(&g_udpErrs) <= 20) status("udp: sendto failed %d (socket %llx, %d bytes)", WSAGetLastError(), s, n);
    return true;   // like the engine's own Send, report success; UDP is best-effort
}
static int udp_recv(uint64_t s, uint8_t* buf, int n, uint32_t* when) {
    SOCKET os = udp_of(s); if (os == INVALID_SOCKET) return -2;   // -2: not ours, use the engine's own receive
    sockaddr_in from{}; int fl = sizeof from;
    int r = ::recvfrom(os, (char*)buf, n, 0, (sockaddr*)&from, &fl);
    if (r < 0) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK && InterlockedIncrement(&g_udpErrs) <= 20) status("udp: recvfrom failed %d (socket %llx)", e, s);
        if (when) *when = 0;
        return -1;
    }
    memset((void*)(s + 0x58), 0, 0x40); memcpy((void*)(s + 0x58), &from, sizeof from); *(uint8_t*)(s + 0x98) = (uint8_t)sizeof from;
    if (when) {
        typedef uint32_t(__fastcall* nettick_t)();
        static int haveTick = -1;
        if (haveTick < 0) haveTick = committed(G(0x140696cb0)) && rd<uint32_t>(G(0x140696cb0), 0) == 0x28ec8348u;   // sub rsp,0x28
        *when = haveTick ? ((nettick_t)G(0x140696cb0))() : timeGetTime();
    }
    return r;
}
typedef void (__fastcall* m_destroy_t)(uint64_t);
typedef uint64_t (__fastcall* m_connect_t)(uint64_t, const char*, bool);
typedef uint64_t (__fastcall* m_create_t)(uint64_t);
typedef uint64_t (__fastcall* dtor_t)(uint64_t, uint32_t);
typedef bool (__fastcall* s_send_t)(uint64_t, const uint8_t*, int, uint32_t);
typedef int (__fastcall* s_recv_t)(uint64_t, uint8_t*, int, uint32_t*);
typedef void (__fastcall* s_setpeer_t)(uint64_t, uint64_t);
typedef bool (__fastcall* s_addr_t)(uint64_t, uint64_t, bool);
typedef bool (__fastcall* s_create_t)(uint64_t, bool);
static m_destroy_t om_destroy; static m_connect_t om_connect, om_listen; static m_create_t om_create; static dtor_t om_dtor;
static dtor_t os_dtor; static s_send_t os_send; static s_recv_t os_recv; static s_setpeer_t os_setpeer;
static s_addr_t os_connect, os_listen; static s_create_t os_create;
static void __fastcall tm_destroy(uint64_t m) { status("net: mgr %llx Destroy", m); om_destroy(m); }
static uint64_t __fastcall tm_connect(uint64_t m, const char* a, bool b) {
    uint64_t s = om_connect(m, a, b); status("net: mgr %llx Connect('%s', %d) = socket %llx", m, safe_str((uint64_t)a), (int)b, s); return s; }
static uint64_t __fastcall tm_listen(uint64_t m, const char* a, bool b) {
    uint64_t s = om_listen(m, a, b); status("net: mgr %llx Listen('%s', %d) = socket %llx", m, safe_str((uint64_t)a), (int)b, s);
    if (s && g_udpTransport) {
        int port = rd<int>(s, 0x54);
        if (!port) { const char* c = strrchr(safe_str((uint64_t)a), ':'); if (c) port = atoi(c + 1); }
        SockStat* st = sock_stat(s); if (st) st->port = port;
        udp_bind_for(s, port);
    }
    return s; }
static uint64_t __fastcall tm_create(uint64_t m) { uint64_t s = om_create(m); status("net: mgr %llx CreateSocket() = socket %llx", m, s); return s; }
static uint64_t __fastcall tm_dtor(uint64_t m, uint32_t f) { status("net: mgr %llx destructor (flags %u)", m, f); return om_dtor(m, f); }
static uint64_t __fastcall ts_dtor(uint64_t s, uint32_t f) {
    SockStat* st = nullptr;
    for (int i = 0; i < 48; i++) if ((uint64_t)g_sockStats[i].sock == s) st = &g_sockStats[i];
    if (st) {
        status("net: socket %llx (port %d) destroyed: sends %ld (%ld B), receives %ld (%ld B)", s, st->port, st->sends, st->sbytes, st->recvs, st->rbytes);
        memset((void*)st, 0, sizeof *st);
    } else status("net: socket %llx destroyed (no traffic)", s);
    udp_close_for(s);
    return os_dtor(s, f);
}
static bool __fastcall ts_send(uint64_t s, const uint8_t* buf, int n, uint32_t f) {
    bool r = (g_udpTransport && udp_of(s) != INVALID_SOCKET) ? udp_send(s, buf, n) : os_send(s, buf, n, f);
    SockStat* st = sock_stat(s);
    if (st) { InterlockedIncrement(&st->sends); InterlockedAdd(&st->sbytes, n); if (n > st->smax) st->smax = n; if (n > 1252) InterlockedIncrement(&st->big); }
    return r;
}
typedef bool(__fastcall* s_pinfo_t)(uint64_t, int*);
static s_pinfo_t os_pinfo;
static bool __fastcall ts_pinfo(uint64_t s, int* out) {
    if (!g_udpTransport) return os_pinfo(s, out);
    out[0] = 1; out[1] = g_udpMaxPacket; out[2] = g_udpMaxPacket; out[3] = 28; out[4] = 1;   // 28 = IPv4 + UDP headers
    return true;
}
static int __fastcall ts_recv(uint64_t s, uint8_t* buf, int n, uint32_t* when) {
    int r = g_udpTransport ? udp_recv(s, buf, n, when) : -2;
    if (r == -2) r = os_recv(s, buf, n, when);
    SockStat* st = sock_stat(s);
    if (st) { if (r > 0) { InterlockedIncrement(&st->recvs); InterlockedAdd(&st->rbytes, r); } else InterlockedIncrement(&st->empty); }
    return r;
}
static void __fastcall ts_setpeer(uint64_t s, uint64_t sa) {
    os_setpeer(s, sa);
    SockStat* st = sock_stat(s);
    if (st) { char a[48]; fmt_sockaddr(sa, a, sizeof a); if (strcmp(a, st->peer)) { strcpy_s(st->peer, a); status("net: socket %llx peer -> %s", s, a); } }
}
static bool __fastcall ts_connect(uint64_t s, uint64_t sa, bool b) {
    bool r = os_connect(s, sa, b); char a[48]; fmt_sockaddr(sa, a, sizeof a); status("net: socket %llx Connect(%s) = %d", s, a, (int)r); return r; }
static bool __fastcall ts_listen(uint64_t s, uint64_t sa, bool b) {
    bool r = os_listen(s, sa, b); char a[48]; fmt_sockaddr(sa, a, sizeof a);
    SockStat* st = sock_stat(s); if (st) st->port = rd<int>(s, 0x54);
    status("net: socket %llx Listen(%s) = %d (port %d)", s, a, (int)r, rd<int>(s, 0x54)); return r;
}
static bool __fastcall ts_create(uint64_t s, bool b) { bool r = os_create(s, b); status("net: socket %llx Create(%d) = %d", s, (int)b, (int)r); return r; }
static bool g_sockPatched;
static bool patch_slot(uint64_t vt, int idx, void* fn, void** orig) {
    uint64_t* slot = (uint64_t*)(vt + 8ull * idx);
    DWORD old; if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
    *orig = (void*)*slot; InterlockedExchange64((volatile LONG64*)slot, (LONG64)fn);
    VirtualProtect(slot, 8, old, &old);
    return true;
}
static void install_socket_passthrough() {
    if (g_sockPatched || !committed(VT_SOCKMGR) || !committed(VT_SOCKET)) return;
    struct Entry { uint64_t vt; int idx; void* fn; void** orig; uint64_t expect; };
    static Entry p[] = {
        {VT_SOCKMGR, 0, (void*)tm_destroy, (void**)&om_destroy, G(0x140295bb0)}, {VT_SOCKMGR, 1, (void*)tm_connect, (void**)&om_connect, G(0x140295dd0)},
        {VT_SOCKMGR, 2, (void*)tm_listen, (void**)&om_listen, G(0x140295c40)},   {VT_SOCKMGR, 3, (void*)tm_create, (void**)&om_create, G(0x140295f90)},
        {VT_SOCKMGR, 4, (void*)tm_dtor, (void**)&om_dtor, G(0x140295b50)},
        {VT_SOCKET, 0, (void*)ts_dtor, (void**)&os_dtor, G(0x140294ff0)},        {VT_SOCKET, 1, (void*)ts_send, (void**)&os_send, G(0x1402951c0)},
        {VT_SOCKET, 2, (void*)ts_recv, (void**)&os_recv, G(0x1402933a0)},        {VT_SOCKET, 5, (void*)ts_setpeer, (void**)&os_setpeer, G(0x140295800)},
        {VT_SOCKET, 7, (void*)ts_pinfo, (void**)&os_pinfo, G(0x140295890)},
        {VT_SOCKET, 17, (void*)ts_connect, (void**)&os_connect, G(0x140295700)}, {VT_SOCKET, 18, (void*)ts_listen, (void**)&os_listen, G(0x1402957a0)},
        {VT_SOCKET, 19, (void*)ts_create, (void**)&os_create, G(0x140295760)},
    };
    const int n = (int)(sizeof p / sizeof p[0]);
#ifndef PROBE_TEST
    // all or nothing: every slot must hold the analysed function before any is patched
    for (int i = 0; i < n; i++) if (rd<uint64_t>(p[i].vt, 8 * p[i].idx) != p[i].expect) {
        status("net: socket vtable %llx slot %d differs from the analysed exe; pass-through NOT installed", p[i].vt, p[i].idx);
        g_sockPatched = true; return; }
#else
    if (!rd<uint64_t>(VT_SOCKET, 8 * 19)) return;   // the test target fills its stand-in vtables after the DLL loads
#endif
    int ok = 0; for (int i = 0; i < n; i++) ok += patch_slot(p[i].vt, p[i].idx, p[i].fn, p[i].orig);
    g_sockPatched = true;
    status("net: socket pass-through installed (%d of %d slots)", ok, n);
}
static DWORD WINAPI sock_summary(LPVOID) {
    for (int k = 0; k < 120; k++) {
        Sleep(5000);
        for (int i = 0; i < 48; i++) {
            SockStat& st = g_sockStats[i];
            if (st.sock && (st.sends || st.recvs))
                status("net: socket %llx port %d peer %s: sends %ld (%ld B, largest %ld B, %ld over 1252 B), receives %ld (%ld B), empty polls %ld",
                       (uint64_t)st.sock, st.port, st.peer, st.sends, st.sbytes, st.smax, st.big, st.recvs, st.rbytes, st.empty);
        }
    }
    return 0;
}
// listen mode diagnostics: for 20 s after a networked match spawn, log every UDP port this process opens (500 ms polling),
// so we can see whether and when the server binds its port relative to the client's connect to 127.0.0.1:<port>
static DWORD WINAPI udp_watch(LPVOID) {
    DWORD pid = GetCurrentProcessId(); static DWORD seen[256]; int nseen = 0;
    for (int k = 0; k < 40; k++) {
        DWORD sz = 0; GetExtendedUdpTable(nullptr, &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        MIB_UDPTABLE_OWNER_PID* t = (MIB_UDPTABLE_OWNER_PID*)malloc(sz + 1024);
        if (t && GetExtendedUdpTable(t, &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                if (t->table[i].dwOwningPid != pid) continue;
                DWORD port = ntohs((u_short)t->table[i].dwLocalPort); bool old = false;
                for (int j = 0; j < nseen; j++) if (seen[j] == port) { old = true; break; }
                if (!old && nseen < 256) { seen[nseen++] = port; if (k > 0) status("listen: UDP port %lu opened (+%d ms)", port, k * 500); }
            }
            if (k == 0) status("listen: %d UDP ports already open at the spawn", nseen);
        }
        free(t); Sleep(500);
    }
    status("listen: UDP watch ended");
    return 0;
}
// Is this LevelSetup a PvP or vs-AI match (GameMode Frontline_Bots, Frontline or Dogfight)? Practice, Missions and
// the Tutorial run on single-player root levels: hosting (listen) and match_mode must leave them single-player, or
// the client is dropped back to the menu (Practice with listen=1, 2026-10-21).
static bool pvp_match(uint64_t ls, char* gm, size_t gmsz) {
    typedef uint64_t(__fastcall* getopt_t)(uint64_t ls, const char* key);
    gm[0] = 0;
    if (!ls) return false;
    safe_copy(((getopt_t)G(0x141191d50))(ls, "GameMode"), gm, gmsz);
    return !_stricmp(gm, "Frontline_Bots") || !_stricmp(gm, "Frontline") || !_stricmp(gm, "Dogfight");
}
static void __fastcall h_spawn(uint64_t gs, uint8_t* info) {
    uint64_t ls = rd<uint64_t>((uint64_t)info, 0x18);
    status("spawnServer gs=%p tick=%u sp=%d localhost=%d dedicated=%d encrypted=%d coop=%d menu=%d keep=%d level='%s'",
           (void*)gs, rd<uint32_t>((uint64_t)info, 0x20), rd<uint8_t>((uint64_t)info, 0x24), rd<uint8_t>((uint64_t)info, 0x25),
           rd<uint8_t>((uint64_t)info, 0x26), rd<uint8_t>((uint64_t)info, 0x27), rd<uint8_t>((uint64_t)info, 0x28),
           rd<uint8_t>((uint64_t)info, 0x29), rd<uint8_t>((uint64_t)info, 0x2A), safe_str(ls ? rd<uint64_t>(ls, 8) : 0));
    // Full dump, so a normal Practice/AI-match spawn shows the real level, game mode and options, and who built them.
    status("  spawnServer caller=%p", _ReturnAddress());
    for (int i = 0; i < 16; i++) status("  info+%02x = %016llx %s", i * 8, rd<uint64_t>((uint64_t)info, i * 8), i == 3 ? "(LevelSetup*)" : "");
    if (ls) {
        // Only the LevelSetup's own 0x40 bytes, and pointers are followed only for the options array (+0x00). Reading past
        // the struct once walked into the process environment block in a test and logged environment variables.
        for (int i = 0; i < 8; i++) {
            uint64_t q = rd<uint64_t>(ls, i * 8);
            char s1[160] = "", s2[200] = "";
            MEMORY_BASIC_INFORMATION mbi{};
            bool stringField = (i == 1 || i == 3 || i == 4 || i == 5 || i == 6);  // +0x08 +0x18 +0x20 +0x28 +0x30
            if ((i == 0 || stringField) && q > 0x10000 && VirtualQuery((void*)q, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                if (stringField) strcpy_s(s1, safe_str(q));
                // options array: {key, value} string pairs, count at ptr-4
                int optStrings = (int)(2 * rd<uint32_t>(q - 4, 0));  // 2 strings per option
                if (i == 0) for (int j = 0; j < 6 && j < optStrings; j++) {
                    uint64_t p = rd<uint64_t>(q, j * 8);
                    if (p > 0x10000 && VirtualQuery((void*)p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                        const char* s = safe_str(p);
                        if (s[0] >= 0x20 && s[0] < 0x7f) {
                            // bounded: strcat_s aborts the process on overflow, so use _TRUNCATE throughout
                            char item[48]; sprintf_s(item, " [%.40s]", s);
                            strncat_s(s2, sizeof s2, item, _TRUNCATE);
                        }
                    }
                }
            }
            bool printable = s1[0] >= 0x20 && s1[0] < 0x7f && strlen(s1) > 2;
            status("  LevelSetup+%02x = %016llx %s%s%s", i * 8, q, printable ? "'" : "", printable ? s1 : "", printable ? "'" : "");
            if (s2[0]) status("      -> %s", s2);
        }
    }
    // join mode, join_local_server=0: a joiner's match needs no server of its own. Skipping the spawn leaves
    // client+0x100 = 0 and the connect port 0, the same state the engine's own remote-join path has (FUN_140a31ff0
    // clears +0x100 when it tears the local server down); the join rewrite then sends the connect to the host.
    if (g_joinAddr[0] && !g_joinLocalServer && !g_dedicated && !info[0x29]) {
        status("join: not spawning a local match server (join_local_server=0)");
        return;
    }
    char spawnMode[48] = "";
    if (g_listen && !g_dedicated && !info[0x29] && info[0x24] && !pvp_match(rd<uint64_t>((uint64_t)info, 0x18), spawnMode, sizeof spawnMode))
        status("listen: '%s' match left single-player (hosting applies to PvP and vs-AI matches)", spawnMode);
    else if (g_listen && !g_dedicated && !info[0x29] && info[0x24]) {
        info[0x24] = 0;
        status("listen: match spawn made networked (isSinglePlayer 1 -> 0, isLocalHost=%d)", (int)info[0x25]);
        HANDLE w = CreateThread(nullptr, 0, udp_watch, nullptr, 0, nullptr); if (w) CloseHandle(w);
        o_spawn(gs, info);
        status("listen: server spawned; port(info+0x58) = %u, server = %p", rd<uint32_t>((uint64_t)info, 0x58), (void*)rd<uint64_t>(gs, 0xF8));
        return;
    }
    o_spawn(gs, info);
    status("spawnServer returned");
}

// GameSimulation clock selector (game subclass) 0x1401bf4d0(gameSim, clockConfig*): first field is the clock's name.
typedef void(__fastcall* clocksel_t)(uint64_t gs, uint64_t* cfg);
static clocksel_t o_clocksel;
static void __fastcall h_clocksel(uint64_t gs, uint64_t* cfg) {
    status("clock selector: name='%s'", safe_str(cfg ? rd<uint64_t>((uint64_t)cfg, 0) : 0));
    o_clocksel(gs, cfg);
}

// ---- Stage 3b: spawn the dedicated server ourselves ---------------------------------------------------------------
// In dedicated mode the main loop ticks GameSimulation (slot 35, 0x1401bfe60) with no server and no client, waiting for
// something to spawn one (Kyber does this on BF2). We hook that tick to get the GameSimulation instance, and a few
// seconds after ticking starts we build a LevelSetup in engine memory with the engine's own helpers and call
// spawnServer(gameSim, info) with isDedicated = 1, on the main thread.
// LevelSetup (0x40, from the engine's copy ctor 0x142e1db30 / dtor 0x140a26b30): +0x00 options array ({key,value}
// pairs, count at ptr-4; empty = 0x14317a6f0), +0x08 root level, +0x10/+0x14 ints, +0x18 string, +0x20 lobby sub-level,
// +0x28 string, +0x30 map level, +0x38..0x3a flags. Empty strings = 0x143f7b968.
typedef void(__fastcall* strassign_t)(uint64_t* field, const char* s, uint32_t len);
typedef void(__fastcall* setoption_t)(uint64_t ls, const char* key, const char* value);
#define kStrAssign G(0x14025c3f0)   // macros, not constants: the build's address is known only once the DLL has loaded
#define kSetOption G(0x141191750)
#define kEmptyStr G(0x143f7b968)
#define kEmptyArr G(0x14317a6f0)

static bool helpers_ok() {
#ifdef PROBE_TEST
    return true;  // test targets plant simplified stand-ins
#else
    static const uint8_t sa[12] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x48};
    static const uint8_t so[12] = {0x48,0x8b,0xc4,0x4c,0x89,0x40,0x18,0x48,0x89,0x48,0x08,0x55};
    return memcmp((void*)kStrAssign, expect_bytes(0x14025c3f0, sa, 12), 12) == 0 &&
           memcmp((void*)kSetOption, expect_bytes(0x141191750, so, 12), 12) == 0;
#endif
}

static bool committed(uint64_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery((void*)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT;
}
// The client constructor's first bundle load, replicated (FUN_140a2b4a0 at 0x140a2c0c2..0x140a2c24b): queue the bundle on the
// engine's job scheduler, wait for it on this (main) thread while pumping the resource manager, release the handle.
// Bundle loads the way the client does them: queue on the engine's job scheduler and let the main loop run, checking each
// tick without blocking. (2026-10-20: a blocking wait inside the GameSimulation tick deadlocked two resource-manager locks.)
// The queue function takes 9 arguments: (handle out, scheduler, u16 0, name array, name count, unused x3, uint 0); the
// client passes the 9th as 0 at [rsp+0x40], which the decompiler shows only as in_stack_00000048.
typedef void (__fastcall* bq_queue_t)(uint32_t* handle, uint64_t sched, uint64_t zero, const char** names, uint32_t count,
                                      uint64_t u6, uint64_t u7, uint64_t u8, uint32_t flags);
typedef bool (__fastcall* bq_done_t)(uint64_t sched, uint32_t handle);           // FUN_14021ba60, non-blocking, own lock
typedef bool (__fastcall* bq_release_t)(uint64_t lock, uint32_t lo, uint32_t hi); // FUN_14021cb70, under the slot lock
typedef void (__fastcall* bq_pump_t)(uint64_t mgr, char a, char b);              // FUN_1401f7960, resource manager pump
typedef void (__fastcall* bq_cs_t)(uint64_t cs);
static uint32_t queue_bundle(const char* name) {
    uint64_t sched = rd<uint64_t>(G(0x1442f7c68), 0);
    if (!sched) { status("3b: bundle %s: no job scheduler", name); return 0x07000000; }
    const char* names[1] = { name };
    InterlockedIncrement((volatile LONG*)G(0x1442d18c0));
    uint32_t h[4] = {};
    ((bq_queue_t)G(0x14021b2b0))(h, sched, 0, names, 1, 0, 0, 0, 0);
    status("3b: bundle %s: queued, handle %08x", name, h[0]);
    if ((h[0] >> 24) == 7) InterlockedDecrement((volatile LONG*)G(0x1442d18c0));
    return h[0];
}
static bool bundle_done(uint32_t h) {
    if ((h >> 24) == 7) return true;
    return ((bq_done_t)G(0x14021ba60))(rd<uint64_t>(G(0x1442f7c68), 0), h);
}
static void release_bundle(const char* name, uint32_t h, ULONGLONG ms) {
    if ((h >> 24) == 7) return;
    uint64_t sched = rd<uint64_t>(G(0x1442f7c68), 0);
    uint64_t cs = sched + 0x22600 + (uint64_t)(((h & 0xffffff) >> 16) & 0xf) * 0x1180;
    (*(bq_cs_t*)G(0x14307f388))(cs);
    bool ok = ((bq_release_t)G(0x14021cb70))(cs, h & 0xffff, (h & 0xffffff) >> 20);
    (*(bq_cs_t*)G(0x14307f2e8))(cs);
    InterlockedDecrement((volatile LONG*)G(0x1442d18c0));
    status("3b: bundle %s: done after %llu ms, release %d", name, ms, (int)ok);
}

static uint8_t* g_spawnLs;  // the LevelSetup handed to spawnServer, kept for the load-level post
static const uint8_t kPostLoadLevelPrologue[6] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30};  // FUN_140ba0c20, from the exe
static void do_spawn(uint64_t gs) {
    if (!helpers_ok()) { status("3b: helper bytes differ from the analysed exe; NOT spawning"); return; }
    uint8_t* ls = (uint8_t*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);  // kept for the server's life
    uint8_t* info = ls + 0x100;
    g_spawnLs = ls;
    *(uint64_t*)(ls + 0x00) = kEmptyArr;
    static const int strFields[] = {0x08, 0x18, 0x20, 0x28, 0x30};
    for (int off : strFields) *(uint64_t*)(ls + off) = kEmptyStr;
    // observed: custom match (PvB Solo) 0/0, Fleet Battles vs AI 7/1; meaning unknown, so configurable
    *(uint32_t*)(ls + 0x10) = (uint32_t)g_lsInt10; *(uint32_t*)(ls + 0x14) = (uint32_t)g_lsInt14;
    auto sa = (strassign_t)kStrAssign; auto so = (setoption_t)kSetOption;
    sa((uint64_t*)(ls + 0x08), g_rootLevel, (uint32_t)strlen(g_rootLevel));
    if (g_lobbyLevel[0]) sa((uint64_t*)(ls + 0x20), g_lobbyLevel, (uint32_t)strlen(g_lobbyLevel));
    if (g_mapLevel[0]) sa((uint64_t*)(ls + 0x30), g_mapLevel, (uint32_t)strlen(g_mapLevel));
    // options "k=v;k=v"
    char opts[512]; strcpy_s(opts, g_options);
    char* ctx = nullptr;
    for (char* tok = strtok_s(opts, ";", &ctx); tok; tok = strtok_s(nullptr, ";", &ctx)) {
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        so((uint64_t)ls, tok, eq + 1);
        status("3b: option %s=%s", tok, eq + 1);
    }
    // ServerSpawnInfo, as the client's start-local-server routine (0x140a33160) builds it, but dedicated
    *(uint64_t*)(info + 0x18) = (uint64_t)ls;
    info[0x24] = 0;  // isSinglePlayer
    info[0x25] = 0;  // isLocalHost
    info[0x26] = 1;  // isDedicated
    *(uint64_t*)(info + 0x30) = kEmptyArr;  // saveData (empty array)
    *(uint32_t*)(info + 0x5c) = 1;          // validLocalPlayersMask
    info[0x26] = g_spawnDedicated ? 1 : 0;  // [spawn] spawn_dedicated (experiment: 0 = headless listen server)
    // Server::init (0x140b9dad0) has a dedicated-only block that queues the job object at 0x1441e2938 and waits for it,
    // WITHOUT the null/valid check the client path makes (0x140a2b4a0). With no client or renderer it never runs, so the
    // server deadlocks (test 3b, 2026-10-19). If the job object is null or invalid, skip that queue+wait exactly as the
    // already-finished case does: 0x140b9dd89 `mov rax,[0x1441e2938]` -> `jmp 0x140b9decb`.
    {
        uint64_t job = rd<uint64_t>(G(0x1441e2938), 0);
        uint8_t valid = job ? rd<uint8_t>(job, 0) : 0;
        status("3b: server-init job object at 0x1441e2938 = %p (first byte %d)", (void*)job, (int)valid);
        if (job) {  // its first qwords, to identify what the job runs (pointers into the exe are code or vtables)
            uint64_t q[6]; for (int i = 0; i < 6; i++) q[i] = rd<uint64_t>(job, i * 8);
            status("3b: job qwords %llx %llx %llx %llx %llx %llx", q[0], q[1], q[2], q[3], q[4], q[5]);
            for (int i = 0; i < 6; i++) if (q[i] >= G(0x140001000ull) && q[i] < G(0x1459b0000ull))
                status("3b: job+%d -> %llx: %llx %llx %llx %llx", i * 8, q[i], rd<uint64_t>(q[i], 0), rd<uint64_t>(q[i], 8),
                       rd<uint64_t>(q[i], 16), rd<uint64_t>(q[i], 24));
        }
        static const uint8_t expect[7] = {0x48, 0x8b, 0x05, 0xa8, 0x4b, 0x64, 0x03};
        static const uint8_t jmp[7] = {0xE9, 0x3D, 0x01, 0x00, 0x00, 0x90, 0x90};
        uint8_t* at = (uint8_t*)G(0x140b9dd89);
        MEMORY_BASIC_INFORMATION mbi{};
        if (g_spawnDedicated && (g_skipJobAlways || (g_skipNullJob && (!job || !valid)))) {
            if (VirtualQuery(at, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && memcmp(at, expect_bytes(0x140b9dd89, expect, 7), 7) == 0) {
                DWORD old; VirtualProtect(at, 7, PAGE_EXECUTE_READWRITE, &old);
                memcpy(at, jmp, 7);
                VirtualProtect(at, 7, old, &old); FlushInstructionCache(GetCurrentProcess(), at, 7);
                status("3b: patched Server::init to skip the job queue+wait (%s)", g_skipJobAlways ? "skip_server_job=1" : "job object null/invalid");
            } else {
                status("3b: job-skip patch: bytes differ or not mapped; NOT patched");
            }
        }
    }
    status("3b: spawning server (dedicated=%d): gameSim=%p root='%s' lobby='%s' map='%s'", (int)info[0x26], (void*)gs, g_rootLevel, g_lobbyLevel, g_mapLevel);
    ((spawn_t)G(0x1401aac90))(gs, info);  // through our spawnServer hook, so it is logged too
    {   // ServerSettings, cached by Server::init in DAT_144331c50 (its +0xa0 DedicatedServerCpu feeds the affinity code)
        uint64_t ss = rd<uint64_t>(G(0x144331c50), 0);
        if (ss) {
            status("3b: ServerSettings %p: JobEnable=%d ThreadingEnable=%d DedicatedServerCpu=%d", (void*)ss,
                   (int)rd<uint8_t>(ss, 0xee), (int)rd<uint8_t>(ss, 0xef), (int)rd<int32_t>(ss, 0xa0));
            if (!g_serverThreading && rd<uint8_t>(ss, 0xef)) { *(volatile uint8_t*)(ss + 0xef) = 0; status("3b: ServerSettings.ThreadingEnable forced to 0"); }
        } else status("3b: ServerSettings not cached yet");
    }
    status("3b: after spawn: gameSim+0xF8 (server) = %p, info+0x50 = %llx, port(info+0x58) = %u",
           (void*)rd<uint64_t>(gs, 0xF8), rd<uint64_t>((uint64_t)info, 0x50), rd<uint32_t>((uint64_t)info, 0x58));
}

// GameSimulation update, slot 35 of the game's GameSimulation vtable (0x1401bfe60)
typedef void(__fastcall* gstick_t)(uint64_t gs, uint64_t a2, uint64_t a3, uint64_t a4);
static gstick_t o_gstick;

// ---- Runtime value engine (issue #3) ----------------------------------------------------------------------------
// Changes data-driven values in memory. Every 2 s, on the main thread (the GameSimulation tick), it walks the loaded
// data the way the engine's own find-by-type does (FUN_140272760): resource compartment table 0x1442d18c8 -> entry
// +0x68 = database domain (lock at +0x70) -> partitions [+0x138, +0x140) -> instances (+0x20, count u16 at +0x28),
// each typed by its ClassInfo's runtime id (+0x48) within the wanted class's id range (+0x48 .. +0x4a). Starship
// configs are Assets, so each names itself (char* at +0x18). For every config of a supported class it records the
// values as loaded (defaults.ini beside the DLL, once per asset per session), then writes the profile's overrides
// wherever the current value differs, so a reloaded copy is patched again. What a config contains comes from the type
// table in config_types.h (see the object-graph walk below); an attribute's IncomAttributeId.Identifier links it to
// its entry in the root config's registration lists.
// vs-AI bots (Pseudo, #3): a player asset's override also goes to its bot counterpart: the same value where the two
// defaults are equal, else the bot's own default scaled by the same ratio. Creep configs are never touched.
#include "config_types.h"   // the type table the engine walks (tools/gen_config_types.py)
#include "bot_map.h"        // player config -> vs-AI bot configs, generated from the pvb_bot collections
#include "schematic_attrs.h"   // match-rule attribute ids -> names (local/tools/gen_schematic_attrs.py)
static const char* schematic_name(uint32_t id) { for (const SchematicAttr& a : kSchematicAttrs) if (a.id == id) return a.key; return nullptr; }
static const char* schematic_camel(uint32_t id) { for (const SchematicAttr& a : kSchematicAttrs) if (a.id == id) return a.name; return nullptr; }
// Object-graph walk (2026-10-21): each root config is walked through the type table, and every number gets a dotted key:
// asset.field, asset.field[i], asset.field.x (Vec3), asset.sub.field (a followed sub-object, its actual class resolved from
// its ClassInfo id), asset.field[i].member (arrays of value types). Attribute leaves (IncomAttributeFloat/Int/Vec3, Value at
// +32) are kept in step with the ROOT config's registration lists: float +0x40 and int +0x48 entries {u32 id, value at +4};
// Vec3 +0x50 entries 32 bytes {Vec3 at 0, u32 id at +16} (layouts from the decoded type info).

struct ValueTarget { char asset[96]; char key[160]; double value; char botOf[96]; bool logged; bool warned; bool hit; const ValueLimit* limit; };
static ValueTarget* g_targets; static int g_targetCount = -1;

struct DefaultSlot { uint32_t hash; char key[256]; double value; };
static DefaultSlot* g_defaults; static const int kDefaultSlots = 1 << 17; static int g_defaultCount;
static char (*g_seenAssets)[96]; static int g_seenCount; static bool g_defaultsDirty;
static char (*g_seenNames)[96];   // each seen config's name as the game spells it (defaults.ini "; name=" lines)
// The engine's state is touched from two threads: the main loop's 2-second scan and the data store's loader (configs
// given the profile as they load). Lock order: a store's own lock first, then this one, never the other way.
static SRWLOCK g_veLock = SRWLOCK_INIT;
static volatile LONG g_loadApplied;   // configs handled in the loader as their partition was created
static const int kVeTypeCount = (int)(sizeof kVeTypes / sizeof kVeTypes[0]);
static const int kVeRootCount = (int)(sizeof kVeRoots / sizeof kVeRoots[0]);

static DefaultSlot* default_slot(const char* key, bool create) {
    uint32_t h = fnv1a(2166136261u, key); if (!h) h = 1;
    for (int i = 0; i < kDefaultSlots; i++) {
        DefaultSlot& d = g_defaults[(h + i) & (kDefaultSlots - 1)];
        if (!d.hash) { if (!create || g_defaultCount >= kDefaultSlots * 3 / 4) return nullptr;
                       d.hash = h; strcpy_s(d.key, key); g_defaultCount++; return &d; }
        if (d.hash == h && !strcmp(d.key, key)) return &d;
    }
    return nullptr;
}
static bool asset_seen(const char* a) { for (int i = 0; i < g_seenCount; i++) if (!strcmp(g_seenAssets[i], a)) return true; return false; }

static void build_targets() {
    g_targets = (ValueTarget*)calloc((size_t)g_overrideCount * 2 + 1, sizeof(ValueTarget));
    g_targetCount = 0;
    for (int i = 0; g_targets && i < g_overrideCount; i++) {
        const char* k = g_overrides[i].key; const char* dot = strchr(k, '.');
        if (!dot || dot == k || !dot[1] || dot - k >= 96 || strlen(dot + 1) >= 160) { status("values: '%s' is not asset.field; ignored", k); continue; }
        ValueTarget& t = g_targets[g_targetCount++];
        strncpy_s(t.asset, k, dot - k); strcpy_s(t.key, dot + 1); t.value = g_overrides[i].value; t.limit = find_limit(t.asset, t.key);
        for (const BotMap& b : kBotMap) if (!strcmp(t.asset, b.player)) {
            ValueTarget& u = g_targets[g_targetCount++];
            strcpy_s(u.asset, b.bot); strcpy_s(u.key, t.key); u.value = t.value; strcpy_s(u.botOf, b.player); u.limit = t.limit;
        }
    }
    status("values: %d override(s) -> %d target(s) including vs-AI bot counterparts", g_overrideCount, g_targetCount);
}

// a number the walk reached: its full key, address, type, and the attribute it belongs to (if any)
// also: a second copy of the same value to keep equal (a list-only config's typed attribute object; 0 = none)
struct VeLeaf { const char* key; uint64_t addr; bool isInt; uint32_t attrId; int list; int comp; bool isBool; uint64_t also; };
typedef void (*ve_fn)(void* ctx, const VeLeaf& l);

// the actual class of a followed sub-object: the most specific type of its family whose id range holds its ClassInfo id
static int ve_resolve(uint64_t obj, int declared) {
    int fam = kVeTypes[declared].followRoot;
    if (fam < 0) return declared;
    uint64_t ci = rd<uint64_t>(obj, 8); if (!ci) return -1;
    uint16_t id = rd<uint16_t>(ci, 0x48);
    int best = -1, span = 1 << 30;
    for (int t = 0; t < kVeTypeCount; t++) {
        if (kVeTypes[t].followRoot != fam || !kVeTypes[t].classInfo) continue;
        uint16_t lo = rd<uint16_t>(G(kVeTypes[t].classInfo), 0x48), hi = rd<uint16_t>(G(kVeTypes[t].classInfo), 0x4a);
        if (hi < lo || (uint16_t)(id - lo) > (uint16_t)(hi - lo)) continue;
        if (hi - lo < span) { span = hi - lo; best = t; }
    }
    return best;
}
// an attribute's key name from its IncomAttributeId asset (name at +0x18): ".../AttributeId_HealthMax" -> "healthmax"
static void attr_name(uint64_t idObj, char* out, size_t cap) {
    out[0] = 0; if (!idObj) return;
    char full[160]; safe_copy(rd<uint64_t>(idObj, 0x18), full, sizeof full);
    const char* s = strrchr(full, '/'); s = s ? s + 1 : full;
    if (!_strnicmp(s, "attributeid_", 12)) s += 12;
    size_t k = 0;
    for (; *s && k + 1 < cap; s++) if (isalnum((unsigned char)*s) || *s == '_') out[k++] = (char)tolower((unsigned char)*s);
    out[k] = 0;
}
static void ve_walk(uint64_t obj, int type, char* key, size_t cap, int depth, ve_fn fn, void* ctx) {
    if (!obj || depth > 5) return;
    const VeType& T = kVeTypes[type];
    static const char* xyz[3] = { "x", "y", "z" };
    for (int i = 0; i < T.fieldCount; i++) {
        const VeField& f = kVeFields[T.firstField + i];
        size_t n = strlen(key);
        if (n + strlen(f.name) + 12 >= cap) continue;
        sprintf_s(key + n, cap - n, ".%s", f.name);
        uint64_t a = obj + f.offset;
        switch (f.kind) {
        case 0: case 1: { VeLeaf l{ key, a, f.kind == 1, 0, 0, 0 }; fn(ctx, l); break; }
        case 2: for (int c = 0; c < 3; c++) { size_t m = strlen(key); sprintf_s(key + m, cap - m, ".%s", xyz[c]);
                    VeLeaf l{ key, a + 4 * c, false, 0, 0, 0 }; fn(ctx, l); key[m] = 0; } break;
        case 11: { VeLeaf l{ key, a, true, 0, 0, 0, true }; fn(ctx, l); break; }   // a yes/no byte
        case 12: {   // an IncomAttributeBool: its byte at +32, and the root's bool registration list (+0x38)
            uint64_t at = rd<uint64_t>(a, 0); if (!at) break;
            uint64_t idObj = rd<uint64_t>(at, 24); uint32_t id = idObj ? rd<uint32_t>(idObj, 32) : 0;
            VeLeaf l{ key, at + 32, true, id, 4, 0, true }; fn(ctx, l); break; }
        case 3: case 4: case 5: {
            uint64_t at = rd<uint64_t>(a, 0); if (!at) break;
            uint64_t idObj = rd<uint64_t>(at, 24); uint32_t id = idObj ? rd<uint32_t>(idObj, 32) : 0;
            if (f.kind == 3) { VeLeaf l{ key, at + 32, false, id, 1, 0 }; fn(ctx, l); }
            else if (f.kind == 4) { VeLeaf l{ key, at + 32, true, id, 2, 0 }; fn(ctx, l); }
            else for (int c = 0; c < 3; c++) { size_t m = strlen(key); sprintf_s(key + m, cap - m, ".%s", xyz[c]);
                     VeLeaf l{ key, at + 32 + 4 * c, false, id, 3, c }; fn(ctx, l); key[m] = 0; }
            break; }
        case 6: { uint64_t sub = rd<uint64_t>(a, 0); int t2 = sub ? ve_resolve(sub, f.type) : -1;
                  if (t2 >= 0) ve_walk(sub, t2, key, cap, depth + 1, fn, ctx); break; }
        case 7: ve_walk(a, f.type, key, cap, depth + 1, fn, ctx); break;
        case 8: case 9: {
            uint64_t arr = rd<uint64_t>(a, 0); int cnt = arr ? (int)rd<uint32_t>(arr - 4, 0) : 0; if (cnt > 64) cnt = 0;
            uint32_t stride = f.kind == 8 ? 4 : kVeTypes[f.type].size;
            for (int j = 0; j < cnt; j++) {
                size_t m = strlen(key); sprintf_s(key + m, cap - m, "[%d]", j);
                if (f.kind == 8) { VeLeaf l{ key, arr + 4ull * j, false, 0, 0, 0 }; fn(ctx, l); }
                else ve_walk(arr + (uint64_t)stride * j, f.type, key, cap, depth + 1, fn, ctx);
                key[m] = 0;
            }
            break; }
        case 13: {   // a list-only config's registration list (IncomSchematicAttributesConfig: Fleet Battles' and
            // Dogfight's match rules): 8-byte entries {u32 attribute id, value at +4}, each named by its attribute
            // The same value is also in a typed attribute object, Attributes (+0x60, IncomAttributeFloat/Int/Bool: Value at
            // +0x20), named by the parallel AttributeNameHash (+0x68, u32: djb2-xor of the attribute's name). The match
            // logic's attributes entity reads the registered list when it finds it and falls back to that object (the
            // stock 3/1 creep morale came back that way), so both are written.
            key[n] = 0;
            uint64_t arr = rd<uint64_t>(a, 0); int cnt = arr ? (int)rd<uint32_t>(arr - 4, 0) : 0; if (cnt > 256) cnt = 0;
            uint64_t attrs = rd<uint64_t>(obj, 0x60), hashes = rd<uint64_t>(obj, 0x68);
            uint32_t na = attrs ? rd<uint32_t>(attrs - 4, 0) : 0, nh = hashes ? rd<uint32_t>(hashes - 4, 0) : 0;
            if (na > 256 || na != nh) na = 0;
            for (int j = 0; j < cnt; j++) {
                uint32_t id = rd<uint32_t>(arr, 8 * j);
                const char* nm = schematic_name(id); if (!nm) continue;
                size_t m = strlen(key); if (m + strlen(nm) + 2 >= cap) continue;
                sprintf_s(key + m, cap - m, ".%s", nm);
                uint64_t also = 0; uint32_t want = 5381;
                for (const char* p = schematic_camel(id); p && *p; p++) want = (want * 33) ^ (uint8_t)*p;
                for (uint32_t k = 0; k < na && !also; k++)
                    if (rd<uint32_t>(hashes, 4 * k) == want) { uint64_t o = rd<uint64_t>(attrs, 8 * k); if (o) also = o + 0x20; }
                VeLeaf l{ key, arr + 8ull * j + 4, f.type == 1, 0, 0, 0, f.type == 2, also }; fn(ctx, l); key[m] = 0;
            }
            break; }
        case 10: {   // a component's AttributeModifier list: each Float modifier, named by its attribute
            key[n] = 0;
            uint64_t arr = rd<uint64_t>(a, 0); int cnt = arr ? (int)rd<uint32_t>(arr - 4, 0) : 0; if (cnt > 64) cnt = 0;
            char names[64][64]; uint64_t mods[64];
            for (int j = 0; j < cnt; j++) {
                mods[j] = rd<uint64_t>(arr, 8 * j); names[j][0] = 0;
                if (mods[j] && rd<uint64_t>(mods[j], 8) == G(kVeTypes[f.type].classInfo)) attr_name(rd<uint64_t>(mods[j], 0x18), names[j], 64);
            }
            for (int j = 0; j < cnt; j++) {
                if (!names[j][0]) continue;
                int total = 0, k = 0;
                for (int q = 0; q < cnt; q++) if (!strcmp(names[q], names[j])) { if (q < j) k++; total++; }
                size_t m = strlen(key);
                if (m + strlen(names[j]) + 8 >= cap) continue;
                if (total > 1) sprintf_s(key + m, cap - m, ".%s[%d]", names[j], k); else sprintf_s(key + m, cap - m, ".%s", names[j]);
                VeLeaf l{ key, mods[j] + 0x30, false, 0, 0, 0 }; fn(ctx, l);
                key[m] = 0;
            }
            break; }
        }
        key[n] = 0;
    }
}
static double read_num(uint64_t addr, bool isInt, bool isBool) {
    return isBool ? (rd<uint8_t>(addr, 0) ? 1.0 : 0.0) : isInt ? (double)rd<int32_t>(addr, 0) : (double)rd<float>(addr, 0);
}
static double leaf_value(const VeLeaf& l) { return read_num(l.addr, l.isInt, l.isBool); }
static bool leaf_write(uint64_t addr, bool isInt, double v, bool isBool = false) {
    __try {
        if (isBool) *(uint8_t*)addr = llround(v) != 0 ? 1 : 0;
        else if (isInt) *(int32_t*)addr = (int32_t)llround(v); else *(float*)addr = (float)v;
        return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// the root config's registration-list slot for an attribute (and Vec3 component)
static uint64_t register_slot(uint64_t root, int list, uint32_t id, int comp) {
    if (!id || !list) return 0;
    uint64_t arr = rd<uint64_t>(root, list == 1 ? 0x40 : list == 2 ? 0x48 : list == 4 ? 0x38 : 0x50); if (!arr) return 0;
    uint32_t n = rd<uint32_t>(arr - 4, 0), stride = list == 3 ? 32 : 8, idOff = list == 3 ? 16 : 0;
    for (uint32_t i = 0; i < n && i < 4096; i++)
        if (rd<uint32_t>(arr, i * stride + idOff) == id) return arr + (uint64_t)i * stride + (list == 3 ? 4ull * comp : 4);
    return 0;
}

struct VeCtx { uint64_t root; const char* asset; };
static void record_leaf(void* c, const VeLeaf& l) {
    DefaultSlot* d = default_slot(l.key, true); if (d) d->value = leaf_value(l);
}
static void apply_leaf(void* c, const VeLeaf& l) {
    VeCtx* x = (VeCtx*)c; char key[256];
    size_t al = strlen(x->asset);
    for (int t = 0; t < g_targetCount; t++) {
        ValueTarget& v = g_targets[t];
        if (strcmp(v.asset, x->asset) || strcmp(l.key + al + 1, v.key)) continue;
        v.hit = true;
        double target = v.value;
        if (v.botOf[0]) {   // follow the player's change: same value, or the bot's own default scaled by the same ratio
            sprintf_s(key, "%s.%s", v.botOf, v.key); DefaultSlot* pd = default_slot(key, false);
            DefaultSlot* bd = default_slot(l.key, false);
            if (!pd || !bd) continue;   // the player's config hasn't been seen yet; next scan
            if ((float)pd->value == (float)bd->value) target = v.value;
            else if (pd->value != 0) target = bd->value * v.value / pd->value;
            else continue;
            double c = clamp_to(v.limit, target);   // a bot's scaled value stays within the same field's limit
            if (c != target && !v.logged) status("limits: %s (vs-AI bot) %.9g clamped to %.9g", l.key, target, c);
            target = c;
        }
        double now = leaf_value(l);
        uint64_t reg = register_slot(x->root, l.list, l.attrId, l.comp);
        double regNow = reg ? read_num(reg, l.isInt, l.isBool) : 0;
        bool same = l.isInt ? llround(now) == llround(target) : (float)now == (float)target;
        bool regSame = !reg || (l.isInt ? llround(regNow) == llround(target) : (float)regNow == (float)target);
        double alsoNow = l.also ? read_num(l.also, l.isInt, l.isBool) : 0;
        bool alsoSame = !l.also || (l.isInt ? llround(alsoNow) == llround(target) : (float)alsoNow == (float)target);
        if (same && regSame && alsoSame) continue;
        bool ok = leaf_write(l.addr, l.isInt, target, l.isBool) && (!reg || leaf_write(reg, l.isInt, target, l.isBool)) &&
                  (!l.also || leaf_write(l.also, l.isInt, target, l.isBool));
        if (!v.logged) {
            v.logged = true;
            status("values: %s %.9g -> %.9g%s%s%s", l.key, now, l.isInt ? (double)llround(target) : (double)(float)target,
                   v.botOf[0] ? " (vs-AI bot, following the player)" : "",
                   l.list ? (reg ? " (attribute and its list entry)" : " (attribute; no list entry)") : l.also ? " (list entry and its attribute)" : "",
                   ok ? "" : " WRITE FAILED");
        }
    }
}
static void process_config(uint64_t inst, int type, const char* asset) {
    char key[256]; VeCtx c{ inst, asset };
    if (!asset_seen(asset) && g_seenCount < 4096) {
        if (g_seenNames) {   // the asset's own name, before lowercasing: "HullCustom_Chameleon_Xwing"
            const char* base = safe_str(rd<uint64_t>(inst, 0x18)); const char* sl = strrchr(base, '/');
            strncpy_s(g_seenNames[g_seenCount], sl ? sl + 1 : base, _TRUNCATE);
        }
        strcpy_s(g_seenAssets[g_seenCount++], asset);
        status("values: config %llx %s", inst, asset);
        strcpy_s(key, asset); ve_walk(inst, type, key, sizeof key, 0, record_leaf, &c);
        g_defaultsDirty = true;
    }
    bool any = false;
    for (int t = 0; t < g_targetCount; t++) if (!strcmp(g_targets[t].asset, asset)) any = true;
    if (!any) return;
    strcpy_s(key, asset); ve_walk(inst, type, key, sizeof key, 0, apply_leaf, &c);
    for (int t = 0; t < g_targetCount; t++) {
        ValueTarget& v = g_targets[t];
        if (!strcmp(v.asset, asset) && !v.hit && !v.warned && !v.botOf[0]) {
            v.warned = true; status("values: %s.%s: no such value in this config (see defaults.ini for the names)", asset, v.key); }
    }
}
// defaults.ini accumulates across launches: each session loads only some configs (some weapons only in a match), and
// the limits (tools/gen_limits.py) must come from every config ever seen, not from whichever launch wrote last. The
// file as it was when this session started is kept, and its sections for configs this session hasn't seen are carried
// over after the ones it has; a config seen again is rewritten whole.
static char* g_prevDefaults; static bool g_prevLoaded;
static void write_defaults_file() {
    if (!g_defaultsDirty || !g_dllDir[0]) return;
    g_defaultsDirty = false;
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%ls\\defaults.ini", g_dllDir);
    if (!g_prevLoaded) {
        g_prevLoaded = true;
        FILE* p = nullptr;
        if (!_wfopen_s(&p, path, L"rb") && p) {
            fseek(p, 0, SEEK_END); long len = ftell(p); fseek(p, 0, SEEK_SET);
            if (len > 0 && len < (64 << 20) && (g_prevDefaults = (char*)malloc(len + 1)) != nullptr) {
                size_t got = fread(g_prevDefaults, 1, len, p); g_prevDefaults[got] = 0;
            }
            fclose(p);
        }
    }
    FILE* f = nullptr; if (_wfopen_s(&f, path, L"w") || !f) return;
    fprintf(f, "; Values as the game loaded them (written by the value engine, issue #3). Sections are assets, keys are fields.\n");
    for (int a = 0; a < g_seenCount; a++) {
        fprintf(f, "\n[%s]\n", g_seenAssets[a]);
        if (g_seenNames && g_seenNames[a][0]) fprintf(f, "; name=%s\n", g_seenNames[a]);   // the game's own spelling, for readable labels
        size_t al = strlen(g_seenAssets[a]);
        for (int i = 0; i < kDefaultSlots; i++) {
            const DefaultSlot& d = g_defaults[i];
            if (d.hash && !strncmp(d.key, g_seenAssets[a], al) && d.key[al] == '.') fprintf(f, "%s=%.9g\n", d.key + al + 1, d.value);
        }
    }
    int carried = 0; bool keep = false;   // earlier launches' configs this session hasn't seen
    for (const char* line = g_prevDefaults; line && *line; ) {
        const char* end = strchr(line, '\n'); size_t len = end ? (size_t)(end - line) : strlen(line);
        char name[96];
        if (line[0] == '[') {
            const char* close = (const char*)memchr(line, ']', len);
            size_t nl = close ? (size_t)(close - line - 1) : 0;
            keep = close && nl > 0 && nl < sizeof name;
            if (keep) { memcpy(name, line + 1, nl); name[nl] = 0; keep = !asset_seen(name); }
            if (keep) { fprintf(f, "\n"); carried++; }
        }
        if (keep && len && line[0] != '\r' && line[0] != '\n') fprintf(f, "%.*s\n", (int)(len && line[len - 1] == '\r' ? len - 1 : len), line);
        line = end ? end + 1 : nullptr;
    }
    fclose(f);
    static int lastCarried = -1;
    if (carried != lastCarried) { lastCarried = carried; status("values: defaults.ini keeps %d config(s) from earlier launches", carried); }
}
static void grid_apply();
// with g_veLock held: the targets and the defaults table, made once
static bool ve_ready() {
    if (g_targetCount < 0) build_targets();
    if (!g_defaults) { g_defaults = (DefaultSlot*)calloc(kDefaultSlots, sizeof(DefaultSlot)); g_seenAssets = (char(*)[96])calloc(4096, 96); g_seenNames = (char(*)[96])calloc(4096, 96); }
    return g_defaults && g_seenAssets;
}
static_assert(sizeof kVeRoots / sizeof kVeRoots[0] <= 64, "raise kVeMaxRoots");
static const int kVeMaxRoots = 64;
static void ve_root_ranges(uint16_t* lo, uint16_t* hi) {
    for (int k = 0; k < kVeRootCount; k++) {
        lo[k] = rd<uint16_t>(G(kVeTypes[kVeRoots[k]].classInfo), 0x48); hi[k] = rd<uint16_t>(G(kVeTypes[kVeRoots[k]].classInfo), 0x4a); }
}
// one partition's instances (+0x20, count u16 at +0x28): each config of a root type gets process_config, under g_veLock
static long ve_scan_partition(uint64_t part, const uint16_t* lo, const uint16_t* hi, long* inspected) {
    long matched = 0;
    uint64_t insts = rd<uint64_t>(part, 0x20); uint16_t n = rd<uint16_t>(part, 0x28);
    for (uint16_t i = 0; insts && i < n; i++) {
        uint64_t inst = rd<uint64_t>(insts, i * 8); if (!inst) continue;
        uint64_t ci = rd<uint64_t>(inst, 8); if (!ci) continue;
        uint16_t id = rd<uint16_t>(ci, 0x48); (*inspected)++;
        for (int k = 0; k < kVeRootCount; k++) {
            if (hi[k] < lo[k] || (uint16_t)(id - lo[k]) > (uint16_t)(hi[k] - lo[k])) continue;
            const char* base = safe_str(rd<uint64_t>(inst, 0x18)); const char* sl = strrchr(base, '/');
            char asset[96]; int j = 0;
            for (const char* q = sl ? sl + 1 : base; *q && j < 95; q++) asset[j++] = (char)tolower((unsigned char)*q);
            asset[j] = 0; if (!j) break;
            matched++;
            AcquireSRWLockExclusive(&g_veLock);
            if (ve_ready()) process_config(inst, kVeRoots[k], asset);
            ReleaseSRWLockExclusive(&g_veLock);
            break;
        }
    }
    return matched;
}
static void value_engine_scan() {
    if (!g_valuesOn) return;
    AcquireSRWLockExclusive(&g_veLock); bool ready = ve_ready(); ReleaseSRWLockExclusive(&g_veLock);
    if (!ready || !committed(G(0x1442d18c8))) return;
    typedef void (__fastcall* cs_t)(uint64_t cs);
    cs_t enter = rd<cs_t>(G(0x14307f388), 0), leave = rd<cs_t>(G(0x14307f2e8), 0);
    if (!enter || !leave) return;
    uint16_t lo[kVeMaxRoots], hi[kVeMaxRoots]; ve_root_ranges(lo, hi);
    ULONGLONG t0 = GetTickCount64(); uint64_t domains[64]; int nd = 0; long inspected = 0, matched = 0;
    for (int c = 0; c < 0xbbe; c++) {
        uint64_t entry = rd<uint64_t>(G(0x1442d18c8), c * 8); if (!entry) continue;
        uint64_t dom = rd<uint64_t>(entry, 0x68); if (!dom) continue;
        bool dup = false; for (int i = 0; i < nd; i++) if (domains[i] == dom) dup = true;
        if (dup || nd >= 64) continue;
        domains[nd++] = dom;
        enter(dom + 0x70);
        uint64_t pb = rd<uint64_t>(dom, 0x138), pe = rd<uint64_t>(dom, 0x140);
        for (uint64_t pp = pb; pp && pp < pe; pp += 8) {
            uint64_t part = rd<uint64_t>(pp, 0); if (!part) continue;
            matched += ve_scan_partition(part, lo, hi, &inspected);
        }
        leave(dom + 0x70);
    }
    AcquireSRWLockExclusive(&g_veLock);
    grid_apply();
    static int reports; ULONGLONG ms = GetTickCount64() - t0;
    if (reports < 5 || ms > 50) { reports++; status("values: scan %llu ms, %d domains, %ld instances, %ld configs, %d assets with defaults", ms, nd, inspected, matched, g_seenCount); }
    static LONG lastLoad; LONG nowLoad = g_loadApplied;
    if (nowLoad != lastLoad) { lastLoad = nowLoad; status("values: %ld config(s) handled as they loaded, before anything was built from them", nowLoad); }
    write_defaults_file();
    ReleaseSRWLockExclusive(&g_veLock);
}

// Configs given the profile as they load (2026-10-23). Capital ships and their turrets are built once, when the map loads,
// and take their configs' values then; the 2-second scan finds a map's configs only after that (Zavian Abyss: the map's
// bundles at 12:47:45, its passive-damage configs written at 12:47:46, and the match ran with stock passive damage).
// FUN_140273060(store, ?, loader) makes a partition object from a parsed partition: called only by the store's
// load-partition FUN_1402722c0, which holds the store's lock (+0x70) around it, after the parse has finished (state 7). The
// partition it returns holds its instances (+0x20, count +0x28), so its configs get the profile before the call returns,
// and before anything can be built from them. The scan still runs, for the menu's configs and as a second pass.
// Mistakes in the maps' own data, fixed as their partition loads when the profile's [rules] asks (each off by default:
// players are used to the game as it is, and keeping a flaw is their choice, as with the exploit rules).
// Zavian Abyss: each creep pack is two spawn points, one of 3 ships and one of 2, which the respawn logic tells to Spawn
// (EventConnections in the PvP layer, SubWorldData "Game/Levels/PvP/PVP_Abyss/PvP": event -202538501 -> "Spawn", djb2
// 230891742). One of the twelve connections, to the middle X-wing pack's 2-ship point, sends "ReturnControl" (-597186424)
// instead, so that pack starts and respawns with 3 ships (2026-10-23: the spawn log showed 13 X-wings, 3+3+3 and 2+2; the
// level data has 11 Spawns and the one ReturnControl; Yavin has 12 Spawns). The connection is a server event, so the host's
// fix is the one that changes the match; it is in the fingerprint anyway, so players know what the server plays. The root (an Asset: name +0x18) keeps EventConnections at +0x40 (count at -4), 32-byte
// entries with SourceEvent at +0x10 and TargetEvent at +0x14. Rewritten only if exactly that one connection is found.
static void map_fix_partition(uint64_t part) {
    uint64_t insts = rd<uint64_t>(part, 0x20); uint16_t n = rd<uint16_t>(part, 0x28);
    for (uint16_t i = 0; insts && i < n; i++) {
        uint64_t inst = rd<uint64_t>(insts, i * 8); if (!inst) continue;
        if (_stricmp(safe_str(rd<uint64_t>(inst, 0x18)), "Game/Levels/PvP/PVP_Abyss/PvP") != 0) continue;
        uint64_t arr = rd<uint64_t>(inst, 0x40); uint32_t cnt = arr ? rd<uint32_t>(arr - 4, 0) : 0;
        int found = 0; uint32_t at = 0;
        for (uint32_t k = 0; k < cnt && k < 4096; k++)
            if (rd<int32_t>(arr, k * 32 + 0x10) == -202538501 && rd<int32_t>(arr, k * 32 + 0x14) == -597186424) { found++; at = k; }
        if (found == 1 && leaf_write(arr + at * 32ull + 0x14, true, 230891742.0))
            status("map fix: Zavian Abyss, the middle X-wing creep pack's 2-ship spawn point now gets Spawn, not ReturnControl (event connection %u of %u)", at, cnt);
        else status("map fix: Zavian Abyss's PvP layer loaded, but %d connection(s) match the miswired creep spawn; left alone", found);
        return;
    }
}
// Ship size (issue #20). The profile value shipscale_<ship>.scale (a pseudo-config like materialgrid: clamped by
// limits.ini's shipscale.scale, in the fingerprint like any value) sizes a starfighter, player and AI alike, as its
// blueprint partition loads. The partition holds a StarshipBlueprint (an Asset: name +0x18, ".../Starship_Ywing",
// ".../Starship_Ywing_AI") and its StarshipEntityData, whose Components (+0x60, count at -4) are the model's top: the
// root PartComponentData (the composite mesh's parts hang under it) and a MeshComponentData. Each component's
// LinearTransform is at +0x30 (right, up, forward, trans, 16 bytes apart); scaling its axes and its translation by s
// scales everything under it about the ship's origin. The laser hitbox follows: the game places each hitbox mesh at its
// part, with the part's scale (measured 2026-10-23/24 with hit_log: at 0.5, all 86 AI hits on Y-wings lay within 0.25 m
// of the half-size hull). Uniform only: a per-axis stretch distorts the models, and the game gives the hitbox one scale
// whatever the axes say (tried 2026-10-24). The cockpit is the exception: under a scaled root part its graphics jump
// around, worse the faster the ship turns (Pseudo, 2026-10-23), so the root part's StarshipCockpitComponentData (ClassInfo
// 0x144a4e5c0, in the part's Components at +0x70) gets the inverse: with the root part's original translation t (its
// axes are the identity), a child at local (axes A, translation u) keeps its old world placement with axes A/s and
// translation (u + t)/s - t. ClassInfos (runtime type info -> .rdata): StarshipBlueprint 0x144ade220, StarshipEntityData
// 0x1449fad80, PartComponentData 0x1449fa760, MeshComponentData 0x144a035c0.
static double ship_scale_for(const char* ship) {   // "ywing" or "ywing_ai": the player ship's value, for both
    char key[96]; int k = sprintf_s(key, "shipscale_");
    for (const char* c = ship; *c && *c != '_' && k < 80; c++) key[k++] = *c;
    strcpy_s(key + k, sizeof key - k, ".scale");
    for (int i = 0; i < g_overrideCount; i++) if (!strcmp(g_overrides[i].key, key)) return g_overrides[i].value;
    return 0;
}
static void model_scale_partition(uint64_t part) {
    uint64_t insts = rd<uint64_t>(part, 0x20); uint16_t n = rd<uint16_t>(part, 0x28);
    float s = 0; char ship[64] = "";
    for (uint16_t i = 0; insts && i < n && s == 0; i++) {   // the blueprint names the ship
        uint64_t inst = rd<uint64_t>(insts, i * 8);
        if (!inst || rd<uint64_t>(inst, 8) != G(0x144ade220)) continue;
        const char* name = safe_str(rd<uint64_t>(inst, 0x18));
        const char* leaf = strrchr(name, '/'); leaf = leaf ? leaf + 1 : name;
        if (_strnicmp(leaf, "starship_", 9) != 0) return;
        int k = 0; for (const char* c = leaf + 9; *c && k < 63; c++) ship[k++] = (char)tolower((unsigned char)*c);
        ship[k] = 0;
        s = (float)ship_scale_for(ship);
    }
    if (s <= 0 || s == 1.0f) return;
    int scaled = 0;
    for (uint16_t i = 0; insts && i < n; i++) {
        uint64_t ent = rd<uint64_t>(insts, i * 8);
        if (!ent || rd<uint64_t>(ent, 8) != G(0x1449fad80)) continue;
        uint64_t arr = rd<uint64_t>(ent, 0x60); uint32_t cnt = arr ? rd<uint32_t>(arr - 4, 0) : 0;
        for (uint32_t k = 0; k < cnt && k < 512; k++) {
            uint64_t c = rd<uint64_t>(arr, k * 8); if (!c) continue;
            uint64_t ci = rd<uint64_t>(c, 8);
            if (ci != G(0x1449fa760) && ci != G(0x144a035c0)) continue;
            float t0[3] = { rd<float>(c, 0x60), rd<float>(c, 0x64), rd<float>(c, 0x68) };   // the original translation
            bool ident = fabsf(rd<float>(c, 0x30) - 1) < 1e-4f && fabsf(rd<float>(c, 0x44) - 1) < 1e-4f && fabsf(rd<float>(c, 0x58) - 1) < 1e-4f &&
                         fabsf(rd<float>(c, 0x34)) < 1e-4f && fabsf(rd<float>(c, 0x38)) < 1e-4f && fabsf(rd<float>(c, 0x40)) < 1e-4f &&
                         fabsf(rd<float>(c, 0x48)) < 1e-4f && fabsf(rd<float>(c, 0x50)) < 1e-4f && fabsf(rd<float>(c, 0x54)) < 1e-4f;
            bool ok = true;
            for (int f = 0; f < 12 && ok; f++) {   // right, up, forward and trans, x y z each: 4 vectors 16 bytes apart
                uint64_t at = c + 0x30 + (uint64_t)(f / 3) * 16 + (uint64_t)(f % 3) * 4;
                ok = leaf_write(at, false, rd<float>(at, 0) * s);
            }
            if (ok) scaled++;
            status("ship scale: %s %s scaled %.3g as its blueprint loads (translation now %.2f %.2f %.2f)", ship,
                   ci == G(0x1449fa760) ? "root part" : "mesh component", s, rd<float>(c, 0x60), rd<float>(c, 0x64), rd<float>(c, 0x68));
            if (ci != G(0x1449fa760) || !ok) continue;
            uint64_t kids = rd<uint64_t>(c, 0x70); uint32_t nk = kids ? rd<uint32_t>(kids - 4, 0) : 0;
            for (uint32_t j = 0; j < nk && j < 512; j++) {
                uint64_t k = rd<uint64_t>(kids, j * 8);
                if (!k || rd<uint64_t>(k, 8) != G(0x144a4e5c0)) continue;
                if (!ident) { status("ship scale: %s cockpit left scaled: the root part is rotated, which the inverse here doesn't handle", ship); continue; }
                for (int f = 0; f < 9; f++) {   // axes / s
                    uint64_t at = k + 0x30 + (uint64_t)(f / 3) * 16 + (uint64_t)(f % 3) * 4;
                    leaf_write(at, false, rd<float>(at, 0) / s);
                }
                for (int f = 0; f < 3; f++) {   // translation (u + t) / s - t
                    uint64_t at = k + 0x60 + (uint64_t)f * 4;
                    leaf_write(at, false, (rd<float>(at, 0) + t0[f]) / s - t0[f]);
                }
                status("ship scale: %s cockpit kept at its original size and place (local translation now %.2f %.2f %.2f)", ship,
                       rd<float>(k, 0x60), rd<float>(k, 0x64), rd<float>(k, 0x68));
            }
        }
    }
    if (!scaled) status("ship scale: %s blueprint loaded, but no part or mesh component found under its entity", ship);
}
// ---- Aggressive creep that keep to the phases ([creep] aggressive=1) ------------------------------------------------
// Stock creep fly Defensive within 500 m, so they fight only what their path brings near (2026-10-24: bomber creep mostly
// idle); Aggressive within 3000 m sent them off the opener's paths at the enemy frigates. The vs-AI bots are Aggressive
// at 2000-3000 m and still keep to the phases: each phase enables a configuration with its own target scores (kickoff:
// only the small capital ship among capital ships; offense: the frigates). The creep get the same in their own data
// (notes/creep-ai.md). The maps place the creep logic prefabs PF_PvP_CreepLogic_Exploit_XWings / _TieFighters, which take
// the match's phase, FrontlineIndex (field 862952468: 0 Rebel zone 2, 1 Rebel zone 1, 2 face-off, 3 Imperial zone 1,
// 4 Imperial zone 2, 5 overtime, 6 transition), and also feed it to a debug display (PropertyDebugEntityData, field
// 5685821) and, through a SelectInt, to the controller's BehaviourIndex (the path for the phase; the blueprint's
// PropertyConnections at +40, 32-byte entries: Source +0, Target +8, SourceFieldId +16, TargetFieldId +20, count at -4).
// The squadron (IncomSquadronAISquadronEntityData 0x144a028c0, in the blueprint's Objects at +72) reads its target list
// only once, as it registers, so all seven entries of its TargetPrioritisationSettingsList (+104) are one shared list
// whose scores h_ctrlfield swaps as the controller's behaviour follows the phase: offense (the enemy frigates, then their
// flagship), face-off and transition (the small capital ship), defense (the small capital ship: the attacker brings a new corvette or Raider each offense). Each keeps the stock scores apart from the generic capital-ship one (tag 2),
// and adds frigate (tag bit 29), flagship (22) and small capital ship (28) scores. The squadron engages as its own
// EngagementModeOverride (+112, Aggressive) says, within [creep] radius (2000 m by default; TargetingRules +32: EngagementMode +32, search radius
// +40). Done as the prefab's partition loads, before a squadron is built from it. The lists and scores are copies of the
// stock ones (TargetPrioritisationSettings: scores array +24; TargetPrioritisationScore: TargetTags +40, Priority +44;
// a DataContainer's refcount is at +16) in memory kept for the process's life, their refcounts set so they never run out.
static bool write_u64(uint64_t at, uint64_t v) { __try { *(uint64_t*)at = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static uint64_t creep_alloc(size_t n) {
    static volatile LONG64 used;
    if (!g_creepPool) return 0;
    LONG64 sz = (LONG64)((n + 15) & ~(size_t)15), at = InterlockedExchangeAdd64(&used, sz);
    return at + sz <= (1 << 20) ? (uint64_t)(g_creepPool + at) : 0;
}
static uint64_t creep_copy(uint64_t obj, size_t size) {   // a DataContainer's copy, with the 16 bytes (its GUID) ahead of it
    uint64_t m = creep_alloc(16 + size); if (!m) return 0;
    __try { memcpy((void*)m, (void*)(obj - 16), 16 + size); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    *(uint32_t*)(m + 16 + 16) = 0x40000000;
    return m + 16;
}
static uint64_t creep_array(uint32_t n) {   // an array of pointers, its count at -4
    uint64_t m = creep_alloc(16 + n * 8ull); if (!m) return 0;
    *(uint32_t*)(m + 12) = n; return m + 16;
}
static void creep_ai_partition(uint64_t part) {
    uint64_t insts = rd<uint64_t>(part, 0x20); uint16_t n = rd<uint16_t>(part, 0x28);
    uint64_t root = insts && n ? rd<uint64_t>(insts, 0) : 0; if (!root) return;
    const char* name = safe_str(rd<uint64_t>(root, 0x18)); const char* leaf = strrchr(name, '/'); leaf = leaf ? leaf + 1 : name;
    if (_strnicmp(leaf, "pf_pvp_creeplogic", 17)) return;
    char low[96]; strncpy_s(low, leaf, _TRUNCATE); _strlwr_s(low);
    int fac = strstr(low, "xwings") ? 0 : strstr(low, "tiefighters") ? 1 : -1; if (fac < 0) return;
    uint64_t objs = rd<uint64_t>(root, 72); uint32_t no = objs ? rd<uint32_t>(objs - 4, 0) : 0;
    uint64_t squad = 0; int squads = 0;
    for (uint32_t k = 0; k < no && k < 4096; k++) { uint64_t o = rd<uint64_t>(objs, k * 8); if (o && rd<uint64_t>(o, 8) == G(0x144a028c0)) { squad = o; squads++; } }
    uint64_t pc = rd<uint64_t>(root, 40); uint32_t np = pc ? rd<uint32_t>(pc - 4, 0) : 0;
    uint64_t ctrl = 0; int conns = 0;   // the controller, the target of the SelectInt's Out -> BehaviourIndex
    for (uint32_t k = 0; k < np && k < 8192; k++)
        if (rd<int32_t>(pc, k * 32 + 16) == 193453899 && rd<int32_t>(pc, k * 32 + 20) == 875081154) { ctrl = rd<uint64_t>(pc, k * 32 + 8); conns++; }
    uint64_t list = squad ? rd<uint64_t>(squad, 104) : 0; uint32_t nl = list ? rd<uint32_t>(list - 4, 0) : 0;
    uint64_t stock = nl == 1 ? rd<uint64_t>(list, 0) : 0;
    uint64_t sarr = stock ? rd<uint64_t>(stock, 24) : 0; uint32_t ns = sarr ? rd<uint32_t>(sarr - 4, 0) : 0;
    if (squads != 1 || conns != 1 || !ctrl || !stock || !ns || ns > 32) {
        status("creep: %s isn't laid out as expected (%d squadron(s), %d behaviour connection(s), %u target list(s), %u score(s)); its creep keep the stock engagement",
               leaf, squads, conns, nl, ns);
        return;
    }
    // The score lookup (FUN_141617a00) takes the FIRST score whose TargetTags bits are all on the target, and a target
    // that matches none scores 0, which the squadron drops; so the capital-ship scores go first, ahead of the stock ones,
    // and a 0 there keeps that ship class out of the phase.
    struct Extra { uint32_t tags; float prio[3]; };   // priority on offense, face-off, defense
    static const Extra kExtra[3] = { { 1u << 29, { 4, 0, 0 } },     // the frigates
                                     { 1u << 22, { 3, 0, 0 } },     // the flagship, once the frigates are gone
                                     { 1u << 28, { 2, 4, 4 } } };   // the small capital ship: the corvette or the Raider
    // The corvette or Raider, in the opener and on defense (the attacker brings a new one each offense): the bombers go
    // for it first; the other creep (fighters, interceptors, dropships) only below the starfighters, so they don't swarm
    // it (Pseudo, 2026-10-26: fighters and interceptors swarmed it on defense and got wrecked; the opener the same way).
    static const float kSmallcapOthers = 1;
    // The bombers' list (v 0) puts the capital ships above everything the stock scores hold (turrets 5, starfighters and
    // players 3): frigates 10, flagship 9, the small capital ship 8 on offense; the small capital ship 10 in the opener
    // and on defense. (2026-10-27: at 4/3/2 a nearby starfighter tied with the flagship and beat the corvette, and
    // bombers near a capital ship targeted a starfighter 3 times in 4.) The others' list keeps kExtra.
    static const float kBomberPrio[3][3] = { { 10, 0, 0 }, { 9, 0, 0 }, { 8, 10, 10 } };
    uint64_t arrs[3] = {}, arrsF[3] = {};
    for (int v = 0; v < 2; v++)
        for (int r = 0; r < 3; r++) {
            uint64_t arr = creep_array(ns + 3), tmpl = 0; uint32_t m = 0; if (!arr) break;
            for (uint32_t k = 0; k < ns && !tmpl; k++) tmpl = rd<uint64_t>(sarr, k * 8);
            for (int e = 0; e < 3 && tmpl; e++) {
                uint64_t s = creep_copy(tmpl, 48); if (!s) { tmpl = 0; break; }
                *(uint64_t*)(s + 24) = 0; *(uint64_t*)(s + 32) = 0;
                *(uint32_t*)(s + 40) = kExtra[e].tags;
                *(float*)(s + 44) = v == 0 ? kBomberPrio[e][r] : r != 0 && kExtra[e].tags == 1u << 28 ? kSmallcapOthers : kExtra[e].prio[r];
                *(uint64_t*)(arr + m++ * 8ull) = s;
            }
            if (!tmpl) break;
            // [server] bomber_capital_only=1 (a test switch, 2026-10-27): the bombers' list (v 0) stops at the capital-ship
            // scores, so a starfighter matches none, scores 0 and is never their target (torpedoes lock only on the
            // current target, and bombers chose starfighters about 3 times in 4 near a capital ship)
            for (uint32_t k = 0; k < ns && !(v == 0 && g_bomberCapitalOnly); k++) {
                uint64_t s = rd<uint64_t>(sarr, k * 8);
                if (s && rd<uint32_t>(s, 40) != 2) *(uint64_t*)(arr + m++ * 8ull) = s;
            }
            *(uint32_t*)(arr - 4) = m; (v ? arrsF : arrs)[r] = arr;
        }
    // A squadron resolves its target list once, as it registers (FUN_1417a2220), and never reads the index again; the
    // score array inside the list is read on every evaluation. So every phase's entry is one shared list, and the phase
    // swaps the scores inside it (h_ctrlfield, as the controller's behaviour follows the phase).
    uint64_t shared = arrs[0] && arrs[1] && arrs[2] && arrsF[0] && arrsF[1] && arrsF[2] ? creep_copy(stock, 32) : 0;
    uint64_t sharedF = shared ? creep_copy(stock, 32) : 0;
    uint64_t phases = sharedF ? creep_array(7) : 0;
    if (!phases) { status("creep: no room for %s's target lists; its creep keep the stock engagement", leaf); return; }
    *(uint64_t*)(shared + 24) = arrs[1]; *(uint64_t*)(sharedF + 24) = arrsF[1];   // the opener's scores until a phase arrives
    for (int p = 0; p < 7; p++) *(uint64_t*)(phases + p * 8ull) = shared;
    CreepPhase& cp = g_creepPh[fac];
    cp.arr[0] = arrs[0]; cp.arr[1] = arrs[1]; cp.arr[2] = arrs[2]; cp.last = -1;
    cp.arrF[0] = arrsF[0]; cp.arrF[1] = arrsF[1]; cp.arrF[2] = arrsF[2];
    InterlockedExchange64((volatile LONG64*)&cp.sharedF, (LONG64)sharedF);
    InterlockedExchange64((volatile LONG64*)&cp.shared, (LONG64)shared);
    cp.stockMode = rd<int32_t>(squad, 32); cp.aggMode = rd<int32_t>(squad, 112);
    InterlockedExchange(&cp.wantMode, cp.stockMode); InterlockedExchange64(&cp.seenAt, 0);
    InterlockedExchange64((volatile LONG64*)&cp.ctrl, (LONG64)ctrl);
    InterlockedExchange64((volatile LONG64*)&cp.squad, (LONG64)squad);
    int32_t mode = rd<int32_t>(squad, 112);
    bool ok = write_u64(squad + 104, phases) && leaf_write(squad + 124, true, 2) && leaf_write(squad + 139, false, 1, true) &&
              leaf_write(squad + 32, true, mode) && leaf_write(squad + 40, false, (double)g_creepRadius);
    if (ok) status("creep: %s: aggressive within %d m, targets by phase (offense: the frigates, then the flagship; face-off: the small "
                   "capital ship, bombers first; defense: the same)", leaf, g_creepRadius);
    else status("creep: %s: a write failed; its creep may keep the stock engagement", leaf);
}
// The phase, live: the creep logic maps FrontlineIndex to its controller's BehaviourIndex (SelectInt: phases 0..6 ->
// behaviours 1, 2, 3, 4, 5, 3, 3; the same in both creep prefabs), and the controller entity's field-change handler
// (FUN_1416feaf0; change = {int field hash, ..., +8 pointer to the value}) stores it at entity +0xb4. A squadron reads its
// target list only once, as it registers, and a connection into its index is never bound; but it reads the scores inside
// the list on every evaluation, so the phase swaps those (the shared list creep_ai_partition made).
typedef void(__fastcall* ctrlfield_t)(uint64_t ent, const int32_t* change);
static ctrlfield_t o_ctrlfield;
static void __fastcall h_ctrlfield(uint64_t ent, const int32_t* change) {
    o_ctrlfield(ent, change);
    if (!g_creepAggressive || !change || rd<int32_t>((uint64_t)change, 0) != 875081154) return;   // BehaviourIndex
    uint64_t data = rd<uint64_t>(ent, 0x28);
    for (int f = 0; f < 2; f++) {
        CreepPhase& cp = g_creepPh[f];
        if (!data || data != cp.ctrl || !cp.shared) continue;
        int32_t bi = rd<int32_t>(ent, 0xb4);
        int p = bi == 1 ? 0 : bi == 2 ? 1 : bi == 4 ? 3 : bi == 5 ? 4 : 2;   // 3 (and anything unexpected): face-off
        int role = creep_role(f, p);
        InterlockedExchange64((volatile LONG64*)(cp.shared + 24), (LONG64)cp.arr[role]);
        if (cp.sharedF) InterlockedExchange64((volatile LONG64*)(cp.sharedF + 24), (LONG64)cp.arrF[role]);
        InterlockedExchange(&cp.wantMode, role == 0 ? cp.aggMode : cp.stockMode);
        InterlockedCompareExchange64(&cp.seenAt, (LONG64)GetTickCount64(), 0);
        if (InterlockedExchange(&cp.last, bi) != bi)
            status("creep: %s creep: behaviour %d (phase %s), %s targets", kCreepFaction[f], bi,
                   bi == 1 ? "0" : bi == 2 ? "1" : bi == 4 ? "3" : bi == 5 ? "4" : "2, 5 or 6", role == 0 ? "offense" : role == 2 ? "defense" : "face-off");
        break;
    }
}
// Per ship type: a squadron scores its candidates for each of its ships in FUN_14182ade0 (twelve integer or pointer
// arguments: the second the ship choosing, the eighth the target list). When the list is a creep squadron's and the ship
// isn't a bomber, it gets the other creep's list instead. A bomber is a ship the server's starship spawn (FUN_1416bcab0,
// h_sspawn) made from one of the bomber packs' spawn points (their spawn reference data, kept by creep_scan as it gives
// those packs the Y-wing or the TIE bomber). Every creep ship comes through that spawn, so a ship at a reused address is
// re-sorted when it spawns. (2026-10-26: reading the ship's type from its own memory failed twice in flight: radar tags at
// a candidate's offsets, then a data pointer at +0x28; the object the scorer is handed keeps neither there.)
static volatile uint64_t g_bomberRefs[16];    // the bomber packs' spawn points (creep_scan), 0 = none
static volatile uint64_t g_bomberShips[256];  // ships spawned from them (h_sspawn), 0 = free
static volatile uint64_t g_bomberTarget[256]; // track_log: each one's assigned target as h_scorer last saw it (a radar-target interface)
static void creep_note_spawn(uint64_t spawnData, uint64_t ship) {
    if (!ship || !g_creepAggressive) return;
    bool bomber = false; for (int i = 0; i < 16 && g_bomberRefs[i] && !bomber; i++) bomber = spawnData == g_bomberRefs[i];
    int freeAt = -1;
    for (int i = 0; i < 256; i++) {
        if (g_bomberShips[i] == ship) { if (!bomber) InterlockedExchange64((volatile LONG64*)&g_bomberShips[i], 0); return; }
        if (!g_bomberShips[i] && freeAt < 0) freeAt = i;
    }
    if (bomber) {
        if (freeAt < 0) freeAt = (int)(GetTickCount64() & 255);   // full (ships that died stay listed): reuse a slot
        InterlockedExchange64((volatile LONG64*)&g_bomberTarget[freeAt], 0);
        InterlockedExchange64((volatile LONG64*)&g_bomberShips[freeAt], (LONG64)ship);
    }
}
// track_log=1 (2026-10-27, measuring how bomber creep attack capital ships): twice a second, each live bomber's world
// position and its assigned target's go into the flight record (R_TRACK, record=1). Read as the squadron scorer does
// (FUN_14182ade0 at 0x14182b575): a ServerStarshipEntity's component collection is at +0x40 (its +0 points back at the
// ship), and its world matrix at collection + 0x10 + byte[+9]*0x20 + byte[+0xa]*0x40: four float4 rows, the forward
// axis at +0x20 and the translation at +0x30. The target is the squadron's assignment for the ship, FUN_1418259d0(squad,
// ship, 0), a radar-target interface (vtable 0x1436172f8); its owner is [[T-0x38]], its point the translation getter's
// (vtable slot 16, FUN_1416c55b0). The assignment map belongs to the server thread, so it's read here, in the scorer, on
// that thread, and only the result is kept. Every read is checked: the ship's vtable (0x1436184e8) and back-pointer.
enum : uint32_t { R_TRACK = 11 };   // state = ship; f0-2 position, f3-5 forward axis, f6-8 target point, f9 distance,
                                    // f10-11 target owner (u64), f12 the target's radar tags (u32; bit 2 = capital ship,
                                    // read as the scorer does at 0x14182b4ed), f13 slot
static void track_note_target(uint64_t squad, uint64_t ship) {
    int i = 0; while (i < 256 && g_bomberShips[i] != ship) i++;
    if (i == 256) return;
    typedef uint64_t(__fastcall* target_t)(uint64_t squad, uint64_t ship, bool any);
    uint64_t t = 0;
    __try { t = ((target_t)G(0x1418259d0ull))(squad, ship, false); } __except (EXCEPTION_EXECUTE_HANDLER) { t = 0; }
    InterlockedExchange64((volatile LONG64*)&g_bomberTarget[i], (LONG64)t);
}
static bool ship_frame(uint64_t s, float pos[3], float fwd[3]) {
    if (!s || rd<uint64_t>(s, 0) != G(0x1436184e8ull)) return false;
    uint64_t c = rd<uint64_t>(s, 0x40); if (!c || rd<uint64_t>(c, 0) != s) return false;
    uint64_t m = c + 0x10 + rd<uint8_t>(c, 9) * 0x20ull + rd<uint8_t>(c, 0xa) * 0x40ull;
    for (int k = 0; k < 3; k++) { pos[k] = rdf(m, 0x30 + 4 * k); fwd[k] = rdf(m, 0x20 + 4 * k); }
    float n = fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2];
    return n > 0.9f && n < 1.1f;   // a unit forward axis: a live transform, not reused memory
}
static bool target_point(uint64_t t, float pos[3], uint64_t* owner) {
    if (!t || rd<uint64_t>(t, 0) != G(0x1436172f8ull)) return false;
    uint64_t c = rd<uint64_t>(t - 0x38, 0); if (!c) return false;   // (rd's offset is unsigned: step back first)
    uint32_t ci = rd<uint8_t>(t - 0x28, 0), ti = rd<uint8_t>(c, 0x14 + ci * 0x20), na = rd<uint8_t>(c, 0xa), n9 = rd<uint8_t>(c, 9);
    uint64_t k = ti == 0xff ? na : na + 1 + ti, p = c + 0x40 + n9 * 0x20ull + k * 0x40ull;
    for (int j = 0; j < 3; j++) pos[j] = rdf(p, 4 * j);
    *owner = rd<uint64_t>(c, 0);
    return true;
}
static void creep_track() {
    static ULONGLONG last, lastNote; ULONGLONG now = GetTickCount64();
    if (!g_trackLog || now - last < 500) return;
    last = now;
    int live = 0, aimed = 0; float dsum = 0;
    for (int i = 0; i < 256; i++) {
        uint64_t s = g_bomberShips[i]; float p[3], f[3], tp[3] = {}; uint64_t owner = 0;
        if (!s || !ship_frame(s, p, f)) continue;
        live++;
        bool hasT = target_point(g_bomberTarget[i], tp, &owner);
        float d = hasT ? sqrtf((p[0] - tp[0]) * (p[0] - tp[0]) + (p[1] - tp[1]) * (p[1] - tp[1]) + (p[2] - tp[2]) * (p[2] - tp[2])) : -1.f;
        if (hasT) { aimed++; dsum += d; }
        Rec* r = claim(); stamp(r, R_TRACK, s);
        for (int k = 0; k < 3; k++) { r->f[k] = p[k]; r->f[3 + k] = f[k]; r->f[6 + k] = tp[k]; }
        uint64_t t = g_bomberTarget[i];
        uint32_t tags = hasT ? rd<uint32_t>(t, 0x5d0) | rd<uint32_t>(t, 0x5cc) | rd<uint32_t>(rd<uint64_t>(t, 0x30), 0x38) : 0;
        r->f[9] = d; *(uint64_t*)&r->f[10] = owner; memcpy(&r->f[12], &tags, 4); r->f[13] = (float)i;
        commit();
    }
    if (now - lastNote >= 10000) { lastNote = now; status("track: %d bomber creep alive, %d with a target, mean distance %.0f m", live, aimed, aimed ? dsum / aimed : 0.f); }
}
typedef void(__fastcall* scorer_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
static scorer_t o_scorer;
static void __fastcall h_scorer(uint64_t a1, uint64_t ship, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                uint64_t list, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12) {
    if (g_creepAggressive && ship && list)
        for (int f = 0; f < 2; f++) {
            const CreepPhase& cp = g_creepPh[f];
            if (list != cp.shared || !cp.sharedF) continue;
            bool bomber = false; for (int i = 0; i < 256 && !bomber; i++) bomber = g_bomberShips[i] == ship;
            if (!bomber) list = cp.sharedF;
            if (g_creepLog) {   // once per ship: which list it got
                static uint64_t seen[256]; static int ns; static SRWLOCK lk = SRWLOCK_INIT;
                AcquireSRWLockExclusive(&lk);
                bool dup = false; for (int i = 0; i < ns && !dup; i++) dup = seen[i] == ship;
                if (!dup && ns < 256) {
                    seen[ns++] = ship;
                    status("creep targets: %s creep ship %llx scores with the %s list", kCreepFaction[f], ship, bomber ? "bombers'" : "other creep's");
                }
                ReleaseSRWLockExclusive(&lk);
            }
            break;
        }
    // [creep] wingmen: a bomber may share its target with the rest of its squadron. The creep's targeting rules (the
    // scorer's seventh argument, IncomSquadronTargetingRules: DefaultMaxAttackersPerTarget +4, 1 for creep;
    // OtherLeaderTargetScoreWeightMultiplier +32, 0.2) give each target to one ship of the faction's whole squadron (all
    // three packs), so a pack's wingmen got no target and held formation (flown 2026-10-27: 600 of 600 assigned targets
    // had exactly one bomber). Bombers get a copy, on this call's stack, with room for all 15 and no penalty for a target
    // another leader holds; the other creep keep the stock rules, so fighters don't gang up on a player.
    uint8_t rules[64];
    if (g_creepWingmen && ship && a7) {
        bool bomber = false; for (int i = 0; i < 256 && !bomber; i++) bomber = g_bomberShips[i] == ship;
        bool copied = false;
        if (bomber) { __try { memcpy(rules, (void*)a7, 56); copied = true; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (copied && *(int32_t*)(rules + 4) >= 0 && *(int32_t*)(rules + 4) < 15) {
            *(int32_t*)(rules + 4) = 15; *(float*)(rules + 32) = 1.0f; a7 = (uint64_t)rules;
            static volatile LONG told; if (!InterlockedExchange(&told, 1)) status("creep: bombers may share a target with their whole squadron (up to 15 attackers), wingmen included");
        }
    }
    o_scorer(a1, ship, a3, a4, a5, a6, a7, list, a9, a10, a11, a12);
    if (g_trackLog && ship && a1) track_note_target(a1, ship);
}
// [server] creep_log=1 (diagnostic): the score lookup (FUN_141617a00: settings, the target's tags, the matched score out;
// returns the priority in xmm0), logged once per distinct (list, tags, priority) when the list is one the mod made.
typedef float(__fastcall* scorelookup_t)(uint64_t settings, uint32_t tags, uint64_t* out);
static scorelookup_t o_scorelookup;
static SRWLOCK g_creepLogLock = SRWLOCK_INIT;
static float __fastcall h_scorelookup(uint64_t settings, uint32_t tags, uint64_t* out) {
    float r = o_scorelookup(settings, tags, out);
    if (!g_creepLog) return r;
    uint64_t ret = (uint64_t)_ReturnAddress();
    bool ours = g_creepPool && settings >= (uint64_t)g_creepPool && settings < (uint64_t)g_creepPool + (1 << 20);
    {   // any list, ours or not, and who asked: once per (list, caller); for lists that aren't ours only when they score a
        // frigate (tag bit 29) above 0, with the list's scores, to find what sends creep at the frigates
        static struct { uint64_t settings, ret; } callers[256]; static int ncallers;
        if (ours || ((tags & (1u << 29)) && r > 0)) {
            AcquireSRWLockExclusive(&g_creepLogLock);
            bool dup = false;
            for (int i = 0; i < ncallers && !dup; i++) dup = callers[i].settings == settings && callers[i].ret == ret;
            if (!dup && ncallers < 256) {
                callers[ncallers++] = { settings, ret };
                char sc[512] = ""; size_t at = 0;
                uint64_t a = rd<uint64_t>(settings, 24); uint32_t na = a ? rd<uint32_t>(a - 4, 0) : 0;
                for (uint32_t k = 0; k < na && k < 24 && at < sizeof sc - 32; k++) {
                    uint64_t s = rd<uint64_t>(a, k * 8);
                    at += sprintf_s(sc + at, sizeof sc - at, " %x:%g", rd<uint32_t>(s, 40), (double)rd<float>(s, 44));
                }
                status("creep targets: list %p (%s) asked from %p: target tags %08x -> priority %g; scores%s", (void*)settings,
                       ours ? "the creep's" : "NOT the creep's", (void*)ret, tags, (double)r, sc);
            }
            ReleaseSRWLockExclusive(&g_creepLogLock);
        }
    }
    if (!ours) return r;
    static struct { uint64_t arr; uint32_t tags; float r; } seen[512]; static int nseen;
    uint64_t arr = rd<uint64_t>(settings, 24);
    AcquireSRWLockExclusive(&g_creepLogLock);
    bool dup = false;
    for (int i = 0; i < nseen && !dup; i++) dup = seen[i].arr == arr && seen[i].tags == tags && seen[i].r == r;
    if (!dup && nseen < 512) {
        seen[nseen++] = { arr, tags, r };
        int f = -1, role = -1;
        for (int ff = 0; ff < 2; ff++) for (int k = 0; k < 3; k++) if (g_creepPh[ff].arr[k] == arr) { f = ff; role = k; }
        status("creep targets: %s %s list: target tags %08x -> priority %g", f < 0 ? "?" : kCreepFaction[f],
               role == 0 ? "offense" : role == 1 ? "face-off" : role == 2 ? "defense" : "?", tags, (double)r);
        if (nseen == 512) status("creep targets: 512 distinct lookups logged; no more");
    }
    ReleaseSRWLockExclusive(&g_creepLogLock);
    return r;
}
// Runs on the corvette and the Raider ([creep] torpedoes, 2026-10-27). A bombing run is planned over a target's damage
// sections that have a clear side (DamageSectionComponentData 0x144a245c0, ClearFaces +292: Zpos, Zneg, Xpos, Xneg, Ypos
// +296, Yneg +297), which a section copies when it's built. The corvette's and the Raider's single section has none, so no
// run can start on them and a bomber assigned one falls back to dogfighting it, sitting on its tail (flown: "parked
// behind the Raider, the leader using lasers and the rest doing nothing"). Their top and bottom are marked clear as the
// ship's blueprint loads, before any is built. Only when the blueprint holds exactly one section with no clear side.
static const char* inst_leaf(uint64_t inst, char* out, size_t cap);
static void smallcap_runs_partition(uint64_t part) {
    uint64_t insts = rd<uint64_t>(part, 0x20); uint16_t n = rd<uint16_t>(part, 0x28);
    char leaf[128] = ""; bool ours = false; uint64_t sec = 0; int nsec = 0;
    for (uint16_t i = 0; insts && i < n; i++) {
        uint64_t o = rd<uint64_t>(insts, i * 8ull); if (!o) continue;
        uint64_t ci = rd<uint64_t>(o, 8);
        if (ci == G(0x144ade220)) { inst_leaf(o, leaf, sizeof leaf); ours = !_stricmp(leaf, "starship_raider") || !_stricmp(leaf, "starship_corvette"); if (!ours) return; }
        else if (ci == G(0x144a245c0)) { sec = o; nsec++; }
    }
    if (!ours) return;
    bool none = sec != 0; for (int k = 0; k < 6 && none; k++) none = rd<uint8_t>(sec, 292 + k) == 0;
    if (nsec != 1 || !none) { status("creep: %s's damage sections aren't as expected (%d, %s); bombers can't run on it", leaf, nsec, sec ? "some side already clear" : "none"); return; }
    bool ok = leaf_write(sec + 296, false, 1, true) && leaf_write(sec + 297, false, 1, true);
    status("creep: %s: %s", leaf, ok ? "top and bottom open to bombing runs" : "a write failed; bombers can't run on it");
}
typedef uint64_t(__fastcall* partnew_t)(uint64_t store, uint64_t a2, uint64_t loader, uint64_t a4);
static partnew_t o_partnew;
static uint64_t __fastcall h_partnew(uint64_t store, uint64_t a2, uint64_t loader, uint64_t a4) {
    uint64_t part = o_partnew(store, a2, loader, a4);
    if (part && g_ruleZavianCreep) map_fix_partition(part);
    if (part && g_shipScaleOn) model_scale_partition(part);
    if (part && g_creepAggressive) creep_ai_partition(part);
    if (part && g_creepTorpedoes) smallcap_runs_partition(part);
    if (part && g_valuesOn) {
        uint16_t lo[kVeMaxRoots], hi[kVeMaxRoots]; ve_root_ranges(lo, hi);
        long inspected = 0, matched = ve_scan_partition(part, lo, hi, &inspected);
        if (matched) InterlockedExchangeAdd(&g_loadApplied, matched);
    }
    return part;
}

// ---- Observation (issue #11: what the game actually does with each value) ---------------------------------------------
// Each client starship component holds a small handle to its live "output data" (what the HUD reads), built by its
// constructor: movement FUN_14169cf00 (+0xCB0), health FUN_141680fb0 (+0x430), weapon FUN_14172a5e0 (+0x1E0), power
// FUN_1415fa310 (+0x1B8). The handle is {vtable, owner, index, flag, data*}; data* (handle+0x18) is a refcounted
// DataContainer made by FUN_141699400 / FUN_141680ed0 / FUN_14172a5e0's FUN_141713fe0 / FUN_1415fa0d0, with its
// ClassInfo at +8 and its fields from +0x18 (offsets as in the runtime type info). The weapon output's +0x20/+0x28/+0x30
// point at its laser regen, laser charge and ammo outputs, the same kind of object. Hooking the constructors registers
// every component as it is made; a sampler thread reads each output 10 times a second, checks its ClassInfo, and writes
// it to the flight record (type 10: state = component, f[0] = kind, f[1..] = the type's raw 32-bit words from +0x18)
// only when it differs from the last record of that output, so idle turrets cost nothing. A handle that no longer
// carries its vtable (component destroyed) is dropped. Kinds: 1 movement, 2 health, 3 power, 4 laser regen,
// 5 laser charge, 6 ammo, 7 weapon (from +0x60: UI id, barrel index, tags, active), 8 last hit taken (the health
// output's +0x18 LastHitInfo: hit angle, damage dealt, origin, hit type; one record per hit unless two hits within a
// sample are identical or land in the same 100 ms, when only the last shows).
struct ObsComp { volatile LONG64 comp; int kind; };   // kind 1 movement, 2 health, 3 power, 4 weapon component
enum { OBS_SLOTS = 4096, OBS_WORDS = 26 };
static ObsComp g_obs[OBS_SLOTS];
static uint32_t g_obsLast[OBS_SLOTS][4][OBS_WORDS];   // last recorded words per output (movement/health/power use [0])
static uint8_t g_obsHave[OBS_SLOTS][4];
static const uint32_t kObsOff[5] = { 0, 0xCB0, 0x430, 0x1B8, 0x1E0 };
// (kObsVt and kObsCi are Steam addresses until apply_build() puts the running build's in)
static uint64_t kObsVt[5] = { 0, 0x143614da0, 0x143612168, 0x143603d68, 0x14361e2a0 };
// per output kind: the data object's ClassInfo, the first word's offset, and the number of words
static uint64_t kObsCi[9] = { 0, 0x144a16420, 0x144a02ec0, 0x144a030c0, 0x144a36d80, 0x144a36d00, 0x144a05510, 0x144a15420,
                                    0x144a15720 };
static const uint32_t kObsBase[9] = { 0, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x60, 0x18 };
static const int kObsWords[9] = { 0, 23, 16, 12, 3, 3, 12, 4, 7 };
static volatile LONG g_obsCount[5];
static volatile LONG g_obsFull;
static void obs_register(uint64_t comp, int kind) {
    int i = 0;
    for (; i < OBS_SLOTS; i++)
        if (InterlockedCompareExchange64(&g_obs[i].comp, (LONG64)comp, 0) == 0) { g_obs[i].kind = kind; break; }
    if (i == OBS_SLOTS && InterlockedExchange(&g_obsFull, 1) == 0) status("observe: registry full (%d), later components not sampled", OBS_SLOTS);
    LONG n = InterlockedIncrement(&g_obsCount[kind]);
    static const char* names[5] = { "", "movement", "health", "power", "weapon" };
    if (n <= 3 || n % 50 == 0) status("observe: %s component %llx (#%ld)", names[kind], comp, n);
}
// Record one output object if it is the expected type and changed since its last record. slot/sub index g_obsLast.
static void obs_sample(int slot, int sub, uint64_t comp, int kind, uint64_t data) {
    if (!data) return;
    uint64_t ci = rd<uint64_t>(data, 8);
    if (ci != kObsCi[kind]) {   // not the type we expect there: say so (first few per kind), never record it
        static volatile LONG bad[9];
        if (InterlockedIncrement(&bad[kind]) <= 3)
            status("observe: kind %d output %llx of component %llx has ClassInfo %llx, expected %llx; not recorded", kind, data, comp, ci, kObsCi[kind]);
        return;
    }
    uint32_t w[OBS_WORDS]; int n = kObsWords[kind];
    for (int i = 0; i < n; i++) w[i] = rd<uint32_t>(data, kObsBase[kind] + i * 4);
    if (g_obsHave[slot][sub] && memcmp(w, g_obsLast[slot][sub], n * 4) == 0) return;
    memcpy(g_obsLast[slot][sub], w, n * 4); g_obsHave[slot][sub] = 1;
    Rec* r = claim(); stamp(r, R_OUTPUT, comp); r->f[0] = (float)kind;
    for (int i = 0; i < n; i++) memcpy(&r->f[1 + i], &w[i], 4);
    commit();
}
static DWORD WINAPI observe_thread(LPVOID) {
    uint64_t seen[OBS_SLOTS] = {};   // the component each slot's history belongs to
    for (;;) {
        Sleep(100);
        for (int i = 0; i < OBS_SLOTS; i++) {
            uint64_t comp = (uint64_t)g_obs[i].comp; if (!comp) continue;
            if (seen[i] != comp) { seen[i] = comp; memset(g_obsHave[i], 0, sizeof g_obsHave[i]); }
            int kind = g_obs[i].kind; uint64_t h = comp + kObsOff[kind];
            if (rd<uint64_t>(h, 0) != kObsVt[kind]) { InterlockedExchange64(&g_obs[i].comp, 0); seen[i] = 0; continue; }
            uint64_t data = rd<uint64_t>(h, 0x18);
            if (kind < 4) {
                obs_sample(i, 0, comp, kind, data);
                if (kind == 2 && data && rd<uint64_t>(data, 8) == kObsCi[2]) obs_sample(i, 1, comp, 8, rd<uint64_t>(data, 0x18));
                continue;
            }
            if (!data) continue;
            obs_sample(i, 3, comp, 7, data);
            if (rd<uint64_t>(data, 8) != kObsCi[7]) continue;   // its sub-output pointers only mean something on the right type
            obs_sample(i, 0, comp, 4, rd<uint64_t>(data, 0x20));
            obs_sample(i, 1, comp, 5, rd<uint64_t>(data, 0x28));
            obs_sample(i, 2, comp, 6, rd<uint64_t>(data, 0x30));
        }
    }
    return 0;
}
typedef uint64_t(__fastcall* ctor3_t)(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4);
static ctor3_t o_cmovctor, o_chealthctor, o_cweaponctor, o_cpowerctor;
static uint64_t __fastcall h_cmovctor(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t r = o_cmovctor(self, a2, a3, a4); if (g_observe) obs_register(self, 1); return r; }
static uint64_t __fastcall h_chealthctor(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t r = o_chealthctor(self, a2, a3, a4); if (g_observe) obs_register(self, 2); return r; }
static uint64_t __fastcall h_cpowerctor(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t r = o_cpowerctor(self, a2, a3, a4); if (g_observe) obs_register(self, 3); return r; }
static uint64_t __fastcall h_cweaponctor(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t r = o_cweaponctor(self, a2, a3, a4); if (g_observe) obs_register(self, 4); return r; }


// ---- Creep packs (the creep rework, step 1) ------------------------------------------------------------------------
// The profile's [creep] section picks each creep pack's ship: <faction>_pack<1-3> = <ship> (Rebel: xwing ywing awing uwing;
// Imperial: tiefighter tiebomber tieinterceptor tiereaper; B-wings and TIE Defenders have no AI version). A pack is a
// placement of a creep logic prefab (LogicPrefabReferenceObjectData, ClassInfo 0x144a176a0, its Blueprint +0xC0 a
// "PF_PvP_CreepLogic_*" prefab, XWings or TieFighters) and the two spawn points the level links it to (its 3 ships and 2
// ships): StarshipSpawnReferenceObjectData, ClassInfo 0x1449fdc00, Blueprint +0xC0 (the ship), Config +0x1F0 (the
// loadout), BlueprintTransform at +128 (translation +176). The links are a level layer's LinkConnections (a LayerData
// 0x144a56c40 or SubWorldData 0x1449fab00 root: +48, count at -4, 24-byte entries: Source +0, Target +8). A faction's packs
// are numbered 1-3 along the line they spawn in (the axis their centres spread most on, ascending); the log says where
// each is. A changed pack's spawn points get the chosen ship's AI blueprint (found by name, "Starship_YWing_AI") and no
// loadout: the ship flies its blueprint's defaults (see creep_durability). Stock behaviour stays: the creep squadron's
// paths follow the match's phases (2026-10-24: TIE bomber creep bombed the frigates only once the Empire was on offense).
// Done on the 2-second scan, after the engine has resolved the level's references (an earlier write is put back), and
// before the first creep launch (2026-10-24: written about 30 s ahead of it).
static const char* inst_leaf(uint64_t inst, char* out, size_t cap) {   // an Asset's name after its last '/', copied
    const char* n = safe_str(rd<uint64_t>(inst, 0x18)); const char* sl = strrchr(n, '/');
    strncpy_s(out, cap, sl ? sl + 1 : n, _TRUNCATE); return out;
}
template <typename F> static void walk_partitions(F fn) {
    if (!committed(G(0x1442d18c8))) return;
    typedef void (__fastcall* cs_t)(uint64_t cs);
    cs_t enter = rd<cs_t>(G(0x14307f388), 0), leave = rd<cs_t>(G(0x14307f2e8), 0);
    if (!enter || !leave) return;
    uint64_t domains[64]; int nd = 0;
    for (int c = 0; c < 0xbbe; c++) {
        uint64_t entry = rd<uint64_t>(G(0x1442d18c8), c * 8); if (!entry) continue;
        uint64_t dom = rd<uint64_t>(entry, 0x68); if (!dom) continue;
        bool dup = false; for (int i = 0; i < nd; i++) if (domains[i] == dom) dup = true;
        if (dup || nd >= 64) continue;
        domains[nd++] = dom;
        enter(dom + 0x70);
        uint64_t pb = rd<uint64_t>(dom, 0x138), pe = rd<uint64_t>(dom, 0x140);
        for (uint64_t pp = pb; pp && pp < pe; pp += 8) {
            uint64_t part = rd<uint64_t>(pp, 0); if (!part) continue;
            uint64_t insts = rd<uint64_t>(part, 0x20); uint16_t n = rd<uint16_t>(part, 0x28);
            if (insts && n) fn(insts, n);
        }
        leave(dom + 0x70);
    }
}
// A creep pack's loadout (creep_loadout), when torpedoes or the bots' handling ask for one: a copy of the faction's stock
// creep collection (StarshipConfigCollection, ClassInfo 0x144a156a0: an Asset with Configs at +0x20, pointers, count at
// -4) whose Configs are
//   - the creep's hull and shields: a stock pack's own creep configs (that collection's), a changed pack's single-player
//     AI configs (StarshipHealthDamageData 0x144a8ca00 / StarshipShieldData 0x144a8c980, *_AI_SP: the creep durability);
//   - with [creep] torpedoes, for Y-wings and TIE bombers: the weapon slots of the vs-AI bots' torpedo loadout
//     (StarshipWeaponSlotConfig 0x1449fc3f0 in that collection: torpedoes, bombs, flares);
//   - with [creep] bot_handling: the vs-AI bots' movement config for the ship (StarshipMovementConfig 0x144ab56a0,
//     *AI_BOT), in place of the AI blueprint's own.
// The bots' hull, shields and hull component stay out, so creep keep creep durability. The sources are found by type
// and name among all of a level's objects. Kept for the process's life in the creep pool, rebuilt when a source changes.
struct CreepSources { uint64_t stockColl, hull, shields, torpColl, botMove; };
static uint64_t creep_loadout(int f, int t, const CreepSources& s, bool torpedoes, bool handling) {
    static CreepSources cSrc[2][4]; static uint64_t cOut[2][4]; static bool cTold[2][4], cTorp[2][4], cHand[2][4];
    const char* ship = kCreepShips[f][t];
    if (!memcmp(&cSrc[f][t], &s, sizeof s) && cTorp[f][t] == torpedoes && cHand[f][t] == handling && (cOut[f][t] || cTold[f][t])) return cOut[f][t];
    cSrc[f][t] = s; cTorp[f][t] = torpedoes; cHand[f][t] = handling; cOut[f][t] = 0; cTold[f][t] = true;
    const char* missing = !g_creepPool ? "the creep pool" : !s.stockColl ? "the stock creep collection" : t && !s.hull ? "the creep hull config" : nullptr;
    if (missing) { status("creep: %s loadout: %s isn't loaded in this level; its packs keep their usual loadout", ship, missing); return 0; }
    // what's missing of the extras drops only that extra
    if (torpedoes && !s.torpColl) { status("creep: %s loadout: the vs-AI bots' torpedo collection isn't loaded in this level; no torpedoes", ship); torpedoes = false; }
    if (handling && !s.botMove) { status("creep: %s loadout: the vs-AI bots' movement config isn't loaded in this level; the usual handling", ship); handling = false; }
    if (!torpedoes && !handling) return 0;
    uint64_t keep[16]; uint32_t nk = 0, slots = 0, dropped = 0; bool torp = false; char leaf[128];
    if (!t) {   // a stock pack: its own creep configs
        uint64_t sa = rd<uint64_t>(s.stockColl, 0x20); uint32_t sn = sa ? rd<uint32_t>(sa - 4, 0) : 0;
        for (uint32_t k = 0; sn <= 8 && k < sn; k++) { uint64_t c = rd<uint64_t>(sa, k * 8); if (c && nk < 16) keep[nk++] = c; }
    } else { keep[nk++] = s.hull; if (s.shields) keep[nk++] = s.shields; }
    if (torpedoes) {
        uint64_t pa = rd<uint64_t>(s.torpColl, 0x20); uint32_t pn = pa ? rd<uint32_t>(pa - 4, 0) : 0;
        uint64_t torpWeapon = 0;   // the torpedo slot's weapon (StarshipBaseSlotConfig: Config +96, ID +104)
        for (uint32_t k = 0; pn <= 16 && k < pn; k++) {
            uint64_t c = rd<uint64_t>(pa, k * 8); if (!c || rd<uint64_t>(c, 8) != G(0x1449fc3f0)) continue;
            inst_leaf(c, leaf, sizeof leaf); _strlwr_s(leaf);
            if (strstr(leaf, "torpedo") && !torpWeapon) torpWeapon = rd<uint64_t>(c, 96);
        }
        for (uint32_t k = 0; pn <= 16 && k < pn; k++) {
            uint64_t c = rd<uint64_t>(pa, k * 8); if (!c || rd<uint64_t>(c, 8) != G(0x1449fc3f0)) continue;
            inst_leaf(c, leaf, sizeof leaf); _strlwr_s(leaf);
            if (strstr(leaf, "torpedo")) torp = true;
            // the bombs' slot ("slotconfig_ywing_bomb" / "slotconfig_tiebomber_bomb", by the name's end: "tiebomber" alone
            // would match the flares too) carries a second torpedo instead: a copy of the slot, its Config (+96) the
            // torpedo weapon. Left out instead, the AI ship's own default bombs filled it (flown 2026-10-27); kept as the
            // same slot, whatever decides which aux slot it is still does.
            size_t ll = strlen(leaf);
            if (ll >= 5 && !strcmp(leaf + ll - 5, "_bomb")) {
                uint64_t c2 = torpWeapon ? creep_copy(c, 112) : 0;
                if (!c2) { dropped++; continue; }
                *(uint64_t*)(c2 + 96) = torpWeapon; c = c2; dropped++;
            }
            if (nk < 16) { keep[nk++] = c; slots++; }
        }
    }
    if (handling && nk < 16) keep[nk++] = s.botMove;
    uint64_t coll = nk && (!torpedoes || torp) ? creep_copy(s.stockColl, 40) : 0, arr = coll ? creep_array(nk) : 0;
    if (!arr) {
        status("creep: %s loadout: the collections aren't laid out as expected (%s); its packs keep their usual loadout", ship,
               !nk ? "no hull config" : torpedoes && !torp ? "no torpedo slot" : "no room");
        return 0;
    }
    for (uint32_t k = 0; k < nk; k++) *(uint64_t*)(arr + k * 8ull) = keep[k];
    *(uint64_t*)(coll + 0x20) = arr;
    status("creep: %s loadout: the creep's hull%s%s%s", ship, (t ? s.shields : 0) || (!t && f == 0) ? " and shields" : "",
           torpedoes ? (dropped ? ", the vs-AI bots' torpedo loadout's weapon slots with torpedoes in the bombs' slot too" : ", the vs-AI bots' torpedo loadout's weapon slots (no bomb slot found to replace)") : "",
           handling ? ", the vs-AI bots' handling" : "");
    (void)slots;
    return cOut[f][t] = coll;
}
// Bomber torpedoes are for capital ships ([creep] torpedoes): the AI torpedo weapons the bombers carry (the vs-AI bots'
// StarshipWeaponConfig_HighYieldTorpedo_<YWing|TieBomber>_AI_Instalock, ClassInfo 0x1449fc370) lock through their
// WeaponFiring (+96), a TargetLockWeaponFiringData (TargetLockType +224, 2 here; TargetTags +228, a mask the target must
// carry all of, 0 = anything). TargetTags = 2 (the capital-ship tag, which the corvette, the Raider, the frigates and the
// flagships carry) keeps them from locking onto creep. Those weapons are the vs-AI bots' too, so only while no bot logic
// is loaded (Fleet Battles: pf_pvb_botlogic is loaded only with bots); with bots loaded, a filter the mod set goes back.
// [creep] torpedo_lock_min: the creep torpedoes' minimum lock distance (stock 500 m). Flown 2026-10-27: bombers attacking a
// capital ship close in to 200-300 m, inside the stock minimum, and so held their torpedoes; at 150 m they fired far more.
// The firing data's MinLockDistanceAttribute (+184) points to an IncomAttributeFloat: value +32, its id object +24 -> +32;
// the same value sits in the weapon config's float registration list (+0x40, {id, value at +4}), and both are written, as
// the value engine does. The bots' weapons too, so only while no bot logic is loaded; the stock value goes back if it loads.
static void creep_torpedo_minlock(const uint64_t weapon[2], bool botsLoaded) {
    static uint64_t set[2]; static float stock[2];
    static const char* kShip[2] = { "ywing", "tiebomber" };
    for (int b = 0; b < 2; b++) {
        uint64_t f = weapon[b] ? rd<uint64_t>(weapon[b], 96) : 0, at = f ? rd<uint64_t>(f, 184) : 0; if (!at) continue;
        uint64_t idObj = rd<uint64_t>(at, 24); uint32_t id = idObj ? rd<uint32_t>(idObj, 32) : 0;
        uint64_t reg = register_slot(weapon[b], 1, id, 0);
        float now = rdf(at, 32);
        if (botsLoaded) {
            if (set[b] == weapon[b] && leaf_write(at + 32, false, stock[b]) && (!reg || leaf_write(reg, false, stock[b]))) {
                set[b] = 0; status("creep: %s torpedoes: vs-AI bots are loaded, so they lock from %g m again", kShip[b], (double)stock[b]); }
            continue;
        }
        if (set[b] != weapon[b]) { stock[b] = now; set[b] = weapon[b]; }
        float want = (float)g_creepTorpMinLock;
        if (now == want && (!reg || rdf(reg, 0) == want)) continue;
        bool ok = leaf_write(at + 32, false, want) && (!reg || leaf_write(reg, false, want));
        status("creep: %s torpedoes lock from %d m (stock %g m)%s%s", kShip[b], g_creepTorpMinLock, (double)stock[b],
               reg ? "" : "; no list entry", ok ? "" : "; a write failed");
    }
}
static void creep_torpedo_targets(const uint64_t weapon[2], bool botsLoaded) {
    static uint64_t set[2];   // the firing data the mod changed, to put back if bots load
    static const char* kShip[2] = { "ywing", "tiebomber" };
    for (int b = 0; b < 2; b++) {
        uint64_t f = weapon[b] ? rd<uint64_t>(weapon[b], 96) : 0; if (!f) continue;
        if (rd<int32_t>(f, 224) != 2) {
            static bool told[2]; if (!told[b]) { told[b] = true; status("creep: the %s AI torpedo isn't a lock-on weapon as expected (type %d); its lock filter stays", kShip[b], rd<int32_t>(f, 224)); }
            continue;
        }
        uint32_t now = rd<uint32_t>(f, 228);
        if (!botsLoaded && now == 0) {
            if (leaf_write(f + 228, true, 2)) { set[b] = f; status("creep: %s torpedoes lock only onto capital ships (the corvette, the Raider, frigates, flagships)", kShip[b]); }
        } else if (botsLoaded && set[b] == f && now == 2) {
            if (leaf_write(f + 228, true, 0)) { set[b] = 0; status("creep: %s torpedoes: vs-AI bots are loaded, so their lock filter is back to stock", kShip[b]); }
        }
    }
}
// Torpedo runs ([creep] torpedoes, 2026-10-27; notes/creep-ai.md section 9). The bombers' runs are a flight behaviour,
// IncomSquadronAIBombingFlightBehaviour, that a weapon config brings with it (BehaviourBlueprint +120; the weapon
// component instantiates it for any weapon type, FUN_141734180) and the squad AI's loadout nodes run. It only flies:
// approach to a point EntryDistanceToPath short of the target capital ship's run line, follow the line (BombingDistance
// above the ship's surface, built from its damage sections) across the ship, fly on DistanceAfterBombing, then plan a new
// run. It fires nothing; torpedoes go through the missile rule as the ship's nose comes onto the target. The bombers now
// carry torpedoes instead of bombs (creep_loadout), the creep torpedo weapons get the same blueprint the AI bombs carry,
// and its data is set for torpedo runs: a 1,200 m straight leg in (the 500 m lock minimum plus time to lock), the line
// 150 m above the hull, 600 m out before turning back, the afterburner only beyond 1,350 m of the leg's start (off on the
// leg itself). The ship copies the data when its behaviour is built, so this is done before the creep launch, on the scan.
// The bots use the same blueprint and torpedo weapons, so only while no vs-AI bot logic is loaded; stock comes back if
// it loads. Data layout: EntryDistanceToPath +40, PointsApproachTolerance +48, DistanceAfterBombing +52,
// BombingDistance +56, AfterburnerThresholdDistance +68, RunOnlyForLeaders +72 (ClassInfo 0x144a48340).
static uint64_t raid_data(uint64_t raidBp) {   // the run data in the run blueprint's Objects (+72), by its ClassInfo
    uint64_t objs = raidBp ? rd<uint64_t>(raidBp, 72) : 0; uint32_t no = objs ? rd<uint32_t>(objs - 4, 0) : 0;
    for (uint32_t k = 0; k < no && k < 16; k++) { uint64_t o = rd<uint64_t>(objs, k * 8); if (o && rd<uint64_t>(o, 8) == G(0x144a48340)) return o; }
    return 0;
}
// [creep] wingmen: the run data's RunOnlyForLeaders (+72) off, so a wingman the loadout gate lets through
// (creep_wingmen_graph) may start a run. Shared with the bots, so only while no bot logic is loaded, as the torpedo runs.
static void creep_wingmen_raid(uint64_t raidBp, bool botsLoaded) {
    static uint64_t set; static bool told;
    if (botsLoaded) {
        if (set && rd<uint8_t>(set, 72) == 0 && leaf_write(set + 72, false, 1, true)) { status("creep: vs-AI bots are loaded, so the runs are the leader's alone again"); set = 0; }
        return;
    }
    uint64_t data = raid_data(raidBp);
    if (!data) { if (!told && raidBp) { told = true; status("creep: the bombing-run behaviour has no run data as expected; wingmen don't fly runs"); } return; }
    if (rd<uint8_t>(data, 72) == 1 && leaf_write(data + 72, false, 0, true)) { set = data; status("creep: the bombers' runs are open to wingmen (RunOnlyForLeaders off)"); }
}
// The run data, by kind: stock, bombs (aggressive creep that keep their bombs: half way to the torpedo values, flown
// 2026-10-27: the torpedo distances kept bombers from flying into their target but looked silly from so far out), and
// torpedoes. Set while no bots are loaded, stock again if they load. Returns the data once it holds the kind asked for.
struct RunField { uint32_t off; float stock, bomb, torp; };
static const RunField kRunFields[] = { { 40, 400.f, 800.f, 1200.f }, { 48, 50.f, 75.f, 100.f }, { 52, 250.f, 425.f, 600.f },
                                       { 56, 50.f, 100.f, 150.f }, { 68, 500.f, 925.f, 1350.f } };
static float run_val(const RunField& f, int kind) { return kind == 0 ? f.stock : kind == 1 ? f.bomb : f.torp; }
static uint64_t creep_run_data(uint64_t raidBp, bool botsLoaded, int kind) {
    static uint64_t setData; static bool toldBp, toldData;
    if (botsLoaded) {
        if (setData) {
            for (const RunField& f : kRunFields) leaf_write(setData + f.off, false, f.stock);
            status("creep: vs-AI bots are loaded, so the bombing-run behaviour is back to stock"); setData = 0;
        }
        return 0;
    }
    if (!raidBp) { if (!toldBp) { toldBp = true; status("creep: the AI bombs' run behaviour isn't loaded in this level; the runs stay as they are"); } return 0; }
    uint64_t data = raid_data(raidBp);
    int now = -1;
    for (int k = 0; data && k < 3 && now < 0; k++) { bool all = true; for (const RunField& f : kRunFields) all = all && rdf(data, f.off) == run_val(f, k); if (all) now = k; }
    if (!data || now < 0) {
        if (!toldData) { toldData = true; status("creep: the bombing-run behaviour isn't laid out as expected (%s); the runs stay as they are", data ? "unexpected values" : "no run data in it"); }
        return 0;
    }
    if (now != kind) {
        bool ok = true; for (const RunField& f : kRunFields) ok = leaf_write(data + f.off, false, run_val(f, kind)) && ok;
        if (kind == 2) status("creep: the bombing-run behaviour is set for torpedo runs (%s): a 1,200 m leg in, 150 m above the hull, 600 m out", ok ? "done" : "a write failed");
        else status("creep: the bombers' bombing runs start 800 m out, 100 m above the hull, 425 m out the far side (%s)", ok ? "done" : "a write failed");
    }
    setData = data;
    return data;
}
static void creep_torpedo_runs(const uint64_t weapon[2], uint64_t raidBp, bool botsLoaded) {
    static uint64_t setW[2], setBp;
    if (botsLoaded) {   // stock again for the bots
        bool any = false;
        for (int b = 0; b < 2; b++) if (setW[b] && rd<uint64_t>(setW[b], 120) == setBp && write_u64(setW[b] + 120, 0)) { setW[b] = 0; any = true; }
        if (any) status("creep: vs-AI bots are loaded, so the torpedoes carry no run behaviour");
        creep_run_data(raidBp, true, 0);
        return;
    }
    if (!creep_run_data(raidBp, false, 2)) return;
    setBp = raidBp;
    for (int b = 0; b < 2; b++) {
        if (!weapon[b] || rd<uint64_t>(weapon[b], 120) == raidBp) continue;
        if (rd<uint64_t>(weapon[b], 120) != 0) { status("creep: a creep torpedo weapon already carries a behaviour; left as it is"); continue; }
        if (write_u64(weapon[b] + 120, raidBp)) { setW[b] = weapon[b]; status("creep: %s torpedoes fly torpedo runs", b ? "TIE bomber" : "Y-wing"); }
    }
}
static void creep_scan() {
    if (!g_creepOn && !g_creepBotHandling && !g_creepWingmen) return;
    uint64_t torpWeapon[2] = {}, raidBp = 0; bool botsLoaded = false;   // creep_torpedo_targets, creep_torpedo_runs
    static const char* kTorpWeapon[2] = { "starshipweaponconfig_highyieldtorpedo_ywing_ai_instalock", "starshipweaponconfig_highyieldtorpedo_tiebomber_ai_instalock" };
    // the loadouts' sources (creep_loadout), per faction and ship (CreepShips order), found by type and name
    struct Want { uint64_t cls; char name[96]; uint64_t* at; };
    CreepSources src[2][4] = {}; Want want[40]; int nw = 0;
    bool needLoadouts = g_creepTorpedoes || g_creepBotHandling;
    static const char* kStockColl[2] = { "starshipconfigcollection_xwing_ai", "starshipconfigcollection_tiefighter_ai" };
    static const char* kTorpColl[2] = { "starshipconfigcollection_ywing_ai_pvb_bot_torpedo_bomb", "starshipconfigcollection_tiebomber_ai_pvb_bot_bomb_torpedo_heavyhull" };
    static const char* kBotMove[2][4] = { { "starshipmovementconfig_xwingai_bot", "starshipmovementconfig_ywingai_bot", "starshipmovementconfig_awing_ai_bot", "starshipmovementconfig_uwing_ai_bot" },
        { "starshipmovementconfig_tiefighterai_bot", "starshipmovementconfig_tiebomberai_bot", "starshipmovementconfig_tieinterceptorai_bot", "starshipmovementconfig_tiereaperai_bot" } };
    static const bool kHasShields[2][4] = { { true, true, true, true }, { false, false, false, true } };
    auto addWant = [&](uint64_t cls, const char* name, uint64_t* at) { if (nw < 40) { Want& w = want[nw++]; w.cls = cls; strcpy_s(w.name, name); w.at = at; } };
    for (int f = 0; needLoadouts && f < 2; f++) for (int t = 0; t < 4; t++) {
        char nm[96];
        addWant(G(0x144a156a0), kStockColl[f], &src[f][t].stockColl);
        if (t) {
            sprintf_s(nm, "starshiphealthdamagedata_%s_ai_sp", kCreepShips[f][t]); addWant(G(0x144a8ca00), nm, &src[f][t].hull);
            if (kHasShields[f][t]) { sprintf_s(nm, "starshipshielddata_%s_ai_sp", kCreepShips[f][t]); addWant(G(0x144a8c980), nm, &src[f][t].shields); }
        }
        if (t == 1) addWant(G(0x144a156a0), kTorpColl[f], &src[f][t].torpColl);
        addWant(G(0x144ab56a0), kBotMove[f][t], &src[f][t].botMove);
    }
    static const char* kBlueprint[2][4] = { { "Starship_XWing_AI", "Starship_YWing_AI", "Starship_AWing_AI", "Starship_UWing_AI" },
        { "Starship_TieFighter_AI", "Starship_TieBomber_AI", "Starship_TieInterceptor_AI", "Starship_TieReaper_AI" } };
    uint64_t bp[2][4] = {};
    struct Link { uint64_t src, dst; }; Link links[128]; int nl = 0;
    char leaf[128];
    walk_partitions([&](uint64_t insts, uint16_t n) {
        uint64_t root = rd<uint64_t>(insts, 0); if (!root) return;
        uint64_t ci = rd<uint64_t>(root, 8);
        if (g_creepTorpedoes || g_creepWingmen || g_creepAggressive) {   // vs-AI bot logic loaded? (then the bots' torpedo weapons and runs stay stock)
            inst_leaf(root, leaf, sizeof leaf);
            if (!_strnicmp(leaf, "pf_pvb_botlogic", 15)) botsLoaded = true;
        }
        for (uint16_t i = 0; (nw || g_creepWingmen || g_creepAggressive) && i < n; i++) {   // the loadouts' sources (and the run blueprint), by type, then name
            uint64_t o = rd<uint64_t>(insts, i * 8ull); if (!o) continue;
            uint64_t oc = rd<uint64_t>(o, 8);
            if (oc != G(0x144a156a0) && oc != G(0x144a8ca00) && oc != G(0x144a8c980) && oc != G(0x144ab56a0) && oc != G(0x1449fc370)) continue;
            inst_leaf(o, leaf, sizeof leaf); _strlwr_s(leaf);
            if (oc == G(0x1449fc370)) {
                for (int b = 0; b < 2; b++) if (!torpWeapon[b] && !strcmp(leaf, kTorpWeapon[b])) torpWeapon[b] = o;
                // the AI bombs' run behaviour (creep_torpedo_runs): every bomb config carries the same blueprint
                if (!raidBp && !strncmp(leaf, "starshipweaponconfig_bomb_standard_", 35)) raidBp = rd<uint64_t>(o, 120);
                continue;
            }
            for (int w = 0; w < nw; w++) if (want[w].cls == oc && !*want[w].at && !strcmp(leaf, want[w].name)) *want[w].at = o;
        }
        if (ci == G(0x144ade220)) {   // a starship blueprint
            inst_leaf(root, leaf, sizeof leaf);
            for (int f = 0; f < 2; f++) for (int t = 0; t < 4; t++) if (!bp[f][t] && !_stricmp(leaf, kBlueprint[f][t])) bp[f][t] = root;
            return;
        }
        if (ci != G(0x144a56c40) && ci != G(0x1449fab00)) return;   // a level layer: its link connections
        uint64_t lc = rd<uint64_t>(root, 48); uint32_t cnt = lc ? rd<uint32_t>(lc - 4, 0) : 0;
        for (uint32_t k = 0; k < cnt && k < 16384; k++) {
            uint64_t src = rd<uint64_t>(lc, k * 24), dst = rd<uint64_t>(lc, k * 24 + 8);
            if (!src || !dst || rd<uint64_t>(src, 8) != G(0x144a176a0) || rd<uint64_t>(dst, 8) != G(0x1449fdc00)) continue;
            uint64_t prefab = rd<uint64_t>(src, 0xC0); if (!prefab) continue;
            if (_strnicmp(inst_leaf(prefab, leaf, sizeof leaf), "pf_pvp_creeplogic", 17)) continue;
            bool dup = false; for (int i = 0; i < nl; i++) if (links[i].src == src && links[i].dst == dst) dup = true;
            if (!dup && nl < 128) links[nl++] = { src, dst };
        }
    });
    // The lock filter (TargetTags 2) only with the test switch torpedo_capital_lock=1, and not with bomber_capital_only
    // (every bomber target is a capital ship then): 2026-10-27, one torpedo a flight since it went in, many before.
    if (g_creepTorpedoes) {
        creep_torpedo_targets(torpWeapon, botsLoaded || g_bomberCapitalOnly || !g_torpCapitalLock);
        if (g_creepTorpMinLock != 500) creep_torpedo_minlock(torpWeapon, botsLoaded);
        if (g_torpRuns) creep_torpedo_runs(torpWeapon, raidBp, botsLoaded);   // test switch torpedo_runs=0: no runs
    }
    else if (g_creepAggressive) creep_run_data(raidBp, botsLoaded, 1);   // bombers that keep their bombs: mid-range runs
    if (g_creepWingmen) creep_wingmen_raid(raidBp, botsLoaded);
    // packs: one per placement, its faction from the prefab's name
    struct Pack { uint64_t src; int faction, n; uint64_t dst[4]; float c[3]; } packs[16]; int np = 0;
    for (int i = 0; i < nl; i++) {
        int at = -1; for (int j = 0; j < np; j++) if (packs[j].src == links[i].src) at = j;
        if (at < 0) {
            if (np >= 16) continue;
            inst_leaf(rd<uint64_t>(links[i].src, 0xC0), leaf, sizeof leaf); _strlwr_s(leaf);
            int fac = strstr(leaf, "xwing") ? 0 : strstr(leaf, "tiefighter") ? 1 : -1; if (fac < 0) continue;
            at = np++; packs[at] = {}; packs[at].src = links[i].src; packs[at].faction = fac;
        }
        Pack& p = packs[at]; if (p.n >= 4) continue;
        p.dst[p.n++] = links[i].dst;
        for (int a = 0; a < 3; a++) p.c[a] += rd<float>(links[i].dst, 176 + 4 * a);
    }
    for (int i = 0; i < np; i++) for (int a = 0; a < 3; a++) packs[i].c[a] /= packs[i].n;
    static uint64_t logged[64]; static int nlogged;
    auto once = [&](uint64_t key) { for (int i = 0; i < nlogged; i++) if (logged[i] == key) return false; logged[nlogged++ % 64] = key; if (nlogged > 64) nlogged = 64; return true; };
    for (int f = 0; f < 2; f++) {
        int idx[16], m = 0; for (int i = 0; i < np; i++) if (packs[i].faction == f) idx[m++] = i;
        if (!m) continue;
        int axis = 0; float best = -1;   // the line they spawn in: the axis their centres spread most on
        for (int a = 0; a < 3; a++) {
            float lo = 1e30f, hi = -1e30f; for (int k = 0; k < m; k++) { float v = packs[idx[k]].c[a]; lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
            if (hi - lo > best) { best = hi - lo; axis = a; }
        }
        for (int x = 1; x < m; x++) for (int y = x; y > 0 && packs[idx[y - 1]].c[axis] > packs[idx[y]].c[axis]; y--) { int t = idx[y]; idx[y] = idx[y - 1]; idx[y - 1] = t; }
        for (int k = 0; k < m && k < 3; k++) {
            Pack& p = packs[idx[k]]; int t = g_creepPack[f][k];
            if (once(p.src)) status("creep: %s pack %d at (%.0f, %.0f, %.0f), %d spawn points: %s", kCreepFaction[f], k + 1, p.c[0], p.c[1], p.c[2], p.n,
                                    t ? kCreepShips[f][t] : "stock");
            bool torpedoes = g_creepTorpedoes && t == 1;   // the Y-wing and the TIE bomber
            if (!t && !g_creepBotHandling) continue;   // a stock pack stays as the level has it
            uint64_t target = t ? bp[f][t] : 0;
            if (t && !target) { if (once(p.src ^ 1)) status("creep: %s pack %d: %s isn't loaded in this level; the pack stays stock", kCreepFaction[f], k + 1, kBlueprint[f][t]); continue; }
            // a changed pack: no loadout (the blueprint's defaults) unless torpedoes or the bots' handling ask for one;
            // a stock pack (only with the bots' handling): its creep configs and the bots' movement
            uint64_t loadout = torpedoes || g_creepBotHandling ? creep_loadout(f, t, src[f][t], torpedoes, g_creepBotHandling) : 0;
            if (!t && !loadout) continue;
            for (int d = 0; d < p.n && t == 1; d++) {   // the Y-wing and TIE bomber packs' spawn points, for h_scorer
                bool have = false; int freeAt = -1;
                for (int i = 0; i < 16; i++) { if (g_bomberRefs[i] == p.dst[d]) have = true; if (!g_bomberRefs[i] && freeAt < 0) freeAt = i; }
                if (!have && freeAt >= 0) InterlockedExchange64((volatile LONG64*)&g_bomberRefs[freeAt], (LONG64)p.dst[d]);
            }
            for (int d = 0; d < p.n; d++) {
                uint64_t ship = t ? target : rd<uint64_t>(p.dst[d], 0xC0);
                if (rd<uint64_t>(p.dst[d], 0xC0) == ship && rd<uint64_t>(p.dst[d], 0x1F0) == loadout) continue;
                if (write_u64(p.dst[d] + 0xC0, ship) && write_u64(p.dst[d] + 0x1F0, loadout))
                    status("creep: %s pack %d spawn point %llx now spawns %s%s%s", kCreepFaction[f], k + 1, p.dst[d], kCreepShips[f][t],
                           torpedoes && loadout ? " with torpedoes" : "", g_creepBotHandling && loadout ? " with the bots' handling" : "");
            }
        }
        if (m != 3 && once(0xC0EE0000ull + f)) status("creep: %d %s packs in this level (3 expected); only the first 3 follow the profile", m, kCreepFaction[f]);
    }
}

// ---- Creep missiles and torpedoes ([creep] missiles=1) --------------------------------------------------------------
// The squad behaviours (bhv_aisquadron_basic_imperial and _rebel, shared by creep and the vs-AI bots) let a ship fire
// missiles, torpedoes included (they go through the missile rule), only if all hold (notes/creep-ai.md 3.2): the
// AllowMissileFiring input, which the bots feed and the creep don't, so it stays at its default, off; a test that the
// ship is a bot or its squadron's leader, which creep wingmen fail; and a minimum target distance the rule takes from a
// SelectFloat, 700 m for AI and 200 m for bots, picked when the bot test changes. For creep: the input's default goes on
// (the Bool feeding the DefaultValue of the PropertyDefaultValue that the input feeds), the bot test's Bool goes on (the
// one Bool that feeds an Or alongside DogfighterInfo.IsBot; it also gives creep the bots' tail-attack dogfighting), and
// the SelectFloat starts on the bots' 200 m. The bots set all three themselves, so they don't change. The behaviours also
// drive the campaign's AI, so this waits for a Fleet Battles creep logic prefab to be loaded; it runs on the 2-second
// scan, ahead of the first creep launch. Field ids (djb2-xor of the names): AllowMissileFiring 1336394533, InValue
// 1658462601, DefaultValue 2066049125, Value 225375086, IsBot 215512070, MinTargetDistance -1814841251, Out 193453899.
// BoolEntityData 0x144a58840: DefaultValue +36. SelectFloatEntityData: InputSelect +48.
// On the 2-second scan: put each creep squadron's data EngagementMode where the phase wants it (see CreepPhase), once
// the squadron has had time to register with the Aggressive rules (a few seconds after its controller first reported a
// behaviour), and only while its creep logic prefab is still loaded (the pointers belong to that level).
static void creep_mode_scan() {
    if (!g_creepAggressive) return;
    bool loaded[2] = {}; char leaf[128];
    walk_partitions([&](uint64_t insts, uint16_t n) {
        uint64_t root = rd<uint64_t>(insts, 0); if (!root) return;
        inst_leaf(root, leaf, sizeof leaf);
        if (_strnicmp(leaf, "pf_pvp_creeplogic", 17)) return;
        uint64_t objs = rd<uint64_t>(root, 72); uint32_t no = objs ? rd<uint32_t>(objs - 4, 0) : 0;
        for (uint32_t k = 0; k < no && k < 4096; k++) {
            uint64_t o = rd<uint64_t>(objs, k * 8);
            for (int f = 0; f < 2; f++) if (o && o == g_creepPh[f].squad) loaded[f] = true;
        }
    });
#ifdef PROBE_TEST
    const ULONGLONG settle = 1000;
#else
    const ULONGLONG settle = 5000;
#endif
    for (int f = 0; f < 2; f++) {
        CreepPhase& cp = g_creepPh[f];
        ULONGLONG seen = (ULONGLONG)cp.seenAt;
        if (!loaded[f] || !seen || GetTickCount64() - seen < settle) continue;
        int32_t want = cp.wantMode;
        if (rd<int32_t>(cp.squad, 32) == want) continue;
        if (leaf_write(cp.squad + 32, true, want))
            status("creep: %s squadron: %s", kCreepFaction[f], want == cp.aggMode
                ? "bomb raids on the nearest enemy capital ship (offense)" : "bomb raids only on their current target");
    }
}
// Creep wingmen ([creep] wingmen=1; 2026-10-27, notes/creep-ai.md section 9). Stock, two things are a squadron leader's
// alone: attacking on its own (the Or that also takes DogfighterInfo.IsBot, opened through its Bool; missiles=1 opens it
// too), and the weapon behaviours the two loadout nodes (#288/#289, aux One and Two) run, the bombers' runs among them:
// their Enabled is And(..., Or(Or(Not IsInSquadron, IsSquadronLeader), DogfighterInfo field 1354684448, ShipClass ==
// Support, interface field 555857281)), which a creep wingman never passes. Nothing feeds that interface field for creep
// or bots, so there is no default to flip: its two connections (into the two Ors' In4, field 193450870) are repointed
// at the Out (193453899) of an existing Not node, Not(the AllowCountermeasures PropertyDefaultValue's HasValueSet) (In
// 5862146, HasValueSet -645606250, AllowCountermeasures -1073460925), which is true for creep (they don't feed that
// input) and false for the bots (they do), so the bots' gate doesn't change. Done only when the wiring is exactly that:
// two such connections, one such Not node fed from that input's default. The bombers' run data also has
// RunOnlyForLeaders (+72), which creep_wingmen_raid turns off.
static void creep_wingmen_graph(uint64_t root, const char* leaf, uint64_t pc, uint32_t np, uint64_t flag) {
    auto src = [&](uint32_t k) { return rd<uint64_t>(pc, k * 32); };
    auto dst = [&](uint32_t k) { return rd<uint64_t>(pc, k * 32 + 8); };
    auto sf = [&](uint32_t k) { return rd<int32_t>(pc, k * 32 + 16); };
    auto tf = [&](uint32_t k) { return rd<int32_t>(pc, k * 32 + 20); };
    uint32_t in4[4]; int n4 = 0, nNot = 0; uint64_t notNode = 0, notPdv = 0;
    for (uint32_t k = 0; k < np && k < 8192; k++)
        if (sf(k) == -645606250 && tf(k) == 5862146) { notNode = dst(k); notPdv = src(k); nNot++; }
    // the interface field's connections, or ones already repointed at that Not's Out (193450870 is any And/Or's In4,
    // which other nodes' Outs feed too: 2026-10-27 flight, "gate inputs 4", from counting every Out into an In4)
    for (uint32_t k = 0; k < np && k < 8192; k++)
        if (tf(k) == 193450870 && (sf(k) == 555857281 || (nNot == 1 && sf(k) == 193453899 && src(k) == notNode)) && n4 < 4) in4[n4++] = k;
    bool fedByCountermeasures = false;
    for (uint32_t k = 0; nNot == 1 && k < np && k < 8192; k++) if (dst(k) == notPdv && sf(k) == -1073460925) fedByCountermeasures = true;
    bool shaped = n4 == 2 && nNot == 1 && fedByCountermeasures;
    for (int i = 0; shaped && i < n4; i++) shaped = sf(in4[i]) == 555857281 || src(in4[i]) == notNode;
    if (!shaped) {
        static uint64_t warned[8]; bool w = false; for (uint64_t x : warned) if (x == root) w = true;
        if (!w) { for (uint64_t& x : warned) if (!x) { x = root; break; }
                  status("creep: %s isn't laid out as expected for wingmen (gate inputs %d, Not node %d, fed by the countermeasure input %d); wingmen left as they are", leaf, n4, nNot, (int)fedByCountermeasures); }
        return;
    }
    bool did = false, ok = true;
    for (int i = 0; i < n4; i++) {
        uint64_t e = pc + in4[i] * 32ull;
        if (src(in4[i]) == notNode && sf(in4[i]) == 193453899) continue;
        ok = write_u64(e, notNode) && leaf_write(e + 16, true, 193453899) && ok; did = true;
    }
    if (rd<uint8_t>(flag, 36) != 1) { ok = leaf_write(flag + 36, false, 1, true) && ok; did = true; }
    if (did) status("creep: %s: %s", leaf, ok ? "wingmen attack on their own and run their weapon behaviours (the bombers' runs) like the leader" : "a write failed; wingmen may stay as they are");
}
static void creep_behaviour_scan() {
    if (!g_creepMissiles && !g_creepWingmen) return;
    bool fleet = false; uint64_t bhv[8]; int nb = 0; char leaf[128];
    walk_partitions([&](uint64_t insts, uint16_t n) {
        uint64_t root = rd<uint64_t>(insts, 0); if (!root) return;
        inst_leaf(root, leaf, sizeof leaf);
        if (!_strnicmp(leaf, "pf_pvp_creeplogic", 17)) fleet = true;
        else if ((!_stricmp(leaf, "bhv_aisquadron_basic_imperial") || !_stricmp(leaf, "bhv_aisquadron_basic_rebel")) && nb < 8) bhv[nb++] = root;
    });
    if (!fleet) return;
    for (int b = 0; b < nb; b++) {
        uint64_t root = bhv[b]; inst_leaf(root, leaf, sizeof leaf);
        uint64_t pc = rd<uint64_t>(root, 40); uint32_t np = pc ? rd<uint32_t>(pc - 4, 0) : 0;
        auto src = [&](uint32_t k) { return rd<uint64_t>(pc, k * 32); };
        auto dst = [&](uint32_t k) { return rd<uint64_t>(pc, k * 32 + 8); };
        auto sf = [&](uint32_t k) { return rd<int32_t>(pc, k * 32 + 16); };
        auto tf = [&](uint32_t k) { return rd<int32_t>(pc, k * 32 + 20); };
        uint64_t pdv = 0, allow = 0, sel = 0, flag = 0; int nPdv = 0, nAllow = 0, nSel = 0, nFlag = 0;
        for (uint32_t k = 0; k < np && k < 8192; k++) {
            if (sf(k) == 1336394533 && tf(k) == 1658462601) { pdv = dst(k); nPdv++; }
            if (sf(k) == 193453899 && tf(k) == -1814841251) { sel = src(k); nSel++; }
        }
        for (uint32_t k = 0; k < np && k < 8192; k++) {
            if (nPdv == 1 && dst(k) == pdv && tf(k) == 2066049125 && rd<uint64_t>(src(k), 8) == G(0x144a58840)) { allow = src(k); nAllow++; }
            if (sf(k) != 225375086 || rd<uint64_t>(src(k), 8) != G(0x144a58840)) continue;
            for (uint32_t j = 0; j < np && j < 8192; j++)   // a Bool into a node that IsBot also feeds
                if (sf(j) == 215512070 && dst(j) == dst(k)) { if (flag != src(k)) nFlag++; flag = src(k); }
        }
        if (nAllow != 1 || nSel != 1 || nFlag != 1) {
            static uint64_t warned[8]; bool w = false; for (uint64_t x : warned) if (x == root) w = true;
            if (!w) { for (uint64_t& x : warned) if (!x) { x = root; break; }
                      status("creep: %s isn't laid out as expected (missile default %d, distance selector %d, bot test %d); creep missiles left off", leaf, nAllow, nSel, nFlag); }
            continue;
        }
        if (g_creepMissiles && !(rd<uint8_t>(allow, 36) == 1 && rd<uint8_t>(flag, 36) == 1 && rd<int32_t>(sel, 48) == 1)) {
            bool ok = leaf_write(allow + 36, false, 1, true) && leaf_write(flag + 36, false, 1, true) && leaf_write(sel + 48, true, 1);
            status("creep: %s: %s", leaf, ok ? "creep fire missiles and torpedoes, wingmen too, from 200 m" : "a write failed; creep missiles may stay off");
        }
        if (g_creepWingmen) creep_wingmen_graph(root, leaf, pc, np, flag);
    }
}

static void __fastcall h_gstick(uint64_t gs, uint64_t a2, uint64_t a3, uint64_t a4) {
    o_gstick(gs, a2, a3, a4);
    { static ULONGLONG lastScan; ULONGLONG nowScan = GetTickCount64();
      if (nowScan - lastScan >= 2000) { lastScan = nowScan; value_engine_scan(); creep_scan(); creep_behaviour_scan(); creep_mode_scan(); } }
    creep_track();
    // phases: 0 waiting for the delay; 1 startup data loading; 2 game config loading; 3 spawned (or given up)
    static ULONGLONG first, phaseStart, lastNote; static int phase; static bool announced, pumping;
    static uint32_t handle; static const char* bundle;
    if (!g_dedicated) return;
    if (phase == 3) {  // after the spawn: a heartbeat every 10 s, so the log shows the simulation still ticking
        static ULONGLONG lastBeat; static uint64_t ticks; ticks++;
        // Post the load-level message once the dedicated level lookup (FUN_140cd57f0, over the list FUN_140cd55e0 builds from
        // LevelListReport.BuiltLevels = LevelDescriptionAssets) finds the root level. If it doesn't, re-run the preload once
        // a second (its descriptions live in game/incomgameconfiguration, which may still be registering), for up to 30 s.
        static bool posted, gaveUp; static ULONGLONG lastPreload, postWaitStart;
        if (!posted && !gaveUp && g_postLoadLevel && g_spawnLs && rd<uint64_t>(gs, 0xF8) && committed(G(0x140ba0c20)) &&
            memcmp((void*)G(0x140ba0c20), expect_bytes(0x140ba0c20, kPostLoadLevelPrologue, 6), sizeof kPostLoadLevelPrologue) == 0) {
            typedef uint64_t (__fastcall* lookup_t)(const char* levelName);
            typedef void (__fastcall* preload_t)();
            typedef void (__fastcall* post_t)(void* levelSetup, uint64_t unused, uint64_t flag, uint64_t u4);
            ULONGLONG pt = GetTickCount64();
            if (!postWaitStart) postWaitStart = pt;
            uint64_t lb = rd<uint64_t>(G(0x143ea0768), 0), le = rd<uint64_t>(G(0x143ea0770), 0);
            uint64_t found = ((lookup_t)G(0x140cd57f0))(g_rootLevel);
            if (found) {
                status("3b: level lookup found '%s' (%p); list has %llu entries", g_rootLevel, (void*)found, (le - lb) / 8);
                status("3b: posting ServerLoadLevelMessageBase (FUN_140ba0c20) with the spawn LevelSetup");
                ((post_t)G(0x140ba0c20))(g_spawnLs, 0, 0, 0);
                status("3b: load-level message posted");
                posted = true;
            } else if (pt - lastPreload >= 1000) {
                lastPreload = pt;
                char names[600] = ""; size_t n = 0;
                for (uint64_t q = lb; q && q < le && n < 4; q += 8, n++) {
                    strncat_s(names, safe_str(rd<uint64_t>(rd<uint64_t>(q, 0), 0x30)), _TRUNCATE); strncat_s(names, "; ", _TRUNCATE);
                }
                status("3b: level lookup: '%s' not in the list (%llu entries: %s); re-running the preload", g_rootLevel, (le - lb) / 8, names);
                ((preload_t)G(0x140cd55e0))();
                if (pt - postWaitStart > 30000) { status("3b: level lookup: gave up after 30 s; not posting"); gaveUp = true; }
            }
        }
        ULONGLONG t = GetTickCount64();
        if (!lastBeat) lastBeat = t;
        if (t - lastBeat >= 10000) { lastBeat = t; status("3b: heartbeat: %llu ticks since spawn, server=%p", ticks, (void*)rd<uint64_t>(gs, 0xF8)); }
        return;
    }
    if (!announced) { status("3b: GameSimulation ticking: gameSim=%p server=%p client=%p", (void*)gs, (void*)rd<uint64_t>(gs, 0xF8), (void*)rd<uint64_t>(gs, 0xE0)); announced = true; }
    ULONGLONG now = GetTickCount64();
    if (!first) first = now;
    if (phase == 0) {
        if (now - first < (ULONGLONG)g_spawnDelayMs) return;
        if (rd<uint64_t>(gs, 0xF8)) { status("3b: a server already exists; not spawning"); phase = 3; return; }
        bool engine = committed(G(0x14021b2b0)) && committed(G(0x1442f7c68)) && committed(G(0x1442d18c0));
        if (!g_loadStartupData || !engine) {
            if (g_loadStartupData) status("3b: bundles: engine not present (test target); skipped");
            phase = 3; do_spawn(gs); return;
        }
        bundle = "Systems/FrostbiteStartupData"; handle = queue_bundle(bundle); phase = 1; phaseStart = lastNote = now;
        return;
    }
    if (!bundle_done(handle)) {
        if (now - phaseStart > 5000 && !pumping) { pumping = true; status("3b: bundle %s: not done after 5 s; pumping the resource manager each tick", bundle); }
        if (pumping) ((bq_pump_t)G(0x1401f7960))(G(0x1442d18c0), 1, 0);
        if (now - lastNote > 5000) { lastNote = now; status("3b: bundle %s: still loading (%llu ms)", bundle, now - phaseStart); }
        return;
    }
    release_bundle(bundle, handle, now - phaseStart);
    pumping = false;
    if (phase == 1 && g_loadGameConfig) {
        const char* cfg = (const char*)rd<uint64_t>(G(0x1441e2938), 0);
        if (cfg && rd<uint8_t>((uint64_t)cfg, 0)) { bundle = cfg; handle = queue_bundle(bundle); phase = 2; phaseStart = lastNote = now; return; }
    }
    phase = 3;
    if (rd<uint64_t>(gs, 0xF8)) { status("3b: a server already exists; not spawning"); return; }
    do_spawn(gs);
}

// FUN_140cd6420 (level-load step 2 completion): once the level's bundle jobs finish it looks up the LevelData in the server's
// resource compartment (FUN_1401f9860(server+0x90, LevelData type 0x144a17ea0)) and proceeds even if that is still null, which
// asserts (0xdeadcccc) or races on. On a dedicated process "job done" can precede the bundle's objects being registered.
// So: when both jobs are done but LevelData isn't findable yet, report "still loading" (0) and ask again next tick (30 s cap).
// FUN_14057dd20(grid, MaterialDecl* a, MaterialDecl* b): the material grid's relation lookup (notes/material-grid.md),
// asked by the health components on every hit. A MaterialDecl packs flags in bits 0-7 (bit 7: set; otherwise the
// default material), the effect material in bits 8-19 and the property index in bits 20-31, which picks the damage
// relation. material_log=1 logs each distinct (a, b, caller) once, so a test flight shows which pairs damage asks for.
// FUN_1416bcab0(spawn entity, request): ServerStarshipSpawnEntity's spawn (its vtable 0x143618a18, slot 27), for players,
// creep and capital ships alike. The entity's data is at +0x38. It calls the engine's generic spawn FUN_140c0c550 and
// returns the new ship, or 0 when the spawn didn't happen. (Where the request keeps the spawn position isn't known: +0x30,
// the first guess, read 0 for every spawn in game, so the position isn't logged.) spawn_log=1 logs each call (2026-10-23: one of Zavian Abyss's X-wing creep packs spawns 3 ships of 5,
// every time, possibly at points inside an asteroid).
typedef uint64_t(__fastcall* sspawn_t)(uint64_t ent, uint64_t req);
static sspawn_t o_sspawn;
static uint64_t __fastcall h_sspawn(uint64_t ent, uint64_t req) {
    uint64_t r = o_sspawn(ent, req);
    creep_note_spawn(rd<uint64_t>(ent, 0x38), r);
    if (g_spawnLog) status("spawn: entity %llx (data %llx) -> %s %llx", ent, rd<uint64_t>(ent, 0x38), r ? "ship" : "NOTHING SPAWNED", r);
    return r;
}
// FUN_14168a8b0(component, out, info): ServerStarshipHealthComponent's direct hit (vtable slot 41, notes/material-grid.md),
// on the server, for every starfighter, player or AI. hit_log=1 logs where each hit landed in the struck ship's own frame
// (#20: checking hitboxes and ship size against where lasers actually land; the game places each hitbox mesh at its
// blueprint part, so compare against the meshes placed there, not as the physics file stores them). The
// info's layout isn't reflected: +0x20 is the attacker's material, +0x30, +0x40, +0x70 and +0x90 are vectors, +0x58 the
// part, +0x60 the kind (2 = collision), +0x84 the damage. Which vector is the impact point, and where the ship's transform
// lives, are found from the data: the transform is the first orthonormal block (three unit, mutually orthogonal vectors,
// then a translation) within 30 m of a candidate point, in the component, its physics component (+0x990), the physics
// body, or an object one of those points to. Once found, its place is kept per ship. The ship is named by its
// StarshipHealthDamageData asset, found through the component's data (its HealthDamageDataAsset is at +0x90).
typedef uint64_t(__fastcall* shiphit_t)(uint64_t comp, uint64_t out, uint64_t info);
static shiphit_t o_shiphit;
struct HitShip { uint64_t comp; int obj, ptrOff, off, cand; char name[160]; int misses; float lastT[3]; LONGLONG lastQ; };
static volatile LONG g_hitRaw;   // the first few hits' info vectors are logged raw
static HitShip g_hitShips[512]; static int g_hitShipCount; static SRWLOCK g_hitLock = SRWLOCK_INIT;
static const uint32_t kHitCand[3] = { 0x30, 0x70, 0x90 };
static const char* kHitObj[3] = { "component", "physics component", "physics body" };
static uint64_t hit_body(uint64_t pc) {   // the body the hit function itself reaches when the physics component forwards
    if (!pc || rd<uint64_t>(rd<uint64_t>(pc, 0), 0x1c8) != G(0x141ca4ec0)) return 0;
    uint64_t w = rd<uint64_t>(pc, 0x30), idp = rd<uint64_t>(pc, 0x38);
    if (!w || !idp) return 0;
    uint32_t id = rd<uint32_t>(idp, 0);
    uint64_t t1 = rd<uint64_t>(w, 0x138); if (!t1) return 0;
    uint64_t t2 = rd<uint64_t>(t1, ((id >> 16) & 0xff) * 8); if (!t2) return 0;
    uint64_t t3 = rd<uint64_t>(t2, 0x138); if (!t3) return 0;
    return rd<uint64_t>(t3, (id & 0xffff) * 8);
}
static uint64_t hit_obj(uint64_t comp, int obj, int ptrOff) {
    uint64_t pc = rd<uint64_t>(comp, 0x990);
    uint64_t b = obj == 0 ? comp : obj == 1 ? pc : hit_body(pc);
    if (b && ptrOff >= 0) b = rd<uint64_t>(b, ptrOff);
    return b < 0x10000 ? 0 : b;
}
static bool hit_frame(uint64_t at, float m[12]) {   // three orthonormal vectors and a translation, 16 bytes apart
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) m[i * 3 + k] = rd<float>(at, i * 16 + k * 4);
    for (int i = 0; i < 12; i++) if (!(m[i] == m[i]) || fabsf(m[i]) > 1e6f) return false;
    for (int i = 0; i < 3; i++) for (int j = i; j < 3; j++) {
        float d = m[i * 3] * m[j * 3] + m[i * 3 + 1] * m[j * 3 + 1] + m[i * 3 + 2] * m[j * 3 + 2];
        if (fabsf(d - (i == j ? 1.0f : 0.0f)) > 0.002f) return false;
    }
    return true;
}
static float hit_dist2(const float m[12], const float p[3]) {
    float dx = p[0] - m[9], dy = p[1] - m[10], dz = p[2] - m[11];
    return dx * dx + dy * dy + dz * dz;
}
static bool hit_find(uint64_t comp, uint64_t info, HitShip& s) {
    for (int obj = 0; obj < 3; obj++) for (int ptrOff = -8; ptrOff < 0x100; ptrOff += 8) {
        uint64_t b = hit_obj(comp, obj, ptrOff < 0 ? -1 : ptrOff);
        if (!b) continue;
        for (int off = 0; off < 0x400; off += 16) {
            float m[12];
            if (!hit_frame(b + off, m)) continue;
            // a frame at the world origin is a local or identity block, not a ship flying in a match (2026-10-23: the
            // first flight matched an identity block at the origin against a unit direction in the info)
            if (m[9] * m[9] + m[10] * m[10] + m[11] * m[11] < 4.0f) continue;
            for (int c = 0; c < 3; c++) {
                float p[3] = { rd<float>(info, kHitCand[c]), rd<float>(info, kHitCand[c] + 4), rd<float>(info, kHitCand[c] + 8) };
                if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < 4.0f) continue;   // a direction, not a point
                if (hit_dist2(m, p) < 30.0f * 30.0f) {
                    s.obj = obj; s.ptrOff = ptrOff < 0 ? -1 : ptrOff; s.off = off; s.cand = c;
                    char where[96];
                    if (s.ptrOff >= 0) sprintf_s(where, "the %s's pointer at +%#x, then +%#x", kHitObj[obj], s.ptrOff, off);
                    else sprintf_s(where, "the %s+%#x", kHitObj[obj], off);
                    status("hit: %s: transform found in %s; the impact point is info+%#x; axes (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)",
                           s.name, where, kHitCand[c], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
                    return true;
                }
            }
        }
    }
    return false;
}
static void hit_name(uint64_t comp, char* out, size_t n) {
    uint16_t lo = rd<uint16_t>(G(0x144a8ca00), 0x48), hi = rd<uint16_t>(G(0x144a8ca00), 0x4a);
    for (uint32_t off = 0; off < 0x400; off += 8) {
        uint64_t q = rd<uint64_t>(comp, off); if (q < 0x10000) continue;
        uint64_t a = rd<uint64_t>(q, 0x90); if (a < 0x10000) continue;
        uint64_t ci = rd<uint64_t>(a, 8); if (ci < 0x10000) continue;
        uint16_t id = rd<uint16_t>(ci, 0x48);
        if ((uint16_t)(id - lo) <= (uint16_t)(hi - lo)) { safe_copy(rd<uint64_t>(a, 0x18), out, n); if (out[0]) return; }
    }
    strcpy_s(out, n, "?");
}
static uint64_t __fastcall h_shiphit(uint64_t comp, uint64_t out, uint64_t info) {
    if (g_hitLog && comp && info) {
        if (InterlockedIncrement(&g_hitRaw) <= 8)
            status("hit: raw info +0x30 (%.2f %.2f %.2f) +0x40 (%.3f %.3f %.3f) +0x70 (%.3f %.3f %.3f) +0x90 (%.3f %.3f %.3f) part %d, called from %p",
                   rd<float>(info, 0x30), rd<float>(info, 0x34), rd<float>(info, 0x38), rd<float>(info, 0x40), rd<float>(info, 0x44),
                   rd<float>(info, 0x48), rd<float>(info, 0x70), rd<float>(info, 0x74), rd<float>(info, 0x78), rd<float>(info, 0x90),
                   rd<float>(info, 0x94), rd<float>(info, 0x98), rd<int32_t>(info, 0x58), _ReturnAddress());
        AcquireSRWLockExclusive(&g_hitLock);
        HitShip* s = nullptr;
        for (int i = 0; i < g_hitShipCount && !s; i++) if (g_hitShips[i].comp == comp) s = &g_hitShips[i];
        if (!s && g_hitShipCount < 512) {
            s = &g_hitShips[g_hitShipCount++]; *s = {}; s->comp = comp; s->obj = -1;
            hit_name(comp, s->name, sizeof s->name);
        }
        if (s && s->obj < 0 && s->misses < 3 && !hit_find(comp, info, *s)) {
            if (++s->misses <= 3)
                status("hit: %s: no transform found near info+0x30 (%.1f %.1f %.1f), +0x70 (%.1f %.1f %.1f) or +0x90 (%.1f %.1f %.1f)", s->name,
                       rd<float>(info, 0x30), rd<float>(info, 0x34), rd<float>(info, 0x38), rd<float>(info, 0x70), rd<float>(info, 0x74),
                       rd<float>(info, 0x78), rd<float>(info, 0x90), rd<float>(info, 0x94), rd<float>(info, 0x98));
        }
        if (s && s->obj >= 0) {
            float m[12]; uint64_t b = hit_obj(comp, s->obj, s->ptrOff);
            if (b && hit_frame(b + s->off, m)) {
                uint32_t c = kHitCand[s->cand];
                float d[3] = { rd<float>(info, c) - m[9], rd<float>(info, c + 4) - m[10], rd<float>(info, c + 8) - m[11] };
                float l[3]; for (int i = 0; i < 3; i++) l[i] = d[0] * m[i * 3] + d[1] * m[i * 3 + 1] + d[2] * m[i * 3 + 2];
                uint32_t a = rd<uint32_t>(info, 0x20);
                // the ship's velocity, from its frame now and at its last hit (in its own frame; hits trail fast ships, so
                // the trail can be measured against the speed: 2026-10-23, TIE fighters' hits reached 20 m behind them)
                LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
                char vel[64] = "velocity ?";
                float mv[3] = { m[9] - s->lastT[0], m[10] - s->lastT[1], m[11] - s->lastT[2] };
                double dt = s->lastQ ? (double)(q.QuadPart - s->lastQ) / (double)f.QuadPart : 0;
                if (dt > 0.02 && dt < 1.0 && mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2] > 1e-4f) {
                    float lv[3]; for (int i = 0; i < 3; i++) lv[i] = (float)((mv[0] * m[i * 3] + mv[1] * m[i * 3 + 1] + mv[2] * m[i * 3 + 2]) / dt);
                    sprintf_s(vel, "velocity (%.1f, %.1f, %.1f)", lv[0], lv[1], lv[2]);
                }
                if (mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2] > 1e-4f || !s->lastQ) {
                    s->lastT[0] = m[9]; s->lastT[1] = m[10]; s->lastT[2] = m[11]; s->lastQ = q.QuadPart; }
                status("hit: %s part %d kind %d material %u damage %.1f at (%.2f, %.2f, %.2f) %s", s->name, rd<int32_t>(info, 0x58),
                       rd<int32_t>(info, 0x60), (a & 0x80) ? a >> 20 : 0, rd<float>(info, 0x84), l[0], l[1], l[2], vel);
            }
        }
        ReleaseSRWLockExclusive(&g_hitLock);
    }
    return o_shiphit(comp, out, info);
}
// capital_damage_log=1 (2026-10-27): what reaches each capital ship's hull and shield bubble, and from whom (creep torpedo
// hits on the Raider seemed to do nothing). The hull: every direct hit (0x1417db200) and blast (0x1417dbca0) builds a hit
// record and passes it to FUN_1417dbe90(component, out, record, skipTeamFilter), which drops it through 0x1417da0e0 when the
// component ignores AI damage (+0x1c8), the record's flags (+0xa4) lack 8, and its attacker (+0xb0, the hit's +0xc0) is
// neither a player directly ([[+8]] - 8) nor a ServerStarshipEntity (through [[+0x40]] - 8) whose byte +0x338 is set. The
// record's +0x88 is the damage after the material grid; the health is the component's vtable +0x108, read around the call
// as the hit functions do. The shield bubble: ServerIncomShieldHealthComponent's direct hit (0x140582ab0) and blast
// (0x140582d90) take (component, out, info); the damage asked is info +0x84 (direct) or +0x74 (blast), the attacker info
// +0xc0. Out's first float isn't what they took off (flown, the Raider's 17000 shield "took" 92 blasts of 3400), so like
// the hull's, a shield's health is read around the call: both components' vtable +0x108 is 0x140412280, the float at +0x20.
static const char* kCapFrom[4] = { "bomber creep", "starships without the player mark", "players or marked starships", "no starship" };
struct CapStat { uint64_t comp; bool shield; uint8_t ignoresAI; char name[96]; int hits[4], dropped[4]; double asked[4], dealt[4]; ULONGLONG since; };
static CapStat g_capStats[64]; static int g_capStatCount; static SRWLOCK g_capLock = SRWLOCK_INIT;
static int cap_from(uint64_t inst, uint8_t* mark) {   // the drop rule's own reads, without calling into the game
    *mark = 0; if (!inst) return 3;
    uint64_t p = rd<uint64_t>(inst, 8), pp = p ? rd<uint64_t>(p, 0) : 0;
    if (pp && pp != 8) return 2;
    uint64_t c = rd<uint64_t>(inst, 0x40), s = c ? rd<uint64_t>(c, 0) : 0; if (s) s -= 8;
    if (!s || rd<uint64_t>(s, 0) != G(0x1436184e8)) return 3;
    *mark = rd<uint8_t>(s, 0x338);
    for (int i = 0; i < 256; i++) if (g_bomberShips[i] == s) return 0;
    return *mark ? 2 : 1;
}
static void cap_name(uint64_t comp, bool shield, char* out, size_t n) {   // the health settings asset its data points at
    uint64_t want = shield ? G(0x144a244c0) : G(0x144a24a40);
    for (uint32_t off = 0; off < 0x200; off += 8) {
        uint64_t d = rd<uint64_t>(comp, off); if (d < 0x10000 || rd<uint64_t>(d, 8) != want) continue;
        uint64_t s = rd<uint64_t>(d, shield ? 160 : 128); if (s < 0x10000) continue;
        safe_copy(rd<uint64_t>(s, 0x18), out, n); if (out[0]) return;
    }
    strcpy_s(out, n, "?");
}
static float cap_health(uint64_t comp) {
    typedef float(__fastcall* health_t)(uint64_t);
    health_t f = (health_t)rd<uint64_t>(rd<uint64_t>(comp, 0), 0x108);
    if (!f) return -1;
    __try { return f(comp); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static void cap_note(uint64_t comp, bool shield, bool blast, int from, uint8_t mark, float asked, float dealt, bool dropped, bool creepThrough, float health) {
    AcquireSRWLockExclusive(&g_capLock);
    CapStat* s = nullptr;
    for (int i = 0; i < g_capStatCount && !s; i++) if (g_capStats[i].comp == comp) s = &g_capStats[i];
    if (!s && g_capStatCount < 64) {
        s = &g_capStats[g_capStatCount++]; *s = {}; s->comp = comp; s->shield = shield; s->since = GetTickCount64();
        s->ignoresAI = shield ? 0 : rd<uint8_t>(comp, 0x1c8); cap_name(comp, shield, s->name, sizeof s->name);
        status("capdmg: first hit on %s (%s%s)", s->name, shield ? "shield bubble" : "hull",
               shield ? "" : s->ignoresAI ? ", which ignores AI damage" : ", which takes AI damage");
    }
    if (s) {
        s->hits[from]++; s->asked[from] += asked; s->dealt[from] += dealt; if (dropped) s->dropped[from]++;
        if (blast) status("capdmg: %s %s: blast of %.0f from %s (mark %d) -> %.0f taken%s; health %.0f", s->name,
                          shield ? "shield" : "hull", (double)asked, kCapFrom[from], mark, (double)dealt,
                          dropped ? " (DROPPED: AI damage ignored)" : creepThrough ? " (creep: the ignore-AI rule set aside)" : "", (double)health);
        ULONGLONG now = GetTickCount64();
        if (now - s->since >= 5000) {
            char line[640]; size_t at = 0;
            for (int k = 0; k < 4; k++) if (s->hits[k])
                at += sprintf_s(line + at, sizeof line - at, "; %s %d hits, %.0f asked, %.0f taken%s", kCapFrom[k], s->hits[k],
                                s->asked[k], s->dealt[k], s->dropped[k] ? " (some dropped)" : "");
            status("capdmg: %s %s, %.1f s%s; health %.0f", s->name, shield ? "shield" : "hull", (now - s->since) / 1000.0, line, (double)health);
            for (int k = 0; k < 4; k++) { s->hits[k] = s->dropped[k] = 0; s->asked[k] = s->dealt[k] = 0; }
            s->since = now;
        }
    }
    ReleaseSRWLockExclusive(&g_capLock);
}
// Creep damage counts against capital ships ([creep] aggressive). Every PvP level connects a level field (default true)
// into each capital ship's IgnoreAIDamage (blueprint interface field -1337701026 -> its health component; the component's
// property handler, vtable slot 2 0x1417d9450, sets +0x1c8). FUN_1417da0e0(capital health component, hit record) then
// says to drop a hit from a starship without the mark (+0x338), which is creep, and four places ask it: the hull's apply
// (0x1417dc005), the shield bubble (its vtable 0x14362d508 slot 43, 0x1417e1c70, asking with the ship's hull component
// [+0x570]), the turrets (0x1416ed5b0) and the weak points (0x1417dfc50). Flown 2026-10-27: every creep torpedo and laser
// on the Raider, corvette, frigates and cruisers was dropped, hull and shield, while a player's counted. So the check is
// replaced whole, never called: the same reads in the same order (flag 8 on the record lets anything through; no flag on
// the component, no attacker, or a player behind it ([[attacker+8]] - 8) keeps the hit; else the attacker's starship,
// cast through 0x1412631b0 from [[attacker+0x40]] - 8, keeps it if marked), and with aggressive creep an unmarked
// starship's hit is kept too. Hits with no starship behind them keep the stock rule.
typedef bool(__fastcall* aicheck_t)(uint64_t comp, uint64_t rec);
static aicheck_t o_aicheck;   // its trampoline is never called
static bool __fastcall h_aicheck(uint64_t comp, uint64_t rec) {
    if (rd<uint8_t>(rec, 0xa4) & 8) return false;
    if (!rd<uint8_t>(comp, 0x1c8)) return false;
    uint64_t inst = rd<uint64_t>(rec, 0xb0); if (!inst) return false;
    uint64_t p = rd<uint64_t>(inst, 8), q = p ? rd<uint64_t>(p, 0) : 0;
    if (q && q != 8) return false;
    uint64_t c = rd<uint64_t>(inst, 0x40), s = c ? rd<uint64_t>(c, 0) : 0; if (s) s -= 8;
    uint64_t ship = 0;
    if (s) __try { ship = ((uint64_t(__fastcall*)(uint64_t))G(0x1412631b0))(s); } __except (EXCEPTION_EXECUTE_HANDLER) { ship = 0; }
    if (ship && rd<uint8_t>(ship, 0x338)) return false;
    if (ship && g_creepAggressive) {
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1)) status("creep: capital ships take creep damage, hull and shield (the PvP levels' ignore-AI-damage rule set aside for creep)");
        return false;
    }
    return true;
}
// [creep] capital_damage (percent, aggressive creep): a creep starship's hit on a capital hull or shield is scaled. The hull's
// apply starts from the record's +0x88 (the team filter 0x14168ffb0 reads it first; the record is the hit function's own
// copy); the shield's direct hit reads the info's +0x84 (times the grid's multiplier), its blast +0x74, and those belong to
// the caller, so they're put back after.
static bool creep_scaled(int from) { return g_creepAggressive && g_creepCapDamage != 100 && (from == 0 || from == 1); }
typedef uint64_t(__fastcall* capapply_t)(uint64_t comp, uint64_t out, uint64_t rec, uint8_t skipTeam);
static capapply_t o_capapply;
static uint64_t __fastcall h_capapply(uint64_t comp, uint64_t out, uint64_t rec, uint8_t skipTeam) {
    if ((!g_capDmgLog && !(g_creepAggressive && g_creepCapDamage != 100)) || !comp || !rec) return o_capapply(comp, out, rec, skipTeam);
    uint64_t inst = rd<uint64_t>(rec, 0xb0);
    uint8_t mark; int from = cap_from(inst, &mark);
    float asked = rd<float>(rec, 0x88);
    if (creep_scaled(from)) leaf_write(rec + 0x88, false, asked * g_creepCapDamage / 100.0);
    if (!g_capDmgLog) return o_capapply(comp, out, rec, skipTeam);
    bool stockDrop = !(rd<uint8_t>(rec, 0xa4) & 8) && rd<uint8_t>(comp, 0x1c8) && inst && from != 2;
    bool creepThrough = stockDrop && g_creepAggressive && (from == 0 || from == 1);
    bool blast = _ReturnAddress() == (void*)G(0x1417dbdec);
    float before = cap_health(comp);
    uint64_t r = o_capapply(comp, out, rec, skipTeam);
    float after = cap_health(comp);
    cap_note(comp, false, blast, from, mark, asked, before >= 0 && after >= 0 ? before - after : 0, stockDrop && !creepThrough, creepThrough, after);
    return r;
}
typedef uint64_t(__fastcall* shieldhit_t)(uint64_t comp, uint64_t out, uint64_t info);
static shieldhit_t o_shdirect, o_shblast;
static uint64_t cap_shield(shieldhit_t orig, uint64_t comp, uint64_t out, uint64_t info, bool blast) {
    if ((!g_capDmgLog && !(g_creepAggressive && g_creepCapDamage != 100)) || !comp || !out || !info) return orig(comp, out, info);
    uint8_t mark; int from = cap_from(rd<uint64_t>(info, 0xc0), &mark);
    uint32_t at = blast ? 0x74 : 0x84; float asked = rd<float>(info, at);
    bool scaled = creep_scaled(from) && leaf_write(info + at, false, asked * g_creepCapDamage / 100.0);
    float before = g_capDmgLog ? cap_health(comp) : 0;
    uint64_t r = orig(comp, out, info);
    if (scaled) leaf_write(info + at, false, asked);
    if (g_capDmgLog) {
        float after = cap_health(comp);
        cap_note(comp, true, blast, from, mark, asked, before >= 0 && after >= 0 ? before - after : 0, false, false, after);
    }
    return r;
}
static uint64_t __fastcall h_shdirect(uint64_t comp, uint64_t out, uint64_t info) { return cap_shield(o_shdirect, comp, out, info, false); }
static uint64_t __fastcall h_shblast(uint64_t comp, uint64_t out, uint64_t info) { return cap_shield(o_shblast, comp, out, info, true); }
typedef uint64_t(__fastcall* matlookup_t)(uint64_t grid, const uint32_t* a, const uint32_t* b);
static matlookup_t o_matlookup;
struct MatSeen { uint32_t a, b; uint64_t ret; };
static MatSeen g_matSeen[2048]; static int g_matSeenCount; static SRWLOCK g_matLock = SRWLOCK_INIT;
// Laser and ion damage against capital ships (profile keys materialgrid.<weapon>damage.<part>, a multiplier on the cell's
// DamageProtectionMultiplier, stock 1). The damage code asks this lookup for the cell of (projectile material, hit-part
// material) and multiplies a direct hit's damage by the cell's +0x20; explosions (torpedoes, bombs, missiles) don't use
// it. The grid's cells are shared objects (laser vs capital hull is also laser vs the weak-point decal; laser vs capital
// shield is also beam vs shield), so nothing in the grid is edited: when one of the damage functions below asks for a
// laser or ion cell on its part, it gets a private copy of the stock capital cell with the multiplier changed. Which
// function is asking is its return address (fixed: no ASLR). Corvettes (Raider, CR90) have hulls on the default material
// (3,0), which is also laser vs missiles in flight; the hull site maps default and capital-part targets to the capital
// copy too, so every capital hull takes the same setting and missiles keep theirs. At stock all of these cells are 1.
enum { GW_LASER, GW_ION, GW_N };
enum { GP_HULL, GP_SHIELDS, GP_TURRETS, GP_N };
static const char* kGridWeapon[GW_N] = { "laserdamage", "iondamage" };
static const char* kGridPart[GP_N] = { "capitalhull", "capitalshields", "capitalturrets" };
static const uint32_t kGridWeaponProp[GW_N] = { 3, 10 };   // MaterialDecl property index (bits 20-31): laser, ion
static const uint32_t kGridStockTarget[GP_N] = { 0x00b00980, 0x00600580, 0x00b00980 };   // capital ship, capital shield
struct GridSite { uint64_t ret; int part; };
static GridSite kGridSites[] = {   // Steam addresses until apply_build() puts the running build's in
    { 0x1417db400, GP_HULL },      // ServerCapitalShipHealthComponent, direct hit (FUN_1417db200)
    { 0x140582ba0, GP_SHIELDS },   // ServerIncomShieldHealthComponent, direct hit (FUN_140582ab0)
    { 0x140546f7e, GP_SHIELDS },   // ClientShieldHealthComponent slot 41 (FUN_140546e90): the client's own figure
    { 0x1416edef6, GP_TURRETS },   // ServerTurretHealthComponent, direct hit (FUN_1416edb50)
};
static uint8_t g_gridCopy[GW_N][GP_N][64];
static volatile uint64_t g_gridSub[GW_N][GP_N];   // the private copy in use, 0 = the game's own cell
// Everything that touches the grid happens inside the hook, on the game's own call, with the grid it just passed in: the
// grid belongs to the level and is freed with it (2026-10-22: a scan that kept the first match's grid and looked it up
// every 2 s crashed the game on the way back to the front end). The main thread only publishes the wanted values and
// records the stock ones the hook saw.
static float g_gridWant[GW_N][GP_N];              // the profile's multiplier, or -1 = leave the game's cell
static volatile LONG g_gridReady;                 // g_gridWant is filled in (main thread, after the targets are built)
static float g_gridStock[GW_N][GP_N];             // the stock multiplier the hook read
static volatile LONG g_gridStockKnown;            // 1 = g_gridStock filled; -1 = the cells were not damage relations
static SRWLOCK g_gridLock = SRWLOCK_INIT;
static bool grid_target_ok(int part, uint32_t pb) {
    uint32_t prop = (pb & 0x80) ? pb >> 20 : 0;
    if (part == GP_SHIELDS) return prop == 6;
    if (part == GP_TURRETS) return prop == 11 || prop == 0;
    return prop == 11 || prop == 0 || prop == 18;   // hull: capital, default (corvettes), capital part (SD/MC75)
}
// the stock capital cell for (weapon, part) in this grid, if it is a damage relation (MaterialRelationDamageData)
static uint64_t grid_stock_cell(uint64_t grid, int w, int p) {
    uint32_t a = (kGridWeaponProp[w] << 20) | 0x80, b = kGridStockTarget[p];
    uint64_t cell = o_matlookup(grid, &a, &b);
    uint64_t ci = cell ? rd<uint64_t>(cell, 8) : 0;
    uint16_t id = ci ? rd<uint16_t>(ci, 0x48) : 0, lo = rd<uint16_t>(G(0x1449f9290), 0x48), hi = rd<uint16_t>(G(0x1449f9290), 0x4a);
    return ci && (uint16_t)(id - lo) <= (uint16_t)(hi - lo) ? cell : 0;
}
static uint64_t __fastcall h_matlookup(uint64_t grid, const uint32_t* a, const uint32_t* b) {
    uint64_t r = o_matlookup(grid, a, b);
    if (grid && !g_gridStockKnown) {   // first lookup with a grid: read the stock multipliers once
        AcquireSRWLockExclusive(&g_gridLock);
        if (!g_gridStockKnown) {
            LONG ok = 1;
            for (int w = 0; w < GW_N; w++) for (int p = 0; p < GP_N; p++) {
                uint64_t cell = grid_stock_cell(grid, w, p);
                if (cell) g_gridStock[w][p] = rd<float>(cell, 0x20); else ok = -1;
            }
            InterlockedExchange(&g_gridStockKnown, ok);
        }
        ReleaseSRWLockExclusive(&g_gridLock);
    }
    uint64_t ret = (uint64_t)_ReturnAddress();
    for (const GridSite& s : kGridSites) {
        if (s.ret != ret) continue;
        if (!g_gridReady || g_gridStockKnown != 1) break;
        uint32_t pa = a ? rd<uint32_t>((uint64_t)a, 0) : 0, pb = b ? rd<uint32_t>((uint64_t)b, 0) : 0;
        uint32_t aprop = (pa & 0x80) ? pa >> 20 : 0;
        for (int w = 0; w < GW_N; w++) {
            if (aprop != kGridWeaponProp[w] || g_gridWant[w][s.part] < 0 || !grid_target_ok(s.part, pb)) continue;
            if (!g_gridSub[w][s.part]) {   // first such hit: copy this grid's stock capital cell (a private 56 bytes)
                AcquireSRWLockExclusive(&g_gridLock);
                if (!g_gridSub[w][s.part]) {
                    uint64_t cell = grid_stock_cell(grid, w, s.part);
                    if (cell) {
                        uint8_t* c = g_gridCopy[w][s.part];
                        memcpy(c, (const void*)cell, 56);
                        float stock = rd<float>(cell, 0x20), now = (float)(stock * g_gridWant[w][s.part]);
                        *(float*)(c + 0x20) = now;
                        InterlockedExchange64((volatile LONG64*)&g_gridSub[w][s.part], (LONG64)(uint64_t)c);
                        status("values: materialgrid.%s.%s %.9g -> %.9g (a private copy of the grid cell, for capital-ship damage only)",
                               kGridWeapon[w], kGridPart[s.part], (double)stock, (double)now);
                    }
                }
                ReleaseSRWLockExclusive(&g_gridLock);
            }
            if (g_gridSub[w][s.part]) r = g_gridSub[w][s.part];
        }
        break;
    }
    if (!g_materialLog) return r;
    uint32_t pa = a ? rd<uint32_t>((uint64_t)a, 0) : 0, pb = b ? rd<uint32_t>((uint64_t)b, 0) : 0;
    AcquireSRWLockExclusive(&g_matLock);
    bool seen = false;
    for (int i = 0; i < g_matSeenCount && !seen; i++)
        seen = g_matSeen[i].a == pa && g_matSeen[i].b == pb && g_matSeen[i].ret == ret;
    if (!seen && g_matSeenCount < 2048) {
        g_matSeen[g_matSeenCount++] = { pa, pb, ret };
        status("material: caller %p  a %08x (%s, property %u)  b %08x (%s, property %u)  -> %p", (void*)ret,
               pa, (pa & 0x80) ? "set" : "default", pa >> 20, pb, (pb & 0x80) ? "set" : "default", pb >> 20, (void*)r);
        if (g_matSeenCount == 2048) status("material: 2048 distinct pairs logged; no more");
    }
    ReleaseSRWLockExclusive(&g_matLock);
    return r;
}

// The grid settings, main thread: publish the profile's values for the hook (once the targets exist), and record the
// stock multipliers the hook read as the pseudo-config "materialgrid" (defaults.ini, for the app and the limits).
// It never looks anything up itself.
static bool g_gridRecorded;
static void grid_apply() {
    if (!g_gridReady && g_targetCount >= 0) {
        for (int w = 0; w < GW_N; w++) for (int p = 0; p < GP_N; p++) {
            g_gridWant[w][p] = -1;
            char want[64]; sprintf_s(want, "%s.%s", kGridWeapon[w], kGridPart[p]);
            for (int t = 0; t < g_targetCount; t++)
                if (!strcmp(g_targets[t].asset, "materialgrid") && !strcmp(g_targets[t].key, want)) {
                    g_gridWant[w][p] = (float)g_targets[t].value; g_targets[t].hit = true; }
        }
        InterlockedExchange(&g_gridReady, 1);
    }
    if (g_gridRecorded || !g_gridStockKnown) return;
    g_gridRecorded = true;
    if (g_gridStockKnown < 0) { status("materialgrid: the grid's laser/ion capital cells are not damage relations; grid settings off"); return; }
    for (int w = 0; w < GW_N; w++) for (int p = 0; p < GP_N; p++) {
        char key[96]; sprintf_s(key, "materialgrid.%s.%s", kGridWeapon[w], kGridPart[p]);
        DefaultSlot* d = default_slot(key, true); if (d) d->value = g_gridStock[w][p];
    }
    if (!asset_seen("materialgrid") && g_seenCount < 4096) {
        if (g_seenNames) strcpy_s(g_seenNames[g_seenCount], "MaterialGrid");
        strcpy_s(g_seenAssets[g_seenCount++], "materialgrid");
    }
    g_defaultsDirty = true;
}

typedef uint64_t(__fastcall* lvlstep2_t)(uint64_t mgr, uint64_t server, uint64_t state);
static lvlstep2_t o_lvlstep2;
static uint64_t __fastcall h_lvlstep2(uint64_t mgr, uint64_t server, uint64_t state) {
    if (!g_dedicated) return o_lvlstep2(mgr, server, state);   // only the dedicated path needs the wait
    typedef bool(__fastcall* bq_done_t)(uint64_t sched, uint32_t handle);
    typedef bool(__fastcall* job_done_t)(uint32_t handle);
    typedef uint64_t(__fastcall* find_t)(uint16_t compartment, void* type);
    static ULONGLONG waitStart, lastNote;
    uint32_t h1 = rd<uint32_t>(state, 0x628), h2 = rd<uint32_t>(state, 0x62c);
    bool d1 = !h1 || (h1 >> 24) == 7 || ((bq_done_t)G(0x14021ba60))(rd<uint64_t>(G(0x1442f7c68), 0), h1);
    bool d2 = !h2 || (h2 >> 24) == 7 || ((job_done_t)G(0x1401f75c0))(h2);
    if (d1 && d2) {
        // the lookup assumes the compartment exists: (&DAT_1442d18c8)[c] null → it locks through a null pointer (my 13:07 crash)
        uint16_t comp = rd<uint16_t>(server, 0x90);
        bool compReady = comp != 0xbbe && rd<uint64_t>(G(0x1442d18c8) + 8ull * comp, 0) != 0;
        uint64_t ld = compReady ? ((find_t)G(0x1401f9860))(comp, (void*)G(0x144a17ea0)) : 0;
        ULONGLONG now = GetTickCount64();
        // found is not enough: on 2026-10-20 it was found with its arrays still null (AutoLoadBundles +0x100 crashed the
        // next step). Loaded data arrays are never null, so wait until VoiceOverLogic +0xd0, CameraModes +0xe8 and
        // AutoLoadBundles +0x100 are set, and stay set for two consecutive checks.
        static int readyChecks;
        bool filled = ld && rd<uint64_t>(ld, 0xd0) && rd<uint64_t>(ld, 0xe8) && rd<uint64_t>(ld, 0x100);
        readyChecks = filled ? readyChecks + 1 : 0;
        if (!compReady && !waitStart) status("3b: server resource compartment %u not created yet", (unsigned)comp);
        if (ld && !filled && !waitStart) status("3b: LevelData %p found but not filled in yet (+0xd0 %llx +0xe8 %llx +0x100 %llx)", (void*)ld,
                                                rd<uint64_t>(ld, 0xd0), rd<uint64_t>(ld, 0xe8), rd<uint64_t>(ld, 0x100));
        if (readyChecks < 2) ld = 0;
        if (!ld) {
            if (!waitStart) { waitStart = lastNote = now; status("3b: level bundles done but LevelData not ready yet; waiting"); }
            if (now - waitStart < 30000) {
                if (now - lastNote >= 2000) { lastNote = now; status("3b: still waiting for LevelData (%llu ms)", now - waitStart); }
                return 0;
            }
            status("3b: LevelData still missing after 30 s; letting the engine continue");
        } else if (waitStart) {
            status("3b: LevelData ready after %llu ms (%p; AutoLoadBundles %llx)", now - waitStart, (void*)ld, rd<uint64_t>(ld, 0x100)); waitStart = 0;
        }
    }
    return o_lvlstep2(mgr, server, state);
}

// Client network init (FUN_140a31d50 = Kyber's ClientInitNetwork(client, singleplayer, localhost, coop, hosted)).
// Mode inside: 0 = local (singleplayer or localhost; in-memory manager, which transport=udp makes real UDP),
// 1 = networked (needs a socket-manager creator), 2 = networked co-op. listen=1: a non-menu init (the client's
// pending LevelSetup at client+0x408 is not the Frontend) has singleplayer cleared and localhost kept, so the host's
// client matches the networked server and stays on our UDP-backed sockets (2026-10-20).
typedef void(__fastcall* cinit_t)(uint64_t client, char sp, char localhost, char coop, uint8_t hosted);
static cinit_t o_cinit;
static volatile bool g_lastInitMatch;   // the latest client network init was for a match (not the Frontend)
static void __fastcall h_cinit(uint64_t client, char sp, char localhost, char coop, uint8_t hosted) {
    const char* lvl = safe_str(rd<uint64_t>(client, 0x408 + 8));
    bool menu = strstr(lvl, "Frontend") != nullptr;
    g_lastInitMatch = !menu;
    status("client init network(sp %d, localhost %d, coop %d, hosted %d) level '%s'", (int)sp, (int)localhost, (int)coop, (int)hosted, lvl);
    if (g_joinAddr[0] && g_joinRemote && !g_dedicated && !menu) {
        status("join: match connection set up as a remote client (sp %d, localhost %d, hosted %d -> 0, 0, 0)", (int)sp, (int)localhost, (int)hosted);
        sp = 0; localhost = 0; coop = 0; hosted = 0;
        // Game type 3 (Joined), as the engine's own remote join sets it: state 6 (0x140a3104b) then has the client set up
        // the Level memory arena and mount the level itself (FUN_140a32a10 -> FUN_140b29e80) instead of assuming an
        // in-process server did. Without it the client's level create returned null (crash 17:37, build 2 try 1).
        int oldType = rd<int>(client, 0x4ac);
        *(int*)(client + 0x4ac) = 3;
        status("join: client game type %d (%s) -> 3 (Joined)", oldType, oldType == 1 ? "Hosted" : oldType == 2 ? "SoloPvB" : oldType == 0 ? "SinglePlayer" : "?");
    }
    // match_mode without the Solo flag: the custom-match module no longer turns the start into the local SoloPvB flow,
    // so the client stays "Hosted" (EA's peer-hosted path: not local host, the backend chooser finds no backend, crash
    // 18:49 build 3). Put the host's client back on the local flow; the server still gets the rewritten options.
    if (g_listen && g_matchMode[0] && !g_joinAddr[0] && !g_dedicated && !menu && rd<int>(client, 0x4ac) == 1) {
        *(int*)(client + 0x4ac) = 2; *(uint8_t*)(client + 0x4b3) = 1;
        status("mode: host client kept on the local flow (game type 1 Hosted -> 2, localhost %d -> 1, hosted %d -> 0)", (int)localhost, (int)hosted);
        sp = 1; localhost = 1; hosted = 0;
    }
    char initMode[48] = "";
    if (g_listen && !g_dedicated && !menu && sp && !pvp_match(client + 0x408, initMode, sizeof initMode))
        status("listen: host client left single-player for a '%s' match", initMode);
    else if (g_listen && !g_dedicated && !menu && sp) {
        sp = 0;
        status("listen: host client init made networked (sp 1 -> 0, localhost %d)", (int)localhost);
    }
    o_cinit(client, sp, localhost, coop, hosted);
}

// Joining as a remote client, and join diagnostics (2026-10-20 evening; notes/test1-static-map.md "Joiner stall").
// The first two-player match stalled the joiner at the map load: its client ran the "host a local match" flow with the
// connect address rewritten, so the engine treated it as a local host. The engine's own remote join (client state 14,
// game type 3) inits the network with sp = localhost = 0, which makes the connection mode 1 ("networked"): FUN_140cb0080
// stores the mode at conn+0x78 and the client tells the game simulation it is remote. Mode 1 normally takes its socket
// manager from a creator (client+0xb8); join_remote=1 builds it the local way first (mode 0: the in-memory manager, which
// transport=udp makes our UDP), then lets mode 1 run with the manager already in place.
typedef uint64_t(__fastcall* cset_t)(uint64_t conn, int mode, uint64_t creator, uint64_t a4, uint8_t a5);
static cset_t o_cset;
static uint64_t __fastcall h_cset(uint64_t conn, int mode, uint64_t creator, uint64_t a4, uint8_t a5) {
    if (mode == 1 && g_joinAddr[0] && g_joinRemote && !g_dedicated && !rd<uint64_t>(conn, 0x38)) {
        status("join: remote-client connection %llx: building its socket manager the local way (our UDP); creator %llx unused", conn, creator);
        o_cset(conn, 0, creator, a4, a5);
    }
    uint64_t r = o_cset(conn, mode, creator, a4, a5);
    if (g_joinAddr[0]) status("join: connection %llx setup mode %d, manager %llx, result %llu", conn, mode, rd<uint64_t>(conn, 0x38), r);
    return r;
}

// Match mode (host, 2026-10-20 evening). The level's world parts are chosen by the LevelSetup option GameMode
// (WorldPartInclusion game/gameplay/gamemodes/gamemodeinclusionsettings: Frontline, Frontline_Bots, Missions, Dogfight,
// Practice, Tutorial); the code compares SquadronComposition only with "Solo" (solo vs AI: one human plus AI wingmen).
// EA's custom matches asked the backend for Dogfight / Frontline / Frontline_Bots (FUN_1417503e0) with no Solo flag.
// match_mode rewrites both options on every match LevelSetup the host's client and server pass around.
static void apply_match_mode(uint64_t ls, const char* where) {
    if (!g_matchMode[0] || !g_listen || g_joinAddr[0] || g_dedicated || !ls) return;
    char lvl[160]; safe_copy(rd<uint64_t>(ls, 8), lvl, sizeof lvl);
    if (!lvl[0] || strstr(lvl, "Frontend")) return;
    const char* gm = nullptr; const char* sc = "None";
    if (!_stricmp(g_matchMode, "coop")) gm = "Frontline_Bots";
    else if (!_stricmp(g_matchMode, "fleet")) gm = "Frontline";
    else if (!_stricmp(g_matchMode, "dogfight")) gm = "Dogfight";
    else if (!_stricmp(g_matchMode, "solo")) { gm = "Frontline_Bots"; sc = "Solo"; }
    else { status("mode: unknown match_mode '%s' (solo, coop, fleet, dogfight)", g_matchMode); return; }
    typedef uint64_t(__fastcall* getopt_t)(uint64_t ls, const char* key);
    auto go = (getopt_t)G(0x141191d50); auto so = (setoption_t)kSetOption;
    char nm[160], g0[48], s0[48], g1[48], s1[48];
    safe_copy(rd<uint64_t>(ls, 8), nm, sizeof nm);
    safe_copy(go(ls, "GameMode"), g0, sizeof g0); safe_copy(go(ls, "SquadronComposition"), s0, sizeof s0);
    // Only matches that are already a PvP / vs-AI mode (the solo custom match starts as Frontline_Bots): Practice,
    // Missions and the Tutorial run on single-player levels that can't be Fleet Battles, and stay as they are.
    char pm[48];
    if (!pvp_match(ls, pm, sizeof pm)) {
        status("mode: %s '%s': GameMode '%s' left as it is (match_mode applies to PvP and vs-AI matches)", where, nm, g0);
        return;
    }
    so(ls, "GameMode", gm); so(ls, "SquadronComposition", sc);
    safe_copy(go(ls, "GameMode"), g1, sizeof g1); safe_copy(go(ls, "SquadronComposition"), s1, sizeof s1);
    status("mode: %s '%s': GameMode/SquadronComposition %s/%s -> %s/%s", where, nm, g0, s0, g1, s1);
}

static volatile LONG g_diagClient, g_diagBundle;
static const char* client_type_name(int t) {
    switch (t) { case 0: return "SinglePlayer"; case 1: return "Hosted"; case 2: return "SoloPvB"; case 3: return "Joined"; default: return "?"; }
}
// FUN_140a30230(client, state): the client's state request; +0x380 = current state, +0x4ac = game type.
typedef uint64_t(__fastcall* cstate_t)(uint64_t client, int state, uint64_t a3, uint64_t a4);
static cstate_t o_cstate;
static uint64_t __fastcall h_cstate(uint64_t client, int state, uint64_t a3, uint64_t a4) {
    int old = rd<int>(client, 0x380), type = rd<int>(client, 0x4ac);
    if (InterlockedIncrement(&g_diagClient) <= 2000)
        status("client: state %d -> %d requested (game type %d %s, connection %llx)", old, state, type, client_type_name(type), rd<uint64_t>(client, 0x4e0));
    return o_cstate(client, state, a3, a4);
}
// FUN_140a338d0(client, message): the client's message handler. Only the ids it acts on are logged.
static const char* client_msg_name(uint32_t id) {
    switch (id) {
    case 0x107569eb: return "start local game";
    case 0xbf585a6d: return "host level";
    case 0x4551edba: return "join server";
    case 0x0036a95b: return "server level info";
    case 0xe2321d7c: return "e2321d7c";
    case 0xbd4b8796: return "bd4b8796";
    case 0xe4f8cff7: return "e4f8cff7";
    case 0x0f05426f: return "password";
    case 0x69286ebc: return "69286ebc";
    case 0x43777fdf: return "43777fdf";
    case 0x64e4e684: return "64e4e684";
    case 0x6f8b48d5: return "6f8b48d5";
    case 0x7ef51b9f: return "7ef51b9f";
    default: return nullptr;
    }
}
typedef void(__fastcall* cmsg_t)(uint64_t client, uint64_t msg, uint64_t a3, uint64_t a4);
static cmsg_t o_cmsg;
static void __fastcall h_cmsg(uint64_t client, uint64_t msg, uint64_t a3, uint64_t a4) {
    uint32_t id = rd<uint32_t>(msg, 0xc);
    if (id == 0x107569eb || id == 0xbf585a6d) apply_match_mode(msg + 0x30, "client start");
    const char* n = client_msg_name(id);
    if (n && InterlockedIncrement(&g_diagClient) <= 2000) {
        status("client: message %08x (%s), game type %d, state %d", id, n, rd<int>(client, 0x4ac), rd<int>(client, 0x380));
        if (id == 0x107569eb || id == 0xbf585a6d)
        { char a[160], b[160], c[160];
            status("  level '%s', lobby '%s', map '%s'", safe_copy(rd<uint64_t>(msg, 0x38), a, sizeof a), safe_copy(rd<uint64_t>(msg, 0x50), b, sizeof b), safe_copy(rd<uint64_t>(msg, 0x60), c, sizeof c)); }
    }
    o_cmsg(client, msg, a3, a4);
}
// FUN_140ba0340(listener, message): the server-side level messages (load level, level change); LevelSetup at +0x30.
typedef void(__fastcall* smsg_t)(uint64_t self, uint64_t msg);
static smsg_t o_smsg;
static void __fastcall h_smsg(uint64_t self, uint64_t msg) {
    uint32_t id = rd<uint32_t>(msg, 0xc);
    if (id == 0xab12c7a3 || id == 0xf8429a0e) apply_match_mode(msg + 0x30, id == 0xf8429a0e ? "server level change" : "server load level");
    if ((id == 0xab12c7a3 || id == 0xb455ada6 || id == 0xf8429a0e) && InterlockedIncrement(&g_diagClient) <= 2000) {
        status("server: level message %08x (%s)", id, id == 0xf8429a0e ? "level change" : id == 0xab12c7a3 ? "load level" : "b455ada6");
        if (id != 0xb455ada6)
        { char a[160], b[160], c[160];
            status("  level '%s', lobby '%s', map '%s'", safe_copy(rd<uint64_t>(msg, 0x38), a, sizeof a), safe_copy(rd<uint64_t>(msg, 0x50), b, sizeof b), safe_copy(rd<uint64_t>(msg, 0x60), c, sizeof c)); }
    }
    o_smsg(self, msg);
}
// FUN_14021b2b0: the job scheduler's bundle queue (9 arguments; see queue_bundle). Logs the requested bundle names.
typedef void(__fastcall* bqueue_t)(uint32_t* h, uint64_t sched, uint64_t zero, const char** names, uint32_t count,
                                   uint64_t u6, uint64_t u7, uint64_t u8, uint32_t flags);
static bqueue_t o_bqueue;
static void __fastcall h_bqueue(uint32_t* h, uint64_t sched, uint64_t zero, const char** names, uint32_t count,
                                uint64_t u6, uint64_t u7, uint64_t u8, uint32_t flags) {
    if (g_bundleLog && count <= 4096 && InterlockedIncrement(&g_diagBundle) <= 3000) {
        char buf[640]; buf[0] = 0; size_t used = 0;
        for (uint32_t i = 0; names && i < count && i < 12; i++) {
            int w = _snprintf_s(buf + used, sizeof buf - used, _TRUNCATE, "%s%s", i ? ", " : "", safe_str(rd<uint64_t>((uint64_t)names, i * 8)));
            if (w < 0) break;
            used += (size_t)w;
        }
        status("bundle: queue %u: %s%s", count, buf, count > 12 ? ", ..." : "");
    }
    o_bqueue(h, sched, zero, names, count, u6, u7, u8, flags);
}

// Connection reason watcher: after a connect, poll the connection manager's state (+0xc0) and SecureReason (+0xc4,
// set to Ok at connect) every 20 ms for 60 s and log changes by name (djb2-xor of the SecureReason_* names in the exe).
static const struct { uint32_t h; const char* n; } kReasons[] = {
    {0x102cbecd,"Ok"},{0xb0d2286,"WrongProtocolVersion"},{0x207b2770,"WrongTitleVersion"},{0xeea836df,"ServerFull"},
    {0x230e676c,"KickedOut"},{0xc9360c4b,"Banned"},{0x5fce71a0,"GenericError"},{0x18237655,"WrongPassword"},
    {0x16086164,"MissingContent"},{0x64a5ce5a,"NotVerified"},{0xb59c9896,"TimedOut"},{0x280af454,"ConnectFailed"},
    {0xcd9691da,"NoReply"},{0xb0ccc0ca,"AcceptFailed"},{0x78d1ca4,"MismatchingContent"},{0x345f5d54,"ConfigurationNotAllowed"},
    {0x15c4a48e,"EAC"},{0xd58f5f94,"LoadingTimeout"},{0x93dab6db,"InteractivityTimeout"},{0x4bec2c45,"KickedFromQueue"},
    {0xc765936,"KickedByAdmin"},{0x9f3e83a,"KickedOutServerFull"},{0x78b15894,"KickedViaFairFight"},{0x8ae867c3,"KickedViaShield"},
    {0x83254231,"ServerMaintenance"},{0x464a7a90,"ServerReclaimed"},{0xa4592562,"RankRestricted"},{0xef204ac1,"KickedOutDemoOver"},
};
static const char* reason_name(uint32_t h) { for (auto& r : kReasons) if (r.h == h) return r.n; return "?"; }
static volatile LONG64 g_watchConn;
static DWORD WINAPI conn_watch(LPVOID p) {
    uint64_t c = (uint64_t)p; int lastS = -12345; uint32_t lastR = 0;
    for (int k = 0; k < 3000 && (uint64_t)g_watchConn == c; k++) {
        int st = rd<int>(c, 0xc0); uint32_t r = rd<uint32_t>(c, 0xc4);
        if (st != lastS || r != lastR) { status("conn %llx: state %d reason %08x (%s)", c, st, r, reason_name(r)); lastS = st; lastR = r; }
        Sleep(20);
    }
    return 0;
}
static void watch_connection(uint64_t connobj) {
    InterlockedExchange64(&g_watchConn, (LONG64)connobj);
    HANDLE t = CreateThread(nullptr, 0, conn_watch, (LPVOID)connobj, 0, nullptr); if (t) CloseHandle(t);
}
// All-unlocked inventory (2026-10-20, notes/inventory-unlocks.md): FUN_14123a310(store, vec) is the only writer of the
// client's inventory vector; it copies [vec->begin, vec->end) (0x38-byte items) and reads nothing else from vec. Ownership
// checks compare item+0x10 (u64) with a config's Identifier or its family's. With all_unlocked=1 we pass our own array:
// the original items plus one item per id not already present (both strings = the engine's empty-string sentinel
// 0x143f7b968, which the string code never frees).
// Loadout preset slots (2026-10-27, all_unlocked): a ship's presets 2-5 unlock by player rank, from
// Game/Persistence/IncomRankData's RanksOfStarshipPresets (0, 2, 14, 24, 34). A new account (rank 0) gets one preset per
// ship; an account that played the live game had them all, so this only showed on a fresh EA app account. The game reaches
// the asset at [[0x1441c5f00]+0xf8]: FUN_141744200, the server's preset check, reads the array at +0x30 (count at -4)
// against the player's rank. With all_unlocked=1 the array is set to 0 everywhere, in the one shared copy, so the client's
// slot list and the host's check both allow every preset. Checked first: it must look like the asset's array.
static bool write_u32(uint64_t at, uint32_t v) {
    __try { *(uint32_t*)at = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD old; if (!VirtualProtect((void*)at, 4, PAGE_READWRITE, &old)) return false;
    bool ok = false; __try { *(uint32_t*)at = v; ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    VirtualProtect((void*)at, 4, old, &old);
    return ok;
}
static void unlock_presets() {
    static bool done = false, told = false;
    if (done) return;
    uint64_t rank = rd<uint64_t>(rd<uint64_t>(G(0x1441c5f00ull), 0), 0xf8), arr = rank ? rd<uint64_t>(rank, 0x30) : 0;
    if (!arr) { if (!told) { told = true; status("unlocks: the rank data isn't loaded yet; preset slots unchanged for now"); } return; }
    uint32_t n = rd<uint32_t>(arr - 4, 0), v[16] = {};
    bool shaped = n >= 1 && n <= 16;
    for (uint32_t i = 0; shaped && i < n; i++) { v[i] = rd<uint32_t>(arr, 4 * i); shaped = v[i] < 1000 && (i ? v[i] >= v[i - 1] : v[i] == 0); }
    done = true;
    if (!shaped) { status("unlocks: the preset-slot ranks at %llx don't look as expected (count %u); left as they are", arr, n); return; }
    char was[128] = ""; int w = 0, failed = 0;
    for (uint32_t i = 0; i < n; i++) { w += sprintf_s(was + w, sizeof was - w, i ? " %u" : "%u", v[i]); if (v[i] && !write_u32(arr + 4 * i, 0)) failed++; }
    status("unlocks: loadout preset slots needed ranks %s; %s", was, failed ? "couldn't change them" : "all available now (all_unlocked)");
}
typedef void(__fastcall* invset_t)(uint64_t store, uint64_t* vec);
static invset_t o_invset;
static void __fastcall h_invset(uint64_t store, uint64_t* vec) {
    uint64_t b = vec ? vec[0] : 0, e = vec ? vec[1] : 0;
    int n = (b && e > b) ? (int)((e - b) / 0x38) : 0;
    static int s_dumped = -1;   // loadout_log=1: the backend's own items, each time their number changes (2026-10-27 probe:
    if (g_loadoutLog && n > 0 && n <= 4000 && n != s_dumped) {   // what a fresh EA account owns, against an old one)
        s_dumped = n;
        for (int k = 0; k < n; k++) {
            uint64_t it = b + (uint64_t)k * 0x38; char s0[96], s1[96];
            status("inventory:   [%d] id %llx (%llu) +18=%llx +20=%llx new %u +2c=%x +30=%x type %x '%s' '%s'", k,
                   rd<uint64_t>(it, 0x10), rd<uint64_t>(it, 0x10), rd<uint64_t>(it, 0x18), rd<uint64_t>(it, 0x20), rd<uint8_t>(it, 0x28),
                   rd<uint32_t>(it, 0x2c), rd<uint32_t>(it, 0x30), rd<uint32_t>(it, 0x34),
                   safe_copy(rd<uint64_t>(it, 0), s0, sizeof s0), safe_copy(rd<uint64_t>(it, 8), s1, sizeof s1));
        }
    }
    if (g_allUnlocked) unlock_presets();
    if (!g_allUnlocked || !g_unlockCount || n < 0 || n > 100000) { status("inventory: set with %d items", n); o_invset(store, vec); return; }
    size_t cap = (size_t)n + g_unlockCount;
    uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * 0x38);
    if (!buf) { o_invset(store, vec); return; }
    if (n) memcpy(buf, (void*)b, (size_t)n * 0x38);
    int added = 0;
    for (int i = 0; i < g_unlockCount; i++) {
        uint64_t id = g_unlockIds[i]; bool have = false;
        for (int k = 0; k < n; k++) if (*(uint64_t*)(buf + (size_t)k * 0x38 + 0x10) == id) { have = true; break; }
        if (have) continue;
        uint8_t* it = buf + (size_t)(n + added) * 0x38;
        *(uint64_t*)(it + 0x00) = G(0x143f7b968ull); *(uint64_t*)(it + 0x08) = G(0x143f7b968ull); *(uint64_t*)(it + 0x10) = id;
        added++;
    }
    uint64_t v[4] = { (uint64_t)buf, (uint64_t)(buf + (size_t)(n + added) * 0x38), (uint64_t)(buf + cap * 0x38), 0 };
    status("inventory: set with %d items from the backend; all_unlocked adds %d (total %d)", n, added, n + added);
    o_invset(store, v);
    HeapFree(GetProcessHeap(), 0, buf);   // the store copied the items
}

// Local loadouts (2026-10-26, [server] local_loadouts, default 1). The game keeps its online presets in the online data
// model ("store", [[0x1441c5f28]+8]) as three EASTL sets: pilot loadouts at +8, ship loadouts at +0x40, and the player profile
// (each faction's default ship, preset and pilot) at +0x78. A set's anchor is at set+8 {right +0, left +8, parent +0x10},
// its size at set+0x28; a node is {right +0, left +8, parent +0x10, colour +0x18, value +0x20}. A record's value starts with
// its name (char*, +0x20); the item ids the save serializes follow, then the EA item id (a GUID string):
//   pilot   ids +0x2c..+0x6b (body type, body parts, voice, idle/emote/victory/play animations), GUID +0x70
//   ship    ids +0x28..+0x57 (countermeasure, hull, shields, engine, primary, secondary, auxiliary, three flair, visual,
//           decal), GUID +0x60
//   profile ids +0x28..+0x3f (default ship, preset and pilot, Imperial then Rebel), GUID +0x40
//   FUN_1418f58f0(store, presets, state, status) sets them: from EA's inventory reply (FUN_1418fd3d0), or from a save.
//   FUN_1419001c0(manager, presets): the online inventory manager's save (vtable 0x143686860, slot 0xb0; the manager is the
//   online client's +0x2058, its store at +0x10). It sets the store's presets, posts ClientOnlineLoadoutUpdated, then sends
//   every loadout to EA through FUN_1418f61b0 (the call at 0x141903195). The preset sanitizer and the hangar and briefing
//   room all save through it.
// The mod's unlocks are local, so a save would tell EA about items the account doesn't own (2026-10-26: EA stores them
// without checking), and saves made in a match go nowhere (EA's connection drops when the match starts; the game's
// "connection lost" event is suppressed). So with local_loadouts on, the save's send to EA is dropped (the purchase flow's
// sends pass), every save's records go to loadouts.txt beside this DLL, and when EA's loadouts arrive the saved records
// are laid over them by name. Only the id words are written, and only into a record whose name and GUID read as expected.
struct LdKind { const char* name; uint32_t set, first, words, guid; };
static const LdKind kLd[3] = { { "pilot", 0x08, 0x2c, 16, 0x70 }, { "ship", 0x40, 0x28, 12, 0x60 }, { "profile", 0x78, 0x28, 6, 0x40 } };
struct LdRec { int kind; char name[64]; uint32_t w[16]; };
static LdRec g_ld[512]; static int g_ldCount; static SRWLOCK g_ldLock = SRWLOCK_INIT;
static uint64_t ld_next(uint64_t n) {   // EASTL rbtree increment
    uint64_t r = rd<uint64_t>(n, 0);
    if (r) { n = r; for (uint64_t l; (l = rd<uint64_t>(n, 8)) != 0;) n = l; return n; }
    uint64_t p = rd<uint64_t>(n, 0x10);
    for (int g = 0; p && n == rd<uint64_t>(p, 0) && g < 64; g++) { n = p; p = rd<uint64_t>(n, 0x10); }
    if (rd<uint64_t>(n, 0) != p) n = p;
    return n;
}
static void ld_text(uint64_t p, char* out, size_t cap) {   // a short printable string at p, or empty
    out[0] = 0; if (p < 0x10000) return;
    size_t i = 0;
    for (; i + 1 < cap; i++) { char c = rd<char>(p, (uint32_t)i); if (!c) break; if (c < 32 || c > 126) { out[0] = 0; return; } out[i] = c; }
    out[i] = 0;
}
static bool ld_guid(uint64_t p) {   // an EA item id: 36 characters, dashes at 8, 13, 18, 23
    char g[48]; ld_text(p, g, sizeof g);
    return strlen(g) == 36 && g[8] == '-' && g[13] == '-' && g[18] == '-' && g[23] == '-';
}
// A record's name, if its layout reads as expected (a short identifier, and its GUID in place), else false.
static bool ld_record(uint64_t node, int k, char* name, size_t cap) {
    ld_text(rd<uint64_t>(node, 0x20), name, cap);
    if (!name[0] || strchr(name, ' ') || strlen(name) >= 63) return false;
    return ld_guid(rd<uint64_t>(node, kLd[k].guid));
}
template <class F> static void ld_each(uint64_t presets, F f) {   // f(kind, node), every node of the three sets
    for (int k = 0; k < 3; k++) {
        uint64_t anchor = presets + kLd[k].set + 8, n = rd<uint64_t>(anchor, 8);
        for (int i = 0; n && n != anchor && i < 200; i++, n = ld_next(n)) f(k, n);
    }
}
static int ld_find(int k, const char* name) {
    for (int i = 0; i < g_ldCount; i++) if (g_ld[i].kind == k && !strcmp(g_ld[i].name, name)) return i;
    return -1;
}
static void ld_load() {   // loadouts.txt: "<kind> <name> <word> ...", words in hex; lines starting with ; are comments
    FILE* f = nullptr; if (_wfopen_s(&f, g_ldPath, L"r") != 0 || !f) { status("loadouts: no saved loadouts yet"); return; }
    char line[512];
    while (g_ldCount < 512 && fgets(line, sizeof line, f)) {
        char kind[16], name[64]; int used = 0;
        if (line[0] == ';' || sscanf_s(line, "%15s %63s%n", kind, (unsigned)sizeof kind, name, (unsigned)sizeof name, &used) < 2) continue;
        int k = -1; for (int i = 0; i < 3; i++) if (!strcmp(kind, kLd[i].name)) k = i;
        if (k < 0) continue;
        LdRec r{}; r.kind = k; strcpy_s(r.name, name);
        const char* s = line + used; int got = 0;
        for (; got < (int)kLd[k].words; got++) { char* e; unsigned long v = strtoul(s, &e, 16); if (e == s) break; r.w[got] = (uint32_t)v; s = e; }
        if (got != (int)kLd[k].words) continue;   // a line from another layout: skip it
        int at = ld_find(k, name); g_ld[at >= 0 ? at : g_ldCount++] = r;
    }
    fclose(f);
    status("loadouts: %d saved loadout records from loadouts.txt", g_ldCount);
}
static void ld_write() {   // the whole file, through a temporary file, so a crash mid-write leaves the old one
    wchar_t tmp[MAX_PATH]; swprintf_s(tmp, L"%s.tmp", g_ldPath);
    FILE* f = nullptr; if (_wfopen_s(&f, tmp, L"w") != 0 || !f) { status("loadouts: couldn't write loadouts.txt"); return; }
    fprintf(f, "; Your loadouts, kept by the community mod (local_loadouts=1 in server.ini). They are not sent to EA.\n"
               "; One record per line: kind, name, then its item ids in hex. Delete this file to start again from EA's loadouts.\n");
    for (int i = 0; i < g_ldCount; i++) {
        fprintf(f, "%s %s", kLd[g_ld[i].kind].name, g_ld[i].name);
        for (uint32_t j = 0; j < kLd[g_ld[i].kind].words; j++) fprintf(f, " %08x", g_ld[i].w[j]);
        fputc('\n', f);
    }
    fclose(f);
    if (!MoveFileExW(tmp, g_ldPath, MOVEFILE_REPLACE_EXISTING)) status("loadouts: couldn't replace loadouts.txt (%lu)", GetLastError());
}
static void ld_dump(const char* what, uint64_t presets) {   // loadout_log=1: every node's value words, raw
    status("loadouts: %s: pilot %d, ship %d, profile %d", what, rd<int32_t>(presets, 0x08 + 0x28), rd<int32_t>(presets, 0x40 + 0x28),
           rd<int32_t>(presets, 0x78 + 0x28));
    int idx[3] = {};
    ld_each(presets, [&](int k, uint64_t n) {
        char line[700]; int w = sprintf_s(line, "loadouts:   %s[%d]", kLd[k].name, idx[k]++);
        char txt[48];
        for (uint32_t o = 0x20; o <= kLd[k].guid && w > 0 && w < (int)sizeof line - 40; o += 8) {
            uint64_t q = rd<uint64_t>(n, o);
            ld_text(q, txt, sizeof txt);
            if (txt[0]) w += sprintf_s(line + w, sizeof line - w, " +%x=\"%s\"", o, txt);
            else w += sprintf_s(line + w, sizeof line - w, " +%x=%08x,%08x", o, (uint32_t)q, (uint32_t)(q >> 32));
        }
        status("%s", line);
    });
}
#define kLdSaveSetRet G(0x141900234)    // the save's own call to the presets setter returns here
#define kLdSaveSendRet G(0x14190319a)   // the save's call to the EA sender returns here
typedef void(__fastcall* presetset_t)(uint64_t store, uint64_t presets, uint32_t state, uint32_t st);
static presetset_t o_presetset;
static void __fastcall h_presetset(uint64_t store, uint64_t presets, uint32_t state, uint32_t st) {
    uint64_t ret = (uint64_t)_ReturnAddress();
    if (g_localLoadouts && ret != kLdSaveSetRet && presets) {   // loadouts from EA: the saved records go on top
        AcquireSRWLockShared(&g_ldLock);
        int seen = 0, laid = 0, changed = 0, odd = 0; bool hit[512] = {};
        ld_each(presets, [&](int k, uint64_t n) {
            char name[64]; seen++;
            if (!ld_record(n, k, name, sizeof name)) { odd++; return; }
            int at = ld_find(k, name); if (at < 0) return;
            hit[at] = true; laid++;
            for (uint32_t j = 0; j < kLd[k].words; j++) {
                uint32_t off = kLd[k].first + 4 * j;
                if (rd<uint32_t>(n, off) != g_ld[at].w[j]) { *(volatile uint32_t*)(n + off) = g_ld[at].w[j]; changed++; }
            }
        });
        int missing = 0; for (int i = 0; i < g_ldCount; i++) if (!hit[i]) missing++;
        ReleaseSRWLockShared(&g_ldLock);
        if (seen || g_ldCount)
            status("loadouts: from EA (%llx): %d records; your saved loadouts laid over %d of them (%d item ids changed)%s%s",
                   (unsigned long long)ret, seen, laid, changed, odd ? "; some records not laid out as expected, left as EA sent them" : "",
                   missing ? "; some saved loadouts have no record from EA (not applied)" : "");
        if (missing) for (int i = 0; i < g_ldCount; i++) if (!hit[i]) status("loadouts:   no record from EA for %s %s", kLd[g_ld[i].kind].name, g_ld[i].name);
    }
    if (g_loadoutLog) {
        char what[160];
        sprintf_s(what, "set by %llx, state %u, status %u, connection %d", (unsigned long long)ret, state, st, rd<int32_t>(store, 0x20));
        ld_dump(what, presets);
    }
    o_presetset(store, presets, state, st);
}
typedef void(__fastcall* presetsave_t)(uint64_t mgr, uint64_t presets);
static presetsave_t o_presetsave;
static void __fastcall h_presetsave(uint64_t mgr, uint64_t presets) {
    if (g_loadoutLog) status("loadouts: SAVE from %llx (connection %d)", (unsigned long long)(uint64_t)_ReturnAddress(),
                             rd<int32_t>(rd<uint64_t>(mgr, 0x10), 0x20));
    if (g_localLoadouts && presets) {
        AcquireSRWLockExclusive(&g_ldLock);
        int kept = 0;
        ld_each(presets, [&](int k, uint64_t n) {
            char name[64]; if (!ld_record(n, k, name, sizeof name)) return;
            int at = ld_find(k, name); if (at < 0) { if (g_ldCount >= 512) return; at = g_ldCount++; }
            LdRec& r = g_ld[at]; r.kind = k; strcpy_s(r.name, name);
            for (uint32_t j = 0; j < kLd[k].words; j++) r.w[j] = rd<uint32_t>(n, kLd[k].first + 4 * j);
            kept++;
        });
        if (kept) ld_write();
        ReleaseSRWLockExclusive(&g_ldLock);
        status("loadouts: saved %d loadout records to loadouts.txt (not sent to EA)", kept);
    }
    o_presetsave(mgr, presets);
}
// FUN_1418f61b0(component, request, rpc, callback): sends a marketplace request to EA. With local_loadouts on, the save's
// call (returning to 0x14190319a) is dropped; it returns its request argument, as the real one does. Other callers pass.
typedef uint64_t(__fastcall* ldsend_t)(uint64_t comp, uint64_t req, uint64_t rpc, uint64_t cb, uint64_t a5);
static ldsend_t o_ldsend;
static uint64_t __fastcall h_ldsend(uint64_t comp, uint64_t req, uint64_t rpc, uint64_t cb, uint64_t a5) {
    if (g_localLoadouts && (uint64_t)_ReturnAddress() == kLdSaveSendRet) { status("loadouts: the save's send to EA dropped"); return req; }
    return o_ldsend(comp, req, rpc, cb, a5);
}

// Ruleset matching (2026-10-20): every connection transport is set up by FUN_140d7bb70(transport, socket, titleVersion,
// protocolVersion) (+0x748 / +0x74c); the server refuses a client whose protocol differs (WrongProtocolVersion, checked in
// FUN_140d798a0). The profile fingerprint (rules + value overrides, issue #2) is mixed into the protocol version on both
// sides, so players on different profiles cannot join each other. Default (fingerprint 0) keeps the stock value.
typedef uint8_t(__fastcall* tinit_t)(uint64_t transport, uint64_t socket, uint32_t title, uint32_t protocol);
static tinit_t o_tinit;
static uint8_t __fastcall h_tinit(uint64_t transport, uint64_t socket, uint32_t title, uint32_t protocol) {
    uint32_t p = protocol ^ g_profileFp;
    status("ruleset: transport %llx title %08x protocol %08x -> %08x (profile '%s', fingerprint %08x)", transport, title, protocol, p, g_profileName, g_profileFp);
    return o_tinit(transport, socket, title, p);
}

// Online error event handler (2026-10-20): FUN_1412df8a0(self, event) branches on event+4 == OnlineErrorReason_Generic
// (0x8ec30718, shown as error 327). Log every event it receives (first 32 bytes) and who called, to find 327's source.
typedef void(__fastcall* errflow_t)(uint64_t self, uint64_t ev);
static errflow_t o_errflow;
static void __fastcall h_errflow(uint64_t self, uint64_t ev) {
    uint8_t type = rd<uint8_t>(ev, 0); uint32_t reason = rd<uint32_t>(ev, 4), st = rd<uint32_t>(ev, 8);
    // type 0x1e status 1 = the "online backend connection lost" path of FUN_1412dd520: popup 327 + teardown of the online
    // data model (connection state, inventory, presets). In our listen/join modes we drop it (suppress_backend_lost=1).
    if (type == 0x1e && st == 1 && g_suppressBackendLost && !g_dedicated && (g_listen || g_joinAddr[0])) {
        status("online: suppressed the backend-lost event (type 0x1e, status 1, reason %08x)", reason);
        return;
    }
    if (type != 0x1e && reason != 0x8ec30718u) { o_errflow(self, ev); return; }   // log only the relevant events
    char fr[220]; caller_frames(_AddressOfReturnAddress(), fr, sizeof fr);
    uint32_t w[8]; for (int i = 0; i < 8; i++) w[i] = rd<uint32_t>(ev, i * 4);
    status("error event: self %llx (vtable %llx) event %llx: %08x %08x%s %08x %08x %08x %08x %08x %08x frames:%s", self, rd<uint64_t>(self, 0), ev,
           w[0], w[1], w[1] == 0x8ec30718u ? " (OnlineErrorReason_Generic = 327)" : "", w[2], w[3], w[4], w[5], w[6], w[7], fr);
    o_errflow(self, ev);
}

// Online error capture (2026-10-20): FUN_1412d43d0 maps a backend operation's result (+0x34: 0..6 = specific reasons,
// anything else = OnlineErrorReason_Generic, shown as "lost connection to EA Servers, error code 327") and queues the
// error. Log the result fields and who called, to identify the operation that fails in a networked match.
typedef void(__fastcall* onlerr_t)(uint64_t self, uint64_t result);
static onlerr_t o_onlerr;
static void __fastcall h_onlerr(uint64_t self, uint64_t result) {
    char fr[220]; caller_frames(_AddressOfReturnAddress(), fr, sizeof fr);
    status("online error: self %llx (vtable %llx) result %llx: +0x30 %d, +0x34 %d, +0x38 %llx, +0x00 %llx %llx %llx %llx frames:%s",
           self, rd<uint64_t>(self, 0), result, rd<int>(result, 0x30), rd<int>(result, 0x34), rd<uint64_t>(result, 0x38),
           rd<uint64_t>(result, 0), rd<uint64_t>(result, 8), rd<uint64_t>(result, 0x10), rd<uint64_t>(result, 0x18), fr);
    o_onlerr(self, result);
}

// Transport capture (2026-10-20, Kyber's way): log the client's connect call and the online-server constructor's
// socket-manager-position argument, with the vtables of the objects involved, to map Squadrons' socket interfaces.
// Addresses and vtables only; a connect password is logged as its length.
static bool looks_vtable(uint64_t v) { return v >= G(0x143000000ull) && v < G(0x146000000ull); }
static void dump_vtable(const char* tag, uint64_t obj) {
    uint64_t vt = rd<uint64_t>(obj, 0);
    if (!looks_vtable(vt)) { status("  %s %llx: first qword %llx (no vtable)", tag, obj, vt); return; }
    uint64_t e[20]; for (int i = 0; i < 20; i++) e[i] = rd<uint64_t>(vt, i * 8);
    status("  %s %llx vtable %llx: %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx", tag, obj, vt,
           e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16], e[17], e[18], e[19]);
}
static void dump_members(const char* tag, uint64_t obj, int bytes) {
    int n = 0;
    for (int off = 0; off < bytes && n < 40; off += 8) {
        uint64_t q = rd<uint64_t>(obj, off);
        if (q < 0x10000 || looks_vtable(q) || (q >= G(0x140000000ull) && q < 0x160000000ull)) continue;
        uint64_t v = rd<uint64_t>(q, 0);
        if (looks_vtable(v)) { status("  %s+%03x -> obj %llx vtable %llx", tag, off, q, v); n++; }
    }
}
typedef uint64_t(__fastcall* connect_t)(uint64_t obj, const char* addr, uint64_t pw, uint64_t a4);
static connect_t o_connect;
static uint64_t __fastcall h_connect(uint64_t obj, const char* addr, uint64_t pw, uint64_t a4) {
    {   // does the client's connect password equal ServerSettings.ServerPassword? compare in memory; log only the result
        uint64_t ss = rd<uint64_t>(G(0x144331c50), 0);
        const char* cp = (const char*)pw;   // the password is passed as a plain char* (client+0x458's value)
        const char* sp = ss ? (const char*)rd<uint64_t>(ss, 0xb0) : nullptr;
        char a1[64] = "", a2[64] = "";
        __try { if (cp) strncpy_s(a1, cp, _TRUNCATE); if (sp) strncpy_s(a2, sp, _TRUNCATE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        status("capture: connect password length %d, ServerSettings.ServerPassword length %d, equal %d", (int)strlen(a1), (int)strlen(a2), strcmp(a1, a2) == 0);
        SecureZeroMemory(a1, sizeof a1); SecureZeroMemory(a2, sizeof a2);
    }
    static char joinBuf[96];
    if (g_joinAddr[0] && addr && strncmp(safe_str((uint64_t)addr), "127.0.0.1:", 10) == 0 && g_lastInitMatch) {
        strcpy_s(joinBuf, g_joinAddr);
        status("join: connect rewritten from '%s' to '%s'", safe_str((uint64_t)addr), joinBuf);
        addr = joinBuf;
    }
    status("capture: client connect(obj %llx, '%s')", obj, safe_str((uint64_t)addr));
    dump_vtable("connect obj", obj);
    dump_members("connect obj", obj, 0x400);
    watch_connection(obj);
    uint64_t r = o_connect(obj, addr, pw, a4);
    // after the connect: the client's network peer (obj+0x40), its transport (peer+0x88) and the transport's socket
    // (+0x740, called at vtable +0x38 for packet info, Kyber's ISocket slot 7), plus client+0xa8 (BF2's creator slot)
    uint64_t peer = rd<uint64_t>(obj, 0x40), tr = peer ? rd<uint64_t>(peer, 0x88) : 0, sock = tr ? rd<uint64_t>(tr, 0x740) : 0;
    status("capture: after connect: peer %llx transport %llx socket %llx", peer, tr, sock);
    if (peer) dump_vtable("peer", peer);
    if (tr) dump_vtable("transport", tr);
    if (sock) dump_vtable("socket", sock);
    if (tr) dump_members("transport", tr, 0x800);
    return r;
}
typedef uint64_t(__fastcall* osrv_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static osrv_t o_osrv;
static uint64_t __fastcall h_osrv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                  uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11) {
    status("capture: online server ctor(this %llx, a3 %llx, a4 %llx, a5 %llx, a6 %llx, a7 %llx, a8 %llx, a9 %llx, a11 %llx)",
           a1, a3, a4, a5, a6, a7, a8, a9, a11 & 0xff);
    if (a7) dump_vtable("a7 (socket-manager position)", a7);
    if (a9) dump_vtable("a9", a9);
    if (a5) dump_vtable("a5", a5);
    uint64_t r = o_osrv(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    status("capture: online server constructed");
    return r;
}

// Crash catcher (diagnostics): logs fatal-looking exceptions (code, faulting address, registers, stack code pointers).
// It never handles anything: it always returns EXCEPTION_CONTINUE_SEARCH, so the game behaves exactly as without it.
// First-chance exceptions the game handles itself may also appear; at most 40 are logged.
extern "C" IMAGE_DOS_HEADER __ImageBase;
static volatile LONG g_crashLogs;
// Highest readable address of the stack region holding sp, so scans stop at the top of the stack instead of faulting.
static uint64_t stack_end(uint64_t sp) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)sp, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return sp;
    return (uint64_t)mbi.BaseAddress + mbi.RegionSize;
}
static bool in_this_dll(uint64_t a) {
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) &&
           m == (HMODULE)&__ImageBase;
}
static __declspec(thread) bool t_inCatcher;
static LONG CALLBACK crash_catcher(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (t_inCatcher || in_this_dll(ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_SEARCH;  // our own guarded reads
    bool interesting = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_PRIV_INSTRUCTION ||
                       code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == 0xC0000374 /* heap corruption */ ||
                       code == 0xC0000409 /* fail-fast / stack buffer overrun */ || code == 0xC000000D /* invalid parameter */;
    if (!interesting || InterlockedIncrement(&g_crashLogs) > 40) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    char line[1100]; int n = 0;
    n += sprintf_s(line + n, sizeof line - n, "EXCEPTION %08lx tid %lu at rip=%llx", code, GetCurrentThreadId(), c->Rip);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        n += sprintf_s(line + n, sizeof line - n, " (%s %llx)", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                       (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    n += sprintf_s(line + n, sizeof line - n, " rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx r8=%llx r9=%llx r12=%llx r13=%llx r14=%llx r15=%llx frames:",
                   c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->R8, c->R9, c->R12, c->R13, c->R14, c->R15);
    t_inCatcher = true;
    int found = 0;
    uint64_t top = stack_end(c->Rsp);
    __try {
        for (int i = 0; i < 4096 && found < 20 && c->Rsp + 8ull * i + 8 <= top; i++) {
            uint64_t v = ((uint64_t*)c->Rsp)[i];
            if (v >= G(0x140001000ull) && v < G(0x14307e400ull)) { n += sprintf_s(line + n, sizeof line - n, " %llx", v); found++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    status("%s", line);
    // 2026-10-20 entity-creation crash (FUN_1411590d0: data object in rsi has a null first qword): dump the first 8 qwords
    // of the data object (rsi) and its owner (rdi) as numbers, without following pointers, to identify the entity
    if (c->Rip == G(0x140b95224ull) || c->Rip == G(0x140b95214ull) || c->Rip == G(0x140b95204ull)) {
        // level-load assertion in FUN_140b950a0 state 2: rdi = level manager, rbp = load state; the bundle lookup
        // FUN_140cd57f0 used the name at [rdi+0xf0+0x38]
        status("  level manager %llx: name@+0x128 '%s' level@+0x110 %llx; load state %llx: +0x62c %x +0x694 %d",
               c->Rdi, safe_str(rd<uint64_t>(c->Rdi, 0x128)), rd<uint64_t>(c->Rdi, 0x110), c->Rbp,
               rd<uint32_t>(c->Rbp, 0x62c), (int)rd<uint8_t>(c->Rbp, 0x694));
    }
    if (c->Rip == G(0x14115910bull)) {
        uint64_t objs[2] = { c->Rsi, c->Rdi }; const char* nm[2] = { "rsi(data)", "rdi(owner)" };
        for (int k = 0; k < 2; k++) {
            uint64_t q[8] = {};
            __try { for (int i = 0; i < 8; i++) q[i] = ((uint64_t*)objs[k])[i]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            status("  %s %llx: %llx %llx %llx %llx %llx %llx %llx %llx", nm[k], objs[k], q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        }
    }
    t_inCatcher = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

// Stack sampler (diagnostics): every 2 s for the first 3 minutes, briefly suspends each other thread and logs its
// instruction pointer plus the first return addresses into the game's code found on its stack, so we can see what the
// engine is waiting on. Only reads; threads are resumed immediately. Only in dedicated mode or with stack_sample=1: in
// normal play a round can stall (2026-10-22 18:08:20, one round took 1.7 s and the simulation froze 0.52 s; Pseudo
// felt two stutters), and since 10-20 it had run for the first 3 minutes of every launch.
static void sample_threads(int round) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snapH = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapH == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    int idx = 0;
    for (BOOL ok = Thread32First(snapH, &te); ok; ok = Thread32Next(snapH, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u; ULONGLONG cpu = 0;
        if (GetThreadTimes(h, &c, &e, &k, &u)) cpu = (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime) / 10000;  // user ms
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            char line[640]; int n = 0;
            if (GetThreadContext(h, &ctx)) {
                n += sprintf_s(line + n, sizeof line - n, "sample %d tid %lu user=%llums rip=%llx frames:", round, te.th32ThreadID, cpu, ctx.Rip);
                // scan the top of the stack for values that point into the exe's code (.text1/.data2: 0x140001000-0x14307e400)
                int found = 0;
                uint64_t top = stack_end(ctx.Rsp);
                __try {
                    for (int i = 0; i < 2048 && found < 12 && ctx.Rsp + 8ull * i + 8 <= top; i++) {
                        uint64_t v = ((uint64_t*)ctx.Rsp)[i];
                        if (v >= G(0x140001000ull) && v < G(0x14307e400ull)) { n += sprintf_s(line + n, sizeof line - n, " %llx", v); found++; }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            ResumeThread(h);
            if (n) status("%s", line);
        }
        CloseHandle(h);
        idx++;
    }
    CloseHandle(snapH);
}

static DWORD WINAPI sampler(LPVOID) {
    for (int round = 0; round < 90; round++) {
        Sleep(round == 0 ? 500 : 2000);
        if (!g_netWrapped && round < 10) install_socket_wrappers();
#ifdef PROBE_TEST
        // the stand-in fills its socket vtables partway through its run, later on a slow machine (CI): keep trying
        if (g_sockLog && !g_sockPatched) install_socket_passthrough();
#else
        if (g_sockLog && !g_sockPatched && round < 10) install_socket_passthrough();
#endif
        if (g_dedicated || g_stackSample) sample_threads(round);
        else if (round >= 10) break;
    }
    return 0;
}

// Online backend chooser 0x1418f2930(spawnInfo): patched to always return OnlineBackend_Local (0xB8566ABC).
static void patch_backend_local() {
    static const uint8_t expect[6] = {0x48, 0x83, 0xec, 0x28, 0x80, 0x79};
    static const uint8_t patch[6] = {0xB8, 0xBC, 0x6A, 0x56, 0xB8, 0xC3};  // mov eax, 0xB8566ABC; ret
    uint8_t* at = (uint8_t*)G(0x1418f2930);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || memcmp(at, expect_bytes(0x1418f2930, expect, 6), 6) != 0) {
        status("backend patch: bytes differ or not mapped; NOT patched"); return;
    }
    DWORD old; VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, patch, 6);
    VirtualProtect(at, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), at, 6);
    status("backend chooser patched: always OnlineBackend_Local");
}
#endif

struct Site {
    const char* name;
    uint64_t addr;
    uint32_t steal;          // whole instructions, >= 14
    uint8_t expect[24];
    void* detour;
    void** orig;
};

static Site g_sites[] = {
    {"tick 0x1416ad2a0", 0x1416ad2a0, 21,
     {0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0x41,0x28,0x0f,0x28,0xc1,0xf3,0x0f,0x58,0x81,0x80,0x02,0x00,0x00},
     (void*)h_tick, (void**)&o_tick},
    {"drift 0x1416aa3a0", 0x1416aa3a0, 16,
     {0x40,0x55,0x41,0x56,0x48,0x8d,0x6c,0x24,0xb1,0x48,0x81,0xec,0x98,0x00,0x00,0x00},
     (void*)h_drift, (void**)&o_drift},
    {"drive 0x1416a98c0", 0x1416a98c0, 14,
     {0x40,0x53,0x48,0x83,0xec,0x70,0x48,0x8b,0x84,0x24,0xa8,0x00,0x00,0x00},
     (void*)h_drive, (void**)&o_drive},
    {"bounds 0x1416a7cf0", 0x1416a7cf0, 17,
     {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x40,0x80,0xb9,0xe0,0x03,0x00,0x00,0x00},
     (void*)h_bounds, (void**)&o_bounds},
    {"integ 0x1416a87c0", 0x1416a87c0, 15,
     {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x60,0x0f,0x29,0x74,0x24,0x50},
     (void*)h_integ, (void**)&o_integ},
#ifdef SERVER
    {"mainloop-ctor 0x1401ae7d0", 0x1401ae7d0, 19,
     {0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
     (void*)h_mlctor, (void**)&o_mlctor},
    {"spawnServer 0x1401aac90", 0x1401aac90, 17,
     {0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x48},
     (void*)h_spawn, (void**)&o_spawn},
    {"clocksel 0x1401bf4d0", 0x1401bf4d0, 22,
     {0x48,0x89,0x4c,0x24,0x08,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
     (void*)h_clocksel, (void**)&o_clocksel},
    {"gamesim-tick 0x1401bfe60", 0x1401bfe60, 14,
     {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x50},
     (void*)h_gstick, (void**)&o_gstick},
    {"inventory-set 0x14123a310", 0x14123a310, 20,
     {0x48,0x8b,0xc4,0x57,0x48,0x81,0xec,0x00,0x01,0x00,0x00,0x48,0xc7,0x44,0x24,0x28,0xfe,0xff,0xff,0xff},
     (void*)h_invset, (void**)&o_invset},
    {"loadouts-set 0x1418f58f0", 0x1418f58f0, 19,
     {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xf0,0x00,0x00,0x00},
     (void*)h_presetset, (void**)&o_presetset},
    {"loadouts-save 0x1419001c0", 0x1419001c0, 20,
     {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xc0,0xfc,0xff,0xff},
     (void*)h_presetsave, (void**)&o_presetsave},
    {"loadouts-send 0x1418f61b0", 0x1418f61b0, 18,
     {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40},
     (void*)h_ldsend, (void**)&o_ldsend},
    {"transport-setup 0x140d7bb70", 0x140d7bb70, 15,
     {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18},
     (void*)h_tinit, (void**)&o_tinit},
    {"error-event 0x1412df8a0", 0x1412df8a0, 20,
     {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xc0,0xf8,0xff,0xff},
     (void*)h_errflow, (void**)&o_errflow},
    {"online-error 0x1412d43d0", 0x1412d43d0, 15,
     {0x40,0x55,0x48,0x8d,0xac,0x24,0xd0,0xe3,0xff,0xff,0xb8,0x30,0x1d,0x00,0x00},
     (void*)h_onlerr, (void**)&o_onlerr},
    {"client-init-network 0x140a31d50", 0x140a31d50, 15,
     {0x48,0x8b,0xc4,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x60},
     (void*)h_cinit, (void**)&o_cinit},
    {"client-connect 0x140cb0500", 0x140cb0500, 19,
     {0x4c,0x8b,0xdc,0x57,0x48,0x81,0xec,0x90,0x00,0x00,0x00,0x49,0xc7,0x43,0x88,0xfe,0xff,0xff,0xff},
     (void*)h_connect, (void**)&o_connect},
    {"online-server-ctor 0x1412e5e50", 0x1412e5e50, 14,
     {0x4c,0x8b,0xdc,0x4d,0x89,0x43,0x18,0x49,0x89,0x4b,0x08,0x55,0x56,0x57},
     (void*)h_osrv, (void**)&o_osrv},
    {"conn-setup 0x140cb0080", 0x140cb0080, 15,
     {0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x83,0x79,0x38,0x00},
     (void*)h_cset, (void**)&o_cset},
    {"client-state 0x140a30230", 0x140a30230, 17,
     {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x83,0xb9,0xe8,0x02,0x00,0x00,0x00},
     (void*)h_cstate, (void**)&o_cstate},
    {"client-message 0x140a338d0", 0x140a338d0, 15,
     {0x48,0x8b,0xc4,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x90,0x00,0x00,0x00},
     (void*)h_cmsg, (void**)&o_cmsg},
    {"server-level-message 0x140ba0340", 0x140ba0340, 16,
     {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x8b,0x42,0x0c,0x48,0x8b,0xda},
     (void*)h_smsg, (void**)&o_smsg},
    {"bundle-queue 0x14021b2b0", 0x14021b2b0, 20,
     {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x50,0xff,0xff,0xff},
     (void*)h_bqueue, (void**)&o_bqueue},
    {"client-movement-ctor 0x14169cf00", 0x14169cf00, 15,
     {0x48,0x8b,0xc4,0x48,0x89,0x48,0x08,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x70},
     (void*)h_cmovctor, (void**)&o_cmovctor},
    {"client-health-ctor 0x141680fb0", 0x141680fb0, 19,
     {0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xa0,0x00,0x00,0x00},
     (void*)h_chealthctor, (void**)&o_chealthctor},
    {"client-weapon-ctor 0x14172a5e0", 0x14172a5e0, 14,
     {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x56,0x57},
     (void*)h_cweaponctor, (void**)&o_cweaponctor},
    {"client-power-ctor 0x1415fa310", 0x1415fa310, 21,
     {0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
     (void*)h_cpowerctor, (void**)&o_cpowerctor},
    {"level-step2 0x140cd6420", 0x140cd6420, 15,
     {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20},
     (void*)h_lvlstep2, (void**)&o_lvlstep2},
    {"starship-spawn 0x1416bcab0", 0x1416bcab0, 20,
     {0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xb0,0x48,0x81,0xec,0x50,0x01,0x00,0x00},
     (void*)h_sspawn, (void**)&o_sspawn},
    {"starship-direct-hit 0x14168a8b0", 0x14168a8b0, 18,
     {0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xa0},
     (void*)h_shiphit, (void**)&o_shiphit},
    {"ignore-ai-damage-check 0x1417da0e0", 0x1417da0e0, 20,
     {0x48,0x83,0xec,0x28,0xf6,0x82,0xa4,0x00,0x00,0x00,0x08,0x75,0x71,0x80,0xb9,0xc8,0x01,0x00,0x00,0x00},
     (void*)h_aicheck, (void**)&o_aicheck},
    {"capital-damage-apply 0x1417dbe90", 0x1417dbe90, 14,
     {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57},
     (void*)h_capapply, (void**)&o_capapply},
    {"capital-shield-hit 0x140582ab0", 0x140582ab0, 14,
     {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57},
     (void*)h_shdirect, (void**)&o_shdirect},
    {"capital-shield-blast 0x140582d90", 0x140582d90, 19,
     {0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xb0,0x48,0x81,0xec,0x50,0x01,0x00,0x00},
     (void*)h_shblast, (void**)&o_shblast},
    {"partition-new 0x140273060", 0x140273060, 15,
     {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20},
     (void*)h_partnew, (void**)&o_partnew},
    {"material-lookup 0x14057dd20", 0x14057dd20, 15,
     {0x40,0x53,0x48,0x83,0xec,0x20,0x41,0x8b,0x00,0x48,0x8b,0xd9,0x44,0x8b,0x02},
     (void*)h_matlookup, (void**)&o_matlookup},
    {"creep-scorer 0x14182ade0", 0x14182ade0, 14,
     {0x48,0x8b,0xc4,0x44,0x89,0x48,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55},
     (void*)h_scorer, (void**)&o_scorer},
    {"controller-field 0x1416feaf0", 0x1416feaf0, 15,
     {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x8b,0x02,0x48,0x8b,0xfa},
     (void*)h_ctrlfield, (void**)&o_ctrlfield},
    {"score-lookup 0x141617a00", 0x141617a00, 21,
     {0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
     (void*)h_scorelookup, (void**)&o_scorelookup},
#endif
};

static FILE* g_status;
static void status(const char* fmt, ...) {
    if (!g_status) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_status, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_status, fmt, a); va_end(a);
    fputc('\n', g_status); fflush(g_status);
}

static void absjmp(uint8_t* at, uint64_t to) {
    at[0] = 0xFF; at[1] = 0x25; *(uint32_t*)(at + 2) = 0; *(uint64_t*)(at + 6) = to;
}

static bool suspend_others(HANDLE* hs, int* n, int max) {
    *n = 0;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snapH = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapH == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snapH, &te); ok; ok = Thread32Next(snapH, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || *n >= max) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
        hs[(*n)++] = h;
    }
    CloseHandle(snapH);
    return true;
}
static void resume_all(HANDLE* hs, int n) { for (int i = 0; i < n; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); } }

// Which build this is, and its addresses in the tables made before that was known (file-scope initializers run before
// DllMain). Once, first thing in DllMain.
static void apply_build() {
    g_build = detect_build();
    if (g_build != BUILD_EA) return;
#ifdef SERVER
    for (uint64_t& v : kObsVt) v = G(v);
    for (uint64_t& v : kObsCi) v = G(v);
    for (GridSite& s : kGridSites) s.ret = G(s.ret);
#endif
    for (Site& s : g_sites) { memcpy(s.expect, expect_bytes(s.addr, s.expect, s.steal), s.steal); s.addr = G(s.addr); }
}

// The EA app's build is encrypted on disk: its wrapper decrypts the code on the main thread, after the DLL has loaded
// (about a second in), checks it, gets the licence and then starts the game (about five seconds in, 2026-10-27). Until
// then the hooks wait; waiting is not a failure.
static const DWORD kEaCodeWaitMs = 120000;

enum Inst { I_OK, I_RETRY, I_FAIL };
static Inst install(bool report = true) {
    HMODULE exe = GetModuleHandleW(nullptr);
#ifndef PROBE_TEST
    if ((uint64_t)exe != G(0x140000000ull)) { status("ABORT: exe base %p, expected 0x140000000", exe); return I_FAIL; }
#else
    (void)exe;
#endif
    static bool reported;
    for (auto& s : g_sites) {
        MEMORY_BASIC_INFORMATION mbi{};
        bool mapped = VirtualQuery((void*)s.addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (!mapped || memcmp((void*)s.addr, s.expect, s.steal) != 0) {
            if (!reported && report) {
                char hex[80] = {};
                if (mapped) for (uint32_t i = 0; i < s.steal; i++) sprintf_s(hex + 2 * i, 3, "%02x", ((uint8_t*)s.addr)[i]);
                status("ABORT: bytes at %s differ from the analysed exe: %s", s.name, mapped ? hex : "(not mapped)");
                reported = true;
            }
            return I_FAIL;
        }
    }
    // trampolines: stolen bytes + jmp back
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { status("ABORT: VirtualAlloc failed %lu", GetLastError()); return I_FAIL; }
    uint8_t* p = tramp;
    for (auto& s : g_sites) {
        memcpy(p, (void*)s.addr, s.steal);
        absjmp(p + s.steal, s.addr + s.steal);
        *s.orig = p;
        p += 64;
    }
    FlushInstructionCache(GetCurrentProcess(), tramp, 4096);

    static HANDLE hs[4096]; int n = 0;
    if (!suspend_others(hs, &n, 4096)) { status("ABORT: thread snapshot failed"); return I_FAIL; }
    // refuse if any suspended thread sits inside a patch range
    for (int i = 0; i < n; i++) {
        CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(hs[i], &c)) continue;
        for (auto& s : g_sites)
            if (c.Rip >= s.addr && c.Rip < s.addr + s.steal) {
                resume_all(hs, n); VirtualFree(tramp, 0, MEM_RELEASE); status("RETRY: a thread is inside %s", s.name); return I_RETRY;
            }
    }
    bool ok = true;
    for (auto& s : g_sites) {
        DWORD old;
        if (!VirtualProtect((void*)s.addr, s.steal, PAGE_EXECUTE_READWRITE, &old)) { ok = false; status("VirtualProtect failed at %s", s.name); break; }
        uint8_t patch[32]; memset(patch, 0x90, sizeof(patch));
        absjmp(patch, (uint64_t)s.detour);
        memcpy((void*)s.addr, patch, s.steal);
        VirtualProtect((void*)s.addr, s.steal, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)s.addr, s.steal);
        if (g_build == BUILD_STEAM) status("hooked %s (stole %u bytes)", s.name, s.steal);
        else status("hooked %s at %llx in the %s build (stole %u bytes)", s.name, s.addr, build_name(), s.steal);
    }
#ifdef SERVER
    if (ok && g_forceLocal) patch_backend_local();
#endif
    resume_all(hs, n);
    return ok ? I_OK : I_FAIL;
}

// ---------------------------------------------------------------- writer
static FILE* g_out;
static DWORD WINAPI worker(LPVOID);
static void read_server_ini();

// Logs, config and the record ring. Called from the worker thread (probe/rules) or from DllMain (server).
static void setup() {
    // logs: E:\sws_community\probe_logs on the development PC, otherwise a "logs" folder beside this DLL
    char logDir[MAX_PATH] = "E:\\sws_community\\probe_logs";
    DWORD la = GetFileAttributesA(logDir);
    if (la == INVALID_FILE_ATTRIBUTES || !(la & FILE_ATTRIBUTE_DIRECTORY)) {
        HMODULE me = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&setup, &me);
        GetModuleFileNameA(me, logDir, MAX_PATH);
        char* bs = strrchr(logDir, '\\'); if (bs) strcpy_s(bs + 1, MAX_PATH - (bs + 1 - logDir), "logs");
        CreateDirectoryA(logDir, nullptr);
    }
    SYSTEMTIME st; GetLocalTime(&st);
    char base[MAX_PATH];
#if defined(SERVER)
    const char* prefix = "server";
#elif defined(RULES)
    const char* prefix = "rules";
#else
    const char* prefix = "probe";
#endif
    sprintf_s(base, "%s\\%s_%04d%02d%02d_%02d%02d%02d", logDir, prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char path[MAX_PATH];
    // _SH_DENYWR: other processes may read the logs while the game is running (fopen_s opens exclusively)
    sprintf_s(path, "%s.status.txt", base); g_status = _fsopen(path, "w", _SH_DENYWR);
    char binPath[MAX_PATH]; sprintf_s(binPath, "%s.bin", base);
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    status("%s loaded; build %s %s; qpc freq %lld; log %s", prefix, __DATE__, __TIME__, fq.QuadPart, path);
    status("game build: %s", build_name());
#ifdef RULES
    {
        // the profile (issue #2), or rules.ini when server.ini names none
        wchar_t dir[MAX_PATH]; HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&worker, &self);
        GetModuleFileNameW(self, dir, MAX_PATH);
        wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) *sl = 0;
        load_profile(dir);
        status("rules: no_zero_throttle_pinball=%d no_multidrift=%d (cruise margin %.1f)", g_rulePinball, g_ruleMultidrift, kCruiseMargin);
    }
#endif

#ifdef SERVER
    read_server_ini();
#endif
    if (g_record) {   // the flight record is created only when recording (server.ini [server] record=1)
        g_out = _fsopen(binPath, "wb", _SH_DENYWR);
        status("record: on; flight record %s", binPath);
    } else status("record: off (the settings app's \"Record flight data\" checkbox; server.ini record=1)");
    g_ring = (Rec*)VirtualAlloc(nullptr, sizeof(Rec) * RING, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    { Rec* r = claim(); stamp(r, R_INFO, 0); r->f[0] = (float)fq.QuadPart; *(int64_t*)&r->f[2] = fq.QuadPart; commit(); }
}

static volatile LONG g_hooked;

// Installs the hooks, retrying while a thread sits inside a patch range. In test builds it also waits (up to 10 s)
// for the stand-in code to be planted, since a test target maps it after the DLL loads.
static void install_retrying() {
    if (g_build == BUILD_EA) {
        // The wrapper checks the decrypted code before it starts the game, and stalls for good if our patches are in it
        // (2026-10-27: hooked as soon as the code appeared, it never asked for its licence). Its last step before the
        // game's own code runs is filling the game's import table, so wait for that too; the game's start-up then takes
        // seconds before the main loop is created. Checked every millisecond.
        void* enter = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "EnterCriticalSection");
        uint64_t slot = G(0x14307f388ull);   // the game's import of EnterCriticalSection
        auto started = [&] {
            MEMORY_BASIC_INFORMATION mbi{};
            return VirtualQuery((void*)slot, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                   !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && *(void* volatile*)slot == enter;
        };
        DWORD t0 = GetTickCount();
        while (!g_hooked && GetTickCount() - t0 < kEaCodeWaitMs) {
            if (started() && install(false) == I_OK) {
                g_hooked = 1; status("EA app build: the game has started; hooked after %lu ms", GetTickCount() - t0);
            }
            else Sleep(1);
        }
        if (!g_hooked) status("EA app build: the game did not start within %lu s", kEaCodeWaitMs / 1000);
    }
    for (int attempt = 0; attempt < 200 && !g_hooked; attempt++) {
        Inst r = install();
        if (r == I_OK) { g_hooked = 1; break; }
#ifndef PROBE_TEST
        if (r == I_FAIL) break;
#endif
        Sleep(50);
    }
    status(g_hooked ? "all hooks installed; logging" : "NOT HOOKED; probe idle (game unaffected)");
    if (g_buildMisses) status("WARNING: %ld game addresses missing from the %s build's table (build_ea.h is older than the source)", g_buildMisses, build_name());
}

static DWORD WINAPI worker(LPVOID) {
#ifndef SERVER
    setup();
#endif
    if (!g_hooked) install_retrying();
    FILE* out = g_out;

    LONG64 tail = 0;
    bool keys[3] = {};
    const int vk[3] = { VK_F8, VK_F9, VK_F10 };
    LONG64 written = 0;
    DWORD lastStat = GetTickCount();
    for (;;) {
        Sleep(20);
        for (int k = 0; k < 3; k++) {
            bool down = (GetAsyncKeyState(vk[k]) & 0x8000) != 0;
            if (down && !keys[k]) {
                Rec* r = claim(); stamp(r, R_MARK, 0); r->f[0] = (float)(k + 1); commit(); status("MARK %d", k + 1);
                Beep(600 + 300 * k, 120);  // audible confirmation: F8 low, F9 mid, F10 high (blocks only this writer thread)
            }
            keys[k] = down;
        }
        LONG64 head = g_done;  // conservative: only records counted as committed
        if (head - tail > RING) { InterlockedAdd64(&g_dropped, head - tail - RING); tail = head - RING; }
        while (tail < head) {
            Rec* r = &g_ring[tail & (RING - 1)];
            if (r->seq != (uint32_t)(tail + 1)) {
                // normally "not finished yet": wait a pass. If we're far behind, it was overwritten: skip it.
                if (head - tail > 4096) { tail++; InterlockedIncrement64(&g_dropped); continue; }
                break;
            }
            if (out) fwrite(r, sizeof(Rec), 1, out);
            tail++; written++;
        }
        if (out) fflush(out);
        if (GetTickCount() - lastStat > 10000) { status("written %lld records, dropped %lld", written, g_dropped); lastStat = GetTickCount(); }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        apply_build();
        if (g_build == BUILD_UNKNOWN) {   // a game exe the mod has no addresses for: say so, and touch nothing in it
            setup();
            const uint8_t* b = (const uint8_t*)GetModuleHandleW(nullptr);
            const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
            status("NOT HOOKING: this game exe (time stamp %08lx, image size %08lx) is neither the Steam build nor the EA app "
                   "build the mod knows. The game runs without the mod. sws_check.exe reports what this copy is.",
                   nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
            return TRUE;
        }
#ifdef SERVER
        // Loaded by sws_launch.exe on the suspended main thread, before the game's own code runs: set up and hook
        // synchronously so the MainLoop constructor hook is in place before the main loop is created. The EA app's build
        // is still encrypted here and is decrypted on this thread once we return, so it is hooked from the worker
        // thread instead, when the wrapper starts the game.
        setup();
        if (g_build == BUILD_EA) status("EA app build: waiting for its wrapper to start the game before hooking");
        else if (install() == I_OK) { g_hooked = 1; status("hooked synchronously at load"); }
#ifdef SERVER
        install_socket_wrappers();
        if (g_sockLog) { install_socket_passthrough(); HANDLE t = CreateThread(nullptr, 0, sock_summary, nullptr, 0, nullptr); if (t) CloseHandle(t); }
#endif
        { HANDLE s = CreateThread(nullptr, 0, sampler, nullptr, 0, nullptr); if (s) CloseHandle(s); }
        if (g_observe) { HANDLE o = CreateThread(nullptr, 0, observe_thread, nullptr, 0, nullptr); if (o) CloseHandle(o); }
        AddVectoredExceptionHandler(1, crash_catcher);
        status("crash catcher installed");
#endif
        HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
