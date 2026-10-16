/* See gamemenu_render.h. */

#include "gamemenu_render.h"

#include "gamefont.h"
#include "gamemenu.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kH = 192;

std::vector<uint32_t> g_scratch;

/* WHERE THE WINDOW STARTS, kept between frames so a list only scrolls when the
 * cursor actually leaves it. Recomputed and clamped every frame, so a menu with
 * fewer rows than the last one cannot inherit a stale offset. */
int g_scroll_first = 0;

/* WHAT THE LAST COMPOSITE PUT WHERE, so a tap can be resolved against the rows
 * that are actually on the glass. Slot k holds the menu row index drawn at
 * kLsw2MenuFirstRowY + k * kLsw2MenuRowPitch, or -1. */
constexpr int kMaxSlots = 16;
int g_slot_row[kMaxSlots];
int g_slot_count = 0;

/* MEASURED 2026-08-31, OFF THE GAME'S OWN MENUS. Not a guess any more.
 *
 * === SUPERSEDED, AND IT INVITED EXACTLY THIS ==========================
 * | THE PALETTE IS A PLACEHOLDER AND SAYS SO. What colour LSW2 draws    |
 * | its menu text in has not been measured - the RAM read took the row  |
 * | RECTANGLES out of RAM, not the pixels inside them. White for an     |
 * | unselected row and yellow for the cursor is the LEGO games' usual   |
 * | scheme and is a reasonable first guess, but it is a guess, and the  |
 * | first screenshot of this next to a real pause menu settles it.      |
 * ======================================================================
 *
 * The presentation of the custom lobby menus (colours, font, flashing) has to
 * match the vanilla lobby menus. The
 * screenshot that comment was waiting for exists now - the base recomp was
 * driven to its pause menu and its save select over the debug surface - so the
 * colours are sampled rather than chosen. Two unrelated vanilla menus, the same
 * three values, counted over every text pixel in the frame:
 *
 *     ordinary row   #2079F3   blue      (shadowed with #0051B2)
 *     selected row   #D318F3   magenta   (shadowed with #9200B2)
 *     title          #F3CB10   yellow    (shadowed with #B29A10)
 *
 * ALL THREE GUESSES WERE WRONG, and not slightly: the rows were white and are
 * blue, the cursor was amber and is magenta, the title was white and is yellow.
 * A menu drawn in the old palette does not read as the game's menu no matter
 * how right the font and the geometry are.
 *
 * THE SHADOW COLOURS ARE RECORDED BUT NOT YET DRAWN. Each row in the cartridge
 * has a darker copy offset beneath it, which is why every sample comes in
 * pairs. The font atlas already carries its own black outline, so adding a
 * second one blindly would thicken the glyphs rather than match them - that is
 * a measurement of the OFFSET, which one more screenshot settles.
 *
 * THE FLASH IS ALSO NOT DONE, and is deliberately left rather than invented:
 * the captures are single frames, so how the selected row pulses - what it
 * alternates with and how fast - is not in them. Naming it here so it is
 * measured next rather than guessed now, which is the mistake this block is a
 * correction to. */
constexpr uint32_t kRowColour = 0x2079F3u;
constexpr uint32_t kCursorColour = 0xD318F3u;
constexpr uint32_t kTitleColour = 0xF3CB10u;

/* The darker companions, for when the offset is measured.
 *
 * kDimColour IS kRowShadow and is not a second constant - the duplicate-value
 * sweep caught them declared separately and it was right to. "The dim version
 * of a row" and "the shadow under a row" are the same colour in this palette
 * because they are the same thing: the row's own darker tone. Two names for it
 * is how they drift apart when one is corrected. */
constexpr uint32_t kRowShadow = 0x0051B2u;
constexpr uint32_t kDimColour = kRowShadow;
constexpr uint32_t kCursorShadow = 0x9200B2u;
constexpr uint32_t kTitleShadow = 0xB29A10u;

/* Blend one glyph pixel over the frame.
 *
 * The atlas is RGBA8 where alpha is the recovered font palette: 255 for the
 * fill, 128 for the anti-aliased edge, 255 for the outline (which is black
 * rather than white). So the source colour has to come from the atlas as well
 * as the tint - an outline pixel tinted yellow would put a yellow halo around
 * every glyph instead of a dark one. */
