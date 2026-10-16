/* See presentshot.h. */

#include "presentshot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kHeight = 192;

/* HOW OFTEN A FRAME IS WRITTEN. Every quarter second at 60fps.
 *
 * Not every frame: two 144KB PPMs sixty times a second is 17MB/s of disk for a
 * picture nobody is looking at that fast. Not on demand either - there is no
 * command channel to demand it on, which is the whole reason this file exists.
 * A quarter second is short enough that "tap, wait, read" sees the result of
 * the tap and long enough that the writing is not the slowest thing in the
 * frame. */
constexpr int kInterval = 15;

/* The directory from LSW2_PRESENT_SHOT, resolved once. Empty means off, which
 * is the case for every run that has not asked for this. */
const std::string& shot_dir() {
    static const std::string dir = [] {
        const char* d = std::getenv("LSW2_PRESENT_SHOT");
        /* SAY WHAT IT RESOLVED TO, once, and only when it was asked for.
         *
         * A run with the variable set produced no files and no message: the
         * fopen report below never runs when the directory is empty, so "the
         * variable did not arrive" and "the port never presented" looked
         * identical from the log. Which one it is decides whether to go
         * looking at the environment or at the present path, and that is two
         * different afternoons. */
        if (d && *d)
            std::fprintf(stderr, "[fork] present shot: capturing to %s\n", d);
        return std::string(d ? d : "");
    }();
    return dir;
}

int g_counter[2] = {0, 0};

/* SAY IT ONCE, so a run with the variable set but a bad path does not look
 * like a run where the port simply never presented anything. A capture tool
 * that fails silently is what this file was written to replace. */
void report_once(int screen, const std::string& path, bool ok) {
    static bool said[2] = {false, false};
    if (screen < 0 || screen > 1 || said[screen]) return;
    said[screen] = true;
    std::fprintf(stderr, "[fork] present shot: screen %d -> %s%s\n", screen,
                 path.c_str(), ok ? "" : "  FAILED TO OPEN");
}

}  // namespace

void lsw2_presentshot_frame(int screen, const uint32_t* pixels, int width) {
    if (shot_dir().empty()) return;
    if (!pixels || width <= 0 || screen < 0 || screen > 1) return;

    if (++g_counter[screen] < kInterval) return;
    g_counter[screen] = 0;

    /* A FIXED NAME PER SCREEN, overwritten. A reader wants "what is on the
     * touch screen now", and a directory filling with numbered frames makes
     * that a question about which file is newest. */
    const std::string path = shot_dir() + (screen == 0 ? "/present-top.ppm"
                                                       : "/present-bottom.ppm");

    /* WRITTEN WHOLE, THEN RENAMED, because the reader is another process
     * polling the same path. Writing in place hands out half a frame whenever
     * the read lands mid-write, and a torn PPM decodes as a truncated-file
     * error that looks like the port crashed. */
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    report_once(screen, path, f != nullptr);
    if (!f) return;

    std::fprintf(f, "P6\n%d %d\n255\n", width, kHeight);

    std::vector<unsigned char> row(size_t(width) * 3u);
    for (int y = 0; y < kHeight; ++y) {
        const uint32_t* src = pixels + size_t(y) * size_t(width);
        for (int x = 0; x < width; ++x) {
            /* ARGB8888, the same order debug_server.cpp unpacks for its own
             * reply - so a PPM from here and one from `pilot shot` are
             * directly comparable, which is the point of having both. */
            const uint32_t px = src[x];
            row[size_t(x) * 3u + 0] = static_cast<unsigned char>(px >> 16);
            row[size_t(x) * 3u + 1] = static_cast<unsigned char>(px >> 8);
            row[size_t(x) * 3u + 2] = static_cast<unsigned char>(px);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);

    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}
