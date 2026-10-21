// inject.exe: loads sws_probe.dll into an already-running starwarssquadrons.exe that was started WITHOUT EasyAntiCheat.
//
// Refuses (and changes nothing) if:
//   - no starwarssquadrons.exe is running, or more than one is;
//   - any EasyAntiCheat process is running, or an EasyAntiCheat service is not stopped;
//   - the probe DLL is already loaded.
// A direct launch still loads EAC's in-game DLL (easyanticheat_x64.dll) with the services stopped. In that case it
// explains and asks the user to type YES (Pseudo's decision, 2026-10-19; the risk on their account is theirs to take).
// Usage: inject.exe            (DLL expected beside inject.exe)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// True unless the service is absent or fully stopped.
static bool service_active(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return true;  // can't tell: treat as active (refuse)
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

static bool icontains(const wchar_t* hay, const wchar_t* needle) {
    size_t n = wcslen(needle);
    for (; *hay; hay++) if (_wcsnicmp(hay, needle, n) == 0) return true;
    return false;
}

static int run(const wchar_t* name);

// Pause before closing so a double-clicked console window stays open long enough to read.
// Usage: inject.exe [sws_probe.dll | sws_rules.dll]   (default sws_probe.dll, the pure observer)
int wmain(int argc, wchar_t** argv) {
    const wchar_t* name = argc > 1 ? argv[1] : L"sws_probe.dll";
    if (_wcsicmp(name, L"sws_probe.dll") != 0 && _wcsicmp(name, L"sws_rules.dll") != 0) {
        wprintf(L"Unknown DLL name %ls (expected sws_probe.dll or sws_rules.dll).\n", name);
        name = nullptr;
    }
    int code = name ? run(name) : 1;
    wprintf(L"\nPress Enter to close.\n");
    getchar();
    return code;
}

static int run(const wchar_t* name) {
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(nullptr, dll, MAX_PATH);
    wchar_t* slash = wcsrchr(dll, L'\\');
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - dll), name);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { wprintf(L"DLL not found: %ls\n", dll); return 1; }

    static const wchar_t* const eacServices[] = { L"EasyAntiCheat", L"EasyAntiCheat_EOS" };
    for (const wchar_t* sv : eacServices) {
        if (service_active(sv)) {
            wprintf(L"REFUSING: the %ls service is not stopped. Close the game and start starwarssquadrons.exe directly.\n", sv);
            return 2;
        }
    }

    DWORD pid = 0; int games = 0;
    HANDLE ps = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(ps, &pe); ok; ok = Process32NextW(ps, &pe)) {
        if (icontains(pe.szExeFile, L"easyanticheat")) {
            wprintf(L"REFUSING: EasyAntiCheat is running (%ls, pid %lu).\n"
                    L"Close the game and start starwarssquadrons.exe directly, not through Steam's Play button.\n", pe.szExeFile, pe.th32ProcessID);
            return 2;
        }
        if (_wcsicmp(pe.szExeFile, L"starwarssquadrons.exe") == 0) { pid = pe.th32ProcessID; games++; }
    }
    CloseHandle(ps);
    if (games == 0) { wprintf(L"starwarssquadrons.exe is not running. Start it directly from the game folder first.\n"); return 3; }
    if (games > 1) { wprintf(L"More than one starwarssquadrons.exe is running; refusing.\n"); return 3; }

    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (ms == INVALID_HANDLE_VALUE) { wprintf(L"Could not list the game's modules (error %lu).\n", GetLastError()); return 4; }
    MODULEENTRY32W me{ sizeof(me) };
    bool eacModule = false;
    for (BOOL ok = Module32FirstW(ms, &me); ok; ok = Module32NextW(ms, &me)) {
        if (icontains(me.szModule, L"easyanticheat")) eacModule = true;
        if (_wcsicmp(me.szModule, L"sws_probe.dll") == 0 || _wcsicmp(me.szModule, L"sws_rules.dll") == 0) {
            // both hook the same sites; only one may be loaded per game run
            wprintf(L"%ls is already loaded in this game; restart the game to load a different one.\n", me.szModule);
            CloseHandle(ms); return 0;
        }
    }
    CloseHandle(ms);

    if (eacModule) {
        wprintf(L"Note: the game has EasyAntiCheat's in-game DLL loaded, but no EasyAntiCheat service or process is running,\n"
                L"so EAC is not active (this is what a direct launch looks like). Whether that inactive DLL records anything\n"
                L"locally is not known for certain.\n\n"
                L"Type YES and press Enter to load the probe anyway, or just press Enter to cancel: ");
        char line[32] = {};
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "YES", 3) != 0) {
            wprintf(L"Cancelled; nothing was changed.\n");
            return 0;
        }
    }

    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!h) { wprintf(L"OpenProcess failed (error %lu).\n", GetLastError()); return 5; }
    size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem || !WriteProcessMemory(h, mem, dll, bytes, nullptr)) { wprintf(L"Could not write the DLL path (error %lu).\n", GetLastError()); return 5; }
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(h, nullptr, 0, ll, mem, 0, nullptr);
    if (!t) { wprintf(L"CreateRemoteThread failed (error %lu).\n", GetLastError()); return 5; }
    WaitForSingleObject(t, 15000);
    DWORD code = 0; GetExitCodeThread(t, &code);
    VirtualFreeEx(h, mem, 0, MEM_RELEASE);
    CloseHandle(t); CloseHandle(h);
    if (!code) { wprintf(L"LoadLibrary failed inside the game.\n"); return 6; }
    wprintf(L"Probe loaded into pid %lu. Status and logs: E:\\sws_community\\probe_logs\\\n"
            L"Markers while flying: F8 = 1, F9 = 2, F10 = 3.\n", pid);
    return 0;
}