inline void blend(uint32_t* dst, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t a) {
    if (a == 0) return;
    if (a == 255) {
        *dst = 0xFF000000u | (uint32_t(sr) << 16) | (uint32_t(sg) << 8) | sb;
        return;
    }
    const uint32_t d = *dst;
    const uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    const uint32_t da = d >> 24;
    if (da == 255u) {
        const uint32_t r = (uint32_t(sr) * a + dr * (255u - a)) / 255u;
        const uint32_t g = (uint32_t(sg) * a + dg * (255u - a)) / 255u;
        const uint32_t b = (uint32_t(sb) * a + db * (255u - a)) / 255u;
        *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
        return;
    }
    /* A DESTINATION THAT IS NOT OPAQUE is the top-screen layer (topoverlay.h),
     * where alpha 0 means "the game's picture shows here". An edge pixel drawn
     * there keeps its partial cover as alpha, so it blends with whatever
     * picture the layer is finally put over - the CPU frame or the GL one -
     * instead of with the layer's empty black. Every frame pixel is opaque, so
     * drawing onto a frame takes the branch above, as it always did. */
    const uint32_t cover = uint32_t(a) * 255u + da * (255u - a);   /* x255 */
    const uint32_t keep = da * (255u - a);
    const uint32_t r = (uint32_t(sr) * a * 255u + dr * keep) / cover;
    const uint32_t g = (uint32_t(sg) * a * 255u + dg * keep) / cover;
    const uint32_t b = (uint32_t(sb) * a * 255u + db * keep) / cover;
    *dst = ((cover / 255u) << 24) | (r << 16) | (g << 8) | b;
}

}  // namespace

void lsw2_menu_draw_text(uint32_t* dst, int width, int x, int y,
                         const char* text, uint32_t rgb) {
    if (!dst || !text || !lsw2_font_ready()) return;

    const uint8_t* atlas = lsw2_font_pixels();
    const int aw = lsw2_font_atlas_width();
    if (!atlas) return;

    const uint8_t tr = uint8_t((rgb >> 16) & 0xFF);
    const uint8_t tg = uint8_t((rgb >> 8) & 0xFF);
    const uint8_t tb = uint8_t(rgb & 0xFF);

    int pen = x;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         *p; ++p) {
        int gx, gy, gw, gh, adv;
        if (!lsw2_font_glyph(uint32_t(*p), &gx, &gy, &gw, &gh, &adv)) continue;

        for (int row = 0; row < gh; ++row) {
            const int py = y + row;
            if (py < 0 || py >= kH) continue;
            for (int col = 0; col < gw; ++col) {
                const int px = pen + col;
                if (px < 0 || px >= width) continue;
                const uint8_t* s = &atlas[((gy + row) * aw + gx + col) * 4];
                /* THE OUTLINE KEEPS ITS OWN COLOUR. Palette entry 3 is opaque
                 * black; tinting it would lose the outline the font
                 * was drawn with. Anything that is not black gets the tint. */
                const bool is_outline = (s[0] | s[1] | s[2]) == 0;
                if (is_outline) blend(&dst[py * width + px], 0, 0, 0, s[3]);
                else blend(&dst[py * width + px], tr, tg, tb, s[3]);
            }
        }
        pen += adv;
    }
}

void lsw2_menu_draw_centred(uint32_t* dst, int width, int y, const char* text,
                            uint32_t rgb) {
    const int w = lsw2_font_width(text);
    lsw2_menu_draw_text(dst, width, width / 2 - w / 2, y, text, rgb);
}

int lsw2_menu_text_rows(const char* text) {
    if (!text) return 0;
    int rows = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         *p; ++p) {
        int gh = 0;
        if (lsw2_font_glyph(uint32_t(*p), nullptr, nullptr, nullptr, &gh, nullptr) &&
            gh > rows)
            rows = gh;
    }
    return rows;
}

