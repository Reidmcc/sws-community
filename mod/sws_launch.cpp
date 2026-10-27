// sws_launch.exe: starts STAR WARS: Squadrons with sws_server.dll loaded before the game's own code runs.
//
// The dedicated-server flag lives in the MainLoop object, which the game builds at startup, so attaching to a running
// game (inject.exe) is too late. This launcher creates the game process suspended, queues LoadLibraryW(sws_server.dll)
// as an APC on its main thread, and resumes it: the DLL loads during process initialisation ("early-bird" APC) and
// installs its hooks before the main loop exists.
//
// The EA app's copy is started the same way, with the environment the EA app would give it, so that its activation
// wrapper asks the running EA app for the licence instead of handing the launch back to it (which would start the game
// under EasyAntiCheat). The variable names and the daily launch code are Maxima's (github.com/ArmchairDevelopers/Maxima,
// maxima-lib/src/core/launch.rs and util/simple_crypto.rs). No login token is set or needed: the EA app answers with the
// licence it already holds for the signed-in account (tested 2026-10-26). The EA app must be running and signed in.
//
// Refuses (and starts nothing) if an EasyAntiCheat service is not stopped or any EAC process is running, or if the game
// is already running. Asks the user to type YES.
//
// Usage: sws_launch.exe                      (the game found as below, DLL sws_server.dll beside this exe)
//        sws_launch.exe --target <exe> --dll <name> --yes     (test use)
//        sws_launch.exe [--target <exe>] --show-ea-env          (print the EA app environment it would set; starts nothing)
//        sws_launch.exe --list-games                            (every copy of the game it finds; starts nothing)
//        sws_launch.exe --target <exe> --yes                    (the settings app: the copy chosen there, after its own confirmation)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const wchar_t* kGame = L"E:\\SteamLibrary\\steamapps\\common\\STAR WARS Squadrons\\starwarssquadrons.exe";

static bool file_exists(const wchar_t* p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

// The EA app's install: the folder it records in the registry (HKLM\SOFTWARE\EA Games\STAR WARS Squadrons, Install Dir).
static bool find_ea_game(wchar_t* out, size_t n) {
    wchar_t dir[MAX_PATH] = L""; DWORD sz = sizeof dir;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EA Games\\STAR WARS Squadrons", L"Install Dir", RRF_RT_REG_SZ, nullptr, dir, &sz) != ERROR_SUCCESS)
        return false;
    size_t len = wcslen(dir); if (len && dir[len - 1] == L'\\') dir[len - 1] = 0;
    wchar_t cand[MAX_PATH]; swprintf_s(cand, L"%ls\\starwarssquadrons.exe", dir);
    if (!file_exists(cand)) return false;
    wcscpy_s(out, n, cand);
    return true;
}

// launch.ini beside this exe: [launch] game=<path to the exe>
static bool ini_game(wchar_t* out, size_t n) {
    wchar_t ini[MAX_PATH]; GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wchar_t* sl = wcsrchr(ini, L'\\'); if (sl) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - ini), L"launch.ini");
    wchar_t g[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"launch", L"game", L"", g, MAX_PATH, ini);
    if (!g[0] || !file_exists(g)) return false;
    wcscpy_s(out, n, g);
    return true;
}

// Steam's library folders (registry SteamPath -> steamapps\libraryfolders.vdf "path" entries -> <lib>\steamapps\common\STAR WARS Squadrons)
static bool steam_game(wchar_t* out, size_t n) {
    wchar_t steam[MAX_PATH] = L""; DWORD sz = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &sz) == ERROR_SUCCESS) {
        for (wchar_t* c = steam; *c; c++) if (*c == L'/') *c = L'\\';
        wchar_t libs[16][MAX_PATH]; int nl = 0;
        wcscpy_s(libs[nl++], steam);
        wchar_t vdf[MAX_PATH]; swprintf_s(vdf, L"%ls\\steamapps\\libraryfolders.vdf", steam);
        FILE* f = nullptr;
        if (_wfopen_s(&f, vdf, L"rb") == 0 && f) {
            char line[1024];
            while (fgets(line, sizeof line, f) && nl < 16) {
                char* k = strstr(line, "\"path\""); if (!k) continue;
                char* q1 = strchr(k + 6, '"'); if (!q1) continue; char* q2 = strchr(q1 + 1, '"'); if (!q2) continue;
                *q2 = 0; char val[MAX_PATH]; int j = 0;
                for (char* c = q1 + 1; *c && j < MAX_PATH - 1; c++) { if (c[0] == '\\' && c[1] == '\\') c++; val[j++] = *c; }
                val[j] = 0;
                MultiByteToWideChar(CP_UTF8, 0, val, -1, libs[nl++], MAX_PATH);
            }
            fclose(f);
        }
        for (int i = 0; i < nl; i++) {
            wchar_t cand[MAX_PATH]; swprintf_s(cand, L"%ls\\steamapps\\common\\STAR WARS Squadrons\\starwarssquadrons.exe", libs[i]);
            if (file_exists(cand)) { wcscpy_s(out, n, cand); return true; }
        }
    }
    return false;
}

