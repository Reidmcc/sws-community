// Test target for sws_launch.exe + sws_server_test.dll: started suspended by the launcher, which APC-injects the DLL.
// It plants the game's real prologue bytes (with small stand-in bodies) at every hook address, waits for the DLL to
// hook them, then checks each server hook's effect. Exit code 0 = all pass.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "hitbox_shapes.h"

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
    // Havok's in-place packfile loader (FUN_142706d60): its real 19-byte prologue, then mov rax, rcx (the packfile) and
    // lea rsp, [rbp+0x3f] and the pops
    if (map(0x142700000, 0x10000) != (void*)0x142700000) { printf("FAIL  could not map the packfile-loader region\n"); return 1; }
    const uint8_t hkload[] = {0x40,0x55,0x57,0x41,0x54,0x41,0x57,0x48,0x8d,0x6c,0x24,0xc1,0x48,0x81,0xec,0x88,0x00,0x00,0x00,
                              0x48,0x89,0xc8, 0x48,0x8d,0x65,0x3f, 0x41,0x5f,0x41,0x5c,0x5f,0x5d,0xc3};
    put(0x142706d60, hkload, sizeof hkload);
    // the second in-place loader (FUN_142706770): its real 18-byte prologue, then mov rax, rcx, add rsp, 0x40, the pops,
    // and rbx/rbp back from their home slots
    const uint8_t hkload2[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40,
                               0x48,0x89,0xc8, 0x48,0x83,0xc4,0x40, 0x41,0x5e,0x5f,0x5e, 0x48,0x8b,0x5c,0x24,0x08, 0x48,0x8b,0x6c,0x24,0x10, 0xc3};
    put(0x142706770, hkload2, sizeof hkload2);
    // Frostbite's Havok resource load (FUN_142707130): its real 16-byte prologue (mov rax, rsp; push rbp; push rdi;
    // lea rbp, [rax-0x5f]; sub rsp, 0x98), then mov rax, rcx, lea rsp, [rbp+0x4f], pop rdi, pop rbp, ret
    const uint8_t hkload3[] = {0x48,0x8b,0xc4,0x55,0x57,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,0x98,0x00,0x00,0x00,
                               0x48,0x89,0xc8, 0x48,0x8d,0x65,0x4f, 0x5f, 0x5d, 0xc3};
    put(0x142707130, hkload3, sizeof hkload3);
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
    uint8_t** insts2 = (uint8_t**)fake_alloc(64); insts2[0] = creep.inst; insts2[1] = shield; insts2[2] = weapon; insts2[3] = sd.inst; insts2[4] = dot;
    uint8_t* part1 = fake_alloc(0x40); *(uint8_t***)(part1 + 0x20) = insts1; *(uint16_t*)(part1 + 0x28) = 6;
    uint8_t* part2 = fake_alloc(0x40); *(uint8_t***)(part2 + 0x20) = insts2; *(uint16_t*)(part2 + 0x28) = 5;
    uint8_t** parts = (uint8_t**)fake_alloc(64); parts[0] = part1; parts[1] = part2;
    uint8_t* domain = fake_alloc(0x200); InitializeCriticalSection((CRITICAL_SECTION*)(domain + 0x70));
    *(uint8_t***)(domain + 0x138) = parts; *(uint8_t***)(domain + 0x140) = parts + 2;
    uint8_t* centry = fake_alloc(0x80); *(uint8_t**)(centry + 0x68) = domain;
    *(uint8_t**)(0x1442d18c8 + 4 * 8) = centry;   // compartment 4 ("Game")
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
    {   // model scale (#20): the Y-wing's blueprint partition (StarshipBlueprint "Game/Gameplay/Starships/Ywing/Starship_Ywing"
        // and its StarshipEntityData) arrives through the partition creator; [model] ywing=0.5 scales the entity's root part
        // and mesh component (axes and translation), and leaves its physics component alone
        uint8_t* bp = fake_alloc(0x40); *(uint64_t*)(bp + 8) = 0x144ade220;
        *(const char**)(bp + 0x18) = "Game/Gameplay/Starships/Ywing/Starship_Ywing";
        auto comp = [](uint64_t cls, float sx, float tz) {
            uint8_t* c = fake_alloc(0x100); *(uint64_t*)(c + 8) = cls; float* m = (float*)(c + 0x30);
            m[0] = sx; m[5] = sx; m[10] = sx; m[12] = 0; m[13] = -0.78f; m[14] = tz; return c; };
        uint8_t* rootPart = comp(0x1449fa760, 1.0f, -7.2f);
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
        check(r[0] == 0.5f && r[5] == 0.5f && r[10] == 0.5f && r[13] == -0.39f && r[14] == -3.6f && g[0] == 70.0f && g[14] == -6.5f &&
              p[0] == 1.0f && p[13] == -0.78f,
              "model scale: the Y-wing's root part and mesh component scale about the ship's origin as its blueprint loads; other components untouched");
    }
    {   // ship scale (#20): the Havok packfile loader (FUN_142706d60) is hooked at its entry; a packfile holding a Y-wing
        // mesh (its domain box at +0x20, its sections 0x60 apart where the table says, the first codec as shipped) has the
        // mesh scaled 0.5 before the loader runs; a TIE fighter mesh (not asked for) and a buffer without the packfile magic
        // are left alone. [hitbox] ywing=0.5.
        const HitboxMesh* yw = nullptr; const HitboxMesh* tf = nullptr;
        for (const HitboxMesh& m : kHitboxMeshes) {
            if (!yw && !strcmp(m.ship, "ywing") && m.count > 1) yw = &m;
            if (!tf && !strcmp(m.ship, "tiefighter")) tf = &m;
        }
        auto put_mesh = [](uint8_t* d, const HitboxMesh& m) {
            memcpy(d + 0x20, m.domain, 32); *(uint32_t*)(d + 0x58) = m.count;
            for (uint32_t j = 0; j < m.count; j++) {
                float* sec = (float*)(d + m.sections + 0x60 * j);
                for (int c = 0; c < 3; c++) { sec[4 + c] = -1.0f - j; sec[8 + c] = 1.0f + j; sec[12 + c] = 0.25f * j; sec[15 + c] = 0.001f; }
            }
            memcpy(d + m.sections + 0x30, m.codec, 24);
        };
        uint32_t ysz = (yw->sections + yw->count * 0x60 + 0x100 + 0xff) & ~0xffu, tsz = (tf->sections + tf->count * 0x60 + 0x100 + 0xff) & ~0xffu;
        uint8_t* pk = fake_alloc(0x100 + ysz + tsz);
        *(uint32_t*)pk = 0x57e0e057; *(uint32_t*)(pk + 4) = 0x10c0c010;
        uint8_t* ym = pk + 0x100; uint8_t* tm = pk + 0x100 + ysz;
        put_mesh(ym, *yw); put_mesh(tm, *tf);
        float ydom[8], ysec1[6], ycodec0[6]; uint8_t tdom[32];
        memcpy(ydom, ym + 0x20, 32); memcpy(ysec1, ym + yw->sections + 0x60 + 0x30, 24); memcpy(ycodec0, yw->codec, 24); memcpy(tdom, tm + 0x20, 32);
        uint8_t* plain = fake_alloc(0x400); put_mesh(plain + 0x100, *yw);   // no packfile magic: not a packfile
        uint8_t plainBefore[0x300]; memcpy(plainBefore, plain + 0x100, 0x300);
        typedef uint64_t(__fastcall* hkload_t)(uint64_t, uint32_t, uint64_t, uint64_t);
        uint64_t r1 = ((hkload_t)0x142706d60)((uint64_t)pk, 0x100 + ysz + tsz, 0, 0);
        uint64_t r2 = ((hkload_t)0x142706d60)((uint64_t)plain, 0x400, 0, 0);
        uint64_t r3 = ((hkload_t)0x142706d60)((uint64_t)pk, 0x100 + ysz + tsz, 0, 0);   // a second load of the same bytes
        bool ok = true;
        const float* d = (const float*)(ym + 0x20); const float* s1 = (const float*)(ym + yw->sections + 0x60 + 0x30);
        const float* c0 = (const float*)(ym + yw->sections + 0x30);
        for (int c = 0; c < 3; c++) if (d[c] != ydom[c] * 0.5f || d[4 + c] != ydom[4 + c] * 0.5f) ok = false;
        if (d[3] != ydom[3] || d[7] != ydom[7]) ok = false;
        for (int c = 0; c < 6; c++) if (s1[c] != ysec1[c] * 0.5f || c0[c] != ycodec0[c] * 0.5f) ok = false;
        check(*(uint8_t*)0x142706d60 == 0xFF && r1 == (uint64_t)pk && r2 == (uint64_t)plain && r3 == (uint64_t)pk,
              "ship scale: the Havok packfile loader is hooked and passes its packfile through");
        check(ok, "ship scale: a Y-wing mesh in a loading packfile is scaled 0.5 (domain, sections' domains, codecs), once");
        check(!memcmp(tm + 0x20, tdom, 32) && !memcmp(plain + 0x100, plainBefore, 0x300),
              "ship scale: a TIE fighter mesh (not asked for) and a buffer without the packfile magic are left alone");
        // the second loader: a fresh packfile with the same Y-wing mesh, loaded through FUN_142706770
        uint8_t* pk2 = fake_alloc(0x100 + ysz); *(uint32_t*)pk2 = 0x57e0e057; *(uint32_t*)(pk2 + 4) = 0x10c0c010;
        put_mesh(pk2 + 0x100, *yw);
        typedef uint64_t(__fastcall* hkload2_t)(uint64_t, uint32_t, uint64_t);
        uint64_t r4 = ((hkload2_t)0x142706770)((uint64_t)pk2, 0x100 + ysz, 0);
        const float* d2 = (const float*)(pk2 + 0x100 + 0x20);
        check(*(uint8_t*)0x142706770 == 0xFF && r4 == (uint64_t)pk2 && d2[0] == ydom[0] * 0.5f && d2[4] == ydom[4] * 0.5f,
              "ship scale: the second packfile loader is hooked too, and scales the same way");
        // Frostbite's Havok resource load: a fresh packfile with the same Y-wing mesh, through FUN_142707130
        uint8_t* pk3 = fake_alloc(0x100 + ysz); *(uint32_t*)pk3 = 0x57e0e057; *(uint32_t*)(pk3 + 4) = 0x10c0c010;
        put_mesh(pk3 + 0x100, *yw);
        typedef uint64_t(__fastcall* hkload3_t)(uint64_t, uint32_t, uint64_t, uint64_t);
        uint64_t r5 = ((hkload3_t)0x142707130)((uint64_t)pk3, 0x100 + ysz, 0, 0);
        const float* d3 = (const float*)(pk3 + 0x100 + 0x20);
        check(*(uint8_t*)0x142707130 == 0xFF && r5 == (uint64_t)pk3 && d3[0] == ydom[0] * 0.5f && d3[4] == ydom[4] * 0.5f,
              "ship scale: Frostbite's Havok resource load is hooked too, and scales the same way");
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
    check(bot.drift->value == 3.0f && bot.reg[5] == 3.0f, "value engine: vs-AI bot scaled where its default differs (1.5 x 5/2.5 = 3)");
    check(creep.ab->value == 240.0f && creep.drift->value == 2.5f, "value engine: creep config untouched");
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
        char buf[16384] = {}; FILE* f = nullptr; size_t n = 0;
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
              strstr(buf, "[damageovertimesettings_flagship]") && strstr(buf, "splitdamagebetweenstarshipsattribute=1"),
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
    {   // all-unlocked: one real item (id 7) in, the setter must see it plus every id from unlock_ids.txt
        static uint8_t item[0x38] = {}; *(uint64_t*)(item + 0x10) = 7;
        uint64_t vec[4] = { (uint64_t)item, (uint64_t)(item + 0x38), (uint64_t)(item + 0x38), 0 };
        ((void(__fastcall*)(uint64_t, uint64_t*))0x14123a310)(0, vec);
        uint64_t b = *(uint64_t*)0x141238000, e = *(uint64_t*)0x141238008;
        int n = (int)((e - b) / 0x38);
        printf("      (inventory setter saw %d items)\n", n);
        check(*(uint8_t*)0x14123a310 == 0xFF && n > 1000, "inventory setter hooked and all ids appended");
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
        const char* canon[] = { "pinball=0;multidrift=1;zaviancreep=1;", "damageovertimesettings_flagship.splitdamagebetweenstarshipsattribute=0;",
                                "damageovertimesettings_medcap.targethealthratiotostopattribute=0.5;",
                                "explosionconfigls_missile_piercingtorpedo.blastdamage=2500;",
                                "hullcustom_heavy_xwing.afterburnerchargerate[1]=-0.2;", "hullcustom_heavy_xwing.axisturnratesfactor=-0.9;",
                                "hullcustom_heavy_xwing.healthmax=0.8;",
                                "materialgrid.laserdamage.capitalhull=0.05;",
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
