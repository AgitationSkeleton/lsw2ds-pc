/* The font loader, checked against widths the GAME produced.
 *
 * This is not a round-trip test of the exporter. The six expected widths below
 * were read out of a live pause menu's row rectangles long before any
 * font was matched to them, so a pass here means the port measures text the way
 * the cartridge does - which is what centring on x=128 depends on.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_gamefont.cpp \
 *       port/frontend/gamefont.cpp -o test_gamefont
 *   ./test_gamefont out/exported/godot_ui/fonts/font_18.lsw2font
 *
 * SKIPS when the font is absent rather than failing: it is a build artifact of
 * tools/export_custom_fonts.py, not something in the repository.
 */

#include "gamefont.h"

#include <cstdio>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/* Read out of RAM from a live pause menu. Not derived from the font. */
struct Case { const char* text; int width; };
const Case kMeasured[] = {
    {"Resume", 58}, {"Host Wireless", 115}, {"Settings", 63},
    {"Extras", 51}, {"Save Game", 87}, {"Quit Game", 84},
};

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1]
                                : "out/exported/godot_ui/fonts/font_18.lsw2font";

    if (!lsw2_font_load(path)) {
        std::printf("SKIPPED - no font at %s\n", path);
        std::printf("  build it with: python tools/export_custom_fonts.py\n");
        return 0;
    }

    std::printf("game font\n");
    check(lsw2_font_ready(), "the font loaded");
    check(lsw2_font_line_height() == 18,
          "line height is 18, the measured pause row height");
    check(lsw2_font_atlas_width() > 0 && lsw2_font_atlas_height() > 0,
          "the atlas has a size");
    check(lsw2_font_pixels() != nullptr, "and pixels");

    for (const Case& c : kMeasured) {
        char what[96];
        std::snprintf(what, sizeof what, "\"%s\" measures %d", c.text, c.width);
        check(lsw2_font_width(c.text) == c.width, what);
    }

    /* The hidden pause rows depend on this exactly: a blanked label must
     * measure to nothing, not to some minimum, or the row it hides would still
     * take space. */
    check(lsw2_font_width("") == 0, "an empty label measures 0, as hidden pause rows need");

    /* A codepoint the font has no glyph for must contribute nothing rather than
     * crash or invent a width. */
    check(lsw2_font_width("\x01\x02") == 0, "unknown codepoints add nothing");

    int gx, gy, gw, gh, adv;
    check(lsw2_font_glyph('A', &gx, &gy, &gw, &gh, &adv) && gw > 0,
          "'A' has a glyph rectangle");
    check(!lsw2_font_glyph(0x10FFFF, &gx, &gy, &gw, &gh, &adv),
          "and an absent codepoint reports absent");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (!g_failures) std::printf("all checks passed\n");
    return g_failures ? 1 : 0;
}
