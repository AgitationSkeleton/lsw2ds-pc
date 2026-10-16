/* Which of the DS's two 2D engines is driving which screen, right now.
 *
 * WHY THIS IS A FUNCTION AND NOT A CONSTANT, which is how two files had it.
 *
 * `slotcolour.cpp` opened with "The TOP screen is the main engine. That is the
 * whole reason the predicate can be as simple as palette 4..7" and read engine
 * 0. `arenahud.cpp` read engine 1 for the touch screen. Both were bare
 * constants with the reasoning written beside them as settled fact.
 *
 * POWCNT1 bit 15 decides it. The hardware calls it Display Swap: 1 puts engine
 * A on the UPPER screen, 0 puts engine A on the LOWER one. It is an ordinary
 * register the game may write whenever it likes.
 *
 * AND THIS GAME DOES. Sampling every 25th of the 286,097 per-frame state.json
 * files under `out/work`, across all 85 capture streams:
 *
 *   - `gpudump_arena`'s summary frame:            oam_main 0 sprites, oam_sub 19
 *   - `gpudump_lobbywait/inst0` f025386:          oam_main 18, oam_sub 1
 *
 * and the eighteen in the second are recognisably bottom-screen furniture -
 * four 40x40 portrait cells at x=9, y=29/69/109/149, each assembled from a
 * 32x32 plus an 8x32 right strip plus a 32x8 bottom strip. Same slot column,
 * opposite engine. 6,229 of 10,931 sampled frames have the MAIN engine's OBJ
 * layer enabled at all, which the older reading had recorded as never.
 *
 * A HARDCODED INDEX FAILS SILENTLY, which is why this is worth a file. It does
 * not return null or garbage - it returns a perfectly valid OAM image belonging
 * to the OTHER screen, so the caller decodes real sprites at real coordinates
 * and reasons about the wrong picture. That is how `slotcolour.cpp` came to
 * treat a four-player portrait column as one player's ring.
 *
 * WHAT THE CAPTURES DO NOT SETTLE, said plainly: they do not record POWCNT1, so
 * they establish that the assignment varies without saying which frame was
 * which way round. Reading the register is how the port answers it for the
 * frame in front of it; the captures are only why it must ask.
 *
 * SEPARATE FROM objsprite.{h,cpp} ON PURPOSE. That pair is pure - `test_
 * objsprite` links it and nothing else - and asking the runner for a register
 * would drag the framework onto that link line. The same seam that keeps
 * characterpick.cpp free of the guest.
 */

#ifndef LSW2_SCREENENGINE_H
#define LSW2_SCREENENGINE_H

#include <cstdint>

/* The two screens, named rather than numbered at the call sites. */
constexpr int kLsw2ScreenTop = 0;
constexpr int kLsw2ScreenTouch = 1;

/* The two engines, in the numbering nds_vram_renderer_oam() takes. */
constexpr int kLsw2EngineMain = 0;   /* engine A */
constexpr int kLsw2EngineSub = 1;    /* engine B */

/* POWCNT1's Display Swap bit. */
constexpr unsigned kLsw2PowcntDisplaySwap = 0x8000u;

/* The engine currently driving `screen`, for nds_vram_renderer_oam().
 *
 * Falls back to the unswapped assignment - main on top, sub on the touch
 * screen - for a screen index outside 0..1, so a caller that passes something
 * odd gets the historical behaviour rather than an out-of-range engine. */
int lsw2_screen_engine(int screen);

/* ------------------------------------------------------------------ the rule
 *
 * Split out so `test_screenengine.cpp` can pin it without a running guest -
 * the register read is one line and the mapping is the part worth testing. */

/* Given POWCNT1, which engine drives `screen`? */
int lsw2_screen_engine_for(unsigned powcnt1, int screen);

/* ------------------------------------------------------- and its DISPCNT
 *
 * WHICH OF THE THREE ACCESSORS MOVE, because they do not all move and getting
 * that wrong in either direction is a bug:
 *
 *   nds_gpu2d_framebuffer(screen)   takes a SCREEN and already handles the
 *                                   swap itself - the framework computes
 *                                   `engine = a_on_top ? screen : screen ^ 1`
 *                                   in its own presenter. Pass the screen.
 *   nds_vram_renderer_oam(engine)   takes an ENGINE. Resolve it.
 *   nds_gpu2d_read/write(addr)      takes a REGISTER ADDRESS, and engine A's
 *                                   DISPCNT is 0x04000000 while engine B's is
 *                                   0x04001000. Resolve it.
 *
 * arenahud.cpp had all three: the framebuffer read was right, the OAM read was
 * a constant, and the starfield suppression poked 0x04001000 unconditionally.
 * That last one is the worst of the three, because it WRITES - with the
 * engines swapped it would disable a background layer on the screen the player
 * is looking at while leaving the starfield in the pixels being mirrored. */
constexpr uint32_t kLsw2DispcntEngineA = 0x04000000u;
constexpr uint32_t kLsw2DispcntEngineB = 0x04001000u;

/* The DISPCNT register of the engine currently driving `screen`. */
uint32_t lsw2_screen_dispcnt(int screen);
uint32_t lsw2_screen_dispcnt_for(unsigned powcnt1, int screen);

#endif /* LSW2_SCREENENGINE_H */
