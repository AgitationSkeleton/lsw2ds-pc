/* See popuphud.h. */

#include "popuphud.h"

#include "gamefont.h"
#include "gamemenu_render.h"
#include "topoverlay.h"
#include "ui.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kH = 192;
constexpr int kQueued = 4;
constexpr int kTextBytes = 64;
constexpr int kShowMs = 3500;

/* THE GAME'S BOX, as its join popup drew it on 2026-09-23 (conn_dcpop, top
 * screen): a dark grey panel with a light rim, white text, its top edge at
 * about y 132 of 192, centred. */
constexpr int kBoxTop = 132;
constexpr int kMaxInner = 168;        /* wrap width, native pixels */
constexpr int kPadX = 8;
constexpr int kPadY = 5;
constexpr uint32_t kFill = 0x3C3C3Cu;
constexpr uint32_t kRim = 0xC8C8C8u;
constexpr uint32_t kText = 0xFFFFFFu;

char g_queue[kQueued][kTextBytes];
int g_queued = 0;
char g_current[kTextBytes] = {0};
bool g_showing = false;
std::chrono::steady_clock::time_point g_since;

/* Greedy word wrap on the font's own widths, at most three lines. */
int wrap(const char* text, char lines[3][kTextBytes]) {
    int n = 0;
    lines[0][0] = '\0';
    const char* p = text;
    while (*p && n < 3) {
        while (*p == ' ') ++p;
        const char* e = p;
        while (*e && *e != ' ') ++e;
        char word[kTextBytes];
        const int wl = int(e - p) < kTextBytes - 1 ? int(e - p) : kTextBytes - 1;
        std::memcpy(word, p, size_t(wl));
        word[wl] = '\0';
        char trial[kTextBytes];
        if (lines[n][0])
            std::snprintf(trial, sizeof trial, "%s %s", lines[n], word);
        else
            std::snprintf(trial, sizeof trial, "%s", word);
        if (lines[n][0] && lsw2_font_width(trial) > kMaxInner) {
            if (++n >= 3) break;
            std::snprintf(lines[n], kTextBytes, "%s", word);
        } else {
            std::snprintf(lines[n], kTextBytes, "%s", trial);
        }
        p = e;
    }
    return lines[0][0] ? (n < 3 ? n + 1 : 3) : 0;
}

/* Opaque: alpha 0xFF is what marks the box in the top-screen layer. */
void fill(uint32_t* dst, int width, int x0, int y0, int x1, int y1, uint32_t rgb) {
    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= kH) continue;
        for (int x = x0; x < x1; ++x)
            if (x >= 0 && x < width)
                dst[size_t(y) * size_t(width) + size_t(x)] = 0xFF000000u | rgb;
    }
}

}  // namespace

void lsw2_popup_hud_raise(const char* text) {
    if (!text || !*text) return;
    if (g_queued >= kQueued) {
        std::fprintf(stderr, "[fork] popup dropped, %d already waiting: %s\n",
                     g_queued, text);
        return;
    }
    std::snprintf(g_queue[g_queued], kTextBytes, "%s", text);
    ++g_queued;
}

void lsw2_popup_composite(uint32_t* layer, int width) {
    if (!layer || width <= 0 || !ui_fork_build() || !lsw2_font_ready()) return;

    const auto now = std::chrono::steady_clock::now();
    if (g_showing &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_since).count() >=
            kShowMs)
        g_showing = false;
    if (!g_showing && g_queued > 0) {
        std::snprintf(g_current, sizeof g_current, "%s", g_queue[0]);
        for (int i = 1; i < g_queued; ++i)
            std::memcpy(g_queue[i - 1], g_queue[i], kTextBytes);
        --g_queued;
        g_showing = true;
        g_since = now;
        /* SAID OUT LOUD when it goes on screen, so a drive that captures the
         * frames can time them by this line. */
        std::fprintf(stderr, "[fork] popup on screen: \"%s\"\n", g_current);
    }
    if (!g_showing) return;

    char lines[3][kTextBytes];
    const int n = wrap(g_current, lines);
    if (n == 0) return;
    int inner = 0;
    for (int i = 0; i < n; ++i) {
        const int w = lsw2_font_width(lines[i]);
        if (w > inner) inner = w;
    }
    const int line_h = lsw2_font_line_height();
    const int box_w = inner + 2 * kPadX;
    const int box_h = n * line_h + 2 * kPadY;
    const int x0 = (width - box_w) / 2;
    const int y0 = (kBoxTop + box_h <= kH - 2) ? kBoxTop : kH - 2 - box_h;

    /* The rim, then the panel inside it, with the corner pixels left out so the
     * box reads rounded like the game's - left transparent in the layer, so the
     * game's picture shows there. */
    fill(layer, width, x0 + 1, y0, x0 + box_w - 1, y0 + box_h, kRim);
    fill(layer, width, x0, y0 + 1, x0 + box_w, y0 + box_h - 1, kRim);
    fill(layer, width, x0 + 1, y0 + 1, x0 + box_w - 1, y0 + box_h - 1, kFill);
    int bottom = y0 + box_h;
    for (int i = 0; i < n; ++i) {
        const int y = y0 + kPadY + i * line_h;
        lsw2_menu_draw_centred(layer, width, y, lines[i], kText);
        if (y + lsw2_menu_text_rows(lines[i]) > bottom)
            bottom = y + lsw2_menu_text_rows(lines[i]);
    }
    lsw2_top_overlay_touched(y0, bottom);
}