void lsw2_menu_draw_signal(uint32_t* dst, int width, int x, int y, int bars,
                           uint32_t rgb) {
    if (!dst || width <= 0) return;

    /* THE DS SIGNAL ICON, AS BARS RATHER THAN AS A SPRITE.
     *
     * The cartridge draws this from OBJ tiles, and the port cannot: the menu
     * renderer composites into the 256x192 frame and has no OBJ pipeline of its
     * own, which is exactly why this was once listed as blocked: "the icon
     * needs sprite drawing the menu renderer does not have".
     *
     * It does not need sprite drawing. The icon is three ascending bars, and
     * three filled rectangles are three filled rectangles whether a tile engine
     * or a loop puts them there. What matters for "the game's own presentation"
     * is that it lands on the DS pixel grid, at the same scale as the text
     * beside it, and it does - it is drawn into the same buffer by the same
     * pass.
     *
     * EMPTY BARS ARE DRAWN, NOT OMITTED. A one-bar icon and a three-bar icon
     * must be the same width or the rows jitter as pings change, and a player
     * reading "how good is this" needs to see the bars that are NOT lit. The
     * unlit ones are drawn dim rather than skipped. */
    constexpr int kBars = 3;
    constexpr int kBarW = 3;
    constexpr int kBarGap = 1;
    constexpr int kStepH = 3;    /* each bar is this much taller than the last */
    constexpr int kBaseH = 3;

    const uint8_t r = uint8_t((rgb >> 16) & 0xFF);
    const uint8_t g = uint8_t((rgb >> 8) & 0xFF);
    const uint8_t b = uint8_t(rgb & 0xFF);

    /* The tallest bar's foot, so every bar sits on one baseline. */
    const int foot = y + kBaseH + (kBars - 1) * kStepH;

    for (int i = 0; i < kBars; ++i) {
        const int h = kBaseH + i * kStepH;
        const int bx = x + i * (kBarW + kBarGap);
        /* 255 for a lit bar, 64 for an unlit one - the same blend the glyphs
         * use, so the dim bars sit over the frame rather than punching a grey
         * block into it. */
        const uint8_t a = (i < bars) ? 255u : 64u;
        for (int row = 0; row < h; ++row) {
            const int py = foot - 1 - row;
            if (py < 0 || py >= kH) continue;
            for (int col = 0; col < kBarW; ++col) {
                const int px = bx + col;
                if (px < 0 || px >= width) continue;
                blend(&dst[size_t(py) * size_t(width) + size_t(px)], r, g, b, a);
            }
        }
    }
}

int lsw2_menu_signal_width() {
    return 3 * 3 + 2 * 1;   /* three bars, two gaps - see the drawing above */
}

