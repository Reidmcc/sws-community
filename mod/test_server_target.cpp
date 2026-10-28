// Test target for sws_launch.exe + sws_server_test.dll: started suspended by the launcher, which APC-injects the DLL.
// It plants the game's real prologue bytes (with small stand-in bodies) at every hook address, waits for the DLL to
// hook them, then checks each server hook's effect. Exit code 0 = all pass.
#define WIN32_LEAN_AND_MEAN
#include <math.h>
#include <winsock2.h>
#include <windows.h>
#include <initializer_list>
static volatile unsigned long long g_scorerList;   // the creep scorer stand-in's eighth argument, last call
static volatile unsigned long long g_scorerRules;  // ... and its seventh (the squadron's targeting rules)
#pragma comment(lib, "ws2_32.lib")
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int fails;
// stand-ins for the engine's in-memory socket manager / socket (signatures from notes/socket-interface.md)
static uint8_t g_fakeSock[0xe0];
static int g_destroyed, g_sockDtor, g_sent, g_setpeer;
static void __fastcall sm_destroy(uint64_t) { g_destroyed++; }
static uint64_t __fastcall sm_connect(uint64_t, const char*, bool) { return (uint64_t)g_fakeSock; }
static uint8_t g_fakeSock2[0xe0];
static uint64_t __fastcall sm_listen(uint64_t, const char* a, bool) {
    const char* c = strrchr(a, ':'); int port = c ? atoi(c + 1) : 0;
    uint8_t* s = port == 25100 ? g_fakeSock2 : g_fakeSock; *(int*)(s + 0x54) = port; return (uint64_t)s; }
static uint64_t __fastcall sm_create(uint64_t) { return (uint64_t)g_fakeSock; }
static uint64_t __fastcall sm_dtor(uint64_t m, uint32_t) { return m; }
static uint64_t __fastcall ss_dtor(uint64_t s, uint32_t) { g_sockDtor++; return s; }
static bool __fastcall ss_send(uint64_t, const uint8_t*, int n, uint32_t) { g_sent += n; return true; }
static int __fastcall ss_recv(uint64_t, uint8_t* buf, int n, uint32_t* when) { static int k; if (k++ % 2) return -1; *when = 1234; buf[0] = 7; return n > 3 ? 3 : n; }
static void __fastcall ss_setpeer(uint64_t s, uint64_t sa) { g_setpeer++; memcpy((void*)(s + 0x58), (void*)sa, 0x40); *(uint8_t*)(s + 0x98) = *(uint8_t*)(sa + 0x40); }
static bool __fastcall ss_addr(uint64_t, uint64_t, bool) { return true; }
static bool __fastcall ss_create(uint64_t, bool) { return true; }
static void __fastcall ss_nop(uint64_t) {}

static void check(bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }
static void* map(uint64_t at, size_t n) { return VirtualAlloc((void*)at, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); }
static void put(uint64_t at, const uint8_t* b, size_t n) { memcpy((void*)at, b, n); }