// The game's path, in order: launch.ini, Steam's library folders, the EA app's install, then the old default. With both
// a Steam and an EA app copy, Steam's is used unless launch.ini or --target names the other (the settings app passes
// --target for the copy chosen there).
static bool find_game(wchar_t* out, size_t n) {
    if (ini_game(out, n)) { wprintf(L"Game (from launch.ini): %ls\n", out); return true; }
    if (steam_game(out, n)) { wprintf(L"Game (from Steam's library folders): %ls\n", out); return true; }
    if (find_ea_game(out, n)) { wprintf(L"Game (the EA app's install): %ls\n", out); return true; }
    if (file_exists(kGame)) { wcscpy_s(out, n, kGame); return true; }
    return false;
}

// The EA app's build: its exe carries EA's activation wrapper, a section named .ooa (the Steam build has none).
static bool is_ea_build(const wchar_t* exe) {
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    static uint8_t h[4096]; DWORD got = 0;
    bool ok = ReadFile(f, h, sizeof h, &got, nullptr) && got >= 0x200;
    CloseHandle(f);
    if (!ok) return false;
    uint32_t pe = *(uint32_t*)(h + 0x3c);
    if (pe + 24 > got || memcmp(h + pe, "PE\0\0", 4)) return false;
    uint16_t nsec = *(uint16_t*)(h + pe + 6), opt = *(uint16_t*)(h + pe + 20);
    for (uint32_t i = 0, o = pe + 24 + opt; i < nsec && o + 40 <= got; i++, o += 40)
        if (!memcmp(h + o, ".ooa\0", 5)) return true;
    return false;
}

// --list-games: every copy found, one "Copy: steam|ea <path>" line each, in find_game's order (for the settings app)
static void list_games() {
    wchar_t seen[3][MAX_PATH]; int ns = 0;
    bool (*finders[])(wchar_t*, size_t) = { ini_game, steam_game, find_ea_game };
    for (auto f : finders) {
        wchar_t p[MAX_PATH];
        if (!f(p, MAX_PATH)) continue;
        bool dup = false;
        for (int i = 0; i < ns; i++) if (!_wcsicmp(seen[i], p)) dup = true;
        if (dup) continue;
        wcscpy_s(seen[ns++], p);
        wprintf(L"Copy: %ls %ls\n", is_ea_build(p) ? L"ea" : L"steam", p);
    }
}

// Whether something listens on 127.0.0.1:3216, the EA app's local service port (LSX), which the wrapper asks for the licence.
static bool ea_app_listening() {
    WSADATA wd; if (WSAStartup(MAKEWORD(2, 2), &wd)) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bool up = false;
    if (s != INVALID_SOCKET) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(3216); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        up = connect(s, (sockaddr*)&a, sizeof a) == 0;
        closesocket(s);
    }
    WSACleanup();
    return up;
}

// The EA app's daily launch code (Maxima's simple_crypto.rs), from today's UTC date.
static uint32_t rtp_launch_code() {
    SYSTEMTIME t; GetSystemTime(&t);
    uint64_t v = (104729ull * t.wYear) ^ (t.wMonth * 224737ull) ^ (t.wDay * 350377ull);
    return (uint32_t)(v ^ (v << 16) ^ (v >> 16));
}