const uint32_t* lsw2_menu_composite(const uint32_t* src, int width,
                                   const Lsw2Menu* menu) {
    /* WHICH GUARD TRIPPED, said once per distinct reason.
     *
     * Seen in play and reproduced live: walking into the arena lobby's
     * console opens the fork's menu, the backdrop drops the touch screen to the
     * starfield, the menu host starts swallowing input - and NOTHING IS DRAWN.
     * The player cannot move and cannot see why, which is the earlier
     * "character frozen in place" fault, still alive.
     *
     * From outside, all four of these conditions look identical: a starfield
     * with no menu on it. Naming the one that fires is the difference between
     * knowing and guessing, and this file's own history is the argument for it
     * - the last three fixes here were aimed at the wrong one of two
     * indistinguishable causes. */
    if (!src || !menu || width <= 0 || !lsw2_font_ready()) {
        const char* why = !src        ? "no source frame"
                        : !menu       ? "no menu is open"
                        : width <= 0  ? "the frame has no width"
                                      : "THE FONT IS NOT READY";
        static const char* said = nullptr;
        if (said != why) {
            said = why;
            std::fprintf(stderr,
                         "[fork] menu not drawn: %s (menu %p, width %d)\n",
                         why, (const void*)menu, width);
        }
        return src;
    }

    g_scratch.resize(size_t(width) * kH);
    std::memcpy(g_scratch.data(), src, g_scratch.size() * sizeof(uint32_t));
    uint32_t* dst = g_scratch.data();

    g_slot_count = 0;
    for (int i = 0; i < kMaxSlots; ++i) g_slot_row[i] = -1;

    if (menu->title())
        lsw2_menu_draw_centred(dst, width,
                               kLsw2MenuFirstRowY - kLsw2MenuRowPitch,
                               menu->title(), kTitleColour);

    /* SCROLLING, BECAUSE THE HOST SETTINGS MENU DOES NOT FIT AND CANNOT BE MADE TO.
     *
     * The settings did not all fit on this menu. The geometry
     * is the game's own - first row at y=58 (`ADD r3,r3,#0x3A` at 0x0209CB6C),
     * pitch 28 (`MOV r4,#0x1C` at 0x0209CB00) - and on a 192-pixel screen that
     * is five rows. The host menu has ten, plus a conditional
     * Character row, plus Start Lobby.
     *
     * SQUASHING THE PITCH WAS THE WRONG FIX and was considered first. Even at
     * the font's own line height the eleven rows do not fit, and the pitch is
     * one of the few numbers here read straight out of the cartridge - giving
     * it up buys one row and loses the game's own presentation.
     *
     * So the menu scrolls, which is what a menu with more rows than screen
     * does. The window follows the cursor and only moves when it has to, so
     * the rows stay put while the player moves through the middle of the list
     * and only shift at the ends.
     *
     * HIDDEN ROWS ARE NOT COUNTED. The host menu's Character row appears only when Roster
     * is "One"; counting it while hidden would leave a gap in the window and
     * scroll the list by a row that is not being drawn. */
    int order[64];
    int shown = 0;
    int cursor_at = 0;
    for (int i = 0; i < menu->row_count() && shown < 64; ++i) {
        if (menu->row(i).hidden) continue;
        if (i == menu->cursor()) cursor_at = shown;
        order[shown++] = i;
    }

    const int usable = kH - kLsw2MenuFirstRowY - lsw2_font_line_height();
    int visible = usable / kLsw2MenuRowPitch + 1;
    if (visible < 1) visible = 1;
    if (visible > shown) visible = shown;

    int first = g_scroll_first;
    if (first > shown - visible) first = shown - visible;
    if (first < 0) first = 0;
    if (cursor_at < first) first = cursor_at;
    if (cursor_at >= first + visible) first = cursor_at - visible + 1;
    g_scroll_first = first;

    for (int k = 0; k < visible; ++k) {
        const int i = order[first + k];
        const Lsw2MenuRow& r = menu->row(i);

        const bool selected = (i == menu->cursor());
        /* THE CURSOR WINS OVER A ROW'S OWN COLOUR. A green LAN row that stayed
         * green under the cursor would leave the player unable to see where the
         * cursor is, which trades a control for a decoration. */
        const uint32_t colour = selected ? kCursorColour
                              : r.colour   ? r.colour
                                           : kRowColour;
        /* THE WINDOW SLOT, not the row's absolute position - the list
         * scrolls, so row 7 may be drawn in slot 2. */
        const int y = kLsw2MenuFirstRowY + k * kLsw2MenuRowPitch;

        /* REMEMBERED FOR THE HIT TEST. See lsw2_menu_row_at. */
        if (k < kMaxSlots) { g_slot_row[k] = i; g_slot_count = k + 1; }

        /* A ROW IS "Label" OR "Label: <VALUE>", and both are centred as one
         * string - which is what the game does, since it measures the finished
         * line and subtracts half of it. Composing the text first and centring
         * once is therefore not a shortcut; it is the same operation. */
        const char* value = menu->value_of(i);
        char line[96];
        const char* text = r.label;
        if (value) {
            std::snprintf(line, sizeof line, "%s: <%s>", r.label, value);
            text = line;
        }

        if (r.badge <= 0) {
            lsw2_menu_draw_centred(dst, width, y, text, colour);
            (void)kDimColour;
            continue;
        }

        /* THE ICON IS PART OF THE LINE, so the line is centred with it rather
         * than the text being centred and the icon hung off the end. Centring
         * the text alone would push every badged row visibly left of the
         * unbadged ones above and below it - the game centres a finished line,
         * and a row with an icon on it is a longer finished line. */
        constexpr int kBadgeGap = 4;
        const int tw = lsw2_font_width(text);
        const int total = tw + kBadgeGap + lsw2_menu_signal_width();
        const int left = width / 2 - total / 2;
        lsw2_menu_draw_text(dst, width, left, y, text, colour);
        /* Sat on the text's own baseline area rather than the row top: the
         * font is 18px and the icon is 9, so aligning the tops would leave it
         * floating above the words. */
        lsw2_menu_draw_signal(dst, width, left + tw + kBadgeGap, y + 4,
                              r.badge, colour);
    }

    return dst;
}

int lsw2_menu_row_at(int y) {
    /* A BAND AROUND EACH ROW, not the glyph box. A finger is not a pixel, and
     * the gaps between rows would otherwise be dead. The band is the pitch, so
     * the bands tile the list with no holes and no overlap. */
    for (int k = 0; k < g_slot_count && k < kMaxSlots; ++k) {
        if (g_slot_row[k] < 0) continue;
        const int top = kLsw2MenuFirstRowY + k * kLsw2MenuRowPitch;
        if (y >= top - kLsw2MenuRowPitch / 2 &&
            y < top + kLsw2MenuRowPitch / 2)
            return g_slot_row[k];
    }
    return -1;
}
