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

int main() {
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
    check(GetModuleHandleW(L"sws_server_test.dll") != nullptr, "DLL was loaded at startup by the launcher's APC");
    for (int i = 0; i < 300 && *(uint8_t*)0x1401ae7d0 != 0xFF; i++) Sleep(50);
    check(*(uint8_t*)0x1401ae7d0 == 0xFF && *(uint8_t*)0x1401aac90 == 0xFF && *(uint8_t*)0x1401bf4d0 == 0xFF, "server hooks installed");

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
    alignas(16) static uint8_t gs[0x200];
    for (int i = 0; i < 150; i++) { ((void(__fastcall*)(void*, uint64_t, uint64_t, uint64_t))0x1401bfe60)(gs, 0, 0, 0); Sleep(30); }
    check(true, "ticked 4.5 s (see status log for the 3b spawn lines)");

    // socket pass-through: stand-in vtables at the real addresses, then calls through them as the engine makes them
    {
        if (map(0x1433f0000, 0x10000) != (void*)0x1433f0000) { printf("FAIL  could not map the socket vtable region\n"); return 1; }
        void** mv = (void**)0x1433fa218; void** sv = (void**)0x1433fa240;
        mv[0] = (void*)sm_destroy; mv[1] = (void*)sm_connect; mv[2] = (void*)sm_listen; mv[3] = (void*)sm_create; mv[4] = (void*)sm_dtor;
        for (int i = 0; i < 20; i++) sv[i] = (void*)ss_nop;
        sv[0] = (void*)ss_dtor; sv[1] = (void*)ss_send; sv[2] = (void*)ss_recv; sv[5] = (void*)ss_setpeer;
        sv[17] = (void*)ss_addr; sv[18] = (void*)ss_addr; sv[19] = (void*)ss_create;
        for (int i = 0; i < 40 && mv[1] == (void*)sm_connect; i++) Sleep(100);
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
        // profile StandIn (mod/test_profiles/StandIn.ini, copied to build/profiles/ by the test run; server_3b.ini names it):
        // multidrift on, pinball off, two overrides written with mixed case, spacing and a duplicate. Expected fingerprint:
        // FNV-1a over the canonical text, keys lowercased and sorted, values %.9g, the duplicate's last value kept.
        const char* canon[] = { "pinball=0;multidrift=1;", "starshipmovementconfig_xwing.afterburnermaxspeed=250;",
                                "starshipshielddata_xwing.maxshieldhealth=900;" };
        uint32_t h = 2166136261u;
        for (const char* c : canon) for (const char* q = c; *q; q++) { h ^= (uint8_t)*q; h *= 16777619u; }
        printf("      (expected fingerprint %08x -> protocol %08x)\n", h, 0x00a0b0c0u ^ h);
        check(got == (0x00a0b0c0u ^ h), "profile fingerprint (rules + normalized overrides) mixed into the protocol");
    }
    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    Sleep(3000);  // lets the DLL's stack sampler (first round at 0.5 s) run in tests
    return fails ? 1 : 0;
}
