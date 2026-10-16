/* THE LOBBY'S HUD GIVES THE TOP SCREEN TO GAME SETTINGS. During the game start
 * phase, four garbled squares appeared on the map thumbnails.
 *
 * WHAT WAS DRAWN OVER THE MAP. Game Settings (state 0x36) draws its map preview
 * on engine A's BG1, map base 0xF800. The fork opens it over the arena lobby,
 * a live LEVEL state whose HUD draws the player's hearts into that same map. The
 * last full heart beats, and 0x0208B064 rewrites its 2x2 cells every frame
 * (0x1042/0x1043 over 0x104A/0x104B, then +2, +4). LSW2_WRITE_WATCH on
 * 0x0600F8D8 named the writer: 0x0208B090, from the level's update
 * (0x0208CE20 -> 0x0208B320). Those are the four squares.
 *
 * THE CARTRIDGE'S OWN WAY TO TAKE THE TOP SCREEN FROM A LEVEL'S HUD. The level
 * state's message handler, 0x0208CFEC (vtable word 0x020FF48C), has two arms:
 *   - message 0x12 hides the HUD (0x0208BF74): its sprites off, and flag 0x04
 *     at HUD+0x78, which makes the HUD update (0x0208B0B8) return at once;
 *   - message 0x13 shows it again (0x0208BFEC) and rebuilds its BG setup
 *     (0x0208AAF0).
 * The cartridge reaches 0x36 from its menu chain, where no level HUD runs, so
 * it never needed them there. The fork's route puts 0x36 over a level, so the
 * fork sends them: hide once 0x36 is asked for - the screen slot fades before it
 * builds, so the preview is drawn after the HEART STOPS - and show when 0x36
 * gives way to anything but a new level.
 *
 * ON THE GUEST'S THREAD: a gear hop pinned to the level's own update
 * (kLsw2HudStep), checked at take time against the level updating then. */

#ifndef LSW2_LEVELHUD_H
#define LSW2_LEVELHUD_H

#include <cstdint>

/* The level state's message handler and the two messages. */
constexpr uint32_t kLsw2LevelStateMessage = 0x0208CFECu;
constexpr uint32_t kLsw2LevelHudHide = 0x12u;
constexpr uint32_t kLsw2LevelHudShow = 0x13u;

/* Once a frame from the fork's tick. */
void lsw2_levelhud_tick();

/* For the gear hop's take: is a HUD message aimed at `state` still meant for
 * the level whose update (`updating`, the host's r0) is running now? */
bool lsw2_levelhud_message_still_live(uint32_t state, uint32_t updating);

#endif /* LSW2_LEVELHUD_H */
