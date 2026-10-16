/* The recording format, pinned without a file, a window, or a guest.
 *
 * The parse/format pair is split out of inputlog.cpp for the same reason
 * customization_policy.cpp and enhanced_policy.cpp are split: a recording is
 * only worth making if it can be read back exactly, and that is a property of
 * two pure functions rather than of a running game. */

#include "inputlog.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); }
}

int main() {
    Lsw2InputFrame f;
    uint64_t v = 0;

    /* ROUND TRIP, which is the whole contract. */
    for (unsigned keys : {0x000u, 0x001u, 0x800u, 0xFFFu}) {
        for (unsigned x : {0u, 128u, 255u}) {
            for (unsigned y : {0u, 96u, 191u}) {
                Lsw2InputFrame in;
                in.keys = uint16_t(keys);
                in.touch_x = uint16_t(x);
                in.touch_y = uint16_t(y);
                in.touch_down = true;
                char buf[64];
                lsw2_inputlog_format(buf, sizeof buf, in, 12345);
                Lsw2InputFrame out;
                uint64_t vb = 0;
                check(lsw2_inputlog_parse(buf, out, vb), "round trip parses");
                check(vb == 12345, "vblank survives");
                check(out.keys == in.keys, "keys survive");
                /* THE PRECISION CLAIM, exercised at both edges of both axes:
                 * a screen pixel must come back as itself, because the touch
                 * is stored as x<<4 in a 12-bit domain and the whole point of
                 * recording pixels is that nothing is lost. */
                check(out.touch_x == in.touch_x, "touch x is exact");
                check(out.touch_y == in.touch_y, "touch y is exact");
                check(out.touch_down, "pen state survives");
            }
        }
    }

    /* COMMENTS AND BLANKS are skipped, not treated as frames. */
    check(!lsw2_inputlog_parse("# lsw2input 1", f, v), "header is not an event");
    check(!lsw2_inputlog_parse("", f, v), "empty line is not an event");
    check(!lsw2_inputlog_parse("   \n", f, v), "blank line is not an event");

    /* A TRUNCATED LAST LINE - the tail of a recording whose process was killed
     * - must be rejected whole rather than half-applied. */
    check(!lsw2_inputlog_parse("120 3F", f, v), "short line is rejected");
    check(!lsw2_inputlog_parse("120 3F 10", f, v), "three fields rejected");

    /* OUT OF RANGE is rejected: a corrupt coordinate would be applied to the
     * guest as a real touch somewhere impossible. */
    check(!lsw2_inputlog_parse("1 000 256 0 1", f, v), "x > 255 rejected");
    check(!lsw2_inputlog_parse("1 000 0 192 1", f, v), "y > 191 rejected");
    check(!lsw2_inputlog_parse("1 1000 0 0 1", f, v), "keys > 0xFFF rejected");

    /* A RELEASED PEN records no position, so parsing one back must not invent
     * a touch. */
    Lsw2InputFrame up;
    up.keys = 0x010;
    up.touch_down = false;
    char buf[64];
    lsw2_inputlog_format(buf, sizeof buf, up, 7);
    Lsw2InputFrame back;
    uint64_t vb = 0;
    check(lsw2_inputlog_parse(buf, back, vb), "pen-up line parses");
    check(!back.touch_down, "pen stays up");
    check(back.keys == 0x010, "keys survive a pen-up line");

    std::printf("%s: %d checks, %d failed\n",
                g_failed ? "FAIL" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
