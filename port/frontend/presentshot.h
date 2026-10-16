/* CAPTURE WHAT THE PLAYER SEES, not what the guest rendered.
 *
 * WHY THIS EXISTS - measured 2026-09-01, at the source.
 *
 * `tools/pilot.py shot` sends {"cmd":"framebuffer"}, and the framework answers
 * it in runner/src/debug_server.cpp with
 *
 *     nds_gpu2d_framebuffer(screen)
 *
 * the GPU2D's raw output for engine A or B. Everything the PORT draws is
 * missing from it. The fork's menus are composited host-side at present time,
 * in present_game():
 *
 *     if (screen == 1)
 *         pixels = lsw2_menu_composite(pixels, src_w, lsw2_menuhost_current());
 *
 * onto a local buffer that goes straight to the window and is never written
 * back. So a capture taken while a menu is open shows the guest screen
 * UNDERNEATH the menu - usually menubackdrop's bare starfield - which is
 * indistinguishable from a menu that failed to draw.
 *
 * WHAT THAT COST. A whole session concluded "the arena menu opens invisible
 * and swallows input" from exactly that starfield, WHILE guard instrumentation
 * added the same session reported the composite running and tripping none of
 * its four early exits. The instrumentation was right; the picture could never
 * have answered the question. The long-standing "touch-drives-the-menu never
 * visually confirmed" item is the same defect wearing a different hat.
 *
 * WHY NOT FIX IT IN THE DEBUG SERVER. It is the framework's file, vendored
 * from ndsrecomp-main, and it has no extension point - an unrecognised command
 * returns {"error":"unknown cmd"}. Editing vendored code to add one would put
 * the fix somewhere a framework update silently reverts it.
 *
 * WHY NOT WRITE THE COMPOSITE BACK INTO THE FRAMEBUFFER. It is handed out as
 * `const uint32_t*`, presentation trails rasterization by a frame boundary
 * (gpu2d.h says so), and the buffer belongs to the renderer. Casting the const
 * away to scribble on someone else's double-buffered surface, so that a
 * DIFFERENT subsystem's read happens to return what we want, is the kind of
 * change that works until the frame pacing moves.
 *
 * SO THE PORT WRITES ITS OWN FRAMES. This is fed the exact pointer that goes to
 * the window, after every host-side pass has run, and writes it out as a PPM -
 * the same format tools/pilot.py already converts. Nothing else in the port
 * changes, and the framework is untouched.
 *
 * OFF UNLESS ASKED. LSW2_PRESENT_SHOT names a directory; without it this costs
 * one pointer comparison per presented frame. With it, both screens are written
 * every kInterval frames, overwriting - so a script taps a button, waits, and
 * reads the file. See the header of tools/pilot.py.
 */

#ifndef LSW2_PRESENTSHOT_H
#define LSW2_PRESENTSHOT_H

#include <cstdint>

/* Called from present_game() with the pixels that are about to be presented -
 * ARGB8888, `width` across, kScreenHeight down. Screen 0 is the top window and
 * screen 1 the touch window, matching present_game's own argument and
 * pilot.py's --screen.
 *
 * Cheap and silent when LSW2_PRESENT_SHOT is unset, which is every normal run.
 * Must be called AFTER the menu composite, or it captures the same thing the
 * framework's own command already returns and this file has no reason to
 * exist. */
void lsw2_presentshot_frame(int screen, const uint32_t* pixels, int width);

#endif /* LSW2_PRESENTSHOT_H */
