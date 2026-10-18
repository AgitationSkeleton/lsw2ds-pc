/* The settings menu's look: a full-window menu in the style of the modern PC
 * ports' menus (Ship of Harkinian's in particular) rather than a floating imgui
 * window with tabs.
 *
 * WHAT THAT LOOK IS, so the pieces below make sense on their own:
 *   - the menu covers the whole game window, which stays visible behind an
 *     85% black backdrop - the game keeps running, dimmed;
 *   - on a large window the menu itself is a centred block (90% of the window,
 *     never wider than 16:9), and on a small one it is the whole window;
 *   - a header row of top-level sections, with a search box and a close
 *     button at its right, and a white rule under it;
 *   - a sidebar listing the active section's pages, a white rule beside it,
 *     and the page itself in one or more columns;
 *   - flat, rounded, blue-filled checkboxes, sliders and combos, labels above
 *     sliders and combos, and -/+ step buttons either side of every slider.
 *
 * ui.cpp owns WHAT the menu contains - every setting, its tooltip and its
 * behaviour. This file owns only how it is drawn, so the content functions
 * read as a list of settings rather than a list of style pushes.
 *
 * EVERYTHING SCALES WITH THE WINDOW. The game window can be anything from a
 * few hundred pixels to a 4K display, so every size here is a design size at a
 * 1280x800 window multiplied by scale(), and the text is drawn in a face baked
 * at the nearest size rather than stretched. */

#ifndef LSW2_UIWIDGETS_H
#define LSW2_UIWIDGETS_H

#include "imgui.h"

#include <cstddef>

namespace uiw {

/* Bakes the menu's typeface into imgui's atlas. Called once, after the imgui
 * context exists and before a backend builds the atlas texture. imgui's own
 * font is added FIRST and so stays the default for everything that is not the
 * menu - the floating touch panel's title bar is sized from it. */
void load_fonts();

/* THE FRAME. begin_menu() opens the full-window backdrop and works out the
 * menu block's geometry for this frame; end_menu() closes it. just_opened
 * brings the menu above every other imgui window on the frame it appears. */
void begin_menu(bool just_opened);
void end_menu();

/* The header row. Between begin_header() and end_header() the caller submits a
 * tab bar of sections; header_tab() follows each BeginTabItem and draws that
 * tab's rounded fill behind its label. end_header() draws the search box and
 * the close button, and returns true when the close button was pressed. */
void begin_header();
void header_tab(bool selected);
bool end_header();

/* The sidebar of the active section's pages. */
void begin_sidebar();
bool sidebar_entry(const char* label, bool selected);
void sidebar_footer();      /* switches to the small face for closing notes */
void end_sidebar();

/* The page area, scrolled when the page is longer than the window. */
void begin_content();
void end_content();

/* SEARCH. The header's search box sets a query; while one is set, the caller
 * draws EVERY page into the content area and each widget below draws itself
 * only if its label or tooltip contains the query, followed by a note of the
 * section and page it lives on. Plain text is left out of the results, except
 * inside a block that matched as a whole (begin_block). */
bool searching();
void set_search(const char* text);      /* prefill, for a scripted screenshot */
void clear_search();
void search_origin(const char* section, const char* page);
int  search_hits();                     /* results drawn so far this frame */

/* A group drawn as a unit: a table, a status readout, a list. Shown always
 * outside a search, and in a search only when the title or keywords match.
 * Returns true when the group should be drawn; end_block() must then follow. */
bool begin_block(const char* title, const char* keywords);
void end_block();

/* COLUMNS. A page asks for up to `count`; a window too narrow for them gets
 * fewer, and a search gets one. */
void columns(int count);
void next_column();
void end_columns();

/* WIDGETS. Each returns what the plain imgui widget would, and each shows its
 * tooltip on hover - including while disabled, because a greyed control is the
 * one whose reason the player most wants to read. */
bool checkbox(const char* label, bool* value, const char* tooltip = nullptr);
bool slider_int(const char* label, int* value, int lo, int hi,
                const char* format, const char* tooltip = nullptr, int step = 1);
bool slider_float(const char* label, float* value, float lo, float hi,
                  const char* format, float step, const char* tooltip = nullptr);
bool combo(const char* label, int* index, const char* const items[], int count,
           const char* tooltip = nullptr);
/* For a combo whose popup is not a plain list. end_combo() only after true. */
bool begin_combo(const char* label, const char* preview,
                 const char* tooltip = nullptr, float width = 0.0f);
void end_combo();
/* width: 0 fills the line, a negative value fits the label, otherwise pixels
 * at scale 1 (scaled here). */
bool button(const char* label, const char* tooltip = nullptr, float width = 0.0f);
bool input_text(const char* label, char* buffer, size_t size,
                const char* tooltip = nullptr);
bool input_int(const char* label, int* value, const char* tooltip = nullptr);

/* Text. section() is a titled rule; the rest wrap to the column. */
void section(const char* title);
void text(const char* format, ...) IM_FMTARGS(1);
void note(const char* format, ...) IM_FMTARGS(1);
void warn(const char* format, ...) IM_FMTARGS(1);
void error(const char* format, ...) IM_FMTARGS(1);
/* SameLine, except in a search, where every result has its own line. */
void same_line();
void spacing();

/* The current scale, for the few sizes ui.cpp sets itself. */
float scale();

}  /* namespace uiw */

#endif /* LSW2_UIWIDGETS_H */
