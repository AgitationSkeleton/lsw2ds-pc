/* See gamefont.h. Reads the .lsw2font container tools/export_custom_fonts.py
 * writes; the format is documented at that writer. */

#include "gamefont.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

struct Glyph {
    uint16_t x = 0, y = 0, w = 0, h = 0;
    int16_t xoffset = 0, yoffset = 0;
    uint16_t advance = 0;
};

bool g_ready = false;
int g_atlas_w = 0, g_atlas_h = 0, g_line_height = 0;
std::vector<uint8_t> g_pixels;
std::unordered_map<uint32_t, Glyph> g_glyphs;

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

}  // namespace

bool lsw2_font_load(const char* path) {
    g_ready = false;
    g_glyphs.clear();
    g_pixels.clear();

    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "[font] not found: %s\n", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> blob(size > 0 ? size_t(size) : 0u);
    const size_t got = blob.empty() ? 0 : std::fread(blob.data(), 1, blob.size(), f);
    std::fclose(f);
    if (got != blob.size() || blob.size() < 24) {
        std::fprintf(stderr, "[font] short read: %s\n", path);
        return false;
    }

    if (std::memcmp(blob.data(), "L2FN", 4) != 0) {
        std::fprintf(stderr, "[font] bad magic in %s\n", path);
        return false;
    }
    const uint32_t version = rd32(&blob[4]);
    if (version != 1u) {
        std::fprintf(stderr, "[font] unknown version %u in %s\n",
                     unsigned(version), path);
        return false;
    }
    g_atlas_w = int(rd32(&blob[8]));
    g_atlas_h = int(rd32(&blob[12]));
    g_line_height = int(rd32(&blob[16]));
    const uint32_t count = rd32(&blob[20]);

    /* SIZE THE FILE BEFORE TRUSTING ANY OF IT. A truncated font would otherwise
     * read glyph records out of the pixel data and draw garbage, which looks
     * like a rendering bug rather than a missing file. */
    const size_t glyph_bytes = size_t(count) * 18u;
    const size_t pixel_bytes = size_t(g_atlas_w) * size_t(g_atlas_h) * 4u;
    if (blob.size() != 24u + glyph_bytes + pixel_bytes) {
        std::fprintf(stderr,
                     "[font] %s is %zu bytes, expected %zu for %ux%u and %u "
                     "glyphs\n",
                     path, blob.size(), 24u + glyph_bytes + pixel_bytes,
                     unsigned(g_atlas_w), unsigned(g_atlas_h), unsigned(count));
        return false;
    }

    size_t off = 24;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t cp = rd32(&blob[off]);
        Glyph g;
        g.x = rd16(&blob[off + 4]);
        g.y = rd16(&blob[off + 6]);
        g.w = rd16(&blob[off + 8]);
        g.h = rd16(&blob[off + 10]);
        g.xoffset = int16_t(rd16(&blob[off + 12]));
        g.yoffset = int16_t(rd16(&blob[off + 14]));
        g.advance = rd16(&blob[off + 16]);
        g_glyphs[cp] = g;
        off += 18;
    }

    g_pixels.assign(blob.begin() + long(off), blob.end());
    g_ready = true;
    std::fprintf(stderr, "[font] %s: %u glyphs, %dx%d atlas, line height %d\n",
                 path, unsigned(count), g_atlas_w, g_atlas_h, g_line_height);
    return true;
}

bool lsw2_font_ready() { return g_ready; }

int lsw2_font_width(const char* text) {
    if (!g_ready || !text) return 0;
    int w = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         *p; ++p) {
        auto it = g_glyphs.find(uint32_t(*p));
        if (it != g_glyphs.end()) w += it->second.advance;
    }
    return w;
}

int lsw2_font_line_height() { return g_line_height; }
const uint8_t* lsw2_font_pixels() { return g_pixels.empty() ? nullptr : g_pixels.data(); }
int lsw2_font_atlas_width() { return g_atlas_w; }
int lsw2_font_atlas_height() { return g_atlas_h; }

bool lsw2_font_glyph(uint32_t codepoint, int* x, int* y, int* w, int* h,
                     int* advance) {
    auto it = g_glyphs.find(codepoint);
    if (it == g_glyphs.end()) return false;
    if (x) *x = it->second.x;
    if (y) *y = it->second.y;
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    if (advance) *advance = it->second.advance;
    return true;
}
