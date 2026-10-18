/* The settings menu's typeface, compiled into the executable.
 *
 * imgui's built-in font is a 13 px bitmap face. It is the right thing for the
 * floating touch panel's title bar, but the menu is drawn at whatever size the
 * game window happens to be - from a 768x576 window up to a 4K display - and a
 * bitmap face scaled past its native size turns to blocks. A vector face baked
 * at the sizes the menu actually uses stays sharp at every one of them.
 *
 * Roboto Medium, the copy that ships with imgui in vendor/imgui/misc/fonts,
 * under the Apache License 2.0. It is embedded rather than loaded from disk so
 * the menu cannot lose its font to a packaging step that forgot the file. */

#ifndef LSW2_UIFONT_H
#define LSW2_UIFONT_H

/* The TTF, stb-compressed, in the form imgui's AddFontFromMemoryCompressedTTF
 * takes: the data and its size in bytes. */
const void* lsw2_ui_font_data();
int lsw2_ui_font_size();

#endif /* LSW2_UIFONT_H */
