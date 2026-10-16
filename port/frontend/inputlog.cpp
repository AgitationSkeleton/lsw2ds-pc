/* See inputlog.h. */

#include "inputlog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kMagic = "lsw2input";
constexpr int kVersion = 1;

Lsw2InputMode g_mode = Lsw2InputMode::Off;
std::FILE* g_file = nullptr;

Lsw2InputFrame g_last;          /* what was written or applied last     */
bool g_have_last = false;

/* The next event waiting to be applied, and whether one is loaded. */
Lsw2InputFrame g_pending;
uint64_t g_pending_vblank = 0;
bool g_pending_valid = false;
bool g_finished = false;

bool same(const Lsw2InputFrame& a, const Lsw2InputFrame& b) {
    if (a.keys != b.keys || a.touch_down != b.touch_down) return false;
    /* POSITION ONLY MATTERS WHILE THE PEN IS DOWN. A released stylus has no
     * coordinates the guest can read, so recording a moving mouse over a
     * released pen would fill the file with events that change nothing. */
    if (!a.touch_down) return true;
    return a.touch_x == b.touch_x && a.touch_y == b.touch_y;
}

/* Read the next event into g_pending, or mark the replay finished. */
void load_next() {
    g_pending_valid = false;
    if (!g_file) { g_finished = true; return; }
    char line[256];
    while (std::fgets(line, sizeof line, g_file)) {
        if (lsw2_inputlog_parse(line, g_pending, g_pending_vblank)) {
            g_pending_valid = true;
            return;
        }
    }
    g_finished = true;
}

}  // namespace

bool lsw2_inputlog_parse(const char* line, Lsw2InputFrame& f,
                         uint64_t& vblank) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '\0' || *line == '#' || *line == '\n' || *line == '\r')
        return false;

    unsigned long long v = 0;
    unsigned keys = 0, x = 0, y = 0, down = 0;
    /* FIVE FIELDS OR NOTHING. A short line is a truncated write - the last
     * line of a recording whose process was killed - and taking the first
     * three fields of one would apply a frame that was never finished. */
    if (std::sscanf(line, "%llu %x %u %u %u", &v, &keys, &x, &y, &down) != 5)
        return false;
    if (keys > 0x0FFFu || x > 255u || y > 191u || down > 1u) return false;

    vblank = uint64_t(v);
    f.keys = uint16_t(keys);
    f.touch_x = uint16_t(x);
    f.touch_y = uint16_t(y);
    f.touch_down = down != 0;
    return true;
}

void lsw2_inputlog_format(char* buf, unsigned n, const Lsw2InputFrame& f,
                          uint64_t vblank) {
    if (!buf || n == 0) return;
    std::snprintf(buf, n, "%llu %03X %u %u %u",
                  static_cast<unsigned long long>(vblank),
                  unsigned(f.keys), unsigned(f.touch_x), unsigned(f.touch_y),
                  f.touch_down ? 1u : 0u);
}

bool lsw2_inputlog_open(const char* record_path, const char* replay_path) {
    lsw2_inputlog_close();

    if (record_path && replay_path) {
        std::fprintf(stderr,
                     "[input] --record and --replay together would truncate "
                     "the file being replayed; refusing\n");
        return false;
    }
    if (!record_path && !replay_path) return true;

    const char* path = record_path ? record_path : replay_path;
    g_file = std::fopen(path, record_path ? "w" : "r");
    if (!g_file) {
        std::fprintf(stderr, "[input] cannot open %s for %s\n", path,
                     record_path ? "recording" : "replay");
        return false;
    }

    if (record_path) {
        g_mode = Lsw2InputMode::Record;
        std::fprintf(g_file, "# %s %d\n", kMagic, kVersion);
        std::fprintf(g_file, "# vblank keys(hex) touch_x touch_y pen_down\n");
        std::fprintf(stderr, "[input] recording to %s\n", path);
    } else {
        g_mode = Lsw2InputMode::Replay;
        std::fprintf(stderr, "[input] replaying %s\n", path);
        load_next();
        if (g_finished)
            std::fprintf(stderr, "[input] %s holds no events\n", path);
    }
    g_have_last = false;
    return true;
}

void lsw2_inputlog_close() {
    if (g_file) {
        if (g_mode == Lsw2InputMode::Record) std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_mode = Lsw2InputMode::Off;
    g_have_last = false;
    g_pending_valid = false;
    g_finished = false;
}

Lsw2InputMode lsw2_inputlog_mode() { return g_mode; }

bool lsw2_inputlog_replay_finished() {
    return g_mode == Lsw2InputMode::Replay && g_finished;
}

void lsw2_inputlog_frame(Lsw2InputFrame& f, uint64_t vblank) {
    if (g_mode == Lsw2InputMode::Off || !g_file) return;

    if (g_mode == Lsw2InputMode::Record) {
        if (g_have_last && same(f, g_last)) return;
        char line[64];
        lsw2_inputlog_format(line, sizeof line, f, vblank);
        std::fprintf(g_file, "%s\n", line);
        /* FLUSHED EVERY EVENT. A recording is made during exactly the kind of
         * session that ends in a crash or a kill, and a buffered tail is the
         * part that mattered. Events are rare - only changes - so this costs
         * far less than it would per frame. */
        std::fflush(g_file);
        g_last = f;
        g_have_last = true;
        return;
    }

    /* REPLAY. Apply every event whose vblank has arrived, not just one: a
     * stutter can leave several due at once, and applying them in order ends
     * on the newest, which is the state that frame should have. */
    while (g_pending_valid && g_pending_vblank <= vblank) {
        g_last = g_pending;
        g_have_last = true;
        load_next();
    }
    /* HOLD THE LAST STATE. Between events nothing changed, which is exactly
     * what "only changes are written" means - and after the final event the
     * held state persists, so a recording that ends with a button down does
     * not silently release it. */
    if (g_have_last) f = g_last;
}

/* The framework's `--record` / `--replay` land here. Declared `extern "C"` and
 * weakly defaulted in the runner (patch 0006) for the same reason
 * `lsw2_register_dispatch_overrides` is: the runner still builds standalone,
 * and the port owns the format. */
extern "C" bool lsw2_inputlog_set_paths(const char* record, const char* replay) {
    return lsw2_inputlog_open(record, replay);
}
