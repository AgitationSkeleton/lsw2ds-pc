/* The name sanitiser, against what a hostile peer can actually send.
 *
 *   g++ -std=c++20 port/net/test_name.cpp port/net/lsw2_name.cpp -o test_name
 */

#include "lsw2_name.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

void expect(const char* in, const char* want, const char* what,
            size_t cap = 24) {
    char out[64];
    if (cap > sizeof out) cap = sizeof out;
    lsw2_sanitise_name(in, out, cap, "Player");
    const bool ok = std::strcmp(out, want) == 0;
    if (!ok) std::printf("       got \"%s\" want \"%s\"\n", out, want);
    check(ok, what);
}

}  // namespace

int main() {
    std::printf("name sanitiser\n");

    expect("Maclunkey", "Maclunkey", "an ordinary name is untouched");

    /* THE POINT OF THE WHOLE FILE. A newline in a name would break any line-
     * oriented log and put a second row into a menu that allocated one. */
    expect("Mac\nlunkey", "Maclunkey", "newlines are dropped");
    expect("Mac\rlunkey", "Maclunkey", "carriage returns are dropped");
    expect("Mac\tlunkey", "Maclunkey", "tabs are dropped");
    expect("\x01\x02\x03", "Player", "a name of only control bytes falls back");

    /* Signed-char trap: 0xC3 arrives negative, and a comparison that forgets to
     * cast keeps it. See the note in lsw2_name.cpp. */
    expect("Ma\xC3\xA9lunkey", "Malunkey", "high bytes are dropped, not kept");

    expect("", "Player", "an empty name falls back");
    expect(nullptr, "Player", "a null name falls back");

    expect("   Maclunkey   ", "Maclunkey", "leading and trailing space go");
    expect("Mac     lunkey", "Mac lunkey", "runs of spaces collapse");
    expect("   ", "Player", "a name of only spaces falls back");

    /* THE FIXED SLOT. Lsw2ConsoleState::peers is char[24], so anything longer
     * has to be cut here rather than by whatever copies it next. */
    char out[8];
    const size_t n = lsw2_sanitise_name("ABCDEFGHIJKLMNOP", out, sizeof out,
                                        "Player");
    check(n == sizeof out - 1, "an overlong name is cut to the buffer");
    check(out[sizeof out - 1] == '\0', "and stays terminated");
    check(std::strcmp(out, "ABCDEFG") == 0, "keeping the front of it");

    /* A cap of one leaves room for the terminator only. */
    char tiny[1];
    lsw2_sanitise_name("Maclunkey", tiny, sizeof tiny, "Player");
    check(tiny[0] == '\0', "a one-byte buffer gets just a terminator");

    /* No fallback means empty is allowed to stay empty. */
    char none[8];
    lsw2_sanitise_name("\x01", none, sizeof none, nullptr);
    check(none[0] == '\0', "without a fallback, empty stays empty");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
