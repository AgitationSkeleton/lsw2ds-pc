/* See forkpopup.h. */

#include "forkpopup.h"

#include <cstdio>
#include <cstring>

/* THE WORDING IS EXACT, punctuation included.
 *
 * "<Playername> has disconnected..." ends in three dots and "<Playername> has
 * joined the game!" in an exclamation mark. That asymmetry is deliberate in the
 * original and copying it exactly is free; paraphrasing it is the sort of small
 * infidelity that accumulates into a fork that does not feel like the game. */
void lsw2_popup_join_text(const char* name, char* out, int cap) {
    if (!out || cap <= 0) return;
    std::snprintf(out, size_t(cap), "%s has joined the game!",
                  name && *name ? name : "Player");
}

void lsw2_popup_disconnect_text(const char* name, char* out, int cap) {
    if (!out || cap <= 0) return;
    std::snprintf(out, size_t(cap), "%s has disconnected...",
                  name && *name ? name : "Player");
}

namespace {
Lsw2PopupSink g_sink = nullptr;
}  // namespace

void lsw2_set_popup_sink(Lsw2PopupSink sink) { g_sink = sink; }

void lsw2_raise_popup(const char* text) {
    if (g_sink && text) g_sink(text);
}

void lsw2_raise_join_popup(const char* name) {
    char text[kLsw2PopupTextBytes];
    lsw2_popup_join_text(name, text, sizeof text);
    lsw2_raise_popup(text);
}

void lsw2_raise_disconnect_popup(const char* name) {
    char text[kLsw2PopupTextBytes];
    lsw2_popup_disconnect_text(name, text, sizeof text);
    lsw2_raise_popup(text);
}
