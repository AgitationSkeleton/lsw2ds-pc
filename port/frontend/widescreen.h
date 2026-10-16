/* Adaptive widescreen. See widescreen.cpp for what this does and does not do
 * yet — in particular, the guest's culling frustum is not widened, so the
 * picture gets wider before the game agrees to draw into it. */

#ifndef LSW2_WIDESCREEN_H
#define LSW2_WIDESCREEN_H

#include <cstdint>

/* The 3D raster width a window of this shape wants, clamped to
 * 256 + max_extra and rounded even so the native viewport stays centred.
 * Returns 256 for anything 4:3 or narrower. */
int lsw2_widescreen_width_for(int window_w, int window_h, int max_extra);

/* THE ACCELERATED RENDERER ONLY TAKES MULTIPLES OF 64 PX. Say which renderer
 * the width is for before the first lsw2_widescreen_apply: while this is set,
 * lsw2_widescreen_width_for snaps to the nearest width the compute renderer
 * accepts, instead of one that stops it from starting. See widescreen.cpp. */
void lsw2_widescreen_set_accelerated(bool accelerated);

/* The largest extra width the renderer will actually accept (448 - 256 = 192).
 * The UI asks rather than hardcoding it, so a slider can never offer a width
 * that gets refused. See the comment in widescreen.cpp. */
int lsw2_widescreen_max_extra();

/* Call once per frame, after the guest frame and before lsw2_widescreen_apply.
 * Looks at how much of the 3D surface was drawn to decide whether this is a
 * world view (where anchoring the HUD to the new edges is right) or 2D artwork
 * (where anchoring would cut the picture into pieces). See widescreen.cpp. */
void lsw2_widescreen_note_frame();

/* Apply it. Cheap to call every frame: it only touches the renderer when the
 * width actually changes. Returns false if the renderer refused, in which case
 * it has been put back to native rather than left inconsistent. */
bool lsw2_widescreen_apply(bool enabled, int window_w, int window_h,
                           int max_extra);

/* LOCK THE RASTER WIDTH.
 *
 * The compute renderer bakes its width into every shader and framebuffer and
 * refuses to change it once it is live - nds_gpu3d_set_output_width returns
 * false for an accelerated renderer. So under OpenGL the width is chosen once,
 * before the renderer starts, and locked. Without this, apply() would retry a
 * refused width on every single frame.
 *
 * The lock only stops width CHANGES. Everything else apply() decides - HUD
 * anchoring, background extension - keeps working per frame. */
void lsw2_widescreen_lock(bool locked);
bool lsw2_widescreen_locked();

int lsw2_widescreen_width();
/* Whether the guest's culling frustum has been widened to match. False means
 * the picture is wider than the game believes it is. */
bool lsw2_widescreen_frustum_patched();
bool lsw2_widescreen_enabled();

#endif /* LSW2_WIDESCREEN_H */
