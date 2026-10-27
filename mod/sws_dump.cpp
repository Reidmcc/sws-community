// sws_dump.dll: a one-time capture of a game build whose code is encrypted on disk (the EA app's copy).
// Loaded by sws_launch.exe (--dll sws_dump.dll) in place of the mod. It hooks nothing and changes nothing in the
// game: a background thread watches the start of the main module's first code section until its bytes stop
// looking encrypted (entropy below 7.2 bits per byte), waits SWS_DUMP_DELAY seconds (default 30) so the wrapper
// finishes, then copies the whole main module image, region by region, into sws_dump_image.bin beside this DLL.
// Unreadable regions are written as zeros and listed in sws_dump_log.txt with their protection.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <share.h>

static HMODULE g_self;
static FILE* g_log;

static void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// Entropy of n bytes at p, or -1 if the bytes can't be read.
static double entropy(const unsigned char* p, size_t n) {
    static unsigned counts[256];
    memset(counts, 0, sizeof counts);
    __try { for (size_t i = 0; i < n; i++) counts[p[i]]++; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    double h = 0;
    for (int i = 0; i < 256; i++) if (counts[i]) { double q = (double)counts[i] / n; h -= q * log2(q); }
    return h;
}

static bool copy_region(unsigned char* dst, const unsigned char* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void path_beside(wchar_t* out, const wchar_t* name) {
    GetModuleFileNameW(g_self, out, MAX_PATH);
    wchar_t* s = wcsrchr(out, L'\\');
    wcscpy_s(s + 1, MAX_PATH - (s + 1 - out), name);
}

static DWORD WINAPI dump_thread(void*) {
    auto base = (unsigned char*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    size_t imageSize = nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    int nsec = nt->FileHeader.NumberOfSections;
    logf("main module at %p, image size 0x%zx, %d sections", base, imageSize, nsec);
    IMAGE_SECTION_HEADER* code = nullptr;
    for (int i = 0; i < nsec; i++) {
        char name[9] = {}; memcpy(name, sec[i].Name, 8);
        logf("  %-8s va 0x%08lx size 0x%08lx flags 0x%08lx", name, sec[i].VirtualAddress, sec[i].Misc.VirtualSize, sec[i].Characteristics);
        if (!code && (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) code = &sec[i];
    }
    if (!code) { logf("no executable section; nothing to watch"); return 0; }

    size_t probe = code->Misc.VirtualSize < (4u << 20) ? code->Misc.VirtualSize : (4u << 20);
    const unsigned char* watch = base + code->VirtualAddress;
    int delay = 30;
    if (const char* d = getenv("SWS_DUMP_DELAY")) delay = atoi(d);
    double last = -2;
    DWORD t0 = GetTickCount();
    for (;;) {
        double h = entropy(watch, probe);
        if (fabs(h - last) > 0.05) { logf("first code section: %.3f bits per byte (%.1f s after load)", h, (GetTickCount() - t0) / 1000.0); last = h; }
        if (h >= 0 && h < 7.2) break;
        if (GetTickCount() - t0 > 600000) { logf("still encrypted after 10 minutes; giving up"); return 0; }
        Sleep(250);
    }
    logf("decrypted; dumping in %d s", delay);
    Sleep(delay * 1000);

    wchar_t path[MAX_PATH]; path_beside(path, L"sws_dump_image.bin");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { logf("could not create the dump file (error %lu)", GetLastError()); return 0; }
    const size_t chunk = 16u << 20;
    auto buf = (unsigned char*)VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    size_t unreadable = 0, off = 0;
    while (off < imageSize) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(base + off, &mbi, sizeof mbi);
        size_t regionEnd = (unsigned char*)mbi.BaseAddress + mbi.RegionSize - base;
        if (regionEnd > imageSize) regionEnd = imageSize;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (!readable) logf("  rva 0x%08zx..0x%08zx not readable (state 0x%lx protect 0x%lx), written as zeros", off, regionEnd, mbi.State, mbi.Protect);
        while (off < regionEnd) {
            size_t n = regionEnd - off < chunk ? regionEnd - off : chunk;
            if (!readable || !copy_region(buf, base + off, n)) { memset(buf, 0, n); if (readable) logf("  rva 0x%08zx: read failed, written as zeros", off); unreadable += n; }
            DWORD w; WriteFile(f, buf, (DWORD)n, &w, nullptr);
            off += n;
        }
    }
    CloseHandle(f);
    VirtualFree(buf, 0, MEM_RELEASE);
    logf("dump written: 0x%zx bytes, 0x%zx of them unreadable, first code section now %.3f bits per byte",
         imageSize, unreadable, entropy(watch, probe));
    logf("done; the game keeps running and you can quit it normally");
    fclose(g_log); g_log = nullptr;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        wchar_t path[MAX_PATH]; path_beside(path, L"sws_dump_log.txt");
        g_log = _wfsopen(path, L"w", _SH_DENYNO); if (!g_log) return TRUE;
        logf("sws_dump loaded into pid %lu", GetCurrentProcessId());
        CloseHandle(CreateThread(nullptr, 0, dump_thread, nullptr, 0, nullptr));
    }
    return TRUE;
}
