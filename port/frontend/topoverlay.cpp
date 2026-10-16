/* See topoverlay.h. */

#include "topoverlay.h"

#include <cstring>
#include <vector>

namespace {

constexpr int kH = 192;

std::vector<uint32_t> g_layer;
std::vector<uint32_t> g_merged;
int g_width = 0;
/* The rows drawn this frame, and the rows the previous frame left to clear. */
int g_y0 = kH, g_y1 = 0;
int g_dirty0 = kH, g_dirty1 = 0;

/* The HUD layer (topoverlay.h): the same bookkeeping at its own size. */
constexpr int kHudH = kH * kLsw2HudLayerScale;
std::vector<uint32_t> g_hud;
int g_hud_width = 0;
int g_hud_y0 = kHudH, g_hud_y1 = 0;
int g_hud_dirty0 = kHudH, g_hud_dirty1 = 0;

/* The window's columns of the frame; -1 until the present path says. */
int g_visible_x0 = -1, g_visible_x1 = -1;

}  // namespace

uint32_t* lsw2_top_overlay_begin(int width) {
    if (width <= 0) width = 256;
    if (width != g_width) {
        g_width = width;
        g_layer.assign(size_t(width) * kH, 0u);
        g_dirty0 = kH;
        g_dirty1 = 0;
    } else if (g_dirty0 < g_dirty1) {
        /* ONLY WHAT LAST FRAME DREW. Most frames draw nothing at all, so
         * clearing the whole layer each frame would be work for nothing. */
        std::memset(&g_layer[size_t(g_dirty0) * size_t(width)], 0,
                    size_t(g_dirty1 - g_dirty0) * size_t(width) * sizeof(uint32_t));
    }
    g_dirty0 = kH;
    g_dirty1 = 0;
    g_y0 = kH;
    g_y1 = 0;
    return g_layer.data();
}

void lsw2_top_overlay_touched(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > kH) y1 = kH;
    if (y0 >= y1) return;
    if (y0 < g_y0) g_y0 = y0;
    if (y1 > g_y1) g_y1 = y1;
    g_dirty0 = g_y0;
    g_dirty1 = g_y1;
}

const uint32_t* lsw2_top_overlay_layer(int* width) {
    if (g_y0 >= g_y1 || g_layer.empty()) return nullptr;
    if (width) *width = g_width;
    return g_layer.data();
}

const uint32_t* lsw2_top_overlay_merge(const uint32_t* src, int width) {
    const bool text = width == g_width && g_y0 < g_y1;
    const bool hud = width * kLsw2HudLayerScale == g_hud_width &&
                     g_hud_y0 < g_hud_y1;
    if (!src || (!text && !hud)) return src;

    g_merged.resize(size_t(width) * kH);
    std::memcpy(g_merged.data(), src, g_merged.size() * sizeof(uint32_t));

    /* THE HUD FIRST, a layer pixel per frame pixel - every kLsw2HudLayerScale-th
     * one. The HUD's pixels are opaque or absent (arenahud.cpp), so there is
     * nothing to blend. */
    if (hud) {
        const int y0 = g_hud_y0 / kLsw2HudLayerScale;
        const int y1 = (g_hud_y1 + kLsw2HudLayerScale - 1) / kLsw2HudLayerScale;
        for (int y = y0; y < y1 && y < kH; ++y) {
            const uint32_t* l =
                &g_hud[size_t(y) * kLsw2HudLayerScale * size_t(g_hud_width)];
            uint32_t* d = &g_merged[size_t(y) * size_t(width)];
            for (int x = 0; x < width; ++x) {
                const uint32_t s = l[size_t(x) * kLsw2HudLayerScale];
                if (s >> 24) d[x] = s;
            }
        }
    }
    if (!text) return g_merged.data();

    for (int y = g_y0; y < g_y1; ++y) {
        const uint32_t* l = &g_layer[size_t(y) * size_t(width)];
        uint32_t* d = &g_merged[size_t(y) * size_t(width)];
        for (int x = 0; x < width; ++x) {
            const uint32_t s = l[x];
            const uint32_t a = s >> 24;
            if (a == 0u) continue;
            if (a == 255u) { d[x] = s; continue; }
            /* A PARTLY COVERED PIXEL - a glyph's anti-aliased edge drawn onto a
             * transparent spot - over an opaque frame pixel: the same sum the
             * text blend makes when it draws straight onto a frame. */
            const uint32_t f = d[x];
            const uint32_t r = (((s >> 16) & 0xFFu) * a + ((f >> 16) & 0xFFu) * (255u - a)) / 255u;
            const uint32_t g = (((s >> 8) & 0xFFu) * a + ((f >> 8) & 0xFFu) * (255u - a)) / 255u;
            const uint32_t b = ((s & 0xFFu) * a + (f & 0xFFu) * (255u - a)) / 255u;
            d[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return g_merged.data();
}

uint32_t* lsw2_top_overlay_hud_begin(int width) {
    if (width <= 0) width = 256;
    const int w = width * kLsw2HudLayerScale;
    if (w != g_hud_width) {
        g_hud_width = w;
        g_hud.assign(size_t(w) * kHudH, 0u);
    } else if (g_hud_dirty0 < g_hud_dirty1) {
        std::memset(&g_hud[size_t(g_hud_dirty0) * size_t(w)], 0,
                    size_t(g_hud_dirty1 - g_hud_dirty0) * size_t(w) *
                        sizeof(uint32_t));
    }
    g_hud_dirty0 = kHudH;
    g_hud_dirty1 = 0;
    g_hud_y0 = kHudH;
    g_hud_y1 = 0;
    return g_hud.data();
}

void lsw2_top_overlay_hud_touched(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > kHudH) y1 = kHudH;
    if (y0 >= y1) return;
    if (y0 < g_hud_y0) g_hud_y0 = y0;
    if (y1 > g_hud_y1) g_hud_y1 = y1;
    g_hud_dirty0 = g_hud_y0;
    g_hud_dirty1 = g_hud_y1;
}

const uint32_t* lsw2_top_overlay_hud_layer(int* width, int* height) {
    if (g_hud_y0 >= g_hud_y1 || g_hud.empty()) return nullptr;
    if (width) *width = g_hud_width;
    if (height) *height = kHudH;
    return g_hud.data();
}

void lsw2_top_overlay_set_visible(int x0, int x1) {
    g_visible_x0 = x0;
    g_visible_x1 = x1;
}

void lsw2_top_overlay_visible(int* x0, int* x1) {
    /* The whole frame until the present path has said otherwise. */
    const int w = g_width > 0 ? g_width : 256;
    int a = g_visible_x0 < 0 ? 0 : g_visible_x0;
    int b = g_visible_x1 < 0 ? w : g_visible_x1;
    if (b > w) b = w;
    if (a >= b) { a = 0; b = w; }
    if (x0) *x0 = a;
    if (x1) *x1 = b;
}
