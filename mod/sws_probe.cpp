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

// Profiles (issue #2). server.ini [server] profile=<name> selects profiles\<name>.ini beside the DLL:
//   [rules]   no_zero_throttle_pinball, no_multidrift, fix_zavian_creep_pack (missing = off; the map fix is in the
//             fingerprint only when on, so profiles without it keep theirs)
//   [values]  <asset>.<field>=<number> overrides (applied by the runtime value engine, issue #3)
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
    if (!g_rulePinball && !g_ruleMultidrift && !g_ruleZavianCreep && !g_overrideCount) return 0;
    int* order = (int*)_alloca(sizeof(int) * (g_overrideCount + 1));
    for (int i = 0; i < g_overrideCount; i++) order[i] = i;
    for (int i = 1; i < g_overrideCount; i++)   // insertion sort by key (lists are small)
        for (int j = i; j > 0 && strcmp(g_overrides[order[j - 1]].key, g_overrides[order[j]].key) > 0; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    char b[192]; uint32_t h = 2166136261u;
    sprintf_s(b, "pinball=%d;multidrift=%d;", (int)g_rulePinball, (int)g_ruleMultidrift); h = fnv1a(h, b);
    if (g_ruleZavianCreep) h = fnv1a(h, "zaviancreep=1;");   // only when on, so profiles without it keep their fingerprint
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
    "materialgrid" };   // laser and ion damage against capital ships (grid_apply)
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
static wchar_t g_dllDir[MAX_PATH];
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
    g_profileFp = profile_fingerprint();
    status("profile: '%s': no_zero_throttle_pinball=%d no_multidrift=%d fix_zavian_creep_pack=%d, %d value override(s), fingerprint %08x%s",
           g_profileName, (int)g_rulePinball, (int)g_ruleMultidrift, (int)g_ruleZavianCreep, g_overrideCount, g_profileFp,
           g_profileFp ? "" : " (stock protocol)");
    for (int i = 0; i < g_overrideCount; i++) status("  value %s = %.9g (applied by the value engine, issue #3)", g_overrides[i].key, g_overrides[i].value);
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
struct HitboxScale { char ship[32]; float scale; };
static HitboxScale g_hitboxScales[32]; static int g_hitboxScaleCount;   // [hitbox] <ship>=<scale>: experiment (#20)
static bool g_spawnLog = false;         // [server] spawn_log: 1 = log every starship the server spawns, and whether it worked
static bool g_materialLog = false;      // [server] material_log: 1 = log the material pairs the grid is asked about (diagnostic)
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
      g_spawnLog = GetPrivateProfileIntW(L"server", L"spawn_log", 0, ini) != 0;
      { wchar_t sec[2048] = L""; DWORD n = GetPrivateProfileSectionW(L"hitbox", sec, 2048, ini);
        for (wchar_t* e = sec; n && *e && g_hitboxScaleCount < 32; e += wcslen(e) + 1) {
            wchar_t* eq = wcschr(e, L'='); if (!eq || e[0] == L';') continue;
            HitboxScale& h = g_hitboxScales[g_hitboxScaleCount]; int k = 0;
            for (wchar_t* c = e; c < eq && k < 31; c++) if (!iswspace(*c)) h.ship[k++] = (char)towlower(*c);
            h.ship[k] = 0; h.scale = (float)_wtof(eq + 1);
            if (k && h.scale > 0.01f && h.scale < 10.0f) g_hitboxScaleCount++;
        } }
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
#define SLOT_BIND 0x14307fef0ull
#define SLOT_SOCKET 0x14307fee0ull
#define SLOT_WSASOCKETW 0x14307ff70ull
#define SLOT_SENDTO 0x14307ff10ull
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
            if (v >= 0x140001000ull && v < 0x14307e400ull) { k += sprintf_s(out + k, n - k, " %llx", v); found++; }
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
#define VT_SOCKMGR 0x1433fa218ull
#define VT_SOCKET  0x1433fa240ull
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
        if (haveTick < 0) haveTick = committed(0x140696cb0) && rd<uint32_t>(0x140696cb0, 0) == 0x28ec8348u;   // sub rsp,0x28
        *when = haveTick ? ((nettick_t)0x140696cb0)() : timeGetTime();
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
        {VT_SOCKMGR, 0, (void*)tm_destroy, (void**)&om_destroy, 0x140295bb0}, {VT_SOCKMGR, 1, (void*)tm_connect, (void**)&om_connect, 0x140295dd0},
        {VT_SOCKMGR, 2, (void*)tm_listen, (void**)&om_listen, 0x140295c40},   {VT_SOCKMGR, 3, (void*)tm_create, (void**)&om_create, 0x140295f90},
        {VT_SOCKMGR, 4, (void*)tm_dtor, (void**)&om_dtor, 0x140295b50},
        {VT_SOCKET, 0, (void*)ts_dtor, (void**)&os_dtor, 0x140294ff0},        {VT_SOCKET, 1, (void*)ts_send, (void**)&os_send, 0x1402951c0},
        {VT_SOCKET, 2, (void*)ts_recv, (void**)&os_recv, 0x1402933a0},        {VT_SOCKET, 5, (void*)ts_setpeer, (void**)&os_setpeer, 0x140295800},
        {VT_SOCKET, 7, (void*)ts_pinfo, (void**)&os_pinfo, 0x140295890},
        {VT_SOCKET, 17, (void*)ts_connect, (void**)&os_connect, 0x140295700}, {VT_SOCKET, 18, (void*)ts_listen, (void**)&os_listen, 0x1402957a0},
        {VT_SOCKET, 19, (void*)ts_create, (void**)&os_create, 0x140295760},
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
    safe_copy(((getopt_t)0x141191d50)(ls, "GameMode"), gm, gmsz);
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
static const uint64_t kStrAssign = 0x14025c3f0, kSetOption = 0x141191750, kEmptyStr = 0x143f7b968, kEmptyArr = 0x14317a6f0;

