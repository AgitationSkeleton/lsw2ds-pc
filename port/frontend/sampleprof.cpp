/* See sampleprof.h. */
#include "sampleprof.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
std::atomic<int> g_state{-1};
std::atomic<int> g_fps_x10{0};
}  // namespace

void lsw2_sampleprof_note(int game_state, double fps) {
    g_state.store(game_state, std::memory_order_relaxed);
    g_fps_x10.store(int(fps * 10.0), std::memory_order_relaxed);
}

void lsw2_sampleprof_start() {
#if defined(_WIN32)
    static bool started = false;
    if (started) return;
    started = true;
    const char* prefix_env = std::getenv("LSW2_SAMPLE_PROF");
    if (!prefix_env || !*prefix_env) return;
    const std::string prefix(prefix_env);

    HANDLE target = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        std::fprintf(stderr, "[sampleprof] could not open the game thread\n");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "[sampleprof] sampling the game thread every ~1 ms into %s.N.txt "
                 "(module base %p)\n", prefix.c_str(), reinterpret_cast<void*>(base));

    std::thread([target, base, prefix] {
        std::unordered_map<uint64_t, uint32_t> hist;
        hist.reserve(1u << 16);
        uint64_t samples = 0;
        int file_index = 0;
        ULONGLONG window_start = GetTickCount64();
        for (;;) {
            Sleep(1);
            if (SuspendThread(target) == DWORD(-1)) return;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(target, &ctx);
            ResumeThread(target);
            if (ok) {
                ++hist[uint64_t(ctx.Rip)];
                ++samples;
            }
            const ULONGLONG now = GetTickCount64();
            if (now - window_start < 10000u) continue;
            window_start = now;

            std::vector<std::pair<uint64_t, uint32_t>> rows(hist.begin(), hist.end());
            std::sort(rows.begin(), rows.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            const std::string path = prefix + "." + std::to_string(file_index++) + ".txt";
            if (std::FILE* f = std::fopen(path.c_str(), "w")) {
                std::fprintf(f, "base %llx\nsamples %llu\nstate %d\nfps %.1f\n",
                             static_cast<unsigned long long>(base),
                             static_cast<unsigned long long>(samples),
                             g_state.load(std::memory_order_relaxed),
                             g_fps_x10.load(std::memory_order_relaxed) / 10.0);
                const size_t n = std::min<size_t>(rows.size(), 20000u);
                for (size_t i = 0; i < n; ++i)
                    std::fprintf(f, "%llx %u\n",
                                 static_cast<unsigned long long>(rows[i].first),
                                 rows[i].second);
                std::fclose(f);
            }
            hist.clear();
            samples = 0;
        }
    }).detach();
#endif
}
