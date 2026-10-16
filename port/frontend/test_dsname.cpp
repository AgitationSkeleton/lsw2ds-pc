/* The DS nickname encoder - the cap and the fallback, pinned.
 *
 * The cartridge's block holds ten UTF-16 units and the fork's names are
 * printable ASCII up to 24; what happens at the seam is the whole of this
 * module's judgement, and it is checkable without a ROM. */

#include "dsname.h"

#include <cstdio>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok: %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL: %s\n", what);
    }
}

int main() {
    uint16_t out[kLsw2DsNicknameChars];

    /* The default player name, exactly. */
    int n = lsw2_dsname_encode("Maclunkey", out);
    check(n == 9, "Maclunkey is nine characters");
    check(out[0] == 'M' && out[8] == 'y' && out[9] == 0,
          "widened to UTF-16 in place, the tenth unit clear");

    /* THE CAP IS THE CARTRIDGE'S: ten units, no more. */
    n = lsw2_dsname_encode("ABCDEFGHIJKLMNOP", out);
    check(n == 10, "a sixteen-character name is cut at ten");
    check(out[9] == 'J', "and the tenth kept is the tenth typed");

    /* Only printable ASCII crosses; a stray byte is skipped, not replaced. */
    /* "Ma" + a control byte + "c" + two UTF-8 bytes + "lunkey": nine
     * characters survive, and the gaps close. The literal is spliced because
     * a hex escape is greedy - "\x01c" would be the single byte 0x1C. */
    n = lsw2_dsname_encode("Ma\x01" "c\xC3\xA9" "lunkey", out);
    check(n == 9 && out[2] == 'c' && out[3] == 'l' && out[8] == 'y',
          "control and non-ASCII bytes are dropped, the rest closes up");

    /* Nothing left: the sanitiser's fallback, never a blank. */
    n = lsw2_dsname_encode("", out);
    check(n == 6 && out[0] == 'P' && out[5] == 'r', "an empty name becomes Player");
    n = lsw2_dsname_encode("\x7f\x80", out);
    check(n == 6, "an unprintable name becomes Player too");
    n = lsw2_dsname_encode(nullptr, out);
    check(n == 6, "a null name becomes Player");

    /* All ten units are always written, so a shorter name after a longer one
     * leaves no tail from the earlier one in the block. */
    lsw2_dsname_encode("ABCDEFGHIJ", out);
    lsw2_dsname_encode("Al", out);
    check(out[2] == 0 && out[9] == 0, "a shorter name clears the units it does not use");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