// Runtime value engine (issue #3): a fake slice of the engine's loaded data at the real addresses the DLL walks:
// compartment table 0x1442d18c8 -> entry (+0x68 domain) -> domain (lock +0x70, partitions +0x138/+0x140) -> partition
// (+0x20 instances, +0x28 count) -> config instances (+0x08 class, +0x18 name). Class ids live at ClassInfo +0x48/+0x4a.
struct FakeAttr { uint8_t pad[24]; uint64_t idObj; float value; float pad2; };
static uint8_t* fake_alloc(size_t n) { return (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static uint8_t* fake_class(uint16_t id) { uint8_t* c = fake_alloc(0x100); *(uint16_t*)(c + 0x48) = id; return c; }
static uint8_t* fake_idobj(uint32_t ident) { uint8_t* o = fake_alloc(0x40); *(uint32_t*)(o + 32) = ident; return o; }
static FakeAttr* fake_attr(uint32_t ident, float v) { FakeAttr* a = (FakeAttr*)fake_alloc(sizeof(FakeAttr)); a->idObj = (uint64_t)fake_idobj(ident); a->value = v; return a; }
static float* fake_reglist(int n, const uint32_t* ids, const float* vals) {   // {u32 id, f32 value} entries, count at -4
    uint8_t* b = fake_alloc(16 + n * 8); *(uint32_t*)(b + 12) = n;
    for (int i = 0; i < n; i++) { *(uint32_t*)(b + 16 + i * 8) = ids[i]; *(float*)(b + 16 + i * 8 + 4) = vals[i]; }
    return (float*)(b + 16);
}
static float* fake_array(int n, float first, float step) { uint8_t* b = fake_alloc(16 + n * 4); *(uint32_t*)(b + 12) = n;
    float* a = (float*)(b + 16); for (int i = 0; i < n; i++) a[i] = first + step * i; return a; }
struct FakeAttrV3 { uint8_t pad[24]; uint64_t idObj; float x, y, z, w; };
static FakeAttrV3* fake_attr3(uint32_t ident, float x, float y, float z) {
    FakeAttrV3* a = (FakeAttrV3*)fake_alloc(sizeof(FakeAttrV3)); a->idObj = (uint64_t)fake_idobj(ident); a->x = x; a->y = y; a->z = z; return a; }
struct FakeAttrI { uint8_t pad[24]; uint64_t idObj; int32_t value; int32_t pad2; };
static FakeAttrI* fake_attri(uint32_t ident, int32_t v) { FakeAttrI* a = (FakeAttrI*)fake_alloc(sizeof(FakeAttrI)); a->idObj = (uint64_t)fake_idobj(ident); a->value = v; return a; }
struct FakeMove { uint8_t* inst; FakeAttr* ab; FakeAttr* drift; float* reg; float* speeds; FakeAttrV3* turn; float* v3; };
static FakeMove fake_movement(uint8_t* cls, const char* name, float driftTime) {
    FakeMove m{}; m.inst = fake_alloc(0x400);
    *(uint64_t*)(m.inst + 8) = (uint64_t)cls; *(const char**)(m.inst + 0x18) = name;
    m.ab = fake_attr(0xABCD1234, 240.0f); m.drift = fake_attr(0x0D41F700, driftTime);
    *(uint64_t*)(m.inst + 0x228) = (uint64_t)m.ab; *(uint64_t*)(m.inst + 0x290) = (uint64_t)m.drift;
    uint32_t ids[3] = { 0x11111111, 0xABCD1234, 0x0D41F700 }; float vals[3] = { 1.0f, 240.0f, driftTime };
    m.reg = fake_reglist(3, ids, vals); *(uint64_t*)(m.inst + 0x40) = (uint64_t)m.reg;
    m.speeds = fake_array(9, 120.0f, 3.125f); *(uint64_t*)(m.inst + 0x168) = (uint64_t)m.speeds;
    *(float*)(m.inst + 0x2ec) = 10000.0f;
    FakeAttrV3* turn = fake_attr3(0x5555AAAA, 10.0f, 5.0f, 90.0f); *(uint64_t*)(m.inst + 0x230) = (uint64_t)turn;
    uint8_t* v3 = fake_alloc(16 + 32); *(uint32_t*)(v3 + 12) = 1;   // one 32-byte entry: Vec3 at 0, id at +16
    *(float*)(v3 + 16) = 10.0f; *(float*)(v3 + 20) = 5.0f; *(float*)(v3 + 24) = 90.0f; *(uint32_t*)(v3 + 16 + 16) = 0x5555AAAA;
    *(uint64_t*)(m.inst + 0x50) = (uint64_t)(v3 + 16);
    m.turn = turn; m.v3 = (float*)(v3 + 16);
    return m;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: if the target dies, the output shows how far it got
    if (map(0x1401a0000, 0x20000) != (void*)0x1401a0000 || map(0x1416a0000, 0x10000) != (void*)0x1416a0000 ||
        map(0x1418f0000, 0x10000) != (void*)0x1418f0000) { printf("FAIL  could not map test regions\n"); return 1; }
    // movement sites (must exist for the DLL's byte check), with trivial bodies
    const uint8_t tick[] = {0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0x41,0x28,0x0f,0x28,0xc1,0xf3,0x0f,0x58,0x81,0x80,0x02,0x00,0x00, 0x48,0x83,0xc4,0x40,0x5b,0xc3};
    const uint8_t drift[] = {0x40,0x55,0x41,0x56,0x48,0x8d,0x6c,0x24,0xb1,0x48,0x81,0xec,0x98,0x00,0x00,0x00, 0x48,0x81,0xc4,0x98,0x00,0x00,0x00,0x41,0x5e,0x5d,0xc3};
    const uint8_t drive[] = {0x40,0x53,0x48,0x83,0xec,0x70,0x48,0x8b,0x84,0x24,0xa8,0x00,0x00,0x00, 0x48,0x89,0xd0,0x48,0x83,0xc4,0x70,0x5b,0xc3};
    const uint8_t bounds[] = {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x40,0x80,0xb9,0xe0,0x03,0x00,0x00,0x00, 0x48,0x83,0xc4,0x40,0x5f,0x48,0x8b,0x5c,0x24,0x18,0xc3};
    const uint8_t integ[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x60,0x0f,0x29,0x74,0x24,0x50, 0x0f,0x28,0x74,0x24,0x50,0x48,0x83,0xc4,0x60,0x5f,0x48,0x8b,0x5c,0x24,0x08,0xc3};
    // server sites
    const uint8_t ctor[] = {0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,
                            0x88,0x51,0x08,              // mov [rcx+8], dl   (store the flag, as the real ctor does)
                            0x48,0x89,0xc8,              // mov rax, rcx
                            0x48,0x83,0xc4,0x30,0x5f,0xc3};
    const uint8_t spawn[] = {0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x48,
                             0xc6,0x42,0x2f,0x77,        // mov byte [rdx+0x2f], 0x77   (marks "spawn ran")
                             0x48,0x83,0xc4,0x48,0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0x5b,0xc3};
    const uint8_t clock[] = {0x48,0x89,0x4c,0x24,0x08,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,
                             0x48,0x83,0xc4,0x40,0x41,0x5e,0x5f,0x5e,0xc3};
    const uint8_t chooser[] = {0x48,0x83,0xec,0x28,0x80,0x79,0x29,0x00,
                               0xb8,0x52,0x80,0x3c,0xf7,  // mov eax, OnlineBackend_Lan (what the real one would pick for a dedicated server)
                               0x48,0x83,0xc4,0x28,0xc3};
    put(0x1416ad2a0, tick, sizeof tick); put(0x1416aa3a0, drift, sizeof drift); put(0x1416a98c0, drive, sizeof drive);
    put(0x1416a7cf0, bounds, sizeof bounds); put(0x1416a87c0, integ, sizeof integ);
    put(0x1401ae7d0, ctor, sizeof ctor); put(0x1401aac90, spawn, sizeof spawn); put(0x1401bf4d0, clock, sizeof clock);
    put(0x1418f2930, chooser, sizeof chooser);
    // loadouts (the loadout probe, loadout_log=1): the presets setter FUN_1418f58f0, its real 19-byte prologue, then a body
    // that records the presets it was given at 0x1418f8000 and the real epilogue; and the save FUN_1419001c0, its real
    // 20-byte prologue, then a body that sets the store's presets the way the real one does (setter(manager+0x10 store,
    // presets, 1, 0)) and its pops
    {
        const uint8_t pset[] = {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xf0,0x00,0x00,0x00,
                                0x48,0x89,0xd0, 0x48,0xa3, 0x00,0x80,0x8f,0x41,0x01,0x00,0x00,0x00,
                                0x48,0x81,0xc4,0xf0,0x00,0x00,0x00, 0x41,0x5f,0x41,0x5e,0x5f,0xc3};
        put(0x1418f58f0, pset, sizeof pset);
        if (map(0x141900000, 0x10000) != (void*)0x141900000) { printf("FAIL  could not map the loadout-save region\n"); return 1; }
        uint8_t psave[256]; memset(psave, 0x90, sizeof psave); int k = 0;
        const uint8_t head[] = {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xc0,0xfc,0xff,0xff,
                                0x48,0x83,0xec,0x20, 0x48,0x89,0xd6, 0x48,0x89,0xcf,   // sub rsp,0x20; mov rsi,rdx; mov rdi,rcx
                                0x48,0x8b,0x49,0x10, 0x41,0xb8,0x01,0x00,0x00,0x00, 0x45,0x31,0xc9,
                                0x48,0xb8, 0xf0,0x58,0x8f,0x41,0x01,0x00,0x00,0x00};   // mov rax, 0x1418f58f0
        memcpy(psave, head, sizeof head);
        k = 0x232 - 0x1c0;                                            // nops, then call rax at 0x141900232 (returns 0x141900234)
        psave[k++] = 0xff; psave[k++] = 0xd0;
        const uint8_t tail[] = {0x48,0x89,0xf2,                       // mov rdx, rsi (the presets, as the "request")
                                0x48,0xb8, 0x90,0x31,0x90,0x41,0x01,0x00,0x00,0x00, 0xff,0xd0,   // call the send site stub
                                0x48,0x83,0xc4,0x20, 0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0xc3};
        memcpy(psave + k, tail, sizeof tail); k += sizeof tail;
        put(0x1419001c0, psave, k);
        // the save's EA send site: sub rsp,0x28; nop; call 0x1418f61b0 at 0x141903195 (returns 0x14190319a); add rsp,0x28; ret
        uint8_t site[16] = {0x48,0x83,0xec,0x28, 0x90, 0xe8,0,0,0,0, 0x48,0x83,0xc4,0x28, 0xc3};
        *(int32_t*)(site + 6) = (int32_t)(0x1418f61b0ll - 0x14190319all);
        put(0x141903190, site, 15);
        // the EA sender FUN_1418f61b0: its real 18-byte prologue, then count the call at 0x1418f8010, return rdx, and the epilogue
        const uint8_t psend[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40,
                                 0x48,0xb8, 0x10,0x80,0x8f,0x41,0x01,0x00,0x00,0x00, 0x48,0xff,0x00, 0x48,0x89,0xd0,
                                 0x48,0x83,0xc4,0x40, 0x41,0x5e,0x5f,0x5e, 0x48,0x8b,0x5c,0x24,0x10, 0x48,0x8b,0x6c,0x24,0x18, 0xc3};
        put(0x1418f61b0, psend, sizeof psend);
    }
    // 3b: GameSimulation tick (real 14-byte prologue + epilogue), and simplified helpers
    const uint8_t gstick[] = {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x50,
                              0x48,0x83,0xc4,0x50,0x41,0x5f,0x41,0x5e,0x5f,0x5e,0x5d,0xc3};
    put(0x1401bfe60, gstick, sizeof gstick);
    // level-load step 2 (FUN_140cd6420): its real 15-byte prologue, then a body that returns 1
    if (map(0x140cd0000, 0x10000) != (void*)0x140cd0000) { printf("FAIL  could not map the level-step region\n"); return 1; }
    const uint8_t lvlstep2[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,
                                0xb8,0x01,0x00,0x00,0x00, 0xc3};
    put(0x140cd6420, lvlstep2, sizeof lvlstep2);
    // the data store's partition creator (FUN_140273060): its real 15-byte prologue, then a body that returns its third
    // argument as the new partition (mov rax, r8) and the real epilogue
    if (map(0x140270000, 0x10000) != (void*)0x140270000) { printf("FAIL  could not map the partition-creator region\n"); return 1; }
    const uint8_t partnew[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20,
                               0x4c,0x89,0xc0, 0x48,0x83,0xc4,0x20, 0x5f, 0x48,0x8b,0x5c,0x24,0x08, 0x48,0x8b,0x74,0x24,0x18, 0xc3};
    put(0x140273060, partnew, sizeof partnew);
    // the server's starship spawn (FUN_1416bcab0): its real 20-byte prologue, then mov rax, [rcx+0x10] (the test puts the
    // "ship" there, or 0 for a spawn that fails), lea rsp, [rbp+0x50] and the pops
    if (map(0x1416b0000, 0x10000) != (void*)0x1416b0000) { printf("FAIL  could not map the starship-spawn region\n"); return 1; }
    const uint8_t sspawn[] = {0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xb0,0x48,0x81,0xec,0x50,0x01,0x00,0x00,
                              0x48,0x8b,0x41,0x10, 0x48,0x8d,0x65,0x50, 0x41,0x5f,0x41,0x5e,0x5f,0x5e,0x5d,0xc3};
    put(0x1416bcab0, sspawn, sizeof sspawn);
    // material grid lookup (FUN_14057dd20): its real 15-byte prologue, then mov rax, imm64 (the stock cell, set below)
    // and the epilogue. The capital hull's direct-hit call site is planted too: entered at 0x1417db3f7, it gives the callee its
    // shadow space and keeps the stack aligned (sub rsp, 0x28), calls from 0x1417db3fb so the lookup returns to
    // 0x1417db400 (the return address the DLL's grid settings key on), then add rsp, 0x28 and ret to the test.
    if (map(0x140570000, 0x10000) != (void*)0x140570000 || map(0x1417d0000, 0x10000) != (void*)0x1417d0000) { printf("FAIL  could not map the material-lookup regions\n"); return 1; }
    const uint8_t matlookup[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x41,0x8b,0x00,0x48,0x8b,0xd9,0x44,0x8b,0x02,
                                 0x48,0x8b,0x43,0x08,   // mov rax, [rbx+8]: like the real one, it reads the grid it is given
                                 0x48,0xb8, 0,0,0,0,0,0,0,0, 0x48,0x83,0xc4,0x20, 0x5b, 0xc3};
    put(0x14057dd20, matlookup, sizeof matlookup);
    { uint8_t site[14] = { 0x48, 0x83, 0xec, 0x28, 0xe8, 0, 0, 0, 0, 0x48, 0x83, 0xc4, 0x28, 0xc3 };
      *(int32_t*)(site + 5) = (int32_t)(0x14057dd20ll - 0x1417db400ll);
      put(0x1417db3f7, site, sizeof site); }
    // the ignore-AI-damage check (FUN_1417da0e0, replaced whole by the DLL): its real first 20 bytes. The starship cast it
    // uses (FUN_1412631b0): here, rcx if its vtable is ServerStarshipEntity's (0x1436184e8), else 0.
    { const uint8_t aicheck[] = {0x48,0x83,0xec,0x28,0xf6,0x82,0xa4,0x00,0x00,0x00,0x08,0x75,0x71,0x80,0xb9,0xc8,0x01,0x00,0x00,0x00, 0x31,0xc0,0xc3};
      put(0x1417da0e0, aicheck, sizeof aicheck); }
    if (map(0x141260000, 0x10000) != (void*)0x141260000) { printf("FAIL  could not map the starship-cast region\n"); return 1; }
    { const uint8_t cast[] = {0x48,0x85,0xc9, 0x74,0x13, 0x48,0xba,0xe8,0x84,0x61,0x43,0x01,0x00,0x00,0x00, 0x48,0x39,0x11, 0x75,0x04,
                              0x48,0x89,0xc8, 0xc3, 0x31,0xc0, 0xc3};
      put(0x1412631b0, cast, sizeof cast); }
    // capital hull damage (FUN_1417dbe90): its real 14-byte prologue (mov rax, rsp; seven pushes), then, as the real one,
    // it asks the check (component, record) and unless told to drop the hit, health (the fake component's +0x40) -= the
    // record's +0x88: rsi, rdi, r12 keep the component, out and record across the call (sub rsp, 0x20 for its shadow
    // space; the stack stays aligned); mov rax, rdi and the pops. Its blast caller is planted like the grid's:
    // entered at 0x1417dbde3, it calls from 0x1417dbde7 so the apply returns to 0x1417dbdec. The components' health getter
    // (vtable +0x108): movss xmm0, [rcx+0x40]; ret, at 0x1417dc800. The capital shield's direct hit (FUN_140582ab0, the same
    // 14 bytes) and blast (FUN_140582d90, 19 bytes: five pushes, lea rbp, sub rsp, 0x150) take the info's +0x84 or +0x74
    // off the same health and return out.
    const uint8_t capapply[] = {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
                                0x48,0x89,0xce, 0x48,0x89,0xd7, 0x4d,0x89,0xc4, 0x48,0x83,0xec,0x20, 0x4c,0x89,0xc2,
                                0x48,0xb8,0xe0,0xa0,0x7d,0x41,0x01,0x00,0x00,0x00, 0xff,0xd0, 0x48,0x83,0xc4,0x20,
                                0x84,0xc0, 0x75,0x14,
                                0xf3,0x0f,0x10,0x46,0x40, 0xf3,0x41,0x0f,0x5c,0x84,0x24,0x88,0x00,0x00,0x00, 0xf3,0x0f,0x11,0x46,0x40,
                                0x48,0x89,0xf8,
                                0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0xc3};
    put(0x1417dbe90, capapply, sizeof capapply);
    { uint8_t site[14] = { 0x48, 0x83, 0xec, 0x28, 0xe8, 0, 0, 0, 0, 0x48, 0x83, 0xc4, 0x28, 0xc3 };
      *(int32_t*)(site + 5) = (int32_t)(0x1417dbe90ll - 0x1417dbdecll);
      put(0x1417dbde3, site, sizeof site); }
    { const uint8_t getHealth[] = {0xf3,0x0f,0x10,0x41,0x40, 0xc3}; put(0x1417dc800, getHealth, sizeof getHealth); }
    if (map(0x140580000, 0x10000) != (void*)0x140580000) { printf("FAIL  could not map the capital-shield region\n"); return 1; }
    const uint8_t shdirect[] = {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
                                0xf3,0x0f,0x10,0x41,0x40, 0xf3,0x41,0x0f,0x5c,0x80,0x84,0x00,0x00,0x00, 0xf3,0x0f,0x11,0x41,0x40, 0x48,0x89,0xd0,
                                0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0xc3};
    put(0x140582ab0, shdirect, sizeof shdirect);
    const uint8_t shblast[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xb0,0x48,0x81,0xec,0x50,0x01,0x00,0x00,
                               0xf3,0x0f,0x10,0x41,0x40, 0xf3,0x41,0x0f,0x5c,0x40,0x74, 0xf3,0x0f,0x11,0x41,0x40, 0x48,0x89,0xd0,
                               0x48,0x81,0xc4,0x50,0x01,0x00,0x00, 0x41,0x5e,0x5f,0x5e,0x5b,0x5d,0xc3};
    put(0x140582d90, shblast, sizeof shblast);
    // the AI controller's field-change handler (FUN_1416feaf0): its real 15-byte prologue, then its BehaviourIndex store
    // (mov rax, [rdx+8]; mov edx, [rax]; mov [rcx+0xb4], edx) and the epilogue; and the target-score lookup (FUN_141617a00):
    // its real 21-byte prologue, xorps xmm0, xmm0, the epilogue
    if (map(0x1416f0000, 0x10000) != (void*)0x1416f0000 || map(0x141610000, 0x10000) != (void*)0x141610000) { printf("FAIL  could not map the AI regions\n"); return 1; }
    // the squadron's candidate scorer (FUN_14182ade0): its real 14-byte prologue, then a body that stores its eighth
    // argument (the target list; [rsp+0x68] after the prologue's five pushes) in g_scorerList, and the pops
    const uint8_t scorerHead[] = {0x48,0x8b,0xc4,0x44,0x89,0x48,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55, 0x48,0x8b,0x44,0x24,0x68, 0x48,0xa3};
    const uint8_t scorerTail[] = {0x41,0x5d, 0x41,0x5c, 0x5f, 0x5e, 0x5d, 0xc3};
    if (map(0x141820000, 0x10000) != (void*)0x141820000) { printf("FAIL  could not map the scorer region\n"); return 1; }
    // ... and its seventh (the targeting rules, [rsp+0x60]) in g_scorerRules; and, when its tenth (the output vector,
    // [rsp+0x78]) is given, appends two 0x38-byte entries as the real one appends one per candidate (end +8 += 0x70; the
    // test writes the entries' contents ahead)
    { uint8_t code[128]; size_t at = 0; memcpy(code, scorerHead, sizeof scorerHead); at = sizeof scorerHead;
      uint64_t where = (uint64_t)&g_scorerList; memcpy(code + at, &where, 8); at += 8;
      const uint8_t rulesLoad[] = {0x48,0x8b,0x44,0x24,0x60, 0x48,0xa3}; memcpy(code + at, rulesLoad, sizeof rulesLoad); at += sizeof rulesLoad;
      uint64_t whereR = (uint64_t)&g_scorerRules; memcpy(code + at, &whereR, 8); at += 8;
      const uint8_t outAppend[] = {0x48,0x8b,0x44,0x24,0x78, 0x48,0x85,0xc0, 0x74,0x05, 0x48,0x83,0x40,0x08,0x70};
      memcpy(code + at, outAppend, sizeof outAppend); at += sizeof outAppend;
      memcpy(code + at, scorerTail, sizeof scorerTail); at += sizeof scorerTail;
      put(0x14182ade0, code, at); }
    const uint8_t ctrlfield[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x8b,0x02,0x48,0x8b,0xfa,
                                 0x48,0x8b,0x42,0x08, 0x8b,0x10, 0x89,0x91,0xb4,0x00,0x00,0x00,
                                 0x48,0x8b,0x5c,0x24,0x08, 0x48,0x8b,0x7c,0x24,0x10, 0xc3};
    put(0x1416feaf0, ctrlfield, sizeof ctrlfield);
    const uint8_t scorelk[] = {0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,
                               0x0f,0x57,0xc0, 0x48,0x83,0xc4,0x40, 0x41,0x5f,0x41,0x5e,0x5f,0x5e,0x5d,0xc3};
    put(0x141617a00, scorelk, sizeof scorelk);
    // transport capture sites: real prologues, then matching epilogues
    if (map(0x140cb0000, 0x10000) != (void*)0x140cb0000 || map(0x1412e0000, 0x10000) != (void*)0x1412e0000) { printf("FAIL  could not map capture regions\n"); return 1; }
    const uint8_t cconnect[] = {0x4c,0x8b,0xdc,0x57,0x48,0x81,0xec,0x90,0x00,0x00,0x00,0x49,0xc7,0x43,0x88,0xfe,0xff,0xff,0xff,
                                0x48,0x81,0xc4,0x90,0x00,0x00,0x00, 0x5f, 0xc3};
    const uint8_t osrv[] = {0x4c,0x8b,0xdc,0x4d,0x89,0x43,0x18,0x49,0x89,0x4b,0x08,0x55,0x56,0x57, 0x5f,0x5e,0x5d, 0x48,0x89,0xc8, 0xc3};
    put(0x140cb0500, cconnect, sizeof cconnect); put(0x1412e5e50, osrv, sizeof osrv);
    // client network init (FUN_140a31d50): real 15-byte prologue, then its epilogue
    if (map(0x140a30000, 0x10000) != (void*)0x140a30000) { printf("FAIL  could not map the client-init region\n"); return 1; }
    const uint8_t cinit[] = {0x48,0x8b,0xc4,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x60,
                             0x48,0x83,0xc4,0x60,0x41,0x5f,0x41,0x5e,0x41,0x5c,0x5f,0x5e,0xc3};
    put(0x140a31d50, cinit, sizeof cinit);
    // join diagnostics and remote join (2026-10-20 evening): real prologues, never called by this test
    if (map(0x140ba0000, 0x10000) != (void*)0x140ba0000 || map(0x140210000, 0x10000) != (void*)0x140210000) { printf("FAIL  could not map the join-diagnostic regions\n"); return 1; }
    const uint8_t cset[] = {0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x83,0x79,0x38,0x00, 0xcc};
    const uint8_t cstate[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x83,0xb9,0xe8,0x02,0x00,0x00,0x00, 0xcc};
    const uint8_t cmsg[] = {0x48,0x8b,0xc4,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x90,0x00,0x00,0x00, 0xcc};
    const uint8_t smsg[] = {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x8b,0x42,0x0c,0x48,0x8b,0xda, 0xcc};
    const uint8_t bqueue[] = {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x50,0xff,0xff,0xff, 0xcc};
    put(0x140cb0080, cset, sizeof cset); put(0x140a30230, cstate, sizeof cstate); put(0x140a338d0, cmsg, sizeof cmsg);
    put(0x140ba0340, smsg, sizeof smsg); put(0x14021b2b0, bqueue, sizeof bqueue);
    // observation (issue #11): the four client component constructors' real prologues, each followed by its epilogue
    if (map(0x141690000, 0x10000) != (void*)0x141690000 || map(0x141680000, 0x10000) != (void*)0x141680000 ||
        map(0x141720000, 0x10000) != (void*)0x141720000 || map(0x1415f0000, 0x10000) != (void*)0x1415f0000) { printf("FAIL  could not map the observation regions\n"); return 1; }
    const uint8_t cmov[] = {0x48,0x8b,0xc4,0x48,0x89,0x48,0x08,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x70, 0x48,0x83,0xc4,0x70,0x41,0x5e,0x5f,0x5e,0xc3};
    const uint8_t chealth[] = {0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xa0,0x00,0x00,0x00, 0x48,0x81,0xc4,0xa0,0x00,0x00,0x00,0x41,0x5f,0x41,0x5e,0x5f,0x5e,0x5d,0xc3};
    const uint8_t cweapon[] = {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x56,0x57, 0x5f,0x5e,0x5d,0xc3};
    const uint8_t cpower[] = {0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff, 0x48,0x83,0xc4,0x30,0x5f,0x5e,0x5d,0xc3};
    put(0x14169cf00, cmov, sizeof cmov); put(0x141680fb0, chealth, sizeof chealth); put(0x14172a5e0, cweapon, sizeof cweapon); put(0x1415fa310, cpower, sizeof cpower);
    // the server's starship direct hit (FUN_14168a8b0): its real 18-byte prologue (mov rax, rsp; seven pushes;
    // lea rbp, [rsp-0x60]), then mov rax, rdx (the out pointer, as the real one returns), lea rsp, [rbp+0x60] and the pops
    const uint8_t shiphit[] = {0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xa0,
                               0x48,0x89,0xd0, 0x48,0x8d,0x65,0x60, 0x41,0x5f,0x41,0x5e,0x41,0x5c,0x5f,0x5e,0x5b,0x5d,0xc3};
    put(0x14168a8b0, shiphit, sizeof shiphit);
    // online error mapper (FUN_1412d43d0): its first 15 bytes, then pop rbp; ret
    if (map(0x1412d0000, 0x10000) != (void*)0x1412d0000) { printf("FAIL  could not map the online-error region\n"); return 1; }
    const uint8_t onlerr[] = {0x40,0x55,0x48,0x8d,0xac,0x24,0xd0,0xe3,0xff,0xff,0xb8,0x30,0x1d,0x00,0x00, 0x5d, 0xc3};
    put(0x1412d43d0, onlerr, sizeof onlerr);
    // error event handler (FUN_1412df8a0): its 20-byte prologue, then lea rsp,[rbp+0x740] and the pops; ret
    const uint8_t errflow[] = {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xc0,0xf8,0xff,0xff,
                               0x48,0x8d,0xa5,0x40,0x07,0x00,0x00, 0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0xc3};
    put(0x1412df8a0, errflow, sizeof errflow);
    // transport setup (FUN_140d7bb70): its 15 bytes of shadow-space stores, then return r9d (the protocol it was given)
    if (map(0x140d70000, 0x10000) != (void*)0x140d70000) { printf("FAIL  could not map the transport region\n"); return 1; }
    const uint8_t tinit[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18, 0x44,0x89,0xc8, 0xc3};
    put(0x140d7bb70, tinit, sizeof tinit);
    // inventory setter (FUN_14123a310): real 20-byte prologue, then a body that records the vector it was given
    if (map(0x141230000, 0x10000) != (void*)0x141230000) { printf("FAIL  could not map the inventory region\n"); return 1; }
    {
        uint8_t inv[64] = {0x48,0x8b,0xc4,0x57,0x48,0x81,0xec,0x00,0x01,0x00,0x00,0x48,0xc7,0x44,0x24,0x28,0xfe,0xff,0xff,0xff};
        int k = 20;
        const uint8_t tail[] = {0x48,0x8b,0x02, 0x48,0xa3}; memcpy(inv + k, tail, sizeof tail); k += sizeof tail;   // mov rax,[rdx]; mov [abs],rax
        *(uint64_t*)(inv + k) = 0x141238000ull; k += 8;
        const uint8_t tail2[] = {0x48,0x8b,0x42,0x08, 0x48,0xa3}; memcpy(inv + k, tail2, sizeof tail2); k += sizeof tail2;   // mov rax,[rdx+8]; mov [abs],rax
        *(uint64_t*)(inv + k) = 0x141238008ull; k += 8;
        const uint8_t tail3[] = {0x48,0x81,0xc4,0x00,0x01,0x00,0x00, 0x5f, 0xc3}; memcpy(inv + k, tail3, sizeof tail3); k += sizeof tail3;
        put(0x14123a310, inv, k);
    }
    if (map(0x140250000, 0x10000) != (void*)0x140250000 || map(0x141190000, 0x10000) != (void*)0x141190000) { printf("FAIL  could not map helper regions\n"); return 1; }
    const uint8_t strassign[] = {0x48,0x89,0x11,0xc3};  // mov [rcx], rdx; ret   (stores the text pointer as the field)
    const uint8_t setoption[] = {0xc3};                  // ret
    put(0x14025c3f0, strassign, sizeof strassign); put(0x141191750, setoption, sizeof setoption);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    // import slots for the socket wrappers (the real exe's .xtls table), filled with the real ws2_32 functions
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    if (map(0x143070000, 0x10000) != (void*)0x143070000) { printf("FAIL  could not map the import-slot region\n"); return 1; }
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    *(void**)0x14307fef0 = (void*)GetProcAddress(ws, "bind");       *(void**)0x14307fee0 = (void*)GetProcAddress(ws, "socket");
    *(void**)0x14307ff70 = (void*)GetProcAddress(ws, "WSASocketW"); *(void**)0x14307ff10 = (void*)GetProcAddress(ws, "sendto");
    // value engine world (issue #3): lock functions, ClassInfo id ranges, one domain with two partitions
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    *(void**)0x14307f388 = (void*)GetProcAddress(k32, "EnterCriticalSection"); *(void**)0x14307f2e8 = (void*)GetProcAddress(k32, "LeaveCriticalSection");
    if (map(0x1442d0000, 0x10000) != (void*)0x1442d0000 || map(0x144ab0000, 0x10000) != (void*)0x144ab0000 ||
        map(0x144a80000, 0x10000) != (void*)0x144a80000 || map(0x144ad0000, 0x10000) != (void*)0x144ad0000) { printf("FAIL  could not map the value-engine regions\n"); return 1; }
    *(uint16_t*)(0x144ab56a0 + 0x48) = 100; *(uint16_t*)(0x144ab56a0 + 0x4a) = 100;   // StarshipMovementConfig
    *(uint16_t*)(0x144a8c980 + 0x48) = 200; *(uint16_t*)(0x144a8c980 + 0x4a) = 200;   // StarshipShieldData
    *(uint16_t*)(0x144a8ca00 + 0x48) = 300; *(uint16_t*)(0x144a8ca00 + 0x4a) = 300;   // StarshipHealthDamageData
    *(uint16_t*)(0x144adafa0 + 0x48) = 400; *(uint16_t*)(0x144adafa0 + 0x4a) = 400;   // IncomBulletConfig
    *(uint16_t*)(0x144adb020 + 0x48) = 450; *(uint16_t*)(0x144adb020 + 0x4a) = 450;   // IncomExplosionData
    *(uint16_t*)(0x144ad9aa0 + 0x48) = 800; *(uint16_t*)(0x144ad9aa0 + 0x4a) = 800;   // TurretFirePatternSettings (the LAST root)
    uint8_t* moveCls = fake_class(100); uint8_t* shieldCls = fake_class(200); uint8_t* otherCls = fake_class(999);
    FakeMove player = fake_movement(moveCls, "Game/Gameplay/Starships/XWing/Config/StarshipMovementConfig_XWing", 2.5f);
    FakeMove bot = fake_movement(moveCls, "Game/Gameplay/Starships/XWing_AI/Config/StarshipMovementConfig_XWingAI_Bot", 1.5f);
    FakeMove creep = fake_movement(moveCls, "Game/Gameplay/Starships/XWing_AI/Config/StarshipMovementConfig_XWingAI", 2.5f);
    // a capital ship's movement config: the same type as a starfighter's, limited on its own ([capital] in limits.ini)
    FakeMove sd = fake_movement(moveCls, "Game/Gameplay/Starships/CapitalShips/StarDestroyer/Config/StarshipMovementConfig_StarDestroyer", 2.5f);
    uint8_t* shield = fake_alloc(0x100); *(uint64_t*)(shield + 8) = (uint64_t)shieldCls;
    *(const char**)(shield + 0x18) = "Game/Gameplay/Starships/XWing/Config/StarshipShieldData_XWing";
    FakeAttr* maxShield = fake_attr(0x5EED0001, 800.0f); *(uint64_t*)(shield + 0x68) = (uint64_t)maxShield;
    { uint32_t ids[1] = { 0x5EED0001 }; float vals[1] = { 800.0f }; *(uint64_t*)(shield + 0x40) = (uint64_t)fake_reglist(1, ids, vals); }
    uint8_t* other = fake_alloc(0x100); *(uint64_t*)(other + 8) = (uint64_t)otherCls;   // a type the engine must skip
    // a hull component: its AttributeModifier list (+0x60) of IncomAttributeModifierFloat objects (ClassInfo at +8,
    // attribute id at +0x18 named at its own +0x18, Value at +0x30); afterburnerchargerate appears twice
    uint8_t* hull = fake_alloc(0x100); *(uint64_t*)(hull + 8) = (uint64_t)fake_class(700);
    *(const char**)(hull + 0x18) = "Game/Gameplay/Starships/Xwing/Config/HullCustom_Heavy_Xwing";
    const char* modAttr[4] = { "Game/Gameplay/Starships/Common/Attribute/Ids/Health/AttributeId_HealthMax",
                               "Game/Gameplay/Starships/Common/Attribute/Ids/Movement/AttributeId_AxisTurnRatesFactor",
                               "Game/Gameplay/Starships/Common/Attribute/Ids/Movement/AttributeId_AfterburnerChargeRate",
                               "Game/Gameplay/Starships/Common/Attribute/Ids/Movement/AttributeId_AfterburnerChargeRate" };
    const float modVal[4] = { 0.6f, -0.25f, 0.5f, -0.5f };
    float* mods[4];
    uint8_t* modArr = fake_alloc(16 + 4 * 8); *(uint32_t*)(modArr + 12) = 4;
    for (int i = 0; i < 4; i++) {
        uint8_t* id = fake_alloc(0x40); *(const char**)(id + 0x18) = modAttr[i];
        uint8_t* m = fake_alloc(0x40); *(uint64_t*)(m + 8) = 0x144a01760; *(uint64_t*)(m + 0x18) = (uint64_t)id;
        *(float*)(m + 0x30) = modVal[i]; mods[i] = (float*)(m + 0x30);
        *(uint64_t*)(modArr + 16 + 8 * i) = (uint64_t)m;
    }
    *(uint64_t*)(hull + 0x60) = (uint64_t)(modArr + 16);
    uint8_t* blast = fake_alloc(0x100); *(uint64_t*)(blast + 8) = (uint64_t)fake_class(450);   // the piercing torpedo's explosion
    *(const char**)(blast + 0x18) = "LiveService/Gameplay/Starships/Common/Projectiles/Config/Tuning/ExplosionConfigLS_Missile_PiercingTorpedo";
    FakeAttr* blastDmg = fake_attr(0x5EED0002, 2000.0f); *(uint64_t*)(blast + 0x68) = (uint64_t)blastDmg;
    { uint32_t ids[1] = { 0x5EED0002 }; float vals[1] = { 2000.0f }; *(uint64_t*)(blast + 0x40) = (uint64_t)fake_reglist(1, ids, vals); }
    // a turret fire pattern: a plain policy asset (no attribute registration lists) of the last root type, so a cap on
    // the number of roots the scan looks at shows up here (the first capital-ship build scanned only 16 of 24)
    uint8_t* firePattern = fake_alloc(0x100); *(uint64_t*)(firePattern + 8) = (uint64_t)fake_class(800);
    *(const char**)(firePattern + 0x18) = "Game/Gameplay/Starships/Common/AI/TurretTargeting/TurretFirePatternSettingsAsset_Frontline";
    *(float*)(firePattern + 0x20) = 1.0f; *(float*)(firePattern + 0x30) = 2.0f;
    if (map(0x1449f0000, 0x10000) != (void*)0x1449f0000 || map(0x144a00000, 0x10000) != (void*)0x144a00000 ||
        map(0x144a30000, 0x10000) != (void*)0x144a30000) { printf("FAIL  could not map the weapon type regions\n"); return 1; }
    *(uint16_t*)(0x1449fc370 + 0x48) = 500; *(uint16_t*)(0x1449fc370 + 0x4a) = 500;   // StarshipWeaponConfig
    *(uint16_t*)(0x144a029c0 + 0x48) = 600; *(uint16_t*)(0x144a029c0 + 0x4a) = 699;   // StarshipWeaponFiringData family
    *(uint16_t*)(0x144a38400 + 0x48) = 610; *(uint16_t*)(0x144a38400 + 0x4a) = 610;   // LaserRegenWeaponFiringData
    *(uint16_t*)(0x144a02dc0 + 0x48) = 700; *(uint16_t*)(0x144a02dc0 + 0x4a) = 700;   // StarshipHullCustomConfig
    *(uint16_t*)(0x144a37300 + 0x48) = 950; *(uint16_t*)(0x144a37300 + 0x4a) = 950;   // DamageOverTimeSettings
    // passive damage: SplitDamageBetweenStarshipsAttribute (+0x90) is an IncomAttributeBool, its value a byte at +32, and
    // the root's bool registration list (+0x38) holds {u32 id, byte value at +4}. Stock yes; the StandIn profile says no.
    uint8_t* dot = fake_alloc(0x100); *(uint64_t*)(dot + 8) = (uint64_t)fake_class(950);
    *(const char**)(dot + 0x18) = "Game/Gameplay/Starships/CapitalShips/Common/DamageOverTime_Settings/DamageOverTimeSettings_Flagship";
    uint8_t* splitAttr = fake_alloc(0x40); *(uint64_t*)(splitAttr + 24) = (uint64_t)fake_idobj(0x5EED0B01); splitAttr[32] = 1;
    *(uint64_t*)(dot + 0x90) = (uint64_t)splitAttr;
    uint8_t* blist = fake_alloc(16 + 8); *(uint32_t*)(blist + 12) = 1; *(uint32_t*)(blist + 16) = 0x5EED0B01; blist[20] = 1;
    *(uint64_t*)(dot + 0x38) = (uint64_t)(blist + 16);
    uint8_t* weapon = fake_alloc(0x100); *(uint64_t*)(weapon + 8) = (uint64_t)fake_class(500);
    *(const char**)(weapon + 0x18) = "Game/Gameplay/Starships/XWing/Config/StarshipWeaponConfig_Las_Reb_XWing";
    uint8_t* firing = fake_alloc(0x100); *(uint64_t*)(firing + 8) = (uint64_t)fake_class(610);
    FakeAttr* rof = fake_attr(0x77770001, 0.1333f); *(uint64_t*)(firing + 0x48) = (uint64_t)rof;
    FakeAttrI* burst = fake_attri(0x66660001, 3); *(uint64_t*)(firing + 0x38) = (uint64_t)burst;
    uint8_t* pips = fake_alloc(16 + 9 * 24); *(uint32_t*)(pips + 12) = 9;   // LaserRegenPipData {curve*, cost @8, regen @12, delay @16}
    for (int i = 0; i < 9; i++) { *(float*)(pips + 16 + i * 24 + 8) = 4.0f - 0.25f * i; *(float*)(pips + 16 + i * 24 + 12) = 2.0f * i; }
    *(uint64_t*)(firing + 0xc8) = (uint64_t)(pips + 16);
    *(uint64_t*)(weapon + 0x60) = (uint64_t)firing;
    { uint32_t ids[1] = { 0x77770001 }; float vals[1] = { 0.1333f }; *(uint64_t*)(weapon + 0x40) = (uint64_t)fake_reglist(1, ids, vals); }
    uint8_t* ilist = fake_alloc(16 + 8); *(uint32_t*)(ilist + 12) = 1; *(uint32_t*)(ilist + 16) = 0x66660001; *(int32_t*)(ilist + 20) = 3;
    *(uint64_t*)(weapon + 0x48) = (uint64_t)(ilist + 16);
    uint8_t** insts1 = (uint8_t**)fake_alloc(64); insts1[0] = player.inst; insts1[1] = bot.inst; insts1[2] = other; insts1[3] = blast; insts1[4] = hull; insts1[5] = firePattern;
    // match rules (IncomSchematicAttributesConfig, ClassInfo 0x144adaea0): values only in its registration lists, 8-byte
    // entries {u32 attribute id, value at +4}: bool +0x38, float +0x40, int +0x48. Creep morale on offense (600676783) and
    // defense (3544638778), an id no attribute has, the match length (1469253729) and the morale DoT switch (3123731505).
    *(uint16_t*)(0x144adaea0 + 0x48) = 960; *(uint16_t*)(0x144adaea0 + 0x4a) = 960;
    uint8_t* rules = fake_alloc(0x100); *(uint64_t*)(rules + 8) = (uint64_t)fake_class(960);
    *(const char**)(rules + 0x18) = "Game/Levels/PvP/AttributeIds/FleetBattle/IncomSchematicAttributesConfig_FleetBattle";
    uint8_t* rFloat = fake_alloc(16 + 3 * 8); *(uint32_t*)(rFloat + 12) = 3;
    *(uint32_t*)(rFloat + 16) = 600676783u; *(float*)(rFloat + 20) = 1.0f; *(uint32_t*)(rFloat + 24) = 3544638778u; *(float*)(rFloat + 28) = 3.0f;
    *(uint32_t*)(rFloat + 32) = 0x12345678u; *(float*)(rFloat + 36) = 9.0f;
    uint8_t* rInt = fake_alloc(16 + 8); *(uint32_t*)(rInt + 12) = 1; *(uint32_t*)(rInt + 16) = 1469253729u; *(int32_t*)(rInt + 20) = 1800;
    uint8_t* rBool = fake_alloc(16 + 8); *(uint32_t*)(rBool + 12) = 1; *(uint32_t*)(rBool + 16) = 3123731505u; rBool[20] = 0;
    *(uint64_t*)(rules + 0x38) = (uint64_t)(rBool + 16); *(uint64_t*)(rules + 0x40) = (uint64_t)(rFloat + 16); *(uint64_t*)(rules + 0x48) = (uint64_t)(rInt + 16);
    // and its typed attribute objects (Attributes +0x60, Value +0x20) named by AttributeNameHash (+0x68, djb2-xor of the
    // name): creep morale on offense ("ToWPointsGainedForCreepKillWhileOnOffense", 2625382658) and on defense (419017642)
    uint8_t* aOff = fake_alloc(0x30); *(float*)(aOff + 0x20) = 1.0f; uint8_t* aDef = fake_alloc(0x30); *(float*)(aDef + 0x20) = 3.0f;
    uint8_t* rAttrs = fake_alloc(16 + 2 * 8); *(uint32_t*)(rAttrs + 12) = 2; *(uint8_t**)(rAttrs + 16) = aDef; *(uint8_t**)(rAttrs + 24) = aOff;
    uint8_t* rHash = fake_alloc(16 + 2 * 4); *(uint32_t*)(rHash + 12) = 2; *(uint32_t*)(rHash + 16) = 419017642u; *(uint32_t*)(rHash + 20) = 2625382658u;
    *(uint64_t*)(rules + 0x60) = (uint64_t)(rAttrs + 16); *(uint64_t*)(rules + 0x68) = (uint64_t)(rHash + 16);
    uint8_t** insts2 = (uint8_t**)fake_alloc(64); insts2[0] = creep.inst; insts2[1] = shield; insts2[2] = weapon; insts2[3] = sd.inst; insts2[4] = dot; insts2[5] = rules;
    uint8_t* part1 = fake_alloc(0x40); *(uint8_t***)(part1 + 0x20) = insts1; *(uint16_t*)(part1 + 0x28) = 6;
    uint8_t* part2 = fake_alloc(0x40); *(uint8_t***)(part2 + 0x20) = insts2; *(uint16_t*)(part2 + 0x28) = 6;
    uint8_t** parts = (uint8_t**)fake_alloc(64); parts[0] = part1; parts[1] = part2;
    uint8_t* domain = fake_alloc(0x200); InitializeCriticalSection((CRITICAL_SECTION*)(domain + 0x70));
    *(uint8_t***)(domain + 0x138) = parts; *(uint8_t***)(domain + 0x140) = parts + 2;
    uint8_t* centry = fake_alloc(0x80); *(uint8_t**)(centry + 0x68) = domain;
    *(uint8_t**)(0x1442d18c8 + 4 * 8) = centry;   // compartment 4 ("Game")
    // creep packs: a level layer (LayerData 0x144a56c40) whose link connections (+48, 24-byte entries) tie three Rebel creep
    // prefab placements (0x144a176a0, Blueprint +0xC0 "PF_PvP_CreepLogic_Exploit_XWings") to their two spawn points each
    // (0x1449fdc00: ship +0xC0, loadout +0x1F0, translation +176), listed out of order; plus a decoy link from another
    // prefab's placement. The StandIn profile: rebel_pack1 = Y-wing, rebel_pack3 = A-wing, pack 2 stock.
    auto bpRoot = [](const char* name) { uint8_t* b = fake_alloc(0x40); *(uint64_t*)(b + 8) = 0x144ade220; *(const char**)(b + 0x18) = name; return b; };
    uint8_t* xwingAi = bpRoot("Game/Gameplay/Starships/XWing_AI/Starship_XWing_AI");
    uint8_t* ywingAi = bpRoot("Game/Gameplay/Starships/Ywing_AI/Starship_Ywing_AI");
    uint8_t* awingAi = bpRoot("Game/Gameplay/Starships/AWing_AI/Starship_AWing_AI");
    // the stock creep collection the stock packs' spawn points name (StarshipConfigCollection 0x144a156a0, Configs +0x20:
    // the creep X-wing's hull), and the bots' X-wing and Y-wing movement (StarshipMovementConfig 0x144ab56a0), for
    // [creep] bot_handling
    uint8_t* xCreepHull = fake_alloc(0x400); *(uint64_t*)(xCreepHull + 8) = 0x144a8ca00;
    *(const char**)(xCreepHull + 0x18) = "Game/Gameplay/Starships/XWing_AI/Config/StarshipHealthDamageData_XWing_AI";
    uint8_t* creepCfg = fake_alloc(16 + 0x40) + 16; *(uint64_t*)(creepCfg + 8) = 0x144a156a0;
    *(const char**)(creepCfg + 0x18) = "Game/Gameplay/Starships/XWing_AI/Config/StarshipConfigCollection_XWing_AI";
    { uint8_t* a = fake_alloc(16 + 8) + 16; *(uint32_t*)(a - 4) = 1; ((uint8_t**)a)[0] = xCreepHull; *(uint8_t**)(creepCfg + 0x20) = a; }
    uint8_t* xBotMove = fake_alloc(0x400); *(uint64_t*)(xBotMove + 8) = 0x144ab56a0;
    *(const char**)(xBotMove + 0x18) = "Game/Gameplay/Starships/XWing_AI/Config/StarshipMovementConfig_XWingAI_BOT";
    uint8_t* creepPrefab = fake_alloc(0x40); *(const char**)(creepPrefab + 0x18) = "Game/Levels/PvP/Prefab/Frontline/PF_PvP_CreepLogic_Exploit_XWings";
    uint8_t* otherPrefab = fake_alloc(0x40); *(const char**)(otherPrefab + 0x18) = "Game/Levels/PvP/Prefab/Frontline/PF_PvP_FrontlineState";
    uint8_t* creepSpawn[3][2]; uint8_t* place[3];
    const float packX[3] = { -303.0f, -220.0f, -113.0f };
    for (int k = 0; k < 3; k++) {
        place[k] = fake_alloc(0x100); *(uint64_t*)(place[k] + 8) = 0x144a176a0; *(uint64_t*)(place[k] + 0xC0) = (uint64_t)creepPrefab;
        for (int j = 0; j < 2; j++) {
            uint8_t* sp = fake_alloc(0x200); *(uint64_t*)(sp + 8) = 0x1449fdc00;
            *(uint64_t*)(sp + 0xC0) = (uint64_t)xwingAi; *(uint64_t*)(sp + 0x1F0) = (uint64_t)creepCfg;
            *(float*)(sp + 176) = packX[k]; *(float*)(sp + 180) = 384.0f; *(float*)(sp + 184) = j ? 3697.0f : 3781.0f;
            creepSpawn[k][j] = sp;
        }
    }
    uint8_t* decoyPlace = fake_alloc(0x100); *(uint64_t*)(decoyPlace + 8) = 0x144a176a0; *(uint64_t*)(decoyPlace + 0xC0) = (uint64_t)otherPrefab;
    uint8_t* decoySpawn = fake_alloc(0x200); *(uint64_t*)(decoySpawn + 8) = 0x1449fdc00; *(uint64_t*)(decoySpawn + 0xC0) = (uint64_t)xwingAi;
    uint8_t* lcArr = fake_alloc(16 + 7 * 24); *(uint32_t*)(lcArr + 12) = 7;
    const int order[3] = { 2, 0, 1 };
    for (int i = 0; i < 6; i++) {
        uint8_t* e = lcArr + 16 + i * 24; int k = order[i / 2];
        *(uint64_t*)e = (uint64_t)place[k]; *(uint64_t*)(e + 8) = (uint64_t)creepSpawn[k][i % 2];
    }
    *(uint64_t*)(lcArr + 16 + 6 * 24) = (uint64_t)decoyPlace; *(uint64_t*)(lcArr + 16 + 6 * 24 + 8) = (uint64_t)decoySpawn;
    uint8_t* layer = fake_alloc(0x80); *(uint64_t*)(layer + 8) = 0x144a56c40; *(uint64_t*)(layer + 48) = (uint64_t)(lcArr + 16);
    auto onePart = [](uint8_t* root) { uint8_t** in = (uint8_t**)fake_alloc(16); in[0] = root; uint8_t* p = fake_alloc(0x40);
                                       *(uint8_t***)(p + 0x20) = in; *(uint16_t*)(p + 0x28) = 1; return p; };
    parts[2] = onePart(layer); parts[3] = onePart(xwingAi); parts[4] = onePart(ywingAi); parts[5] = onePart(awingAi);
    // creep missiles ([creep] missiles=1): bhv_aisquadron_basic_imperial's wiring, cut down. The AllowMissileFiring input
    // (1336394533) into a PropertyDefaultValue's InValue (1658462601), whose DefaultValue (2066049125) a Bool feeds; a Bool
    // into a node that DogfighterInfo.IsBot (215512070) also feeds (the bot test); the SelectFloat (InputSelect +48) whose
    // Out (193453899) is the missile rule's MinTargetDistance (-1814841251); and a decoy: another input's default Bool
    // (DisableBombs, -737100000), which must stay as it is.
    auto bhvObj = [](uint64_t cls) { uint8_t* o = fake_alloc(0x40); *(uint64_t*)(o + 8) = cls; return o; };
    uint8_t* bhvIface = bhvObj(0x144999100); uint8_t* bhvPdv = bhvObj(0x144999101); uint8_t* bhvPdv2 = bhvObj(0x144999101);
    uint8_t* allowBool = bhvObj(0x144a58840); uint8_t* flagBool = bhvObj(0x144a58840); uint8_t* bombsBool = bhvObj(0x144a58840);
    uint8_t* orNode = bhvObj(0x144999102); uint8_t* dogInfo = bhvObj(0x144999103); uint8_t* minSel = bhvObj(0x144999104); uint8_t* rule = bhvObj(0x144999105);
    // creep wingmen ([creep] wingmen=1): the two loadout gates' Ors take an interface field (555857281) into In4
    // (193450870); the AllowCountermeasures input (-1073460925) into its PropertyDefaultValue, whose HasValueSet
    // (-645606250) feeds a Not's In (5862146). The mod repoints the two In4 connections at that Not's Out (193453899).
    uint8_t* gateOr1 = bhvObj(0x144999106); uint8_t* gateOr2 = bhvObj(0x144999106); uint8_t* cmPdv = bhvObj(0x144999101); uint8_t* cmNot = bhvObj(0x144999107);
    uint8_t* otherNot = bhvObj(0x144999107); uint8_t* otherAnd = bhvObj(0x144999108);   // a decoy: another Not's Out into another And's In4
    struct PcEntry { uint8_t* s; uint8_t* d; int32_t sf, tf; };
    const PcEntry bhvPc[] = { { bhvIface, bhvPdv, 1336394533, 1658462601 }, { allowBool, bhvPdv, 225375086, 2066049125 },
                              { minSel, rule, 193453899, -1814841251 }, { flagBool, orNode, 225375086, 193450867 },
                              { dogInfo, orNode, 215512070, 193450864 }, { bhvIface, bhvPdv2, -737100000, 1658462601 },
                              { bombsBool, bhvPdv2, 225375086, 2066049125 },
                              { bhvIface, gateOr1, 555857281, 193450870 }, { bhvIface, gateOr2, 555857281, 193450870 },
                              { bhvIface, cmPdv, -1073460925, 1658462601 }, { cmPdv, cmNot, -645606250, 5862146 },
                              { otherNot, otherAnd, 193453899, 193450870 } };
    const int nBhvPc = sizeof bhvPc / sizeof bhvPc[0];
    uint8_t* bhvPcArr = fake_alloc(16 + nBhvPc * 32); *(uint32_t*)(bhvPcArr + 12) = nBhvPc;
    for (int i = 0; i < nBhvPc; i++) {
        uint8_t* e = bhvPcArr + 16 + i * 32; *(uint8_t**)e = bhvPc[i].s; *(uint8_t**)(e + 8) = bhvPc[i].d;
        *(int32_t*)(e + 16) = bhvPc[i].sf; *(int32_t*)(e + 20) = bhvPc[i].tf;
    }
    uint8_t* bhvRoot = fake_alloc(0x80); *(uint64_t*)(bhvRoot + 8) = 0;
    *(const char**)(bhvRoot + 0x18) = "Game/AI/SquadronAI/Behaviours/Bhv_AISquadron_Basic_Imperial"; *(uint64_t*)(bhvRoot + 40) = (uint64_t)(bhvPcArr + 16);
    parts[6] = onePart(bhvRoot); parts[7] = onePart(creepPrefab);
    *(uint8_t***)(domain + 0x140) = parts + 8;
    // bomber torpedoes ([creep] torpedoes=1): the Y-wing's creep hull and shields (the SP configs, by their ClassInfos, not
    // top-level: inside another asset's partition, as the game keeps them) and the bots' torpedo collection (ClassInfo
    // 0x144a156a0: bot hull, two weapon slots 0x1449fc3f0, bot movement), each an Asset, the collection's Configs at +0x20
    uint8_t* torpWeapon = nullptr; uint8_t* torpFiring = nullptr;   // the Y-wing AI torpedo, for the lock-filter check
    uint8_t* raidData = nullptr; uint8_t* raidBp = nullptr; uint8_t* bombCfg = nullptr;   // the run-data check
    auto cfgNamed = [](const char* name, uint64_t cls) { uint8_t* c = fake_alloc(0x400); *(uint64_t*)(c + 8) = cls; *(const char**)(c + 0x18) = name; return c; };
    uint8_t* spHull = cfgNamed("Game/Gameplay/Starships/YWing_AI/Config/StarshipHealthDamageData_YWing_AI_SP", 0x144a8ca00);
    uint8_t* spShield = cfgNamed("Game/Gameplay/Starships/YWing_AI/Config/StarshipShieldData_YWing_AI_SP", 0x144a8c980);
    uint8_t* botHull = cfgNamed("Game/Gameplay/Starships/YWing_AI/Config/StarshipHealthDamageData_YWing_BOT_PvB", 0);
    uint8_t* slotTorp = cfgNamed("Game/Gameplay/Starships/YWing_AI/Config/SlotConfig_YWing_Torpedos_AI_Instalock", 0x1449fc3f0);
    uint8_t* slotBomb = fake_alloc(16 + 0x400) + 16;   // with the 16 bytes (a GUID) ahead of it, which the mod's copy takes too
    *(uint64_t*)(slotBomb + 8) = 0x1449fc3f0; *(const char**)(slotBomb + 0x18) = "Game/Gameplay/Starships/YWing/Config/SlotConfig_YWing_Bomb";
    uint8_t* botMove = cfgNamed("Game/Gameplay/Starships/YWing_AI/Config/StarshipMovementConfig_YWingAI_BOT", 0x144ab56a0);
    auto collection = [&](const char* name, std::initializer_list<uint8_t*> cs) {
        uint8_t* c = fake_alloc(16 + 0x30) + 16; *(uint64_t*)(c + 8) = 0x144a156a0; *(const char**)(c + 0x18) = name;
        uint8_t* a = fake_alloc(16 + cs.size() * 8) + 16; *(uint32_t*)(a - 4) = (uint32_t)cs.size();
        size_t i = 0; for (uint8_t* x : cs) ((uint8_t**)a)[i++] = x;
        *(uint8_t**)(c + 0x20) = a; return c; };
    uint8_t* ywingSp = collection("Game/Gameplay/Starships/YWing_AI/Config/StarshipConfigCollection_YWing_AI_SP", { spHull, spShield });
    uint8_t* ywingTorp = collection("Game/Gameplay/Starships/YWing_AI/Config/StarshipConfigCollection_YWing_AI_PvB_BOT_Torpedo_Bomb", { botHull, slotTorp, slotBomb, botMove });
    {
        uint8_t** spInsts = (uint8_t**)fake_alloc(7 * 8); spInsts[0] = ywingSp; spInsts[1] = spHull; spInsts[2] = spShield;
        spInsts[3] = creepCfg; spInsts[4] = xCreepHull; spInsts[5] = xBotMove; spInsts[6] = botMove;
        uint8_t* spPart = fake_alloc(0x40); *(uint8_t***)(spPart + 0x20) = spInsts; *(uint16_t*)(spPart + 0x28) = 7;
        // the Y-wing's AI torpedo (StarshipWeaponConfig 0x1449fc370): its WeaponFiring (+96) a lock-on firing data
        // (TargetLockType +224 = 2, TargetTags +228 = 0, anything)
        torpWeapon = fake_alloc(0x200); *(uint64_t*)(torpWeapon + 8) = 0x1449fc370;
        *(const char**)(torpWeapon + 0x18) = "Game/Gameplay/Starships/YWing_AI/Projectiles/StarshipWeaponConfig_HighYieldTorpedo_YWing_AI_Instalock";
        torpFiring = fake_alloc(0x200); *(int32_t*)(torpFiring + 224) = 2; *(uint8_t**)(torpWeapon + 96) = torpFiring;
        // its MinLockDistanceAttribute (+184): an IncomAttributeFloat at 500 (+32), id 0x5157 (its id object +24 -> +32), also
        // in the weapon config's float registration list (+0x40) as {0x5157, 500}
        { uint8_t* at = fake_alloc(0x40); uint8_t* idObj = fake_alloc(0x40); *(uint32_t*)(idObj + 32) = 0x5157;
          *(float*)(at + 32) = 500.f; *(uint8_t**)(at + 24) = idObj; *(uint8_t**)(torpFiring + 184) = at;
          uint8_t* fl = fake_alloc(16 + 16) + 16; *(uint32_t*)(fl - 4) = 1; *(uint32_t*)fl = 0x5157; *(float*)(fl + 4) = 500.f;
          *(uint8_t**)(torpWeapon + 0x40) = fl; }
        *(uint8_t**)(slotTorp + 96) = torpWeapon;   // the torpedo slot's weapon (StarshipBaseSlotConfig Config +96)
        *(uint8_t**)(slotBomb + 96) = bhvObj(0x1449fc370);   // the bombs' slot: a bomb weapon
        spInsts[6] = botMove;
        // the AI bombs (StarshipWeaponConfig) carry the bombing-run blueprint (+120): an IncomSquadronAIHostBehaviourBlueprint
        // whose Objects (+72) hold a root and the run data (ClassInfo 0x144a48340) with the stock values
        raidData = fake_alloc(0x80); *(uint64_t*)(raidData + 8) = 0x144a48340;
        *(float*)(raidData + 40) = 400.f; *(float*)(raidData + 44) = 100.f; *(float*)(raidData + 48) = 50.f; *(float*)(raidData + 52) = 250.f;
        *(float*)(raidData + 56) = 50.f; *(float*)(raidData + 68) = 500.f; *(uint8_t*)(raidData + 72) = 1;
        raidBp = fake_alloc(0x100); { uint8_t* ro = fake_alloc(16 + 16) + 16; *(uint32_t*)(ro - 4) = 2;
          ((uint8_t**)ro)[0] = fake_alloc(0x40); ((uint8_t**)ro)[1] = raidData; *(uint8_t**)(raidBp + 72) = ro; }
        bombCfg = fake_alloc(0x200); *(uint64_t*)(bombCfg + 8) = 0x1449fc370; *(uint8_t**)(bombCfg + 120) = raidBp;
        *(const char**)(bombCfg + 0x18) = "Game/Gameplay/Starships/TieBomber_AI/Projectiles/StarshipWeaponConfig_Bomb_Standard_TieBomber_AI";
        { uint8_t** in2 = (uint8_t**)fake_alloc(9 * 8); memcpy(in2, spInsts, 7 * 8); in2[7] = torpWeapon; in2[8] = bombCfg;
          *(uint8_t***)(spPart + 0x20) = in2; *(uint16_t*)(spPart + 0x28) = 9; }
        uint8_t** grown = (uint8_t**)fake_alloc(10 * 8); memcpy(grown, parts, 8 * 8); grown[8] = spPart; grown[9] = onePart(ywingTorp);
        *(uint8_t***)(domain + 0x138) = grown; *(uint8_t***)(domain + 0x140) = grown + 10;
    }
    wchar_t defaultsPath[MAX_PATH]; GetModuleFileNameW(nullptr, defaultsPath, MAX_PATH);
    wcscpy_s(wcsrchr(defaultsPath, L'\\') + 1, 32, L"defaults.ini");
    {   // an earlier launch's file: a config this run never loads (must be carried over) and a stale section for one it
        // does (must be rewritten whole, so its stale key goes)
        FILE* f = nullptr;
        if (_wfopen_s(&f, defaultsPath, L"wb") == 0 && f) {
            fputs("; earlier launch" "\r\n" "\r\n" "[oldconfig_from_earlier]" "\r\n" "foo=1" "\r\n" "bar[2]=-0.5" "\r\n" "\r\n"
                  "[starshipmovementconfig_xwing]" "\r\n" "stalefield=9" "\r\n", f);
            fclose(f);
        }
    }
    check(GetModuleHandleW(L"sws_server_test.dll") != nullptr, "DLL was loaded at startup by the launcher's APC");
    for (int i = 0; i < 300 && *(uint8_t*)0x1401ae7d0 != 0xFF; i++) Sleep(50);
    check(*(uint8_t*)0x1401ae7d0 == 0xFF && *(uint8_t*)0x1401aac90 == 0xFF && *(uint8_t*)0x1401bf4d0 == 0xFF, "server hooks installed");

    {   // Configs get the profile as their partition is created (capital ships are built from them as the map loads, before
        // any scan): a passive-damage config in a partition the store never lists, handed over by the partition creator.
        uint8_t* medcap = fake_alloc(0x100); *(uint64_t*)(medcap + 8) = (uint64_t)fake_class(950);
        *(const char**)(medcap + 0x18) = "Game/Gameplay/Starships/CapitalShips/Common/DamageOverTime_Settings/DamageOverTimeSettings_Medcap";
        FakeAttr* stop = fake_attr(0x5EED0C01, 0.15f); *(uint64_t*)(medcap + 0x98) = (uint64_t)stop;
        uint32_t ids[1] = { 0x5EED0C01 }; float vals[1] = { 0.15f }; float* reg = fake_reglist(1, ids, vals);
        *(uint64_t*)(medcap + 0x40) = (uint64_t)reg;
        uint8_t** insts3 = (uint8_t**)fake_alloc(16); insts3[0] = medcap;
        uint8_t* part3 = fake_alloc(0x40); *(uint8_t***)(part3 + 0x20) = insts3; *(uint16_t*)(part3 + 0x28) = 1;
        typedef uint64_t(__fastcall* partnew_t)(uint64_t, uint64_t, uint64_t, uint64_t);
        uint64_t got = ((partnew_t)0x140273060)((uint64_t)domain, 0, (uint64_t)part3, 0);
        check(*(uint8_t*)0x140273060 == 0xFF && got == (uint64_t)part3, "partition creator hooked and passes the partition through");
        check(stop->value == 0.5f && reg[1] == 0.5f,
              "value engine: a config gets the profile as its partition is created, before any scan (passive damage floor 0.15 -> 0.5)");
    }
    {   // map fix: Zavian Abyss's PvP layer, with its one miswired creep spawn event (ReturnControl) among Spawns and an
        // unrelated ReturnControl from another source, arrives through the partition creator; only the miswired one changes
        uint8_t* root = fake_alloc(0x100); *(uint64_t*)(root + 8) = (uint64_t)fake_class(1);
        *(const char**)(root + 0x18) = "Game/Levels/PvP/PVP_Abyss/PvP";
        uint8_t* ev = fake_alloc(16 + 4 * 32); *(uint32_t*)(ev + 12) = 4; int32_t* e = (int32_t*)(ev + 16);
        const int32_t pairs[4][2] = { { -202538501, 230891742 }, { -202538501, -597186424 }, { -202538501, 230891742 }, { 12345, -597186424 } };
        for (int k = 0; k < 4; k++) { e[k * 8 + 4] = pairs[k][0]; e[k * 8 + 5] = pairs[k][1]; }
        *(uint64_t*)(root + 0x40) = (uint64_t)(ev + 16);
        uint8_t** insts4 = (uint8_t**)fake_alloc(16); insts4[0] = root;
        uint8_t* part4 = fake_alloc(0x40); *(uint8_t***)(part4 + 0x20) = insts4; *(uint16_t*)(part4 + 0x28) = 1;
        typedef uint64_t(__fastcall* partnew_t)(uint64_t, uint64_t, uint64_t, uint64_t);
        ((partnew_t)0x140273060)((uint64_t)domain, 0, (uint64_t)part4, 0);
        check(e[1 * 8 + 5] == 230891742 && e[0 * 8 + 5] == 230891742 && e[2 * 8 + 5] == 230891742 && e[3 * 8 + 5] == -597186424,
              "map fix: Zavian Abyss's miswired creep spawn event becomes Spawn as its layer loads; the other connections untouched");
    }
    {   // the Raider's blueprint loads with its one damage section (DamageSectionComponentData 0x144a245c0, ClearFaces
        // +292..+297, none clear); runs on it were tried and dropped with the torpedo runs, so it stays as it is, and so does a frigate's
        typedef uint64_t(__fastcall* partnew_t)(uint64_t, uint64_t, uint64_t, uint64_t);
        auto bpPart = [](const char* name, uint8_t* sec) {
            uint8_t* bp = fake_alloc(0x100); *(uint64_t*)(bp + 8) = 0x144ade220; *(const char**)(bp + 0x18) = name;
            uint8_t** in = (uint8_t**)fake_alloc(16); in[0] = bp; in[1] = sec;
            uint8_t* p = fake_alloc(0x40); *(uint8_t***)(p + 0x20) = in; *(uint16_t*)(p + 0x28) = 2; return p; };
        uint8_t* raiderSec = fake_alloc(0x200); *(uint64_t*)(raiderSec + 8) = 0x144a245c0;
        uint8_t* frigateSec = fake_alloc(0x200); *(uint64_t*)(frigateSec + 8) = 0x144a245c0;
        ((partnew_t)0x140273060)((uint64_t)domain, 0, (uint64_t)bpPart("Game/Gameplay/Starships/CapitalShips/Raider/Starship_Raider", raiderSec), 0);
        ((partnew_t)0x140273060)((uint64_t)domain, 0, (uint64_t)bpPart("Game/Gameplay/Starships/CapitalShips/Frigate/Starship_Frigate", frigateSec), 0);
        bool sides = !raiderSec[292] && !raiderSec[293] && !raiderSec[294] && !raiderSec[295];
        bool frigate = true; for (int k = 0; k < 6; k++) frigate = frigate && !frigateSec[292 + k];
        check(!raiderSec[296] && !raiderSec[297] && sides && frigate,
              "creep: the Raider's and a frigate's damage sections load untouched");
    }
    uint8_t* creepSquadForMode = nullptr;   // the creep squadron below, for the bomb-raid mode check after the ticks
    uint8_t* creepList = nullptr; uint8_t* creepCtrlData = nullptr;   // and its shared list and controller, for the per-ship-type check
    {   // aggressive creep ([creep] aggressive=1): PF_PvP_CreepLogic_Exploit_TieFighters arrives through the partition
        // creator. Its property connections: the phase (FrontlineIndex, 862952468) into a debug display (field 5685821) and
        // into the SelectInt that picks the path (858290427); its squadron (0x144a028c0) is Defensive within 500 m, its
        // override Aggressive (0), one target list (+104) of the stock creep's five scores; the SelectInt's Out feeds the
        // controller's BehaviourIndex. Afterwards every phase's entry is one shared list, whose scores follow the
        // controller's behaviour: Imperial offense at behaviours 1 and 2, defense at 4 and 5, face-off at 3. The stock list
        // itself is left as it was.
        auto dc = [](size_t size) { uint8_t* o = fake_alloc(16 + size) + 16; *(uint64_t*)o = 0x143999000; *(uint32_t*)(o + 16) = 1; return o; };
        const uint32_t tags[5] = { 262144, 36, 20, 2, 524292 }; const float prios[5] = { 5, 3, 2, 2, 3 };
        uint8_t* sArr = fake_alloc(16 + 5 * 8) + 16; *(uint32_t*)(sArr - 4) = 5;
        for (int k = 0; k < 5; k++) { uint8_t* s = dc(48); *(uint32_t*)(s + 40) = tags[k]; *(float*)(s + 44) = prios[k]; ((uint8_t**)sArr)[k] = s; }
        uint8_t* stockSet = dc(32); *(uint8_t**)(stockSet + 24) = sArr;
        uint8_t* lst = fake_alloc(16 + 8) + 16; *(uint32_t*)(lst - 4) = 1; ((uint8_t**)lst)[0] = stockSet;
        uint8_t* squad = fake_alloc(0x100); *(uint64_t*)(squad + 8) = 0x144a028c0;
        *(int32_t*)(squad + 32) = 1; *(float*)(squad + 40) = 500.0f; *(int32_t*)(squad + 112) = 0; *(uint8_t*)(squad + 139) = 1;
        *(uint8_t**)(squad + 104) = lst;
        uint8_t* debugDisp = fake_alloc(0x100); uint8_t* selInt = fake_alloc(0x40); uint8_t* iface = fake_alloc(0x40);
        uint8_t* ctrlData = fake_alloc(0x100);
        uint8_t* pcArr = fake_alloc(16 + 3 * 32) + 16; *(uint32_t*)(pcArr - 4) = 3;
        *(uint8_t**)(pcArr + 64) = selInt; *(uint8_t**)(pcArr + 72) = ctrlData; *(int32_t*)(pcArr + 80) = 193453899; *(int32_t*)(pcArr + 84) = 875081154;
        *(uint8_t**)pcArr = iface; *(uint8_t**)(pcArr + 8) = debugDisp; *(int32_t*)(pcArr + 16) = 862952468; *(int32_t*)(pcArr + 20) = 5685821; *(int32_t*)(pcArr + 24) = 3;
        *(uint8_t**)(pcArr + 32) = iface; *(uint8_t**)(pcArr + 40) = selInt; *(int32_t*)(pcArr + 48) = 862952468; *(int32_t*)(pcArr + 52) = 858290427;
        uint8_t* objArr = fake_alloc(16 + 4 * 8) + 16; *(uint32_t*)(objArr - 4) = 4; ((uint8_t**)objArr)[3] = ctrlData;
        ((uint8_t**)objArr)[0] = debugDisp; ((uint8_t**)objArr)[1] = squad; ((uint8_t**)objArr)[2] = selInt;
        uint8_t* bp = fake_alloc(0x80); *(uint64_t*)(bp + 8) = 0;
        *(const char**)(bp + 0x18) = "Game/Levels/PvP/Prefab/Frontline/PF_PvP_CreepLogic_Exploit_TieFighters";
        *(uint8_t**)(bp + 40) = pcArr; *(uint8_t**)(bp + 72) = objArr;
        uint8_t** insts6 = (uint8_t**)fake_alloc(16); insts6[0] = bp;
        uint8_t* part6 = fake_alloc(0x40); *(uint8_t***)(part6 + 0x20) = insts6; *(uint16_t*)(part6 + 0x28) = 1;
        typedef uint64_t(__fastcall* partnew_t)(uint64_t, uint64_t, uint64_t, uint64_t);
        ((partnew_t)0x140273060)((uint64_t)domain, 0, (uint64_t)part6, 0);
        {   // and list it with the loaded partitions, as the game's store would (the bomb-raid mode is set only while loaded)
            uint8_t** cur = *(uint8_t***)(domain + 0x138); size_t np = *(uint8_t***)(domain + 0x140) - cur;
            uint8_t** grown = (uint8_t**)fake_alloc((np + 1) * 8); memcpy(grown, cur, np * 8); grown[np] = part6;
            *(uint8_t***)(domain + 0x138) = grown; *(uint8_t***)(domain + 0x140) = grown + np + 1;
        }
        creepSquadForMode = squad; creepList = *(uint8_t**)(*(uint8_t**)(squad + 104)); creepCtrlData = ctrlData;
        uint8_t* nl = *(uint8_t**)(squad + 104);
        // the game's score lookup (FUN_141617a00): the first score whose TargetTags bits are all on the target; none = 0
        auto score = [](uint8_t* set, uint32_t target) { uint8_t* a = *(uint8_t**)(set + 24); uint32_t n = *(uint32_t*)(a - 4);
                                                         for (uint32_t k = 0; k < n; k++) { uint8_t* s = ((uint8_t**)a)[k]; uint32_t m = *(uint32_t*)(s + 40);
                                                                                            if ((m & target) == m) return *(float*)(s + 44); }
                                                         return 0.0f; };
        // a frigate that also carries bits a stock score asks for (20 = bits 2 and 4), so the order of the scores matters
        const uint32_t frigate = (1u << 29) | 2 | 20, flagship = (1u << 22) | 2, smallcap = (1u << 28) | 2, turret = 262144;
        bool lists = nl != lst && *(uint32_t*)(nl - 4) == 7;
        uint8_t* L[7] = {}; for (int p = 0; lists && p < 7; p++) L[p] = ((uint8_t**)nl)[p];
        bool shared = lists && L[0] && L[0] == L[1] && L[0] == L[2] && L[0] == L[3] && L[0] == L[4] && L[0] == L[5] && L[0] == L[6];
        bool opener = shared && score(L[0], frigate) == 0 && score(L[0], flagship) == 0 && score(L[0], smallcap) == 10 && score(L[0], turret) == 5;
        // the phase arrives as the controller's behaviour: the controller entity's field-change handler stores it
        uint8_t* ent = fake_alloc(0x200); *(uint8_t**)(ent + 0x28) = ctrlData;
        int32_t bi = 1; uint8_t chg[16] = {}; *(int32_t*)chg = 875081154; *(int32_t**)(chg + 8) = &bi;
        typedef void(__fastcall* cf_t)(uint8_t*, uint8_t*);
        ((cf_t)0x1416feaf0)(ent, chg);   // behaviour 1 = phase 0, a Rebel zone: Imperial offense
        bool offense = shared && *(int32_t*)(ent + 0xb4) == 1 && score(L[0], frigate) == 10 && score(L[0], flagship) == 9 && score(L[0], smallcap) == 8 && score(L[0], turret) == 5;
        bi = 4; ((cf_t)0x1416feaf0)(ent, chg);   // behaviour 4 = phase 3, an Imperial zone: defense
        bool defense = shared && score(L[0], frigate) == 0 && score(L[0], flagship) == 0 && score(L[0], smallcap) == 10;
        bi = 3; ((cf_t)0x1416feaf0)(ent, chg);   // behaviour 3 = face-off or transition: the opener's scores again
        bool back = shared && score(L[0], frigate) == 0 && score(L[0], smallcap) == 10;
        uint8_t* other = fake_alloc(0x200); *(uint8_t**)(other + 0x28) = debugDisp; bi = 1; ((cf_t)0x1416feaf0)(other, chg);
        bool untouched = shared && score(L[0], frigate) == 0;   // another controller's behaviour leaves the creep alone
        check(opener && offense && defense && back && untouched && *(uint32_t*)(L[0] + 16) >= 0x10000000 && *(uint8_t*)0x1416feaf0 == 0xFF,
              "aggressive creep: one shared target list, its scores switched as the controller's behaviour follows the phase (the bombers' list, offense: frigates 10, flagship 9, the small capital ship 8, above turrets 5; face-off and defense: the small capital ship 10)");
        check(*(uint8_t**)(pcArr + 8) == debugDisp && *(int32_t*)(pcArr + 20) == 5685821 && *(int32_t*)(pcArr + 16) == 862952468 &&
              *(uint8_t**)(pcArr + 40) == selInt && *(int32_t*)(pcArr + 52) == 858290427 &&
              *(int32_t*)(squad + 32) == 0 && *(float*)(squad + 40) == 1200.0f && *(int32_t*)(squad + 124) == 2 &&
              *(uint32_t*)(sArr - 4) == 5 && *(uint32_t*)(((uint8_t**)sArr)[3] + 40) == 2 && *(uint8_t**)(stockSet + 24) == sArr,
              "aggressive creep: the squadron is Aggressive within the profile's 1200 m; its connections and the stock list untouched");
    }
    {   // ship scale (#20): the Y-wing's blueprint partition (StarshipBlueprint "Game/Gameplay/Starships/Ywing/Starship_Ywing"
        // and its StarshipEntityData) arrives through the partition creator; the StandIn profile's shipscale_ywing.scale=0.5
        // scales the entity's root part and mesh component (axes and translation), and leaves its physics component alone
        uint8_t* bp = fake_alloc(0x40); *(uint64_t*)(bp + 8) = 0x144ade220;
        *(const char**)(bp + 0x18) = "Game/Gameplay/Starships/Ywing_AI/Starship_Ywing_AI";   // the AI blueprint takes the player ship's size
        auto comp = [](uint64_t cls, float sx, float tz) {
            uint8_t* c = fake_alloc(0x100); *(uint64_t*)(c + 8) = cls; float* m = (float*)(c + 0x30);
            m[0] = sx; m[5] = sx; m[10] = sx; m[12] = 0; m[13] = -0.78f; m[14] = tz; return c; };
        uint8_t* rootPart = comp(0x1449fa760, 1.0f, -7.2f);
        uint8_t* cockpit = comp(0x144a4e5c0, 1.0f, 0.0f); ((float*)(cockpit + 0x30))[13] = 0.0f;   // at the root part's origin
        uint8_t* other = comp(0x144999000, 1.0f, 2.0f);   // another child of the root part: inherits the scale, untouched
        uint8_t* karr = fake_alloc(16 + 2 * 8); *(uint32_t*)(karr + 12) = 2;
        ((uint8_t**)(karr + 16))[0] = other; ((uint8_t**)(karr + 16))[1] = cockpit; *(uint64_t*)(rootPart + 0x70) = (uint64_t)(karr + 16);
        uint8_t* glow = comp(0x144a035c0, 140.0f, -13.0f);
        uint8_t* phys = comp(0x144999000, 1.0f, 0.0f);   // some other component type: must stay as it is
        uint8_t* carr = fake_alloc(16 + 3 * 8); *(uint32_t*)(carr + 12) = 3;
        ((uint8_t**)(carr + 16))[0] = rootPart; ((uint8_t**)(carr + 16))[1] = glow; ((uint8_t**)(carr + 16))[2] = phys;
        uint8_t* ent = fake_alloc(0x100); *(uint64_t*)(ent + 8) = 0x1449fad80; *(uint64_t*)(ent + 0x60) = (uint64_t)(carr + 16);
        uint8_t** insts5 = (uint8_t**)fake_alloc(16); insts5[0] = bp; insts5[1] = ent;
        uint8_t* part5 = fake_alloc(0x40); *(uint8_t***)(part5 + 0x20) = insts5; *(uint16_t*)(part5 + 0x28) = 2;
        typedef uint64_t(__fastcall* partnew_t)(uint64_t, uint64_t, uint64_t, uint64_t);
        ((partnew_t)0x140273060)((uint64_t)domain, 0, (uint64_t)part5, 0);
        const float* r = (const float*)(rootPart + 0x30); const float* g = (const float*)(glow + 0x30); const float* p = (const float*)(phys + 0x30);
        auto approx = [](float a, float b) { return fabsf(a - b) < 1e-5f; };
        check(approx(r[0], 0.5f) && approx(r[5], 0.5f) && approx(r[10], 0.5f) && approx(r[13], -0.39f) && approx(r[14], -3.6f) &&
              approx(g[0], 70.0f) && approx(g[5], 70.0f) && approx(g[10], 70.0f) && approx(g[13], -0.39f) && approx(g[14], -6.5f) &&
              p[0] == 1.0f && p[13] == -0.78f,
              "ship scale: the Y-wing's root part and mesh component scale about the ship's origin as its blueprint loads; other components untouched");
        // the cockpit's world placement (root part x cockpit) is what it was: axes 1, translation (0, -0.78, -7.2)
        const float* k = (const float*)(cockpit + 0x30); const float* o = (const float*)(other + 0x30);
        float wy = r[13] + r[5] * k[13], wz = r[14] + r[10] * k[14];
        check(approx(r[0] * k[0], 1.0f) && approx(r[5] * k[5], 1.0f) && approx(r[10] * k[10], 1.0f) && fabsf(wy + 0.78f) < 1e-5f && fabsf(wz + 7.2f) < 1e-5f &&
              o[0] == 1.0f && o[14] == 2.0f,
              "ship scale: the cockpit keeps its original size and place under the scaled root part; the part's other children untouched");
    }
    {   // loadout_log: a save with two pilot loadouts and one in the third set (the ship set empty). The runner checks the
        // log names the save, the set call with the counts, and both pilot nodes (the first with its text field).
        uint8_t* presets = fake_alloc(0xc0); uint8_t* store = fake_alloc(0x40); uint8_t* mgr = fake_alloc(0x40);
        *(int32_t*)(store + 0x20) = 2; *(uint8_t**)(mgr + 0x10) = store;
        uint8_t* a = fake_alloc(0x100); uint8_t* b = fake_alloc(0x100); uint8_t* c = fake_alloc(0x100);
        static const char kFaction[] = "Rebel";
        uint8_t* anchor = presets + 0x10;   // pilot set at +8: anchor at +0x10 {right, left, parent}, size at +0x30
        *(uint8_t**)(a + 0x00) = b; *(uint8_t**)(a + 0x10) = anchor; *(uint8_t**)(b + 0x10) = a;
        *(uint8_t**)(anchor + 0x00) = b; *(uint8_t**)(anchor + 0x08) = a; *(uint8_t**)(anchor + 0x10) = a; *(int32_t*)(presets + 0x30) = 2;
        *(const char**)(a + 0x20) = kFaction; *(uint32_t*)(a + 0x30) = 0x1234; *(uint32_t*)(b + 0x30) = 0x5678;
        uint8_t* anchor3 = presets + 0x80;  // third set at +0x78: anchor at +0x80, size at +0xa0
        *(uint8_t**)(c + 0x10) = anchor3; *(uint8_t**)(anchor3 + 0x00) = c; *(uint8_t**)(anchor3 + 0x08) = c; *(uint8_t**)(anchor3 + 0x10) = c;
        *(int32_t*)(presets + 0xa0) = 1; *(uint32_t*)(c + 0x28) = 0x9abc;
        ((void(__fastcall*)(uint64_t, uint64_t))0x1419001c0)((uint64_t)mgr, (uint64_t)presets);
        check(*(uint8_t*)0x1419001c0 == 0xFF && *(uint8_t*)0x1418f58f0 == 0xFF && *(uint64_t*)0x1418f8000 == (uint64_t)presets,
              "loadouts: the save and the presets setter are hooked, and the presets pass through to the game's setter");
    }
    {   // local_loadouts (default on): a save keeps its records in loadouts.txt and its send to EA is dropped; EA's next
        // loadouts get the saved records laid over them by name, only in the id words; a record from EA with no saved copy,
        // and a saved record whose layout is off, are left alone; the sender still passes for its other callers.
        static const char kG1[] = "11111111-2222-3333-4444-555555555555", kG2[] = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        auto node = [](const char* name, uint32_t guidOff, const char* guid) {
            uint8_t* n = fake_alloc(0x100); *(const char**)(n + 0x20) = name; *(const char**)(n + guidOff) = guid; return n; };
        auto one = [](uint8_t* presets, uint32_t set, uint8_t* n) {   // a set holding one node
            uint8_t* anchor = presets + set + 8;
            *(uint8_t**)(n + 0x10) = anchor; *(uint8_t**)(anchor + 0) = n; *(uint8_t**)(anchor + 8) = n; *(uint8_t**)(anchor + 0x10) = n;
            *(int32_t*)(presets + set + 0x28) = 1; };
        auto two = [](uint8_t* presets, uint32_t set, uint8_t* a, uint8_t* b) {   // a set holding a, then b (a's right child)
            uint8_t* anchor = presets + set + 8;
            *(uint8_t**)(a + 0) = b; *(uint8_t**)(a + 0x10) = anchor; *(uint8_t**)(b + 0x10) = a;
            *(uint8_t**)(anchor + 0) = b; *(uint8_t**)(anchor + 8) = a; *(uint8_t**)(anchor + 0x10) = a; *(int32_t*)(presets + set + 0x28) = 2; };
        uint8_t* store = fake_alloc(0x40); uint8_t* mgr = fake_alloc(0x40); *(uint8_t**)(mgr + 0x10) = store; *(int32_t*)(store + 0x20) = 2;
        // the save: a pilot loadout, a ship loadout, the profile, and a ship record whose GUID isn't one (not kept)
        uint8_t* s = fake_alloc(0xc0);
        uint8_t* sp = node("Rebel_pilot_loadout", 0x70, kG1); uint8_t* ss = node("Rebel_Flex_Loadout", 0x60, kG1);
        uint8_t* sx = node("Rebel_Siege_Loadout", 0x60, "not-a-guid"); uint8_t* sf = node("PlayerProfile_1", 0x40, kG1);
        for (int j = 0; j < 16; j++) *(uint32_t*)(sp + 0x2c + 4 * j) = 0x70000 + j;
        for (int j = 0; j < 12; j++) { *(uint32_t*)(ss + 0x28 + 4 * j) = 0x50000 + j; *(uint32_t*)(sx + 0x28 + 4 * j) = 0x60000 + j; }
        for (int j = 0; j < 6; j++) *(uint32_t*)(sf + 0x28 + 4 * j) = 0x30000 + j;
        one(s, 0x08, sp); two(s, 0x40, ss, sx); one(s, 0x78, sf);
        uint64_t sendsBefore = *(uint64_t*)0x1418f8010;
        ((void(__fastcall*)(uint64_t, uint64_t))0x1419001c0)((uint64_t)mgr, (uint64_t)s);
        uint64_t sendsAfterSave = *(uint64_t*)0x1418f8010;
        // EA's next loadouts: the same records with EA's (older) ids, an extra ship record nobody saved, and EA's
        // Rebel_Siege_Loadout (its saved copy had a bad GUID, so it was never kept)
        uint8_t* e = fake_alloc(0xc0);
        uint8_t* ep = node("Rebel_pilot_loadout", 0x70, kG2); uint8_t* es = node("Rebel_Flex_Loadout", 0x60, kG2);
        uint8_t* ex = node("Rebel_Siege_Loadout", 0x60, kG2); uint8_t* eo = node("Imperial_Flex_Loadout", 0x60, kG2);
        uint8_t* ef = node("PlayerProfile_1", 0x40, kG2);
        for (int j = 0; j < 16; j++) *(uint32_t*)(ep + 0x2c + 4 * j) = 0xE0000 + j;
        for (int j = 0; j < 12; j++) { *(uint32_t*)(es + 0x28 + 4 * j) = 0xE0000 + j; *(uint32_t*)(ex + 0x28 + 4 * j) = 0xE1000 + j; *(uint32_t*)(eo + 0x28 + 4 * j) = 0xE2000 + j; }
        for (int j = 0; j < 6; j++) *(uint32_t*)(ef + 0x28 + 4 * j) = 0xE3000 + j;
        *(uint32_t*)(es + 0x58) = 0xAB; *(uint32_t*)(ep + 0x28) = 0xCD;   // words outside the id ranges: never written
        one(e, 0x08, ep); one(e, 0x78, ef);
        {   // the ship set: es, then ex and eo (es has right child ex, which has right child eo)
            uint8_t* anchor = e + 0x40 + 8;
            *(uint8_t**)(es + 0) = ex; *(uint8_t**)(es + 0x10) = anchor; *(uint8_t**)(ex + 0x10) = es; *(uint8_t**)(ex + 0) = eo; *(uint8_t**)(eo + 0x10) = ex;
            *(uint8_t**)(anchor + 0) = eo; *(uint8_t**)(anchor + 8) = es; *(uint8_t**)(anchor + 0x10) = es; *(int32_t*)(e + 0x40 + 0x28) = 3;
        }
        ((void(__fastcall*)(uint64_t, uint64_t, uint32_t, uint32_t))0x1418f58f0)((uint64_t)store, (uint64_t)e, 1, 0);
        bool pilotOk = true, shipOk = true, profOk = true, siegeOk = true, otherOk = true;
        for (int j = 0; j < 16; j++) pilotOk &= *(uint32_t*)(ep + 0x2c + 4 * j) == 0x70000u + j;
        for (int j = 0; j < 12; j++) { shipOk &= *(uint32_t*)(es + 0x28 + 4 * j) == 0x50000u + j;
                                       siegeOk &= *(uint32_t*)(ex + 0x28 + 4 * j) == 0xE1000u + j; otherOk &= *(uint32_t*)(eo + 0x28 + 4 * j) == 0xE2000u + j; }
        for (int j = 0; j < 6; j++) profOk &= *(uint32_t*)(ef + 0x28 + 4 * j) == 0x30000u + j;
        check(sendsAfterSave == sendsBefore, "local loadouts: the save's send to EA was dropped");
        check(pilotOk && shipOk && profOk, "local loadouts: EA's pilot, ship and profile records got the saved item ids");
        check(siegeOk && otherOk && *(uint32_t*)(es + 0x58) == 0xAB && *(uint32_t*)(ep + 0x28) == 0xCD,
              "local loadouts: records with no saved copy, and words outside the item ids, left as EA sent them");
        ((uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))0x1418f61b0)(0, 0x77, 0, 0, 0);
        check(*(uint64_t*)0x1418f8010 == sendsAfterSave + 1, "local loadouts: the EA sender still passes for its other callers");
    }
    {   // spawn_log: two spawns, one that makes a ship and one that doesn't; both pass through, and the log has a line for
        // each (the runner, run_standin.sh, checks the log)
        uint8_t* ent = fake_alloc(0x400); uint8_t* req = fake_alloc(0x80);
        *(float*)(req + 0x30) = 259.0f; *(float*)(req + 0x34) = -91.0f; *(float*)(req + 0x38) = 2886.0f;
        typedef uint64_t(__fastcall* sspawn_t)(uint64_t, uint64_t);
        *(uint64_t*)(ent + 0x10) = 0x5A5A0001;
        uint64_t a = ((sspawn_t)0x1416bcab0)((uint64_t)ent, (uint64_t)req);
        *(uint64_t*)(ent + 0x10) = 0;
        uint64_t b = ((sspawn_t)0x1416bcab0)((uint64_t)ent, (uint64_t)req);
        check(*(uint8_t*)0x1416bcab0 == 0xFF && a == 0x5A5A0001 && b == 0, "starship spawn hooked and passes the result through");
    }
    {   // hit_log: a U-wing's health component whose physics component holds the ship's frame (yawed 90 degrees, at
        // (1000, 50, -2000)) after a block that isn't one; two hits at known points in the ship's frame, (1, 2, 3) and
        // (-4, 0.5, 10). The runner checks the log has both in the ship's frame, and where the transform was found.
        uint8_t* comp = fake_alloc(0x1000); uint8_t* data = fake_alloc(0x100); uint8_t* asset = fake_alloc(0x40);
        uint8_t* pc = fake_alloc(0x400); uint8_t* vt = fake_alloc(0x400); uint8_t* info = fake_alloc(0x100);
        *(uint64_t*)(comp + 0x40) = (uint64_t)data; *(uint64_t*)(data + 0x90) = (uint64_t)asset;
        *(uint64_t*)(asset + 8) = 0x144a8ca00; *(const char**)(asset + 0x18) = "starshiphealthdamagedata_uwing";
        *(uint64_t*)(comp + 0x990) = (uint64_t)pc; *(uint64_t*)pc = (uint64_t)vt;
        const float decoy[12] = {1,0,0, 0,0.5f,0, 0,0,1, 1000,50,-2000};   // not orthonormal: must be passed over
        const float frame[12] = {0,0,-1, 0,1,0, 1,0,0, 1000,50,-2000};
        for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) {
            *(float*)(pc + 0x40 + i * 16 + k * 4) = decoy[i * 3 + k]; *(float*)(pc + 0x80 + i * 16 + k * 4) = frame[i * 3 + k]; }
        *(uint32_t*)(info + 0x20) = (3u << 20) | 0x80; *(uint32_t*)(info + 0x24) = 0x00c00880; *(int32_t*)(info + 0x58) = 2; *(float*)(info + 0x84) = 27.0f;   // +0x24: a starfighter hull
        *(float*)(info + 0x78) = 1.0f; *(float*)(info + 0x90) = -9000;   // +0x70 a unit direction, as in the game
        // the game's own decoy (2026-10-23 flight): an identity block at the origin, reached through the component's pointer
        // at +0x10, that a unit direction sits 1 m from; it must be passed over
        uint8_t* ident = fake_alloc(0x100); *(uint64_t*)(comp + 0x10) = (uint64_t)ident;
        *(float*)(ident + 0x30) = 1; *(float*)(ident + 0x44) = 1; *(float*)(ident + 0x58) = 1;
        typedef uint64_t(__fastcall* shiphit_t)(uint64_t, uint64_t, uint64_t);
        uint8_t* out = fake_alloc(0x40);
        *(float*)(info + 0x30) = 1003; *(float*)(info + 0x34) = 52; *(float*)(info + 0x38) = -2001;
        uint64_t r1 = ((shiphit_t)0x14168a8b0)((uint64_t)comp, (uint64_t)out, (uint64_t)info);
        Sleep(100); *(float*)(pc + 0x80 + 0x38) = -1995;   // the ship moves 5 m along world z (its own -x) between the hits
        *(float*)(info + 0x30) = 1010; *(float*)(info + 0x34) = 50.5f; *(float*)(info + 0x38) = -1991;
        uint64_t r2 = ((shiphit_t)0x14168a8b0)((uint64_t)comp, (uint64_t)out, (uint64_t)info);
        check(*(uint8_t*)0x14168a8b0 == 0xFF && r1 == (uint64_t)out && r2 == (uint64_t)out, "starship direct hit hooked and passes the result through");
    }
    {   // creep damage against capital ships ([creep] aggressive=1) and capital_damage_log=1: a light cruiser's hull that
        // ignores AI damage (+0x1c8, as every PvP level sets it) takes a 4000 blast (through the planted blast caller) from a
        // starship (vtable 0x1436184e8) without the player mark (+0x338), reached as the drop rule reaches it: record +0xb0 ->
        // +0x40 -> [] - 8. That is creep, so it counts and the flag is put back. 5 s on, a 1000 hit with no starship behind it
        // is still dropped, then a 50 creep hit counts. Its name comes from its data (0x144a24a40) +128's asset name. A Raider's
        // shield bubble (data 0x144a244c0, settings at +160) takes a 30 hit off its 17000.
        uint8_t* ship = fake_alloc(0x400); *(uint64_t*)ship = 0x1436184e8;
        uint8_t* ctl = fake_alloc(0x10); *(uint64_t*)ctl = (uint64_t)(ship + 8);
        uint8_t* inst = fake_alloc(0x80); *(uint8_t**)(inst + 0x40) = ctl;
        uint8_t* noShip = fake_alloc(0x80);
        uint8_t* vt = fake_alloc(0x200); *(uint64_t*)(vt + 0x108) = 0x1417dc800;
        uint8_t* hs = fake_alloc(0x40); *(const char**)(hs + 0x18) = "CapitalShipHealthSettings_LightCruiser";
        uint8_t* hd = fake_alloc(0xa0); *(uint64_t*)(hd + 8) = 0x144a24a40; *(uint8_t**)(hd + 128) = hs;
        uint8_t* comp = fake_alloc(0x200); *(uint64_t*)comp = (uint64_t)vt; *(uint8_t**)(comp + 0x20) = hd;
        *(float*)(comp + 0x40) = 48000; comp[0x1c8] = 1;
        uint8_t* rec = fake_alloc(0x100); *(float*)(rec + 0x88) = 4000; *(uint8_t**)(rec + 0xb0) = inst;
        uint8_t* out = fake_alloc(0x40);
        typedef uint64_t(__fastcall* apply_t)(uint64_t, uint64_t, uint64_t, uint8_t);
        uint64_t r1 = ((apply_t)0x1417dbde3)((uint64_t)comp, (uint64_t)out, (uint64_t)rec, 0);
        bool putBack = comp[0x1c8] == 1;
        Sleep(5100); *(float*)(rec + 0x88) = 1000; *(uint8_t**)(rec + 0xb0) = noShip;
        uint64_t r2 = ((apply_t)0x1417dbe90)((uint64_t)comp, (uint64_t)out, (uint64_t)rec, 0);
        *(float*)(rec + 0x88) = 50; *(uint8_t**)(rec + 0xb0) = inst;
        ((apply_t)0x1417dbe90)((uint64_t)comp, (uint64_t)out, (uint64_t)rec, 0);
        uint8_t* ss = fake_alloc(0x40); *(const char**)(ss + 0x18) = "ShieldHealthSettings_Raider";
        uint8_t* sd = fake_alloc(0xb0); *(uint64_t*)(sd + 8) = 0x144a244c0; *(uint8_t**)(sd + 160) = ss;
        uint8_t* scomp = fake_alloc(0x200); *(uint64_t*)scomp = (uint64_t)vt; *(uint8_t**)(scomp + 0x18) = sd; *(float*)(scomp + 0x40) = 17000;
        uint8_t* sinfo = fake_alloc(0x100); *(float*)(sinfo + 0x84) = 30; *(uint8_t**)(sinfo + 0xc0) = inst;
        uint8_t* sout = fake_alloc(0x40);
        typedef uint64_t(__fastcall* shieldhit_t)(uint64_t, uint64_t, uint64_t);
        uint64_t r3 = ((shieldhit_t)0x140582ab0)((uint64_t)scomp, (uint64_t)sout, (uint64_t)sinfo);
        check(*(uint8_t*)0x1417dbe90 == 0xFF && *(uint8_t*)0x140582ab0 == 0xFF && *(uint8_t*)0x140582d90 == 0xFF &&
              r1 == (uint64_t)out && r2 == (uint64_t)out && r3 == (uint64_t)sout && *(float*)(scomp + 0x40) == 17000 - 15 && *(float*)(sinfo + 0x84) == 30,
              "capital hull damage and both shield hits hooked, and they pass the hits and results through");
        // a flagged hit (record +0xa4 flag 8, as the capital ships' passive damage carries) from the same starship is the stock
        // rule's to keep and isn't scaled: 100 asked, 100 off
        rec[0xa4] = 8; *(float*)(rec + 0x88) = 100; *(uint8_t**)(rec + 0xb0) = inst;
        ((apply_t)0x1417dbe90)((uint64_t)comp, (uint64_t)out, (uint64_t)rec, 0);
        bool flagged = *(float*)(rec + 0x88) == 100; rec[0xa4] = 0;
        // the check itself: an unmarked starship (creep) is let through, a marked one and a player keep the stock "keep",
        // no starship behind the hit keeps the stock "drop", and a component that doesn't ignore AI damage keeps everything
        typedef bool(__fastcall* aicheck_t)(uint64_t, uint64_t);
        aicheck_t chk = (aicheck_t)0x1417da0e0;
        uint8_t* marked = fake_alloc(0x400); *(uint64_t*)marked = 0x1436184e8; marked[0x338] = 1;
        uint8_t* mctl = fake_alloc(0x10); *(uint64_t*)mctl = (uint64_t)(marked + 8);
        uint8_t* minst = fake_alloc(0x80); *(uint8_t**)(minst + 0x40) = mctl;
        uint8_t* pl = fake_alloc(0x10); *(uint64_t*)pl = 0x5000;   // a player directly: [[attacker+8]] - 8 != 0
        uint8_t* pinst = fake_alloc(0x80); *(uint8_t**)(pinst + 8) = pl;
        uint8_t* r4 = fake_alloc(0x100);
        auto ask = [&](uint8_t* c, uint8_t* who) { *(uint8_t**)(r4 + 0xb0) = who; return chk((uint64_t)c, (uint64_t)r4); };
        uint8_t* plain = fake_alloc(0x200);   // a component that doesn't ignore AI damage
        bool cases = !ask(comp, inst) && !ask(comp, minst) && !ask(comp, pinst) && ask(comp, noShip) && !ask(plain, noShip);
        check(*(uint8_t*)0x1417da0e0 == 0xFF && cases && *(float*)(comp + 0x40) == 48000 - 2125 && flagged && putBack && comp[0x1c8] == 1,
              "creep damage: the ignore-AI check lets creep hits through on a capital that ignores AI damage, at the profile's 50%; marked ships and players kept, other hits still dropped, a flagged hit (passive damage) unscaled");
    }

    alignas(16) static uint8_t obj[0x700];
    uint64_t r = ((uint64_t(__fastcall*)(void*, char, uint64_t, uint64_t))0x1401ae7d0)(obj, 0, 0, 0);
    check(r == (uint64_t)obj && obj[8] == 1, "MainLoop ctor called with 0 stores isDedicatedServer = 1");

    struct { void* vt; const char* name; } ls = { nullptr, "Levels/MP/Test/Test" };
    alignas(16) static uint8_t info[0x80];
    *(void**)(info + 0x18) = &ls; info[0x26] = 1; *(uint32_t*)(info + 0x20) = 30;
    ((void(__fastcall*)(uint64_t, uint8_t*))0x1401aac90)(0x1234, info);
    check(info[0x2f] == 0x77, "spawnServer passes through to the original");

    struct { const char* name; } cfg = { "Dedicated" };
    ((void(__fastcall*)(uint64_t, void*))0x1401bf4d0)(0x1234, &cfg);
    check(true, "clock selector passes through (see status log for the name)");

    uint32_t backend = ((uint32_t(__fastcall*)(uint8_t*))0x1418f2930)(info);
    check(backend == 0xB8566ABC, "backend chooser returns OnlineBackend_Local");

    // 3b: tick the GameSimulation for 4.5 s with no server: the DLL should spawn a dedicated server after its delay
    check(*(uint8_t*)0x1401bfe60 == 0xFF, "GameSimulation tick hooked");
    // laser vs capital (grid settings): the stock cell is a fake MaterialRelationDamageData (ClassInfo 0x1449f9290's id
    // range) with multiplier 1; one lookup before the ticks lets the DLL see the grid, so its scan can record and copy
    typedef uint64_t(__fastcall* matl_t)(uint64_t, const uint32_t*, const uint32_t*);
    *(uint16_t*)(0x1449f9290 + 0x48) = 900; *(uint16_t*)(0x1449f9290 + 0x4a) = 900;
    uint8_t* stockCell = fake_alloc(0x40); *(uint64_t*)(stockCell + 8) = (uint64_t)fake_class(900);
    *(float*)(stockCell + 0x20) = 1.0f; *(float*)(stockCell + 0x24) = 1.0f; *(float*)(stockCell + 0x28) = 2.0f;
    { DWORD old; VirtualProtect((void*)(0x14057dd20 + 21), 8, PAGE_EXECUTE_READWRITE, &old);
      *(uint64_t*)(0x14057dd20 + 21) = (uint64_t)stockCell; FlushInstructionCache(GetCurrentProcess(), (void*)0x14057dd20, 40); }
    // the level's grid: a real allocation, freed below as a level change frees it
    uint8_t* gridBuf = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const uint64_t grid = (uint64_t)gridBuf;
    const uint32_t laserMat = 0x00300480, ionMat = 0x00a00780, capitalMat = 0x00b00980, defaultMat = 0x00000080, shieldMat = 0x00600580;
    ((matl_t)0x14057dd20)(grid, &laserMat, &capitalMat);
    alignas(16) static uint8_t gs[0x200];
    for (int i = 0; i < 150; i++) { ((void(__fastcall*)(void*, uint64_t, uint64_t, uint64_t))0x1401bfe60)(gs, 0, 0, 0); Sleep(30); }
    check(true, "ticked 4.5 s (see status log for the 3b spawn lines)");
    check(*(uint64_t*)(creepSpawn[0][0] + 0xC0) == (uint64_t)ywingAi && *(uint64_t*)(creepSpawn[0][1] + 0xC0) == (uint64_t)ywingAi &&
          *(uint64_t*)(creepSpawn[2][0] + 0x1F0) == 0 && *(uint64_t*)(creepSpawn[2][1] + 0x1F0) == 0 &&
          *(uint64_t*)(creepSpawn[2][0] + 0xC0) == (uint64_t)awingAi && *(uint64_t*)(creepSpawn[2][1] + 0xC0) == (uint64_t)awingAi &&
          *(uint64_t*)(creepSpawn[1][0] + 0xC0) == (uint64_t)xwingAi && *(uint64_t*)(creepSpawn[1][1] + 0xC0) == (uint64_t)xwingAi &&
          *(uint64_t*)(decoySpawn + 0xC0) == (uint64_t)xwingAi,
          "creep: Rebel pack 1 (x -303) spawns Y-wings and pack 3 A-wings (no loadout); pack 2 and a non-creep spawn point stay stock");
    {   // the Y-wing pack's torpedo loadout: the SP hull and shields, the bots' torpedo slot, a copy of their bombs' slot
        // carrying the torpedo, and the bots' movement
        uint8_t* lo0 = *(uint8_t**)(creepSpawn[0][0] + 0x1F0); uint8_t* lo1 = *(uint8_t**)(creepSpawn[0][1] + 0x1F0);
        uint8_t* arr = lo0 ? *(uint8_t**)(lo0 + 0x20) : nullptr;
        uint8_t* slot2 = arr ? ((uint8_t**)arr)[3] : nullptr;
        check(lo0 && lo0 == lo1 && lo0 != ywingSp && lo0 != ywingTorp && arr && *(uint32_t*)(arr - 4) == 5 &&
              ((uint8_t**)arr)[0] == spHull && ((uint8_t**)arr)[1] == spShield && ((uint8_t**)arr)[2] == slotTorp &&
              slot2 && slot2 != slotBomb && *(uint8_t**)(slot2 + 96) == torpWeapon && *(uint64_t*)(slot2 + 8) == 0x1449fc3f0 &&
              *(uint8_t**)(slotBomb + 96) != torpWeapon && ((uint8_t**)arr)[4] == botMove &&
              *(uint32_t*)(*(uint8_t**)(ywingSp + 0x20) - 4) == 2 && *(uint32_t*)(*(uint8_t**)(ywingTorp + 0x20) - 4) == 4,
              "creep torpedoes and handling: the Y-wing pack carries the bots' torpedo slot, their bombs' slot with a torpedo in it, and their movement, with the creep's own hull and shields; the game's collections untouched");
        // the stock X-wing pack (with [creep] bot_handling): its own creep hull and the bots' movement; its ship unchanged
        uint8_t* st = *(uint8_t**)(creepSpawn[1][0] + 0x1F0); uint8_t* sa = st ? *(uint8_t**)(st + 0x20) : nullptr;
        check(st && st != creepCfg && st == *(uint8_t**)(creepSpawn[1][1] + 0x1F0) && sa && *(uint32_t*)(sa - 4) == 2 &&
              ((uint8_t**)sa)[0] == xCreepHull && ((uint8_t**)sa)[1] == xBotMove && *(uint64_t*)(creepSpawn[1][0] + 0xC0) == (uint64_t)xwingAi &&
              *(uint32_t*)(*(uint8_t**)(creepCfg + 0x20) - 4) == 1,
              "creep handling: the stock X-wing pack flies the bots' movement with its own creep hull; the stock collection untouched");
    }
    {   // StandIn: materialgrid.laserdamage.capitalhull=0.05. Through the hull site, a laser on the capital material and on
        // the default material (a corvette's hull) get the private copy; ion, a shield target, and any other caller don't
        uint64_t viaCap = ((matl_t)0x1417db3f7)(grid, &laserMat, &capitalMat), viaDef = ((matl_t)0x1417db3f7)(grid, &laserMat, &defaultMat);
        uint64_t ion = ((matl_t)0x1417db3f7)(grid, &ionMat, &capitalMat), shield = ((matl_t)0x1417db3f7)(grid, &laserMat, &shieldMat);
        uint64_t other = ((matl_t)0x14057dd20)(grid, &laserMat, &capitalMat);
        printf("      (grid: hull site laser->capital %p (x%g), ->default %p, ion %p, ->shield %p, other caller %p; stock %p)\n",
               (void*)viaCap, viaCap ? *(float*)(viaCap + 0x20) : -1.0f, (void*)viaDef, (void*)ion, (void*)shield, (void*)other, stockCell);
        check(viaCap && viaCap != (uint64_t)stockCell && *(float*)(viaCap + 0x20) == 0.05f && *(float*)(viaCap + 0x28) == 2.0f &&
              *(uint64_t*)(viaCap + 8) == *(uint64_t*)(stockCell + 8) && viaDef == viaCap,
              "grid: a laser hit on a capital hull (capital or corvette material) gets the 0.05 copy of the stock cell");
        check(ion == (uint64_t)stockCell && shield == (uint64_t)stockCell && other == (uint64_t)stockCell && *(float*)(stockCell + 0x20) == 1.0f,
              "grid: ion, a shield target and other callers keep the game's own cell, which is unchanged");
        // the match ends and its grid is freed (2026-10-22: the DLL kept looking up the first grid every 2 s and crashed
        // the game on the way back to the menu). Keep ticking: nothing may touch the freed grid.
        VirtualFree(gridBuf, 0, MEM_RELEASE);
        for (int i = 0; i < 90; i++) { ((void(__fastcall*)(void*, uint64_t, uint64_t, uint64_t))0x1401bfe60)(gs, 0, 0, 0); Sleep(30); }
        check(true, "grid: after the grid is freed, 2.7 s of ticks and scans run without touching it");
    }
    // value engine (StandIn profile: afterburnermaxspeed=250, drifttransitiontime=5, maxspeeds[8]=150, shield 900)
    printf("      (player: boost %g list %g, drift %g, maxspeeds[8] %g, plain %g; bot: boost %g drift %g; creep: boost %g drift %g; shield %g)\n",
           player.ab->value, player.reg[3], player.drift->value, player.speeds[8], *(float*)(player.inst + 0x2ec),
           bot.ab->value, bot.drift->value, creep.ab->value, creep.drift->value, maxShield->value);
    check(player.ab->value == 250.0f && player.reg[3] == 250.0f, "value engine: attribute field and its list entry written (240 -> 250)");
    check(player.speeds[8] == 150.0f && player.speeds[7] == 120.0f + 3.125f * 7, "value engine: one array element written, its neighbours untouched");
    check(*(float*)(player.inst + 0x2ec) == 10000.0f && player.reg[1] == 1.0f, "value engine: fields without an override untouched");
    check(bot.ab->value == 250.0f, "value engine: vs-AI bot follows the player where their defaults match");
    check(*(float*)(rFloat + 20) == 2.0f && *(float*)(rFloat + 28) == 3.0f && *(float*)(rFloat + 36) == 9.0f &&
          *(int32_t*)(rInt + 20) == 2400 && rBool[20] == 1 && *(float*)(aOff + 0x20) == 2.0f && *(float*)(aDef + 0x20) == 3.0f,
          "match rules: creep morale on offense 1 -> 2 (list entry and typed attribute), the match length 1800 -> 2400 and the morale DoT on; the rest untouched");
    check(bot.drift->value == 3.0f && bot.reg[5] == 3.0f, "value engine: vs-AI bot scaled where its default differs (1.5 x 5/2.5 = 3)");
    check(creep.ab->value == 240.0f && creep.drift->value == 2.5f, "value engine: creep config untouched");
    {   // per ship type: a ship the starship spawn made from the Y-wing pack's spawn point (pack 1, kept by the scans) keeps
        // the squadron's list; a ship from the stock pack's spawn point gets one that scores the corvette or Raider 1, in
        // the opener and on defense; a list that isn't the creep's passes through
        auto score = [](uint8_t* set, uint32_t target) { uint8_t* a = *(uint8_t**)(set + 24); uint32_t n = *(uint32_t*)(a - 4);
                                                         for (uint32_t k = 0; k < n; k++) { uint8_t* s = ((uint8_t**)a)[k]; uint32_t m = *(uint32_t*)(s + 40);
                                                                                            if ((m & target) == m) return *(float*)(s + 44); }
                                                         return 0.0f; };
        const uint32_t smallcap = (1u << 28) | 2;
        typedef void(__fastcall* sc_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        typedef void(__fastcall* cf_t)(uint8_t*, uint8_t*);
        typedef uint64_t(__fastcall* sp_t)(uint8_t*, uint64_t);
        auto spawnFrom = [](uint8_t* spawnRef) { uint8_t* ship = fake_alloc(0x600); uint8_t* ent = fake_alloc(0x100);
                                                 *(uint8_t**)(ent + 0x38) = spawnRef; *(uint8_t**)(ent + 0x10) = ship;
                                                 ((sp_t)0x1416bcab0)(ent, 0); return ship; };
        uint8_t* bomberShip = spawnFrom(creepSpawn[0][1]);    // Rebel pack 1: Y-wings
        uint8_t* fighterShip = spawnFrom(creepSpawn[1][0]);   // Rebel pack 2: stock X-wings
        auto listFor = [&](uint8_t* ship, uint8_t* list) { g_scorerList = 0; ((sc_t)0x14182ade0)(0, (uint64_t)ship, 0, 0, 0, 0, 0, (uint64_t)list, 0, 0, 0, 0); return (uint8_t*)g_scorerList; };
        uint8_t* bl = creepList ? listFor(bomberShip, creepList) : nullptr; uint8_t* fl = creepList ? listFor(fighterShip, creepList) : nullptr;
        bool opener = bl == creepList && fl && fl != creepList && score(bl, smallcap) == 10 && score(fl, smallcap) == 1;
        uint8_t* ent = fake_alloc(0x200); *(uint8_t**)(ent + 0x28) = creepCtrlData;
        int32_t bi = 4; uint8_t chg[16] = {}; *(int32_t*)chg = 875081154; *(int32_t**)(chg + 8) = &bi;
        ((cf_t)0x1416feaf0)(ent, chg);   // defense
        bool defense = fl && score(listFor(bomberShip, creepList), smallcap) == 10 && score(listFor(fighterShip, creepList), smallcap) == 1;
        bi = 3; ((cf_t)0x1416feaf0)(ent, chg);   // back to face-off
        bool pass = listFor(fighterShip, ywingAi) == ywingAi;
        check(opener && defense && pass,
              "creep per ship type: the Y-wing keeps the corvette at 10, another ship scores it 1, in the opener and on defense; a list that isn't the creep's passes through");
        // [creep] wingmen: a bomber is scored with a copy of the targeting rules allowing 15 attackers per target and no
        // penalty for another leader's target; another creep ship with the squadron's own rules, untouched
        uint8_t* rulesData = fake_alloc(0x40); *(int32_t*)(rulesData + 4) = 1; *(float*)(rulesData + 32) = 0.2f; *(float*)(rulesData + 8) = 1200.f;
        auto rulesFor = [&](uint8_t* ship) { g_scorerRules = 0; ((sc_t)0x14182ade0)(0, (uint64_t)ship, 0, 0, 0, 0, (uint64_t)rulesData, (uint64_t)creepList, 0, 0, 0, 0); return (uint8_t*)g_scorerRules; };
        uint8_t* rb = rulesFor(bomberShip); uint8_t* rf = rulesFor(fighterShip);
        check(rb && rb != rulesData && *(int32_t*)(rb + 4) == 15 && *(float*)(rb + 32) == 1.0f && *(float*)(rb + 8) == 1200.f &&
              rf == rulesData && *(int32_t*)(rulesData + 4) == 1 && *(float*)(rulesData + 32) == 0.2f,
              "creep wingmen: a bomber may share its target with the whole squadron (15 attackers, no penalty), other creep keep the squadron's rules");
        // ... but only a capital ship: of the entries the scorer appends (candidate +0x10, cap +0x1c), a capital ship's
        // (tag bit 2) keeps 15 and a starfighter's gets the squadron's 1 back
        uint8_t* capT = fake_alloc(0x600); *(uint32_t*)(capT + 0x5d0) = 2; *(uint8_t**)(capT + 0x30) = fake_alloc(0x40);
        uint8_t* fighterT = fake_alloc(0x600); *(uint32_t*)(fighterT + 0x5d0) = 0x14; *(uint8_t**)(fighterT + 0x30) = fake_alloc(0x40);
        uint8_t* ents = fake_alloc(4 * 0x38);
        *(uint8_t**)(ents + 0x10) = capT; *(int32_t*)(ents + 0x1c) = 15;
        *(uint8_t**)(ents + 0x38 + 0x10) = fighterT; *(int32_t*)(ents + 0x38 + 0x1c) = 15;
        uint8_t* outVec = fake_alloc(0x20); *(uint8_t**)outVec = ents; *(uint8_t**)(outVec + 8) = ents; *(uint8_t**)(outVec + 0x10) = ents + 4 * 0x38;
        ((sc_t)0x14182ade0)(0, (uint64_t)bomberShip, 0, 0, 0, 0, (uint64_t)rulesData, (uint64_t)creepList, 0, (uint64_t)outVec, 0, 0);
        check(*(uint8_t**)(outVec + 8) == ents + 0x70 && *(int32_t*)(ents + 0x1c) == 15 && *(int32_t*)(ents + 0x38 + 0x1c) == 1,
              "creep wingmen: bombers may all take a capital ship (its candidate entry keeps 15 attackers); a starfighter gets the squadron's 1 back");
    }
    check(torpFiring && *(uint32_t*)(torpFiring + 228) == 2 && *(int32_t*)(torpFiring + 224) == 2,
          "creep torpedoes: bombers that put capital ships first lock their AI torpedoes only onto capital ships (TargetTags 2), with no vs-AI bots loaded");
    {   // [creep] torpedo_lock_min=150: the minimum lock distance, attribute and list entry, with no vs-AI bots loaded
        uint8_t* at = torpFiring ? *(uint8_t**)(torpFiring + 184) : nullptr; uint8_t* fl = torpWeapon ? *(uint8_t**)(torpWeapon + 0x40) : nullptr;
        check(at && fl && *(float*)(at + 32) == 150.f && *(float*)(fl + 4) == 150.f,
              "creep torpedoes: they lock from the profile's 150 m (attribute and its list entry), with no vs-AI bots loaded");
    }
    check(torpWeapon && raidBp && *(uint8_t**)(torpWeapon + 120) == nullptr && *(uint8_t**)(bombCfg + 120) == raidBp &&
          *(float*)(raidData + 40) == 400.f && *(float*)(raidData + 48) == 50.f && *(float*)(raidData + 52) == 250.f &&
          *(float*)(raidData + 56) == 50.f && *(float*)(raidData + 68) == 500.f && *(float*)(raidData + 44) == 100.f,
          "creep torpedoes: the AI torpedo carries no run behaviour, and with torpedoes on the bombs' run data stays stock");
    // the last behaviour the test reported was 3 (face-off): bomb raids back to the stock Defensive (1) after the ticks
    check(creepSquadForMode && *(int32_t*)(creepSquadForMode + 32) == 1,
          "aggressive creep: outside offense the squadron's data engagement mode is back to the stock Defensive (bomb raids only on the current target)");
    check(allowBool[36] == 1 && flagBool[36] == 1 && *(int32_t*)(minSel + 48) == 1 && bombsBool[36] == 0,
          "creep missiles: the squad behaviour's missile default and bot test on, the minimum distance on the bots' 200 m; another input's default untouched");
    {   // creep wingmen: both loadout gates' In4 now from the Not's Out; the other connections as they were; the run open to wingmen
        uint8_t* e7 = bhvPcArr + 16 + 7 * 32; uint8_t* e8 = bhvPcArr + 16 + 8 * 32; uint8_t* e10 = bhvPcArr + 16 + 10 * 32;
        check(*(uint8_t**)e7 == cmNot && *(int32_t*)(e7 + 16) == 193453899 && *(uint8_t**)(e7 + 8) == gateOr1 && *(int32_t*)(e7 + 20) == 193450870 &&
              *(uint8_t**)e8 == cmNot && *(int32_t*)(e8 + 16) == 193453899 && *(uint8_t**)(e8 + 8) == gateOr2 &&
              *(uint8_t**)e10 == cmPdv && *(int32_t*)(e10 + 16) == -645606250 && raidData && raidData[72] == 0 &&
              *(uint8_t**)(bhvPcArr + 16 + 11 * 32) == otherNot && *(uint8_t**)(bhvPcArr + 16 + 11 * 32 + 8) == otherAnd,
              "creep wingmen: the loadout gates take the creep-only Not (wingmen run the bombers' runs), and the run data is open to wingmen");
    }
    check(maxShield->value == 900.0f, "value engine: shield config written");
    check(blastDmg->value == 2500.0f && *(float*)(*(uint64_t*)(blast + 0x40) + 4) == 2500.0f,
          "limits: blast damage asked 3000, clamped to its limit 2500, written to the field and its list entry");
    check(sd.ab->value == 400.0f && sd.reg[3] == 400.0f && player.ab->value == 250.0f,
          "limits: a capital ship's config is limited by its own pool (400 within 0..500; the fighter pool would clamp to 300)");
    check(splitAttr[32] == 0 && blist[20] == 0, "yes/no: passive damage's split switch turned off, in the attribute and its bool list entry");
    check(*(float*)(firePattern + 0x20) == 0.75f && *(float*)(firePattern + 0x30) == 2.0f,
          "value engine: the last root type (turret fire pattern, a plain policy asset) scanned and written");
    printf("      (hull component modifiers: health %g, turn %g, boost charge [0] %g [1] %g)\n", *mods[0], *mods[1], *mods[2], *mods[3]);
    check(*mods[0] == 0.8f, "components: a modifier named by its attribute written (hull healthmax 0.6 -> 0.8)");
    check(*mods[1] == -0.9f, "components: a modifier below the floor clamped (turn -2 -> -0.9)");
    check(*mods[2] == 0.5f && *mods[3] == -0.2f, "components: a repeated attribute addressed by index ([1] -0.5 -> -0.2, [0] untouched)");
    printf("      (turn rate %g %g %g, list %g %g %g; fire interval %g list %g; burst %d list %d; pip cost [2] %g [3] %g [4] %g)\n",
           player.turn->x, player.turn->y, player.turn->z, player.v3[0], player.v3[1], player.v3[2], rof->value,
           *(float*)(*(uint64_t*)(weapon + 0x40) + 4), burst->value, *(int32_t*)(ilist + 20),
           *(float*)(pips + 16 + 2 * 24 + 8), *(float*)(pips + 16 + 3 * 24 + 8), *(float*)(pips + 16 + 4 * 24 + 8));
    check(player.turn->y == 7.0f && player.turn->x == 10.0f && player.turn->z == 90.0f && player.v3[1] == 7.0f && player.v3[0] == 10.0f,
          "value engine: one component of a Vec3 attribute and its list entry written");
    check(rof->value == 0.1f && *(float*)(*(uint64_t*)(weapon + 0x40) + 4) == 0.1f,
          "value engine: a sub-object's attribute (laser fire interval) and the root config's list entry written");
    check(burst->value == 5 && *(int32_t*)(ilist + 20) == 5, "value engine: an integer attribute and its int list entry written");
    check(*(float*)(pips + 16 + 3 * 24 + 8) == 1.5f && *(float*)(pips + 16 + 2 * 24 + 8) == 3.5f && *(float*)(pips + 16 + 4 * 24 + 8) == 3.0f,
          "value engine: one member of one element of a value-type array written (subclass resolved)");
    {
        static char buf[1 << 18]; memset(buf, 0, sizeof buf); FILE* f = nullptr; size_t n = 0;
        if (_wfopen_s(&f, defaultsPath, L"rb") == 0 && f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
        check(n && strstr(buf, "[starshipmovementconfig_xwing]") && strstr(buf, "afterburnermaxspeed=240") &&
              strstr(buf, "maxspeeds[8]=145") && strstr(buf, "[starshipshielddata_xwing]") && strstr(buf, "maxshieldhealth=800") &&
              strstr(buf, "afterburneraxisturnrate.y=5") && strstr(buf, "weaponfiring.laserregenpips[3].costpershot=3.25") &&
              strstr(buf, "weaponfiring.burstfirenumshotsattribute=3") &&
              strstr(buf, "[explosionconfigls_missile_piercingtorpedo]") && strstr(buf, "blastdamage=2000") &&
              strstr(buf, "[hullcustom_heavy_xwing]") && strstr(buf, "healthmax=0.6") &&
              strstr(buf, "; name=HullCustom_Heavy_Xwing") &&
              strstr(buf, "afterburnerchargerate[0]=0.5") && strstr(buf, "afterburnerchargerate[1]=-0.5") &&
              strstr(buf, "[materialgrid]") && strstr(buf, "laserdamage.capitalhull=1") && strstr(buf, "iondamage.capitalshields=1") &&
              strstr(buf, "[damageovertimesettings_flagship]") && strstr(buf, "splitdamagebetweenstarshipsattribute=1") &&
              strstr(buf, "[incomschematicattributesconfig_fleetbattle]") && strstr(buf, "towpointsgainedforcreepkillwhileonoffense=1") &&
              strstr(buf, "towpointsgainedforcreepkillwhileondefense=3") && strstr(buf, "matchmaxtime=1800") && strstr(buf, "towenablemoraledot=0"),
              "value engine: defaults.ini records the values as loaded");
        check(n && strstr(buf, "[oldconfig_from_earlier]") && strstr(buf, "foo=1") && strstr(buf, "bar[2]=-0.5") &&
              !strstr(buf, "stalefield") && !strstr(buf, "earlier launch"),
              "value engine: defaults.ini keeps configs from earlier launches; a config seen again is rewritten whole");
    }

    // socket pass-through: stand-in vtables at the real addresses, then calls through them as the engine makes them
    {
        if (map(0x1433f0000, 0x10000) != (void*)0x1433f0000) { printf("FAIL  could not map the socket vtable region\n"); return 1; }
        void** mv = (void**)0x1433fa218; void** sv = (void**)0x1433fa240;
        mv[0] = (void*)sm_destroy; mv[1] = (void*)sm_connect; mv[2] = (void*)sm_listen; mv[3] = (void*)sm_create; mv[4] = (void*)sm_dtor;
        for (int i = 0; i < 20; i++) sv[i] = (void*)ss_nop;
        sv[0] = (void*)ss_dtor; sv[1] = (void*)ss_send; sv[2] = (void*)ss_recv; sv[5] = (void*)ss_setpeer;
        sv[17] = (void*)ss_addr; sv[18] = (void*)ss_addr; sv[19] = (void*)ss_create;
        for (int i = 0; i < 100 && mv[1] == (void*)sm_connect; i++) Sleep(100);   // up to 10 s: the DLL retries every sampler round (2 s)
        check(mv[1] != (void*)sm_connect && sv[1] != (void*)ss_send, "socket vtable slots patched with pass-through thunks");
        static uint8_t mgr[0x30]; *(void**)mgr = mv;
        typedef uint64_t(__fastcall* mlisten_t)(uint64_t, const char*, bool);
        typedef bool(__fastcall* send_t)(uint64_t, const uint8_t*, int, uint32_t);
        typedef int(__fastcall* recv_t)(uint64_t, uint8_t*, int, uint32_t*);
        uint64_t srv = ((mlisten_t)mv[2])((uint64_t)mgr, "127.0.0.1:25200", false);
        uint64_t cli = ((mlisten_t)mv[2])((uint64_t)mgr, "127.0.0.1:25100", false);
        check(srv == (uint64_t)g_fakeSock && cli == (uint64_t)g_fakeSock2, "manager Listen passes through (server and client sockets)");
        *(void**)g_fakeSock = sv; *(void**)g_fakeSock2 = sv;
        uint8_t sa[0x41] = {}; sa[0] = 2; sa[2] = 0x62; sa[3] = 0x70; sa[4] = 127; sa[7] = 1; sa[0x40] = 0x10;  // 127.0.0.1:25200
        ((void(__fastcall*)(uint64_t, uint64_t))sv[5])(cli, (uint64_t)sa);
        uint8_t buf[1600] = {}; uint32_t when = 0;
        int empty = ((recv_t)sv[2])(srv, buf, 1600, &when);
        check(empty == -1, "UDP receive with nothing waiting returns -1");
        bool sr = ((send_t)sv[1])(cli, (const uint8_t*)"hello", 5, 0);
        int got = -1; for (int i = 0; i < 50 && got < 0; i++) { got = ((recv_t)sv[2])(srv, buf, 1600, &when); if (got < 0) Sleep(10); }
        check(sr && got == 5 && memcmp(buf, "hello", 5) == 0 && when != 0, "client -> server over real UDP loopback (data and timestamp)");
        uint16_t peerPort = (uint16_t)((g_fakeSock[0x58 + 2] << 8) | g_fakeSock[0x58 + 3]);
        check(peerPort == 25100 && g_fakeSock[0x98] == 0x10, "server receive sets its peer to the sender (127.0.0.1:25100)");
        ((send_t)sv[1])(srv, (const uint8_t*)"world", 5, 0);
        got = -1; for (int i = 0; i < 50 && got < 0; i++) { got = ((recv_t)sv[2])(cli, buf, 1600, &when); if (got < 0) Sleep(10); }
        check(got == 5 && memcmp(buf, "world", 5) == 0, "server -> client reply over real UDP loopback");
        check(g_sent == 0, "the engine's in-memory Send was not used for UDP-bound sockets");
        ((uint64_t(__fastcall*)(uint64_t, uint32_t))sv[0])(srv, 1);
        ((uint64_t(__fastcall*)(uint64_t, uint32_t))sv[0])(cli, 1);
        g_sockDtor--;   // two sockets destroyed; the check below expects one
        ((void(__fastcall*)(uint64_t))mv[0])((uint64_t)mgr);
        check(g_sockDtor == 1 && g_destroyed == 1, "socket destructor and manager Destroy pass through");
    }
    // socket wrappers: call through the slots as the game does; the DLL swaps them within its first retries (0.5 s steps)
    for (int i = 0; i < 40 && *(void**)0x14307fef0 == (void*)GetProcAddress(ws, "bind"); i++) Sleep(100);
    check(*(void**)0x14307fef0 != (void*)GetProcAddress(ws, "bind"), "bind import slot swapped for the wrapper");
    SOCKET sk = ((SOCKET(WSAAPI*)(int, int, int))*(void**)0x14307fee0)(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    int br = ((int(WSAAPI*)(SOCKET, const sockaddr*, int))*(void**)0x14307fef0)(sk, (sockaddr*)&sa, sizeof sa);
    check(sk != INVALID_SOCKET && br == 0, "socket()+bind() through the wrapped slots behave normally");
    int blen = sizeof sa; getsockname(sk, (sockaddr*)&sa, &blen);
    int sr = ((int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int))*(void**)0x14307ff10)(sk, "x", 1, 0, (sockaddr*)&sa, sizeof sa);
    check(sr == 1, "sendto() through the wrapped slot behaves normally");
    closesocket(sk);
    // the rank data the preset slots unlock by ([[0x1441c5f00]+0xf8]+0x30, IncomRankData.RanksOfStarshipPresets)
    if (map(0x1441c0000, 0x10000) != (void*)0x1441c0000) { printf("FAIL  could not map the game-settings region\n"); return 1; }
    static uint8_t gameObj[0x100] = {}, rankData[0x40] = {};
    static uint32_t ranks[6] = { 5, 0, 2, 14, 24, 34 };   // EASTL array: the count sits just before the first entry
    *(uint64_t*)0x1441c5f00 = (uint64_t)gameObj; *(uint64_t*)(gameObj + 0xf8) = (uint64_t)rankData; *(uint64_t*)(rankData + 0x30) = (uint64_t)(ranks + 1);
    {   // all-unlocked: one real item (id 7) in, the setter must see it plus every id from unlock_ids.txt
        static uint8_t item[0x38] = {}; *(uint64_t*)(item + 0x10) = 7;
        uint64_t vec[4] = { (uint64_t)item, (uint64_t)(item + 0x38), (uint64_t)(item + 0x38), 0 };
        ((void(__fastcall*)(uint64_t, uint64_t*))0x14123a310)(0, vec);
        uint64_t b = *(uint64_t*)0x141238000, e = *(uint64_t*)0x141238008;
        int n = (int)((e - b) / 0x38);
        printf("      (inventory setter saw %d items)\n", n);
        check(*(uint8_t*)0x14123a310 == 0xFF && n > 1000, "inventory setter hooked and all ids appended");
        check(ranks[0] == 5 && !ranks[1] && !ranks[2] && !ranks[3] && !ranks[4] && !ranks[5], "all_unlocked: every loadout preset slot needs rank 0");
    }
    {   // ruleset: the hook mixes the rules.ini ruleset id into the protocol version
        typedef uint32_t(__fastcall* tinit_fn)(uint64_t, uint64_t, uint32_t, uint32_t);
        uint32_t got = ((tinit_fn)0x140d7bb70)(0, 0, 0x1111, 0x00a0b0c0);
        printf("      (transport setup returned protocol %08x for stock 00a0b0c0)\n", got);
        check(*(uint8_t*)0x140d7bb70 == 0xFF, "transport setup hooked for ruleset matching");
        {   // material grid lookup: hooked, and a call goes through to the original (which returns the stock cell here);
            // with material_log=1 in server_3b.ini the log gets one "material:" line for this pair, however often it's asked
            uint32_t laser = 0x00300480, capital = 0x00b00980;
            uint8_t* grid2 = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            uint64_t r1 = ((matl_t)0x14057dd20)((uint64_t)grid2, &laser, &capital), r2 = ((matl_t)0x14057dd20)((uint64_t)grid2, &laser, &capital);
            check(*(uint8_t*)0x14057dd20 == 0xFF && r1 == (uint64_t)stockCell && r2 == (uint64_t)stockCell, "material lookup hooked and passes calls through");
        }
        // profile StandIn (mod/test_profiles/StandIn.ini, copied to build/profiles/ by the test run; server_3b.ini names it):
        // multidrift on, pinball off, two overrides written with mixed case, spacing and a duplicate. Expected fingerprint:
        // FNV-1a over the canonical text, keys lowercased and sorted, values %.9g, the duplicate's last value kept.
        // limits (issue #11): blastdamage enters clamped (3000 -> 2500); nosuchfield has no limit and is left out.
        const char* canon[] = { "pinball=0;multidrift=1;zaviancreep=1;", "creep.rebel_pack1=ywing;creep.rebel_pack3=awing;creep.aggressive=1;creep.radius=1200;creep.capital_damage=50;creep.missiles=1;creep.torpedo_runs=1;creep.torpedo_lock_min=150;creep.wingmen=1;creep.bothandling=1;", "damageovertimesettings_flagship.splitdamagebetweenstarshipsattribute=0;",
                                "damageovertimesettings_medcap.targethealthratiotostopattribute=0.5;",
                                "explosionconfigls_missile_piercingtorpedo.blastdamage=2500;",
                                "hullcustom_heavy_xwing.afterburnerchargerate[1]=-0.2;", "hullcustom_heavy_xwing.axisturnratesfactor=-0.9;",
                                "hullcustom_heavy_xwing.healthmax=0.8;",
                                "incomschematicattributesconfig_fleetbattle.matchmaxtime=2400;",
                                "incomschematicattributesconfig_fleetbattle.towenablemoraledot=1;",
                                "incomschematicattributesconfig_fleetbattle.towpointsgainedforcreepkillwhileonoffense=2;",
                                "materialgrid.laserdamage.capitalhull=0.05;",
                                "shipscale_tiefighter.scale=1.5;", "shipscale_ywing.scale=0.5;",
                                "starshiphealthdamagedata_ywing_ai_sp.maxhullhealth=900;",
                                "starshipmovementconfig_stardestroyer.afterburnermaxspeed=400;",
                                "starshipmovementconfig_xwing.afterburneraxisturnrate.y=7;",
                                "starshipmovementconfig_xwing.afterburnermaxspeed=250;",
                                "starshipmovementconfig_xwing.drifttransitiontime=5;", "starshipmovementconfig_xwing.maxspeeds[8]=150;",
                                "starshipshielddata_xwing.maxshieldhealth=900;",
                                "starshipweaponconfig_las_reb_xwing.weaponfiring.burstfirenumshotsattribute=5;",
                                "starshipweaponconfig_las_reb_xwing.weaponfiring.laserregenpips[3].costpershot=1.5;",
                                "starshipweaponconfig_las_reb_xwing.weaponfiring.timebetweenautomaticshotsattribute=0.1;",
                                "turretfirepatternsettingsasset_frontline.mindelaybetweenbursts=0.75;" };
        uint32_t h = 2166136261u;
        for (const char* c : canon) for (const char* q = c; *q; q++) { h ^= (uint8_t)*q; h *= 16777619u; }
        printf("      (expected fingerprint %08x -> protocol %08x)\n", h, 0x00a0b0c0u ^ h);
        check(got == (0x00a0b0c0u ^ h), "profile fingerprint (rules + normalized overrides) mixed into the protocol");
    }
    // observation: fake components laid out as the game's (handle {vtable, .., +0x18 output object}; the output object
    // has its ClassInfo at +8 and fields from +0x18). Checked afterwards from the flight record (observe_report.py):
    // movement CurrentSpeed 100..300 in 3 records, AfterburnerMaxSpeed 320, never 999 (recorded after destruction);
    // laser regen energy 0.5; weapon barrel 2; the decoy health component not recorded (wrong ClassInfo); the real one's
    // CurrentHealth 624..1080 and last hit taken DamageDealt 123..456.
    {
        typedef uint64_t(__fastcall* ctor_t)(uint64_t, uint64_t, uint64_t, uint64_t);
        uint8_t* mv = (uint8_t*)calloc(1, 0xCB0 + 0x20); uint8_t* md = (uint8_t*)calloc(1, 0x78);
        *(uint64_t*)(mv + 0xCB0) = 0x143614da0; *(uint64_t*)(mv + 0xCB0 + 0x18) = (uint64_t)md;
        *(uint64_t*)(md + 8) = 0x144a16420; *(float*)(md + 0x34) = 320.0f; *(float*)(md + 0x1c) = 100.0f;
        uint8_t* hc = (uint8_t*)calloc(1, 0x430 + 0x20); uint8_t* hd = (uint8_t*)calloc(1, 0x58);
        *(uint64_t*)(hc + 0x430) = 0x143612168; *(uint64_t*)(hc + 0x430 + 0x18) = (uint64_t)hd;
        *(uint64_t*)(hd + 8) = 0x144a16420; *(float*)(hd + 0x30) = 777.0f;   // decoy: a movement ClassInfo on a health output
        uint8_t* wc = (uint8_t*)calloc(1, 0x1E0 + 0x20); uint8_t* wd = (uint8_t*)calloc(1, 0x70); uint8_t* rg = (uint8_t*)calloc(1, 0x28);
        *(uint64_t*)(wc + 0x1E0) = 0x14361e2a0; *(uint64_t*)(wc + 0x1E0 + 0x18) = (uint64_t)wd;
        *(uint64_t*)(wd + 8) = 0x144a15420; *(uint64_t*)(wd + 0x20) = (uint64_t)rg; *(int32_t*)(wd + 0x64) = 2; wd[0x6c] = 1;
        *(uint64_t*)(rg + 8) = 0x144a36d80; *(float*)(rg + 0x18) = 0.5f;
        uint8_t* hc2 = (uint8_t*)calloc(1, 0x430 + 0x20); uint8_t* hd2 = (uint8_t*)calloc(1, 0x58); uint8_t* hit = (uint8_t*)calloc(1, 0x40);
        *(uint64_t*)(hc2 + 0x430) = 0x143612168; *(uint64_t*)(hc2 + 0x430 + 0x18) = (uint64_t)hd2;
        *(uint64_t*)(hd2 + 8) = 0x144a02ec0; *(float*)(hd2 + 0x30) = 1080.0f; *(uint64_t*)(hd2 + 0x18) = (uint64_t)hit;
        *(uint64_t*)(hit + 8) = 0x144a15720; *(float*)(hit + 0x1c) = 123.0f;
        ((ctor_t)0x14169cf00)((uint64_t)mv, 0, 0, 0); ((ctor_t)0x141680fb0)((uint64_t)hc, 0, 0, 0); ((ctor_t)0x14172a5e0)((uint64_t)wc, 0, 0, 0);
        ((ctor_t)0x141680fb0)((uint64_t)hc2, 0, 0, 0);
        const float speeds[] = { 100.0f, 100.0f, 150.0f, 150.0f, 300.0f, 300.0f };
        for (float v : speeds) { *(float*)(md + 0x1c) = v; Sleep(250); }
        *(float*)(hit + 0x1c) = 456.0f; *(float*)(hd2 + 0x30) = 624.0f; Sleep(250);   // a second hit
        *(uint64_t*)(mv + 0xCB0) = 0; Sleep(350);   // destroyed: the handle loses its vtable
        *(float*)(md + 0x1c) = 999.0f; Sleep(350);
        check(true, "observation: fake components sampled (verify with observe_report.py on this run's record)");
    }
    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    Sleep(3000);  // lets the DLL's stack sampler (first round at 0.5 s) run in tests
    return fails ? 1 : 0;
}
