/* The game's own font, for drawing menus that look like the game's.
 *
 * WHY THE PORT CARRIES A FONT AT ALL. The fork's menus live in the game rather
 * than in imgui, so they have to be drawn in the game's presentation - and
 * imgui's own font is not that. The font is **font_18**, matched to the pixel
 * against six row widths read out of a live pause menu. Six exact agreements,
 * total error zero.
 *
 * WHERE THE DATA COMES FROM. `tools/export_custom_fonts.py` decodes the 4bpp
 * archive assets (that decode was corrected twice) and now also writes
 * `.lsw2font` - the same pixels and metrics in a flat binary, so the shell needs
 * neither a PNG decoder nor a text parse.
 *
 * MEASURING MATTERS AS MUCH AS DRAWING. The game centres a label by subtracting
 * half its width, so the port has to compute widths the same way or its menus
 * sit somewhere the game's never would. `lsw2_font_width` is that computation,
 * and it reproduces all six measured widths exactly.
 */

#ifndef LSW2_GAMEFONT_H
#define LSW2_GAMEFONT_H

#include <cstdint>

/* Load once. Returns false and leaves the port drawable-but-fontless if the
 * file is missing, which is deliberate: a fork that cannot find its font should
 * say so and keep running, not fail to start. */
bool lsw2_font_load(const char* path);

bool lsw2_font_ready();

/* Width in DS pixels, the way the game computes it - the sum of the glyphs'
 * advances. Unknown codepoints contribute nothing, matching a font that simply
 * has no glyph for them. */
int lsw2_font_width(const char* text);

int lsw2_font_line_height();

/* The atlas, for the renderer to upload. RGBA8, row-major. */
const uint8_t* lsw2_font_pixels();
int lsw2_font_atlas_width();
int lsw2_font_atlas_height();

/* Where one codepoint lives in the atlas. Returns false when the font has no
 * glyph for it. */
bool lsw2_font_glyph(uint32_t codepoint, int* x, int* y, int* w, int* h,
                     int* advance);

#endif /* LSW2_GAMEFONT_H */
