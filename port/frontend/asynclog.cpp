/* See asynclog.h. */
#include "asynclog.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <cstdint>
/* Declared here rather than through <io.h>: on this include path that name is
 * the framework's NDS I/O-register header (runner/src/io.h), which shadows the
 * C runtime's. These are the CRT's own exports, unchanged. */
extern "C" {
int __cdecl _open_osfhandle(intptr_t osfhandle, int flags);
int __cdecl _dup2(int fd1, int fd2);
int __cdecl _close(int fd);
}
#endif

#if defined(_WIN32)
namespace {

/* logs\lsw2-<YYYYMMDD>-<HHMMSS>-<pid>.log beside the executable, the newest
 * kKeepLogs kept. INVALID_HANDLE_VALUE when it cannot be made - the game then
 * logs exactly as it did before. */
constexpr int kKeepLogs = 20;

HANDLE lsw2_asynclog_open_session_file() {
    char exe[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return INVALID_HANDLE_VALUE;
    char* slash = std::strrchr(exe, '\\');
    if (!slash) return INVALID_HANDLE_VALUE;
    slash[1] = '\0';
    std::string dir = std::string(exe) + "logs";
    CreateDirectoryA(dir.c_str(), nullptr);

    /* PRUNE FIRST: the names sort by time, so the oldest go. */
    std::vector<std::string> old;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\lsw2-*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { old.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + (kKeepLogs - 1) < old.size(); ++i)
        DeleteFileA((dir + "\\" + old[i]).c_str());

    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[96];
    std::snprintf(name, sizeof name, "\\lsw2-%04u%02u%02u-%02u%02u%02u-%lu.log",
                  unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay),
                  unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond),
                  static_cast<unsigned long>(GetCurrentProcessId()));
    return CreateFileA((dir + name).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace
#endif

void lsw2_asynclog_start() {
#if defined(_WIN32)
    static bool started = false;
    if (started) return;
    started = true;

    const char* sync = std::getenv("LSW2_SYNC_LOG");
    if (sync && *sync == '1') return;

    /* Where the lines really go: a console window, or the file a launcher
     * redirected stderr into. */
    HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);
    const bool have_err = std_err != nullptr && std_err != INVALID_HANDLE_VALUE;
    const DWORD err_type = have_err ? GetFileType(std_err) : FILE_TYPE_UNKNOWN;
    const bool redirected = err_type == FILE_TYPE_DISK || err_type == FILE_TYPE_PIPE;

    /* AND A LOG FILE WHEN NOTHING KEEPS ONE.
     * Crashes in a double-clicked build left no log at all: a double-click sends
     * stderr to the console window, which dies with the game. So unless a
     * launcher already redirected stderr into a file, the same lines also go
     * to logs\lsw2-<date>-<time>-<pid>.log beside the executable. */
    HANDLE file = redirected ? INVALID_HANDLE_VALUE : lsw2_asynclog_open_session_file();
    const bool have_file = file != INVALID_HANDLE_VALUE;
    if (!have_err && !have_file) return;

    /* The writer thread holds its OWN handle to the destination, because the
     * _dup2 below closes fd 2 and, with it, the handle fd 2 was using. */
    HANDLE real = nullptr;
    if (have_err &&
        !DuplicateHandle(GetCurrentProcess(), std_err, GetCurrentProcess(), &real,
                         0, FALSE, DUPLICATE_SAME_ACCESS))
        real = nullptr;
    if (!real && !have_file) return;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, nullptr, 1u << 20)) {
        if (real) CloseHandle(real);
        if (have_file) CloseHandle(file);
        return;
    }

    /* The C runtime's stderr is re-pointed at the pipe underneath the FILE, so
     * every existing fprintf(stderr, ...) - the port's, the framework's, the
     * vendored melonDS's - takes the new path without being touched. Text
     * mode, as fd 2 was, so line endings reach the destination unchanged. */
    std::fflush(stderr);
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(wr), _O_WRONLY | _O_TEXT);
    if (fd < 0) {
        CloseHandle(rd);
        CloseHandle(wr);
        if (real) CloseHandle(real);
        if (have_file) CloseHandle(file);
        return;
    }
    /* A GUI-subsystem process (the packaged build, tools/package_build.py)
     * starts with no stderr at all: _fileno(stderr) is negative, the _dup2
     * below failed, and the log file stayed empty - exactly the case it
     * exists for. Give the stream a real descriptor first, unbuffered as the
     * CRT's stderr is, so a crash still leaves every line before it. */
    if (_fileno(stderr) < 0) {
        if (!std::freopen("NUL", "w", stderr)) {
            _close(fd);
            CloseHandle(rd);
            if (real) CloseHandle(real);
            if (have_file) CloseHandle(file);
            return;
        }
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    if (_dup2(fd, _fileno(stderr)) != 0) {
        _close(fd);
        CloseHandle(rd);
        if (real) CloseHandle(real);
        if (have_file) CloseHandle(file);
        return;
    }

    std::thread([rd, real, file] {
        char buf[16384];
        DWORD n = 0;
        HANDLE outs[2] = {real, file};
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) {
            for (HANDLE& out : outs) {
                if (!out || out == INVALID_HANDLE_VALUE) continue;
                DWORD off = 0;
                while (off < n) {
                    DWORD wrote = 0;
                    /* A destination that stops accepting (a console closed
                     * under us) is dropped; the other keeps getting lines. */
                    if (!WriteFile(out, buf + off, n - off, &wrote, nullptr)) {
                        out = nullptr;
                        break;
                    }
                    off += wrote;
                }
            }
            if (!outs[0] && (!outs[1] || outs[1] == INVALID_HANDLE_VALUE)) return;
        }
    }).detach();
#endif
}