// The environment the EA app gives the game, set on this process so the game inherits it. The content id is the
// install's own (__Installer\installerdata.xml), the offer id Squadrons' base game; launch.ini [launch] ea_offer=
// overrides the offer if an edition ever needs another.
static void set_ea_environment(const wchar_t* target, bool show = false) {
    wchar_t content[32] = L"16124549";
    wchar_t xml[MAX_PATH]; wcscpy_s(xml, target); wchar_t* s = wcsrchr(xml, L'\\');
    if (s) { wcscpy_s(s + 1, MAX_PATH - (s + 1 - xml), L"__Installer\\installerdata.xml");
        FILE* f = nullptr;
        if (_wfopen_s(&f, xml, L"rb") == 0 && f) {
            static char buf[1 << 16]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
            char* c = strstr(buf, "<contentID>");
            if (c) { c += 11; int j = 0; while (c[j] >= '0' && c[j] <= '9' && j < 30) { content[j] = (wchar_t)c[j]; j++; } if (j) content[j] = 0; }
        }
    }
    wchar_t ini[MAX_PATH]; GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wchar_t* sl = wcsrchr(ini, L'\\'); if (sl) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - ini), L"launch.ini");
    wchar_t offer[64]; GetPrivateProfileStringW(L"launch", L"ea_offer", L"Origin.OFR.50.0004097", offer, 64, ini);
    wchar_t locale[32] = L"en_US"; DWORD sz = sizeof locale;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EA Games\\STAR WARS Squadrons", L"Locale", RRF_RT_REG_SZ, nullptr, locale, &sz);
    wchar_t code[16]; swprintf_s(code, L"%u", rtp_launch_code());
    const wchar_t* vars[][2] = {
        {L"ContentId", content}, {L"EAConnectionId", offer}, {L"EALicenseToken", offer}, {L"EARtPLaunchCode", code},
        {L"EALsxPort", L"3216"}, {L"EAAuthCode", L"unavailable"}, {L"EALaunchCode", L"unavailable"},
        {L"EAEgsProxyIpcPort", L"0"}, {L"EASteamProxyIpcPort", L"0"}, {L"EAEntitlementSource", L"EA"},
        {L"EAExternalSource", L"EA"}, {L"EAFreeTrialGame", L"false"}, {L"EAGameLocale", locale}, {L"EALaunchOwner", L"EA"},
        {L"EALaunchEnv", L"production"}, {L"EALaunchOfflineMode", L"false"}, {L"EAOnErrorExitRetCode", L"1"},
    };
    for (auto& v : vars) SetEnvironmentVariableW(v[0], v[1]);
    wprintf(L"EA app copy: content %ls, offer %ls, today's launch code %ls.\n", content, offer, code);
    if (show) for (auto& v : vars) wprintf(L"  %ls=%ls\n", v[0], v[1]);
}

static bool icontains(const wchar_t* hay, const wchar_t* needle) {
    size_t n = wcslen(needle);
    for (; *hay; hay++) if (_wcsnicmp(hay, needle, n) == 0) return true;
    return false;
}

static bool service_active(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return true;
    bool active = false;
    SC_HANDLE sv = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (sv) {
        SERVICE_STATUS st{};
        active = !QueryServiceStatus(sv, &st) || st.dwCurrentState != SERVICE_STOPPED;
        CloseServiceHandle(sv);
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
        active = true;
    }
    CloseServiceHandle(scm);
    return active;
}

