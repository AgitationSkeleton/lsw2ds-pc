/* Drawing a Lsw2Menu into the DS frame itself.
 *
 * WHY INTO THE FRAMEBUFFER AND NOT THROUGH IMGUI.
 *
 * The obvious route is imgui's draw list, which is how the match HUD overlay works.
 * It is the wrong one here, for three reasons:
 *
 *   1. **It would not be the game's presentation.** An imgui quad is drawn at
 *      window resolution over the finished frame. A menu drawn into the 256x192
 *      guest frame is scaled, filtered and letterboxed by exactly the same path
 *      as everything the game draws - because it IS something the game draws by
 *      the time the frame is presented.
 *   2. **There are two present backends.** The OpenGL path and the SDL_Renderer
 *      path hold their textures differently, so an imgui route needs the font
 *      atlas uploaded twice, in two places, with two lifetimes. Compositing
 *      before the upload needs none of that: one buffer, both backends.
 *   3. **It keeps the pixel grid honest.** The font is an 18px bitmap face
 *      measured against DS pixels. Drawn at window resolution it
 *      would land on fractional pixels and shimmer; drawn into the frame it
 *      lands on the grid it was designed for.
 *
 * The cost is a 256x192 copy per frame while a menu is up, which is 192 KB and
 * not worth avoiding.
 */

#ifndef LSW2_GAMEMENU_RENDER_H
#define LSW2_GAMEMENU_RENDER_H

#include <cstdint>

class Lsw2Menu;

/* Composite `menu` over `src` and return the result.
 *
 * Returns `src` untouched when there is no menu or no font, so the caller can
 * use this unconditionally in the present path. The returned pointer is a
 * scratch buffer owned here and valid until the next call. */
const uint32_t* lsw2_menu_composite(const uint32_t* src, int width,
                                   const Lsw2Menu* menu);

/* WHICH ROW IS AT THIS TOUCH-SCREEN Y, or -1 for none.
 *
 * THE BUG THIS FIXES: the touch screen did not drive the menu, it drove the
 * widgets underneath. The menu draws on the touch screen and every tap went
 * straight past it to the game's own tabs.
 *
 * THE ANSWER COMES FROM THE LAST COMPOSITE, not from a second copy of the
 * layout. The list scrolls, so "row 7 is at y=114" is only true for the frame
 * that drew it that way, and a hit test that recomputed the window would drift
 * out of step with the drawing the moment either changed. lsw2_menu_composite
 * records what it put where; this reads that back.
 *
 * Returns the row's index in the MENU, not its slot on screen, so the caller
 * can hand it straight to the menu without knowing about scrolling. -1 when no
 * menu has been drawn, or when the tap missed every row. */
int lsw2_menu_row_at(int y);

/* Draw one string into a 256x192 XRGB buffer, left edge at `x`, top at `y`.
 * Exposed because the popups and the room-code display need it without a whole
 * menu around them. */
void lsw2_menu_draw_text(uint32_t* dst, int width, int x, int y,
                         const char* text, uint32_t rgb);

/* Centred on the frame's own axis.
 *
 * Every pause row measures centred on x = 128, which is the middle of a
 * native frame - but adaptive widescreen hands the top screen back WIDER than
 * 256, and a menu pinned to 128 would sit left of centre on it. Centring on the
 * frame's actual width is the same thing at native size and the right thing when
 * widened. */
void lsw2_menu_draw_centred(uint32_t* dst, int width, int y, const char* text,
                            uint32_t rgb);

/* How many rows below its `y` a string drawn by the two calls above covers: the
 * tallest of its glyphs, which can exceed the font's line height. The top-screen
 * layer (topoverlay.h) is cleared by the rows its composites report, so this is
 * what they report. */
int lsw2_menu_text_rows(const char* text);

/* The lobby list's DS signal-strength icon: three ascending bars with `bars` of them lit.
 *
 * This was once recorded as blocked because "the icon needs sprite
 * drawing the menu renderer does not have". It does not: the cartridge draws it
 * from OBJ tiles, but three filled rectangles are three filled rectangles, and
 * drawing them into the same 256x192 buffer as the text keeps them on the same
 * pixel grid - which is the part that made the OBJ route matter.
 *
 * Unlit bars are drawn dim rather than skipped, so the icon is a fixed width and
 * the rows do not jitter as pings change. */
void lsw2_menu_draw_signal(uint32_t* dst, int width, int x, int y, int bars,
                           uint32_t rgb);

/* How wide that icon is, so a caller can centre text and icon as one line. */
int lsw2_menu_signal_width();

#endif /* LSW2_GAMEMENU_RENDER_H */
