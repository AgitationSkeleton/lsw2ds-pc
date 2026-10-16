/* The join and disconnect popups: the wording, and the sink.
 *
 *   g++ -std=c++20 port/frontend/test_forkpopup.cpp \
 *       port/frontend/forkpopup.cpp -o test_forkpopup
 *
 * The wording is SPECIFIED exactly, so it is exactly the kind of thing
 * that gets paraphrased in a rewrite and noticed by nobody until it is on
 * screen. Pinning it costs two lines and makes a silent drift into a red.
 */

#include "forkpopup.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

int g_sink_calls = 0;
char g_last[128];

void record(const char* text) {
    ++g_sink_calls;
    std::snprintf(g_last, sizeof g_last, "%s", text);
}

}  // namespace

int main() {
    std::printf("fork popups\n");

    /* --- the specified wording, punctuation included -------------------- */
    char text[kLsw2PopupTextBytes];
    lsw2_popup_join_text("Maclunkey", text, sizeof text);
    check(std::strcmp(text, "Maclunkey has joined the game!") == 0,
          "the join message is the specified one, exclamation mark included");

    lsw2_popup_disconnect_text("Maclunkey", text, sizeof text);
    check(std::strcmp(text, "Maclunkey has disconnected...") == 0,
          "and the disconnect message ends in three dots");

    /* A nameless peer still reads as something. */
    lsw2_popup_join_text("", text, sizeof text);
    check(std::strcmp(text, "Player has joined the game!") == 0,
          "an empty name falls back rather than reading ' has joined'");
    lsw2_popup_join_text(nullptr, text, sizeof text);
    check(std::strcmp(text, "Player has joined the game!") == 0,
          "and so does a null one");

    /* --- the sink ------------------------------------------------------- */
    g_sink_calls = 0;
    lsw2_raise_join_popup("Nobody");
    check(g_sink_calls == 0, "with no sink installed, raising does nothing");

    lsw2_set_popup_sink(record);
    lsw2_raise_join_popup("Greedo");
    check(g_sink_calls == 1, "an installed sink is called");
    check(std::strcmp(g_last, "Greedo has joined the game!") == 0,
          "with the formatted text");

    lsw2_raise_disconnect_popup("Bossk");
    check(std::strcmp(g_last, "Bossk has disconnected...") == 0,
          "and again for a disconnect");
    lsw2_set_popup_sink(nullptr);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