static int run(int argc, wchar_t** argv) {
    static wchar_t found[MAX_PATH] = L"";
    const wchar_t* target = nullptr;
    const wchar_t* dllName = L"sws_server.dll";
    bool yes = false, showEnv = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--target") && i + 1 < argc) target = argv[++i];
        else if (!wcscmp(argv[i], L"--dll") && i + 1 < argc) dllName = argv[++i];
        else if (!wcscmp(argv[i], L"--yes")) yes = true;
        else if (!wcscmp(argv[i], L"--show-ea-env")) showEnv = true;
        else if (!wcscmp(argv[i], L"--list-games")) { list_games(); return 0; }
    }

    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(nullptr, dll, MAX_PATH);
    wchar_t* slash = wcsrchr(dll, L'\\');
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - dll), dllName);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { wprintf(L"DLL not found: %ls\n", dll); return 1; }
    if (!target) {
        if (!find_game(found, MAX_PATH)) {
            wprintf(L"Could not find starwarssquadrons.exe. Put its full path in launch.ini beside this program:\n[launch]\ngame=C:\\...\\STAR WARS Squadrons\\starwarssquadrons.exe\n");
            return 1;
        }
        target = found;
    }
    if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) { wprintf(L"Game not found: %ls\n", target); return 1; }
    const bool ea = is_ea_build(target);
    if (showEnv) {
        if (ea) set_ea_environment(target, true); else wprintf(L"%ls is not the EA app's copy; no EA environment.\n", target);
        return 0;
    }
    if (ea && !ea_app_listening()) {
        wprintf(L"This is the EA app's copy of the game, and the EA app isn't running (nothing on 127.0.0.1:3216).\n"
                L"Start the EA app, sign in, and run this again. Nothing was started.\n");
        return 1;
    }
    if (!ea) {   // a direct Steam launch needs steam_appid.txt in the game folder (Test 0); warn, never create it ourselves
        wchar_t appid[MAX_PATH]; wcscpy_s(appid, target); wchar_t* as = wcsrchr(appid, L'\\');
        if (as) { wcscpy_s(as + 1, MAX_PATH - (as + 1 - appid), L"steam_appid.txt");
                  if (!file_exists(appid)) wprintf(L"Note: %ls is missing. A direct launch usually needs it (one line: 1222730). See the README.\n", appid); }
    }

    {   // join mode with the README's placeholder still in place: refuse, with the fix
        wchar_t ini[MAX_PATH]; wcscpy_s(ini, dll); wchar_t* is = wcsrchr(ini, L'\\');
        if (is) wcscpy_s(is + 1, MAX_PATH - (is + 1 - ini), L"server.ini");
        wchar_t j[96] = L""; GetPrivateProfileStringW(L"server", L"join", L"", j, 96, ini);
        if (_wcsnicmp(j, L"HOST_ADDRESS_HERE", 17) == 0) {
            wprintf(L"server.ini still says join=HOST_ADDRESS_HERE.\nOpen %ls in Notepad and replace HOST_ADDRESS_HERE with the host's address\n"
                    L"(for example their Tailscale address 100.x.y.z), save, and run this again. Nothing was started.\n", ini);
            return 1;
        }
        if (j[0]) wprintf(L"Join mode: your match will connect to %ls.\n", j);
        else wprintf(L"Host/normal mode (no join= address in server.ini).\n");
    }
    static const wchar_t* const eacServices[] = { L"EasyAntiCheat", L"EasyAntiCheat_EOS" };
    for (const wchar_t* sv : eacServices)
        if (service_active(sv)) { wprintf(L"REFUSING: the %ls service is not stopped.\n", sv); return 2; }

    const wchar_t* targetName = wcsrchr(target, L'\\') ? wcsrchr(target, L'\\') + 1 : target;
    HANDLE ps = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(ps, &pe); ok; ok = Process32NextW(ps, &pe)) {
        if (icontains(pe.szExeFile, L"easyanticheat")) { wprintf(L"REFUSING: %ls is running.\n", pe.szExeFile); CloseHandle(ps); return 2; }
        if (_wcsicmp(pe.szExeFile, targetName) == 0) { wprintf(L"REFUSING: %ls is already running; close it first.\n", targetName); CloseHandle(ps); return 3; }
    }
    CloseHandle(ps);

    if (!yes) {
        wprintf(L"This starts the game directly (no EasyAntiCheat) with %ls loaded from the start.\n"
                L"It is the community server mod (experimental). Its settings are in server.ini beside this program.\n"
                L"The game's EasyAntiCheat DLL will load inactive, as with a direct launch.\n\n"
                L"Type YES and press Enter to launch, or just press Enter to cancel: ", dllName);
        char line[32] = {};
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "YES", 3) != 0) { wprintf(L"Cancelled; nothing was started.\n"); return 0; }
    }

    if (ea) set_ea_environment(target);
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, target);
    wchar_t* ds = wcsrchr(dir, L'\\'); if (ds) *ds = 0;
    wchar_t cmd[MAX_PATH + 4]; swprintf_s(cmd, L"\"%ls\"", target);
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(target, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir, &si, &pi)) {
        wprintf(L"CreateProcess failed (error %lu).\n", GetLastError()); return 5;
    }
    size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    auto ll = (PAPCFUNC)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!mem || !WriteProcessMemory(pi.hProcess, mem, dll, bytes, nullptr) || !QueueUserAPC(ll, pi.hThread, (ULONG_PTR)mem)) {
        wprintf(L"Could not queue the DLL load (error %lu); terminating the suspended process.\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return 5;
    }
    ResumeThread(pi.hThread);
    wprintf(L"Started pid %lu with %ls queued to load at startup.\nLogs: the logs folder beside this program (server_*.status.txt)\n",
            pi.dwProcessId, dllName);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    wprintf(L"Community server mod launcher, build %hs %hs\n", __DATE__, __TIME__);
    int code = run(argc, argv);
    bool yes = false;
    for (int i = 1; i < argc; i++) if (!wcscmp(argv[i], L"--yes") || !wcscmp(argv[i], L"--list-games")) yes = true;
    if (!yes) { wprintf(L"\nPress Enter to close.\n"); getchar(); }
    return code;
}
