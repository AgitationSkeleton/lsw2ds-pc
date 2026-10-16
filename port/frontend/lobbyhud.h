/* The lobby countdown, on screen, in the game's own place.
 *
 * WHY THIS EXISTS AT ALL. `Lsw2ForkSession::lobby_remaining()` was written two
 * passes ago with the comment "For a display", and then nothing displayed it -
 * the same declared-and-never-called defect found several times elsewhere in
 * this code. The choice was to delete it or to finish it; a countdown the
 * player cannot see turns into a match that starts without warning, so it is
 * finished.
 *
 * THE PRESENTATION IS THE CARTRIDGE'S, not invented. The hunter statue select
 * draws its own 30-second countdown as **the bare number, centred at (128, 72)**
 * - reading the calls: the widget takes the remaining seconds, the string
 * id is `0x296`, and `config/strings.json` decodes that to `"@1"`, a lone
 * parameter placeholder. So the game's countdown style is "just the number", and
 * this matches it exactly rather than inventing a label.
 *
 * WHAT IT IS NOT. Not a control, so the rule about where the fork's controls
 * live does not move it to imgui - that rule sends CONTROLS into the game and
 * this is already in the game. Not the statue timer either: that one is the cartridge's and runs
 * itself.
 */

#ifndef LSW2_LOBBYHUD_H
#define LSW2_LOBBYHUD_H

#include <cstdint>

/* The game's seconds clock. ONE COPY: lobbyhud.cpp and scorescreen.cpp both
 * declared it, and both only read it. */
constexpr uint32_t kLsw2SecondsClock = 0x02109258u;

/* Draw the countdown (or the Free Play pickers' words) into `layer`, the top
 * screen's overlay layer for this frame (topoverlay.h), `width` x 192. Draws
 * nothing when there is nothing to say, so the present path can call it
 * unconditionally - the same contract the arena HUD has. */
void lsw2_lobby_countdown_composite(uint32_t* layer, int width);

#endif /* LSW2_LOBBYHUD_H */
