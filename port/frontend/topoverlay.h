/* THE PORT'S LAYER OVER THE TOP SCREEN.
 *
 * Three things the port draws go over the game's top-screen picture: the
 * match HUD (arenahud.h), the lobby countdown and the Free Play pickers'
 * words (lobbyhud.h), and the join and leave popups (popuphud.h). Each used to
 * copy the frame, draw into the copy and hand the copy to the present path.
 *
 * UNDER OPENGL NOTHING SHOWED THE COPY. The GL presenter takes the CPU frame
 * only as a FALLBACK (ComputeHost.cpp, nds_compute_host_present_top): when the
 * compositor described the frame on the GPU - the HD path, which runs whenever
 * the frame is widened with a 3D resolution or texture upscale above 1x, or the
 * direct path - it draws that and never reads the CPU pixels. With common
 * settings (OpenGL, 3D at 2x, adaptive widescreen) all three were computed on
 * every frame and none was ever on screen: the top-screen HUD touch setting
 * appeared to do nothing except hide the touch screen. LSW2_PRESENT_SHOT could
 * not see it either: it captures the CPU
 * frame, which did have the HUD in it.
 *
 * SO THEY DRAW INTO ONE LAYER, transparent (alpha 0) wherever nothing was
 * drawn, and each present path puts the layer over its own picture:
 *   - software: lsw2_top_overlay_merge blends it over the CPU frame. That gives
 *     the old copies' pixels, because every frame pixel is opaque (the
 *     framework's to_rgb32) and the text blend treats an opaque destination
 *     exactly as it did before (gamemenu_render.cpp, blend). The one exception
 *     is rounding, by at most one step, where two glyph edges overlap;
 *   - OpenGL: lsw2_gl_present_overlay draws it over the presented frame, in
 *     the same letterbox rectangle, a DS pixel to a layer pixel.
 * LSW2_PRESENT_SHOT keeps capturing the merged CPU frame. */

#ifndef LSW2_TOPOVERLAY_H
#define LSW2_TOPOVERLAY_H

#include <cstdint>

/* Start this frame's layer at the top screen's width (256, or wider when the
 * adaptive widescreen widened the frame), every pixel transparent. Returns the
 * layer to draw into: width x 192 ARGB, alpha 0 meaning nothing is there. */
uint32_t* lsw2_top_overlay_begin(int width);

/* A composite drew into rows [y0, y1) this frame. An untouched layer costs
 * neither present path anything, and the merge visits only these rows. */
void lsw2_top_overlay_touched(int y0, int y1);

/* This frame's layer, or nullptr when nothing was drawn into it. */
const uint32_t* lsw2_top_overlay_layer(int* width);

/* The software path's picture: `src` with the HUD layer and then the layer over
 * it, or `src` itself when nothing was drawn. The returned buffer is owned
 * here. */
const uint32_t* lsw2_top_overlay_merge(const uint32_t* src, int width);

/* THE MATCH HUD'S OWN LAYER, at kLsw2HudLayerScale times the frame's
 * resolution in each direction (at the frame's resolution the top-screen HUD
 * was messy and too big). The HUD is copied from the touch screen, and half its DS size is the
 * size that fits; in a layer at the frame's resolution half size can only drop
 * every other touch pixel, which loses the radar's dots and the digits' strokes.
 * At twice the resolution a touch pixel lands on exactly one layer pixel.
 *
 * Drawn UNDER the layer above (the menus and popups go over the HUD). OpenGL
 * draws it in the same letterbox rectangle; the software path, which has only
 * the frame's own pixels, takes every kLsw2HudLayerScale-th one. */
constexpr int kLsw2HudLayerScale = 2;

/* Start this frame's HUD layer for a frame `width` wide: (width x 192) times
 * kLsw2HudLayerScale, every pixel transparent. */
uint32_t* lsw2_top_overlay_hud_begin(int width);

/* The HUD drew into HUD-layer rows [y0, y1) this frame. */
void lsw2_top_overlay_hud_touched(int y0, int y1);

/* This frame's HUD layer and its size, or nullptr when nothing was drawn. */
const uint32_t* lsw2_top_overlay_hud_layer(int* width, int* height);

/* THE FRAME COLUMNS THE WINDOW SHOWS, [x0, x1). A widened frame is scaled to
 * fill the window's height and its extra columns run off the sides when the
 * window is narrower (frontend.cpp, viewport_for) - never into the native 256.
 * A piece anchored to the frame's own edge then lands off the window: the
 * radar, cut at the right in a narrower window. Set by the
 * present path before the composites run; the whole frame until it is. */
void lsw2_top_overlay_set_visible(int x0, int x1);
void lsw2_top_overlay_visible(int* x0, int* x1);

#endif /* LSW2_TOPOVERLAY_H */
