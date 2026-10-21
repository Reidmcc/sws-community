// Test host: plants the game's real prologue bytes at the real hook addresses with stand-in bodies,
// loads a PROBE_TEST build of the probe, calls the functions from several threads, and checks results.
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

static uint8_t g_state[0x1000];
static uint8_t g_cfg[0x400];
static float g_maxspeeds[1 + 9];
static volatile LONG g_calls;

static void put(uint64_t at, const uint8_t* b, size_t n) { memcpy((void*)at, b, n); }

static DWORD WINAPI caller(LPVOID) {
    float out[4], p3[4] = {1,2,3,4}, quat[4] = {0,0,0,1}, tgt[4] = {0,0,120,0}, vel[4] = {0,0,100,0};
    for (int i = 0; i < 20000; i++) {
        ((tick_t)0x1416ad2a0)((uint64_t)g_state, 1.0f / 60);
        ((drift_t)0x1416aa3a0)((uint64_t)g_state, (char)(i & 1));
        float* r = ((drive_t)0x1416a98c0)((uint64_t)g_state, out, p3, quat, tgt, vel, 0.25f);
        // stand-in drive body stores its 6th argument (vel pointer) into out[0..1] and returns rdx (out)
        if (r != out || *(uint64_t*)out != (uint64_t)vel) { printf("ARG/RETURN MISMATCH at %d\n", i); ExitProcess(9); }
        float input[12] = {0, -1.f, 0, 0, 0.25f, 0, 0, 0, 0, 0, 0, 0};
        float mn = -9, mx = -9;
        ((bounds_t)0x1416a7cf0)((uint64_t)g_state, (uint8_t*)input, &mn, &mx, 0.5f);
        if (mn != 1.f || mx != 2.f) { printf("BOUNDS MISMATCH at %d: %f %f\n", i, mn, mx); ExitProcess(9); }
        ((integ_t)0x1416a87c0)((uint64_t)g_state, (uint8_t*)input, 5.f, 123.5f, 0.5f);
        if (*(float*)(g_state + 0x258) != 123.5f) { printf("INTEG MISMATCH at %d\n", i); ExitProcess(9); }
        InterlockedIncrement(&g_calls);
    }
    return 0;
}

int main() {
    void* m = VirtualAlloc((void*)0x1416a0000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (m != (void*)0x1416a0000) { printf("could not map 0x1416a0000\n"); return 1; }
    // tick: real prologue (21 bytes) + add rsp,0x40; pop rbx; ret
    const uint8_t tick[] = {0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0x41,0x28,0x0f,0x28,0xc1,0xf3,0x0f,0x58,0x81,0x80,0x02,0x00,0x00,
                            0x48,0x83,0xc4,0x40, 0x5b, 0xc3};
    // drift: real prologue (16 bytes) + add rsp,0x98; pop r14; pop rbp; ret
    const uint8_t drift[] = {0x40,0x55,0x41,0x56,0x48,0x8d,0x6c,0x24,0xb1,0x48,0x81,0xec,0x98,0x00,0x00,0x00,
                             0x48,0x81,0xc4,0x98,0x00,0x00,0x00, 0x41,0x5e, 0x5d, 0xc3};
    // drive: real prologue (14 bytes: push rbx; sub rsp,0x70; mov rax,[rsp+0xa8] = 6th arg)
    //        + mov [rdx],rax; mov rax,rdx; add rsp,0x70; pop rbx; ret
    const uint8_t drive[] = {0x40,0x53,0x48,0x83,0xec,0x70,0x48,0x8b,0x84,0x24,0xa8,0x00,0x00,0x00,
                             0x48,0x89,0x02, 0x48,0x89,0xd0, 0x48,0x83,0xc4,0x70, 0x5b, 0xc3};
    // bounds: real prologue (17 bytes) + mov dword [r8],1.0f; mov dword [r9],2.0f; add rsp,0x40; pop rdi; mov rbx,[rsp+0x18]; ret
    const uint8_t bounds[] = {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x40,0x80,0xb9,0xe0,0x03,0x00,0x00,0x00,
                              0x41,0xc7,0x00,0x00,0x00,0x80,0x3f, 0x41,0xc7,0x01,0x00,0x00,0x00,0x40,
                              0x48,0x83,0xc4,0x40, 0x5f, 0x48,0x8b,0x5c,0x24,0x18, 0xc3};
    // integ: real prologue (15 bytes) + movss [rcx+0x258],xmm3 (cmd = max); movaps xmm6,[rsp+0x50]; add rsp,0x60; pop rdi; mov rbx,[rsp+8]; ret
    const uint8_t integ[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x60,0x0f,0x29,0x74,0x24,0x50,
                             0xf3,0x0f,0x11,0x99,0x58,0x02,0x00,0x00, 0x0f,0x28,0x74,0x24,0x50,
                             0x48,0x83,0xc4,0x60, 0x5f, 0x48,0x8b,0x5c,0x24,0x08, 0xc3};
    put(0x1416ad2a0, tick, sizeof tick); put(0x1416aa3a0, drift, sizeof drift); put(0x1416a98c0, drive, sizeof drive);
    put(0x1416a7cf0, bounds, sizeof bounds); put(0x1416a87c0, integ, sizeof integ);
    FlushInstructionCache(GetCurrentProcess(), m, 0x10000);

    // fake state + config, with recognisable values
    *(uint64_t*)(g_state + 0x28) = (uint64_t)g_cfg;
    *(float*)(g_state + 0x258) = 123.5f; *(uint8_t*)(g_state + 0x289) = 1; *(float*)(g_state + 0x284) = 0.75f;
    *(float*)(g_cfg + 0x2EC) = 10000.f; *(float*)(g_cfg + 0x2F0) = 3.f;
    *(uint32_t*)&g_maxspeeds[0] = 9; for (int i = 0; i < 9; i++) g_maxspeeds[1 + i] = 120.f + 3.125f * i;
    *(uint64_t*)(g_cfg + 0x168) = (uint64_t)&g_maxspeeds[1];
    *(float*)(g_state + 0x1F8) = 1.0f; *(float*)(g_state + 0x7C8) = 0.8f;  // cruise replica should give 145*0.8 = 116
    *(float*)(g_state + 0x6D0) = 0.0f; *(float*)(g_state + 0x780) = 240.0f;

    // call once unhooked as a sanity check of the stand-ins
    caller(nullptr); g_calls = 0;

    HMODULE probe = LoadLibraryW(L"E:/sws_community/probe/build/sws_probe_test.dll");
    if (!probe) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    // wait for the hooks (first byte becomes FF)
    for (int i = 0; i < 200 && *(uint8_t*)0x1416ad2a0 != 0xFF; i++) Sleep(25);
    printf("hooked: tick=%02x drift=%02x drive=%02x\n", *(uint8_t*)0x1416ad2a0, *(uint8_t*)0x1416aa3a0, *(uint8_t*)0x1416a98c0);
    HANDLE th[4]; for (int i = 0; i < 4; i++) th[i] = CreateThread(nullptr, 0, caller, nullptr, 0, nullptr);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    printf("calls through hooks: %ld\n", g_calls);
    Sleep(600);  // let the writer flush
    printf("TEST HOST DONE\n");
    return 0;
}
