// sws_launch.exe: starts STAR WARS: Squadrons with sws_server.dll loaded before the game's own code runs.
//
// The dedicated-server flag lives in the MainLoop object, which the game builds at startup, so attaching to a running
// game (inject.exe) is too late. This launcher creates the game process suspended, queues LoadLibraryW(sws_server.dll)
// as an APC on its main thread, and resumes it: the DLL loads during process initialisation ("early-bird" APC) and
// installs its hooks before the main loop exists.
//
// Refuses (and starts nothing) if an EasyAntiCheat service is not stopped or any EAC process is running, or if the game
// is already running. Asks the user to type YES.
//
// Usage: sws_launch.exe                      (game from the default Steam path, DLL sws_server.dll beside this exe)
//        sws_launch.exe --target <exe> --dll <name> --yes     (test use)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const wchar_t* kGame = L"E:\\SteamLibrary\\steamapps\\common\\STAR WARS Squadrons\\starwarssquadrons.exe";

static bool file_exists(const wchar_t* p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

// The game's path, in order: launch.ini beside this exe ([launch] game=...), Steam's library folders (registry SteamPath
// -> steamapps\libraryfolders.vdf "path" entries -> <lib>\steamapps\common\STAR WARS Squadrons), then the old default.
static bool find_game(wchar_t* out, size_t n) {
    wchar_t ini[MAX_PATH]; GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wchar_t* sl = wcsrchr(ini, L'\\'); if (sl) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - ini), L"launch.ini");
    wchar_t g[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"launch", L"game", L"", g, MAX_PATH, ini);
    if (g[0] && file_exists(g)) { wcscpy_s(out, n, g); wprintf(L"Game (from launch.ini): %ls\n", out); return true; }
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
            if (file_exists(cand)) { wcscpy_s(out, n, cand); wprintf(L"Game (from Steam's library folders): %ls\n", out); return true; }
        }
    }
    if (file_exists(kGame)) { wcscpy_s(out, n, kGame); return true; }
    return false;
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
    bool yes = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--target") && i + 1 < argc) target = argv[++i];
        else if (!wcscmp(argv[i], L"--dll") && i + 1 < argc) dllName = argv[++i];
        else if (!wcscmp(argv[i], L"--yes")) yes = true;
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
    {   // a direct launch needs steam_appid.txt in the game folder (Test 0); warn, never create it ourselves
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
    for (int i = 1; i < argc; i++) if (!wcscmp(argv[i], L"--yes")) yes = true;
    if (!yes) { wprintf(L"\nPress Enter to close.\n"); getchar(); }
    return code;
}
