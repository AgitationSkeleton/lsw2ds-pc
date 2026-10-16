/* The OpenGL top-screen present path.
 *
 * WHY IT EXISTS. Our shell reads the composited CPU framebuffer and pushes it
 * through an SDL_Renderer. That is simple, it works, and it caps the picture at
 * the DS's own 256x192 sample density - so scaling to a modern window magnifies
 * every polygon edge and every texel. "Chunky when viewed larger" is not a
 * filtering problem; there is genuinely no more detail to show.
 *
 * The framework can render the 3D layer at 2x-4x internal resolution and filter
 * textures on decode, but BOTH live in the OpenGL compute renderer, and its
 * output never reaches the CPU framebuffer - the extra sample density exists
 * only in a GPU texture. Presenting it means presenting from GL. That is what
 * this file does.
 *
 * WHAT IT COSTS. The top window becomes an OpenGL window with no SDL_Renderer,
 * so everything drawn over the game has to be drawn in GL too. imgui switches
 * backends to match (see ui_init). Anything that assumed an SDL_Renderer on the
 * top window has to be reworked or disabled, and where it is disabled the UI
 * says so rather than silently doing nothing.
 *
 * WHY IT IS CHOSEN AT STARTUP. The internal scale is baked into the compute
 * shaders and sizes every framebuffer, so it cannot change under a live
 * renderer - the framework refuses. The window flag (SDL_WINDOW_OPENGL) cannot
 * change after creation either. So the mode is read once, and the UI presents
 * it as a restart-required setting instead of pretending it is live.
 */

#ifndef LSW2_GLPRESENT_H
#define LSW2_GLPRESENT_H

#include <SDL.h>
#include <cstdint>

struct Lsw2GlOptions {
    bool enabled = false;         /* use the OpenGL present path            */
    int internal_resolution = 1;  /* 1..4, the 3D raster multiplier         */
    int texture_upscale = 1;      /* 1, 2 or 4                              */
};

/* Read the requested mode. Called before any window exists, because the answer
 * decides how the window is created. */
Lsw2GlOptions lsw2_gl_requested(const Lsw2GlOptions& from_settings);

/* Extra SDL_CreateWindow flags for the top window under this mode. */
uint32_t lsw2_gl_window_flags(const Lsw2GlOptions& options);

/* Bring up the compute renderer against an already-created top window. Must run
 * AFTER the window exists and BEFORE the guest renders a frame.
 *
 * Returns false if the GL path could not be established - a driver without
 * OpenGL 4.3, a shader that would not compile. The caller must then fall back
 * to the software path, which means destroying and recreating the window
 * without SDL_WINDOW_OPENGL: the flag cannot be removed in place. */
bool lsw2_gl_start(SDL_Window* top_window, const Lsw2GlOptions& options);

/* True once the GL path is actually running - not merely requested. Everything
 * that has to behave differently under GL asks this, so a failed start cannot
 * leave half the shell believing it succeeded. */
bool lsw2_gl_active();

/* The GL context, for imgui's SDL2 platform backend. */
SDL_GLContext lsw2_gl_context();

/* Present the top screen. `pixels`/`width` are the composited CPU frame, used
 * as the fallback source when the compositor did not emit GPU surfaces for
 * this frame. `viewport` is the destination rect in drawable pixels, so the
 * picture is letterboxed rather than stretched.
 *
 * Does NOT swap: the caller draws its overlay and then calls lsw2_gl_swap().
 * Returns false if the frame could not be presented, in which case the caller
 * should still swap so the window does not freeze. */
bool lsw2_gl_present_top(const uint32_t* pixels, int width,
                         const SDL_Rect& viewport);

/* THE PORT'S TOP-SCREEN LAYER, OVER THE PRESENTED FRAME (topoverlay.h).
 *
 * lsw2_gl_present_top draws the compositor's GPU description of the frame
 * whenever there is one and reads `pixels` only when there is not - so anything
 * the port composited into `pixels` was never seen under OpenGL. This draws a
 * layer (`width` x `height` ARGB, alpha 0 = nothing) over whatever was
 * presented, stretched over the same `viewport` - the text layer at the frame's
 * size, the HUD's at twice it. `slot` keeps one texture per layer (0 text,
 * 1 HUD) so neither is reallocated every frame. Call it after
 * lsw2_gl_present_top and before the UI. gloverlay.cpp. */
bool lsw2_gl_present_overlay(const uint32_t* layer, int width, int height,
                             const SDL_Rect& viewport, int slot);

/* Release the overlay pass's GL objects; lsw2_gl_stop calls it. */
void lsw2_gl_overlay_stop();

/* LSW2_PRESENT_SHOT's OpenGL half: read the presented letterbox back from the
 * window and write it as present-top-gl.ppm. Test instrument, off unless that
 * variable is set. gloverlay.cpp says why the CPU capture is not enough. */
void lsw2_gl_presentshot(const SDL_Rect& viewport);

/* THE BOTTOM SCREEN AS A GL TEXTURE, for the floating touch panel.
 *
 * The panel is drawn by imgui on the top window. Under SDL that means an
 * SDL_Texture owned by the top window's renderer - an SDL_Texture belongs to
 * the renderer that made it and cannot be drawn by another, which is why the
 * bottom window's own texture produced a correctly-placed, correctly-sized,
 * entirely black panel. Under OpenGL there is no renderer on the top window at
 * all, so the same job needs a GL texture in the same context imgui is drawing
 * with.
 *
 * Uploads `pixels` (256x192 ARGB8888, the bottom screen's composite) and
 * returns the handle to hand imgui. Returns nullptr when the GL path is not
 * running, so the caller can pass the result through unconditionally. */
void* lsw2_gl_touch_texture(const uint32_t* pixels, bool linear);

/* Paint the whole drawable black.
 *
 * NEEDED BECAUSE FEEDING BLACK PIXELS TO lsw2_gl_present_top DOES NOTHING.
 * That function asks the compositor for a GPU-side description of the frame
 * and, when there is one, presents THAT - the CPU buffer is only the fallback
 * for scenes the direct path cannot describe. A 3D scene like the boot splash
 * always has one, so the black buffer was silently ignored and the splash
 * showed through the fork's "Skipping boot..." overlay.
 *
 * glClearColor/glClear are GL 1.1, so no loader is required. */
void lsw2_gl_clear();

void lsw2_gl_swap();

void lsw2_gl_stop();

#endif /* LSW2_GLPRESENT_H */
