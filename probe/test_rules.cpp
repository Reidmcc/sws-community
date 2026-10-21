// Rules test host: plants the game's real prologues at the real hook addresses with stand-in bodies that emulate just
// enough behaviour for the rules to act on, loads sws_rules_test.dll, and checks each rule's decisions.
//   bounds stand-in: *min = the throttle request (+0x04) it was given; *max = 2.0
//   integ stand-in:  state+0x3F4 = the throttle request it was given
//   drift stand-in:  state+0x3F0 = pressed as received; state+0x289 (drifting) = pressed (press starts, release ends)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef void(__fastcall* tick_t)(uint64_t, float);
typedef void(__fastcall* drift_t)(uint64_t, char);
typedef float*(__fastcall* drive_t)(uint64_t, float*, float*, float*, float*, float*, float);
typedef void(__fastcall* bounds_t)(uint64_t, uint8_t*, float*, float*, float);
typedef void(__fastcall* integ_t)(uint64_t, uint8_t*, float, float, float);

static uint8_t S[0x1000], CFG[0x400];
static float MAXS[1 + 9];
static int fails;

static void check(bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

static float input[12];
static float tick_bounds(float req) {  // one tick: bounds then integrator, returns the request the bounds code saw
    input[1] = req;
    float mn = -9, mx = -9;
    ((bounds_t)0x1416a7cf0)((uint64_t)S, (uint8_t*)input, &mn, &mx, 1.f / 30);
    ((integ_t)0x1416a87c0)((uint64_t)S, (uint8_t*)input, 0.f, 240.f, 1.f / 30);
    if (input[1] != req) { printf("FAIL  input not restored (%f vs %f)\n", input[1], req); fails++; }
    if (*(float*)(S + 0x3F4) != mn) { printf("FAIL  integrator saw %f, bounds saw %f\n", *(float*)(S + 0x3F4), mn); fails++; }
    return mn;
}
static uint8_t press(char p) { ((drift_t)0x1416aa3a0)((uint64_t)S, p); return S[0x3F0]; }

int main() {
    void* m = VirtualAlloc((void*)0x1416a0000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (m != (void*)0x1416a0000) { printf("could not map 0x1416a0000\n"); return 1; }
    const uint8_t tick[] = {0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0x41,0x28,0x0f,0x28,0xc1,0xf3,0x0f,0x58,0x81,0x80,0x02,0x00,0x00,
                            0x48,0x83,0xc4,0x40, 0x5b, 0xc3};
    const uint8_t drift[] = {0x40,0x55,0x41,0x56,0x48,0x8d,0x6c,0x24,0xb1,0x48,0x81,0xec,0x98,0x00,0x00,0x00,
                             0x88,0x91,0xf0,0x03,0x00,0x00,   // mov [rcx+0x3f0], dl
                             0x88,0x91,0x89,0x02,0x00,0x00,   // mov [rcx+0x289], dl
                             0x48,0x81,0xc4,0x98,0x00,0x00,0x00, 0x41,0x5e, 0x5d, 0xc3};
    const uint8_t drive[] = {0x40,0x53,0x48,0x83,0xec,0x70,0x48,0x8b,0x84,0x24,0xa8,0x00,0x00,0x00,
                             0x48,0x89,0x02, 0x48,0x89,0xd0, 0x48,0x83,0xc4,0x70, 0x5b, 0xc3};
    const uint8_t bounds[] = {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x40,0x80,0xb9,0xe0,0x03,0x00,0x00,0x00,
                              0x8b,0x42,0x04,                     // mov eax, [rdx+4]
                              0x41,0x89,0x00,                     // mov [r8], eax
                              0x41,0xc7,0x01,0x00,0x00,0x00,0x40, // mov dword [r9], 2.0
                              0x48,0x83,0xc4,0x40, 0x5f, 0x48,0x8b,0x5c,0x24,0x18, 0xc3};
    const uint8_t integ[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x60,0x0f,0x29,0x74,0x24,0x50,
                             0x8b,0x42,0x04,                      // mov eax, [rdx+4]
                             0x89,0x81,0xf4,0x03,0x00,0x00,       // mov [rcx+0x3f4], eax
                             0x0f,0x28,0x74,0x24,0x50, 0x48,0x83,0xc4,0x60, 0x5f, 0x48,0x8b,0x5c,0x24,0x08, 0xc3};
    memcpy((void*)0x1416ad2a0, tick, sizeof tick); memcpy((void*)0x1416aa3a0, drift, sizeof drift);
    memcpy((void*)0x1416a98c0, drive, sizeof drive); memcpy((void*)0x1416a7cf0, bounds, sizeof bounds);
    memcpy((void*)0x1416a87c0, integ, sizeof integ);
    FlushInstructionCache(GetCurrentProcess(), m, 0x10000);

    // an X-wing-like ship: MaxSpeeds 120..145, factor 0.8, full engine power -> cruise 116
    *(uint64_t*)(S + 0x28) = (uint64_t)CFG;
    *(uint32_t*)&MAXS[0] = 9; for (int i = 0; i < 9; i++) MAXS[1 + i] = 120.f + 3.125f * i;
    *(uint64_t*)(CFG + 0x168) = (uint64_t)&MAXS[1];
    *(float*)(S + 0x1F8) = 1.0f; *(float*)(S + 0x7C8) = 0.8f;

    HMODULE dll = LoadLibraryW(L"E:/sws_community/probe/build/sws_rules_test.dll");
    if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    for (int i = 0; i < 200 && *(uint8_t*)0x1416a87c0 != 0xFF; i++) Sleep(25);
    check(*(uint8_t*)0x1416a7cf0 == 0xFF && *(uint8_t*)0x1416a87c0 == 0xFF && *(uint8_t*)0x1416aa3a0 == 0xFF, "hooks installed");

    // --- pinball rule (stateless) ---
    S[0x278] = 0; *(float*)(S + 0x258) = 50.f;
    check(tick_bounds(-1.f) == -1.f, "not boosting: a slow-down request passes through");
    S[0x278] = 1;
    check(tick_bounds(-1.f) == 0.f, "boosting at 50 (below cruise 116) with slow-down: presented as hold");
    check(tick_bounds(1.f) == 1.f, "boosting: a speed-up request passes through");
    check(tick_bounds(0.f) == 0.f, "boosting: a hold request passes through");
    *(float*)(S + 0x258) = 150.f;
    check(tick_bounds(-1.f) == -1.f, "boosting at 150 (above cruise): intended instant boost left alone");
    *(float*)(S + 0x258) = 116.4f;
    check(tick_bounds(-1.f) == 0.f, "boosting at 116.4 (cruise + float noise, within the 1.0 margin): hold");
    *(float*)(S + 0x1F8) = 0.5f; *(float*)(S + 0x258) = 110.f;
    check(tick_bounds(-1.f) == -1.f, "half engine power (cruise 106): at 110 is above cruise, instant allowed");
    *(float*)(S + 0x1F8) = 1.0f;
    // replay consistency: the same state and input give the same answer whatever ran before
    S[0x278] = 1; *(float*)(S + 0x258) = 50.f; float first = tick_bounds(-1.f);
    S[0x278] = 0; tick_bounds(-1.f); S[0x278] = 1; *(float*)(S + 0x258) = 180.f; tick_bounds(-1.f);
    *(float*)(S + 0x258) = 50.f; S[0x278] = 0; tick_bounds(1.f); S[0x278] = 1;
    check(tick_bounds(-1.f) == first && first == 0.f, "replay: same tick re-run after other history gives the same decision");
    S[0x278] = 0;

    // --- multidrift rule (stateless) ---
    S[0x289] = 0; S[0x278] = 1;
    check(press(1) == 1 && S[0x289] == 1, "press while boosting: drift starts");
    press(0); S[0x278] = 0;  // the game turns the afterburner off when a drift starts; release ends the drift
    check(press(1) == 0 && S[0x289] == 0, "re-press while not boosting: blocked (multidrift)");
    check(press(1) == 0, "held re-press stays blocked");
    press(0);
    S[0x278] = 1;  // a new boost
    check(press(1) == 1 && S[0x289] == 1, "after a new boost: drift allowed again");
    S[0x278] = 0;
    check(press(1) == 1, "a press while already drifting is passed through unchanged");
    press(0); S[0x289] = 0;

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    Sleep(300);
    return fails ? 1 : 0;
}