static bool helpers_ok() {
#ifdef PROBE_TEST
    return true;  // test targets plant simplified stand-ins
#else
    static const uint8_t sa[12] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x48};
    static const uint8_t so[12] = {0x48,0x8b,0xc4,0x4c,0x89,0x40,0x18,0x48,0x89,0x48,0x08,0x55};
    return memcmp((void*)kStrAssign, sa, 12) == 0 && memcmp((void*)kSetOption, so, 12) == 0;
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
    uint64_t sched = rd<uint64_t>(0x1442f7c68, 0);
    if (!sched) { status("3b: bundle %s: no job scheduler", name); return 0x07000000; }
    const char* names[1] = { name };
    InterlockedIncrement((volatile LONG*)0x1442d18c0);
    uint32_t h[4] = {};
    ((bq_queue_t)0x14021b2b0)(h, sched, 0, names, 1, 0, 0, 0, 0);
    status("3b: bundle %s: queued, handle %08x", name, h[0]);
    if ((h[0] >> 24) == 7) InterlockedDecrement((volatile LONG*)0x1442d18c0);
    return h[0];
}
static bool bundle_done(uint32_t h) {
    if ((h >> 24) == 7) return true;
    return ((bq_done_t)0x14021ba60)(rd<uint64_t>(0x1442f7c68, 0), h);
}
static void release_bundle(const char* name, uint32_t h, ULONGLONG ms) {
    if ((h >> 24) == 7) return;
    uint64_t sched = rd<uint64_t>(0x1442f7c68, 0);
    uint64_t cs = sched + 0x22600 + (uint64_t)(((h & 0xffffff) >> 16) & 0xf) * 0x1180;
    (*(bq_cs_t*)0x14307f388)(cs);
    bool ok = ((bq_release_t)0x14021cb70)(cs, h & 0xffff, (h & 0xffffff) >> 20);
    (*(bq_cs_t*)0x14307f2e8)(cs);
    InterlockedDecrement((volatile LONG*)0x1442d18c0);
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
        uint64_t job = rd<uint64_t>(0x1441e2938, 0);
        uint8_t valid = job ? rd<uint8_t>(job, 0) : 0;
        status("3b: server-init job object at 0x1441e2938 = %p (first byte %d)", (void*)job, (int)valid);
        if (job) {  // its first qwords, to identify what the job runs (pointers into the exe are code or vtables)
            uint64_t q[6]; for (int i = 0; i < 6; i++) q[i] = rd<uint64_t>(job, i * 8);
            status("3b: job qwords %llx %llx %llx %llx %llx %llx", q[0], q[1], q[2], q[3], q[4], q[5]);
            for (int i = 0; i < 6; i++) if (q[i] >= 0x140001000ull && q[i] < 0x1459b0000ull)
                status("3b: job+%d -> %llx: %llx %llx %llx %llx", i * 8, q[i], rd<uint64_t>(q[i], 0), rd<uint64_t>(q[i], 8),
                       rd<uint64_t>(q[i], 16), rd<uint64_t>(q[i], 24));
        }
        static const uint8_t expect[7] = {0x48, 0x8b, 0x05, 0xa8, 0x4b, 0x64, 0x03};
        static const uint8_t jmp[7] = {0xE9, 0x3D, 0x01, 0x00, 0x00, 0x90, 0x90};
        uint8_t* at = (uint8_t*)0x140b9dd89;
        MEMORY_BASIC_INFORMATION mbi{};
        if (g_spawnDedicated && (g_skipJobAlways || (g_skipNullJob && (!job || !valid)))) {
            if (VirtualQuery(at, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && memcmp(at, expect, 7) == 0) {
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
    ((spawn_t)0x1401aac90)(gs, info);  // through our spawnServer hook, so it is logged too
    {   // ServerSettings, cached by Server::init in DAT_144331c50 (its +0xa0 DedicatedServerCpu feeds the affinity code)
        uint64_t ss = rd<uint64_t>(0x144331c50, 0);
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
struct VeLeaf { const char* key; uint64_t addr; bool isInt; uint32_t attrId; int list; int comp; bool isBool; };
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
        uint16_t lo = rd<uint16_t>(kVeTypes[t].classInfo, 0x48), hi = rd<uint16_t>(kVeTypes[t].classInfo, 0x4a);
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
        case 10: {   // a component's AttributeModifier list: each Float modifier, named by its attribute
            key[n] = 0;
            uint64_t arr = rd<uint64_t>(a, 0); int cnt = arr ? (int)rd<uint32_t>(arr - 4, 0) : 0; if (cnt > 64) cnt = 0;
            char names[64][64]; uint64_t mods[64];
            for (int j = 0; j < cnt; j++) {
                mods[j] = rd<uint64_t>(arr, 8 * j); names[j][0] = 0;
                if (mods[j] && rd<uint64_t>(mods[j], 8) == kVeTypes[f.type].classInfo) attr_name(rd<uint64_t>(mods[j], 0x18), names[j], 64);
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
        if (same && regSame) continue;
        bool ok = leaf_write(l.addr, l.isInt, target, l.isBool) && (!reg || leaf_write(reg, l.isInt, target, l.isBool));
        if (!v.logged) {
            v.logged = true;
            status("values: %s %.9g -> %.9g%s%s%s", l.key, now, l.isInt ? (double)llround(target) : (double)(float)target,
                   v.botOf[0] ? " (vs-AI bot, following the player)" : "",
                   l.list ? (reg ? " (attribute and its list entry)" : " (attribute; no list entry)") : "", ok ? "" : " WRITE FAILED");
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
        lo[k] = rd<uint16_t>(kVeTypes[kVeRoots[k]].classInfo, 0x48); hi[k] = rd<uint16_t>(kVeTypes[kVeRoots[k]].classInfo, 0x4a); }
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
    if (!ready || !committed(0x1442d18c8)) return;
    typedef void (__fastcall* cs_t)(uint64_t cs);
    cs_t enter = rd<cs_t>(0x14307f388, 0), leave = rd<cs_t>(0x14307f2e8, 0);
    if (!enter || !leave) return;
    uint16_t lo[kVeMaxRoots], hi[kVeMaxRoots]; ve_root_ranges(lo, hi);
    ULONGLONG t0 = GetTickCount64(); uint64_t domains[64]; int nd = 0; long inspected = 0, matched = 0;
    for (int c = 0; c < 0xbbe; c++) {
        uint64_t entry = rd<uint64_t>(0x1442d18c8, c * 8); if (!entry) continue;
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
typedef uint64_t(__fastcall* partnew_t)(uint64_t store, uint64_t a2, uint64_t loader, uint64_t a4);
static partnew_t o_partnew;
static uint64_t __fastcall h_partnew(uint64_t store, uint64_t a2, uint64_t loader, uint64_t a4) {
    uint64_t part = o_partnew(store, a2, loader, a4);
    if (part && g_ruleZavianCreep) map_fix_partition(part);
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
static const uint64_t kObsVt[5] = { 0, 0x143614da0, 0x143612168, 0x143603d68, 0x14361e2a0 };
// per output kind: the data object's ClassInfo, the first word's offset, and the number of words
static const uint64_t kObsCi[9] = { 0, 0x144a16420, 0x144a02ec0, 0x144a030c0, 0x144a36d80, 0x144a36d00, 0x144a05510, 0x144a15420,
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
// ---- Ship scale (issue #20) ---------------------------------------------------------------------------------------
// A starfighter's laser hitbox is the compressed triangle meshes in its physics (starship_<ship>_physics_win32, Havok
// 2013.3 hknp). A mesh's data (hknpCompressedMeshShapeData, 64-bit layout) keeps its vertices quantized per section,
// decoded as offset + q * scale with the section's six codec floats (+0x30), its shared vertices relative to the mesh's
// domain box (+0x20/+0x30), and each section has a domain box (+0x10/+0x20) that its culling tree decodes against.
// Multiplying those by s scales the mesh about the ship's origin, whatever reads it (2026-10-23: at 0.05, Rebel ships
// couldn't be hit). The compounds' instance scale (flag 0x08) is ignored by the laser path.
// The scale is applied as the packfile loads: Havok's in-place loader FUN_142706d60(packfile, size, 0, 0), called by
// Frostbite's Havok resource loading (FUN_141cad6d0), is hooked at its entry, before its pointer fixups and finish
// constructors, so everything Havok derives at load, and every body built later, starts from the scaled geometry.
// (Scaling meshes found by searching memory mid-match stuttered and crashed: geometry changed under live bodies.)
// hitbox_shapes.h (local/tools/gen_hitbox_shapes.py) lists every starfighter's meshes: their domain bytes, where their
// sections start and how many, and the first section's codec as shipped. A mesh is scaled only if its count and first
// codec are as shipped. [hitbox] <ship>=<scale> (ywing, xwing_ai, ...) says which; player and AI physics are separate
// resources, but some meshes are identical in both, and those are scaled if either is asked for.
#include "hitbox_shapes.h"
static const int kHitboxMeshCount = (int)(sizeof kHitboxMeshes / sizeof kHitboxMeshes[0]);
static float hitbox_scale_for(const char* ship) {
    for (int i = 0; i < g_hitboxScaleCount; i++) if (!strcmp(g_hitboxScales[i].ship, ship)) return g_hitboxScales[i].scale;
    return 0;
}
// the scale for a mesh that matches table entry `first` (any identical entry asked for), and who it is, for the log
static float hitbox_mesh_scale(int first, char* who, size_t cap, int* matches) {
    const HitboxMesh& m = kHitboxMeshes[first]; float s = 0; *matches = 0; who[0] = 0;
    for (int e = 0; e < kHitboxMeshCount; e++) {
        const HitboxMesh& x = kHitboxMeshes[e];
        if (memcmp(x.domain, m.domain, 32) || x.count != m.count || x.sections != m.sections || memcmp(x.codec, m.codec, 24)) continue;
        (*matches)++;
        if (strlen(who) + strlen(x.ship) + 12 < cap) sprintf_s(who + strlen(who), cap - strlen(who), "%s%s mesh %d", who[0] ? " / " : "", x.ship, x.index);
        float f = hitbox_scale_for(x.ship); if (f > 0 && s == 0) s = f;
    }
    return s;
}
// scale every known mesh found in [buf, buf + size) whose ship is asked for; returns how many
static int hitbox_scale_packfile(uint64_t buf, uint32_t size) {
    int scaled = 0;
    __try {
        for (uint64_t p = buf; p + 0x40 <= buf + size; p += 16) {
            uint64_t v = *(const uint64_t*)p;
            if (!v) continue;
            for (int h = 0; h < kHitboxMeshCount; h++) {
                const HitboxMesh& m = kHitboxMeshes[h];
                uint64_t first; memcpy(&first, m.domain, 8);
                if (v != first || memcmp((const void*)p, m.domain, 32)) continue;
                uint64_t data = p - 0x20;
                if (data < buf || data + m.sections + 0x60ull * m.count > buf + size) continue;
                if (*(const uint32_t*)(data + 0x58) != m.count || memcmp((const void*)(data + m.sections + 0x30), m.codec, 24)) continue;
                char who[160]; int matches;
                float s = hitbox_mesh_scale(h, who, sizeof who, &matches);
                if (!(s > 0 && s != 1.0f)) break;
                for (uint32_t j = 0; j < m.count; j++) {
                    float* sec = (float*)(data + m.sections + 0x60ull * j);
                    for (int c = 0; c < 3; c++) { sec[4 + c] *= s; sec[8 + c] *= s; }   // the section's domain min (+0x10) and max (+0x20)
                    for (int c = 0; c < 6; c++) sec[12 + c] *= s;                      // its codec: offset xyz, scale xyz (+0x30)
                }
                float* dom = (float*)(data + 0x20);
                for (int c = 0; c < 3; c++) { dom[c] *= s; dom[4 + c] *= s; }          // the mesh's domain min (+0x20) and max (+0x30)
                status("hitbox: %s, %u sections, scaled %.3g as it loads%s", who, m.count, s, matches > 1 ? " [identical meshes: can't tell apart]" : "");
                scaled++;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { status("hitbox: a packfile at %llx couldn't be read whole; left as it was", buf); }
    return scaled;
}
// Three in-place loaders take a packfile and its size. Frostbite's physics resources (HavokPhysicsData, resource type
// djb2("havokphysicsdata") = 0x91043f65) come through its create step FUN_141caddc0 (the class's vtable 0x1436e01a8,
// slot 7), which hands FUN_142707130(packfile, size, fixups, ...) the packfile with its pointer fixups in a separate table
// (Frostbite's own packfile variant). Havok's FUN_142706d60 and FUN_142706770 load classic packfiles; two flights hooking
// only those scaled nothing, as no packfile came through them. All three are hooked; each one's packfiles are counted and
// the first few logged, so a flight shows which path is used.
static volatile LONG g_hkSeen[3], g_hkScaled[3];
static void hitbox_on_load(int which, uint64_t packfile, uint32_t size) {
    if (!g_hitboxScaleCount) return;
    LONG seen = InterlockedIncrement(&g_hkSeen[which]);
    uint32_t magic = packfile ? rd<uint32_t>(packfile, 0) : 0;
    if (!packfile || size < 0x100 || size >= (64u << 20) || magic != 0x57e0e057) {
        if (seen <= 3) status("hitbox: loader %d: buffer %llx (%u bytes) starts %08x, not a packfile; left alone", which + 1, packfile, size, magic);
        return;
    }
    int n = hitbox_scale_packfile(packfile, size);
    LONG scaled = n ? InterlockedExchangeAdd(&g_hkScaled[which], n) + n : g_hkScaled[which];
    if (seen <= 3 || n || seen % 200 == 0)
        status("hitbox: loader %d: packfile %llx (%u bytes), %d mesh(es) scaled; %ld packfiles and %ld meshes so far", which + 1, packfile, size, n, seen, scaled);
}
typedef uint64_t(__fastcall* hkload_t)(uint64_t packfile, uint32_t size, uint64_t a3, uint64_t a4);
static hkload_t o_hkload;
static uint64_t __fastcall h_hkload(uint64_t packfile, uint32_t size, uint64_t a3, uint64_t a4) {
    hitbox_on_load(0, packfile, size);
    return o_hkload(packfile, size, a3, a4);
}
typedef uint64_t(__fastcall* hkload3_t)(uint64_t packfile, uint32_t size, uint64_t fixups, uint64_t a4);
static hkload3_t o_hkload3;
static uint64_t __fastcall h_hkload3(uint64_t packfile, uint32_t size, uint64_t fixups, uint64_t a4) {
    hitbox_on_load(2, packfile, size);
    return o_hkload3(packfile, size, fixups, a4);
}
typedef uint64_t(__fastcall* hkload2_t)(uint64_t packfile, uint32_t size, uint64_t a3);
static hkload2_t o_hkload2;
static uint64_t __fastcall h_hkload2(uint64_t packfile, uint32_t size, uint64_t a3) {
    hitbox_on_load(1, packfile, size);
    return o_hkload2(packfile, size, a3);
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


static void __fastcall h_gstick(uint64_t gs, uint64_t a2, uint64_t a3, uint64_t a4) {
    o_gstick(gs, a2, a3, a4);
    { static ULONGLONG lastScan; ULONGLONG nowScan = GetTickCount64();
      if (nowScan - lastScan >= 2000) { lastScan = nowScan; value_engine_scan(); } }
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
        if (!posted && !gaveUp && g_postLoadLevel && g_spawnLs && rd<uint64_t>(gs, 0xF8) && committed(0x140ba0c20) &&
            memcmp((void*)0x140ba0c20, kPostLoadLevelPrologue, sizeof kPostLoadLevelPrologue) == 0) {
            typedef uint64_t (__fastcall* lookup_t)(const char* levelName);
            typedef void (__fastcall* preload_t)();
            typedef void (__fastcall* post_t)(void* levelSetup, uint64_t unused, uint64_t flag, uint64_t u4);
            ULONGLONG pt = GetTickCount64();
            if (!postWaitStart) postWaitStart = pt;
            uint64_t lb = rd<uint64_t>(0x143ea0768, 0), le = rd<uint64_t>(0x143ea0770, 0);
            uint64_t found = ((lookup_t)0x140cd57f0)(g_rootLevel);
            if (found) {
                status("3b: level lookup found '%s' (%p); list has %llu entries", g_rootLevel, (void*)found, (le - lb) / 8);
                status("3b: posting ServerLoadLevelMessageBase (FUN_140ba0c20) with the spawn LevelSetup");
                ((post_t)0x140ba0c20)(g_spawnLs, 0, 0, 0);
                status("3b: load-level message posted");
                posted = true;
            } else if (pt - lastPreload >= 1000) {
                lastPreload = pt;
                char names[600] = ""; size_t n = 0;
                for (uint64_t q = lb; q && q < le && n < 4; q += 8, n++) {
                    strncat_s(names, safe_str(rd<uint64_t>(rd<uint64_t>(q, 0), 0x30)), _TRUNCATE); strncat_s(names, "; ", _TRUNCATE);
                }
                status("3b: level lookup: '%s' not in the list (%llu entries: %s); re-running the preload", g_rootLevel, (le - lb) / 8, names);
                ((preload_t)0x140cd55e0)();
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
        bool engine = committed(0x14021b2b0) && committed(0x1442f7c68) && committed(0x1442d18c0);
        if (!g_loadStartupData || !engine) {
            if (g_loadStartupData) status("3b: bundles: engine not present (test target); skipped");
            phase = 3; do_spawn(gs); return;
        }
        bundle = "Systems/FrostbiteStartupData"; handle = queue_bundle(bundle); phase = 1; phaseStart = lastNote = now;
        return;
    }
    if (!bundle_done(handle)) {
        if (now - phaseStart > 5000 && !pumping) { pumping = true; status("3b: bundle %s: not done after 5 s; pumping the resource manager each tick", bundle); }
        if (pumping) ((bq_pump_t)0x1401f7960)(0x1442d18c0, 1, 0);
        if (now - lastNote > 5000) { lastNote = now; status("3b: bundle %s: still loading (%llu ms)", bundle, now - phaseStart); }
        return;
    }
    release_bundle(bundle, handle, now - phaseStart);
    pumping = false;
    if (phase == 1 && g_loadGameConfig) {
        const char* cfg = (const char*)rd<uint64_t>(0x1441e2938, 0);
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
    if (g_spawnLog) status("spawn: entity %llx (data %llx) -> %s %llx", ent, rd<uint64_t>(ent, 0x38), r ? "ship" : "NOTHING SPAWNED", r);
    return r;
}
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
static const GridSite kGridSites[] = {
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
    uint16_t id = ci ? rd<uint16_t>(ci, 0x48) : 0, lo = rd<uint16_t>(0x1449f9290, 0x48), hi = rd<uint16_t>(0x1449f9290, 0x4a);
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
    bool d1 = !h1 || (h1 >> 24) == 7 || ((bq_done_t)0x14021ba60)(rd<uint64_t>(0x1442f7c68, 0), h1);
    bool d2 = !h2 || (h2 >> 24) == 7 || ((job_done_t)0x1401f75c0)(h2);
    if (d1 && d2) {
        // the lookup assumes the compartment exists: (&DAT_1442d18c8)[c] null → it locks through a null pointer (my 13:07 crash)
        uint16_t comp = rd<uint16_t>(server, 0x90);
        bool compReady = comp != 0xbbe && rd<uint64_t>(0x1442d18c8 + 8ull * comp, 0) != 0;
        uint64_t ld = compReady ? ((find_t)0x1401f9860)(comp, (void*)0x144a17ea0) : 0;
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
    auto go = (getopt_t)0x141191d50; auto so = (setoption_t)kSetOption;
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
typedef void(__fastcall* invset_t)(uint64_t store, uint64_t* vec);
static invset_t o_invset;
static void __fastcall h_invset(uint64_t store, uint64_t* vec) {
    uint64_t b = vec ? vec[0] : 0, e = vec ? vec[1] : 0;
    int n = (b && e > b) ? (int)((e - b) / 0x38) : 0;
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
        *(uint64_t*)(it + 0x00) = 0x143f7b968ull; *(uint64_t*)(it + 0x08) = 0x143f7b968ull; *(uint64_t*)(it + 0x10) = id;
        added++;
    }
    uint64_t v[4] = { (uint64_t)buf, (uint64_t)(buf + (size_t)(n + added) * 0x38), (uint64_t)(buf + cap * 0x38), 0 };
    status("inventory: set with %d items from the backend; all_unlocked adds %d (total %d)", n, added, n + added);
    o_invset(store, v);
    HeapFree(GetProcessHeap(), 0, buf);   // the store copied the items
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
static bool looks_vtable(uint64_t v) { return v >= 0x143000000ull && v < 0x146000000ull; }
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
        if (q < 0x10000 || looks_vtable(q) || (q >= 0x140000000ull && q < 0x160000000ull)) continue;
        uint64_t v = rd<uint64_t>(q, 0);
        if (looks_vtable(v)) { status("  %s+%03x -> obj %llx vtable %llx", tag, off, q, v); n++; }
    }
}
typedef uint64_t(__fastcall* connect_t)(uint64_t obj, const char* addr, uint64_t pw, uint64_t a4);
static connect_t o_connect;
static uint64_t __fastcall h_connect(uint64_t obj, const char* addr, uint64_t pw, uint64_t a4) {
    {   // does the client's connect password equal ServerSettings.ServerPassword? compare in memory; log only the result
        uint64_t ss = rd<uint64_t>(0x144331c50, 0);
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
            if (v >= 0x140001000ull && v < 0x14307e400ull) { n += sprintf_s(line + n, sizeof line - n, " %llx", v); found++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    status("%s", line);
    // 2026-10-20 entity-creation crash (FUN_1411590d0: data object in rsi has a null first qword): dump the first 8 qwords
    // of the data object (rsi) and its owner (rdi) as numbers, without following pointers, to identify the entity
    if (c->Rip == 0x140b95224ull || c->Rip == 0x140b95214ull || c->Rip == 0x140b95204ull) {
        // level-load assertion in FUN_140b950a0 state 2: rdi = level manager, rbp = load state; the bundle lookup
        // FUN_140cd57f0 used the name at [rdi+0xf0+0x38]
        status("  level manager %llx: name@+0x128 '%s' level@+0x110 %llx; load state %llx: +0x62c %x +0x694 %d",
               c->Rdi, safe_str(rd<uint64_t>(c->Rdi, 0x128)), rd<uint64_t>(c->Rdi, 0x110), c->Rbp,
               rd<uint32_t>(c->Rbp, 0x62c), (int)rd<uint8_t>(c->Rbp, 0x694));
    }
    if (c->Rip == 0x14115910bull) {
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
                        if (v >= 0x140001000ull && v < 0x14307e400ull) { n += sprintf_s(line + n, sizeof line - n, " %llx", v); found++; }
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
    uint8_t* at = (uint8_t*)0x1418f2930;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || memcmp(at, expect, 6) != 0) {
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
    {"havok-resource-load 0x142707130", 0x142707130, 16,
     {0x48,0x8b,0xc4,0x55,0x57,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,0x98,0x00,0x00,0x00},
     (void*)h_hkload3, (void**)&o_hkload3},
    {"havok-packfile-load2 0x142706770", 0x142706770, 18,
     {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40},
     (void*)h_hkload2, (void**)&o_hkload2},
    {"havok-packfile-load 0x142706d60", 0x142706d60, 19,
     {0x40,0x55,0x57,0x41,0x54,0x41,0x57,0x48,0x8d,0x6c,0x24,0xc1,0x48,0x81,0xec,0x88,0x00,0x00,0x00},
     (void*)h_hkload, (void**)&o_hkload},
    {"partition-new 0x140273060", 0x140273060, 15,
     {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20},
     (void*)h_partnew, (void**)&o_partnew},
    {"material-lookup 0x14057dd20", 0x14057dd20, 15,
     {0x40,0x53,0x48,0x83,0xec,0x20,0x41,0x8b,0x00,0x48,0x8b,0xd9,0x44,0x8b,0x02},
     (void*)h_matlookup, (void**)&o_matlookup},
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

enum Inst { I_OK, I_RETRY, I_FAIL };
static Inst install() {
    HMODULE exe = GetModuleHandleW(nullptr);
#ifndef PROBE_TEST
    if ((uint64_t)exe != 0x140000000ull) { status("ABORT: exe base %p, expected 0x140000000", exe); return I_FAIL; }
#else
    (void)exe;
#endif
    static bool reported;
    for (auto& s : g_sites) {
        MEMORY_BASIC_INFORMATION mbi{};
        bool mapped = VirtualQuery((void*)s.addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (!mapped || memcmp((void*)s.addr, s.expect, s.steal) != 0) {
            if (!reported) {
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
        status("hooked %s (stole %u bytes)", s.name, s.steal);
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
    for (int attempt = 0; attempt < 200 && !g_hooked; attempt++) {
        Inst r = install();
        if (r == I_OK) { g_hooked = 1; break; }
#ifndef PROBE_TEST
        if (r == I_FAIL) break;
#endif
        Sleep(50);
    }
    status(g_hooked ? "all hooks installed; logging" : "NOT HOOKED; probe idle (game unaffected)");
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
#ifdef SERVER
        // Loaded by sws_launch.exe on the suspended main thread, before the game's own code runs: set up and hook
        // synchronously so the MainLoop constructor hook is in place before the main loop is created.
        setup();
        if (install() == I_OK) { g_hooked = 1; status("hooked synchronously at load"); }
#ifdef SERVER
        install_socket_wrappers();
        if (g_sockLog) { install_socket_passthrough(); HANDLE t = CreateThread(nullptr, 0, sock_summary, nullptr, 0, nullptr); if (t) CloseHandle(t); }
#endif
        { HANDLE s = CreateThread(nullptr, 0, sampler, nullptr, 0, nullptr); if (s) CloseHandle(s); }
        if (g_observe) { HANDLE o = CreateThread(nullptr, 0, observe_thread, nullptr, 0, nullptr); if (o) CloseHandle(o); }
        for (int i = 0; i < g_hitboxScaleCount; i++) status("hitbox: %s scale %.3g", g_hitboxScales[i].ship, g_hitboxScales[i].scale);
        AddVectoredExceptionHandler(1, crash_catcher);
        status("crash catcher installed");
#endif
        HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
