/* Giving the fork's menus the vanilla starfield backdrop.
 *
 * THE PROBLEM: the host settings menu was drawn on the top screen,
 * and when it moved to the touch screen it was composited over whatever the
 * touch screen already had - the Party Members tab, the Free Play portraits.
 * The requirement is more precise than "on the touch screen": the menu uses
 * the same starfield-backdrop handling as the vanilla arena lobby menus.
 *
 * A vanilla LSW2 menu OWNS the bottom screen. "Host Multi-Card Game", the pause
 * menu, the save-select - every one of them is rows on a starfield, with the
 * tabs and portraits gone. A menu floating over the live tab is an overlay
 * wearing the game's font, which is exactly what the fork's menus must not
 * be.
 *
 * HOW, AND IT COSTS NOTHING TO DRAW. The starfield is already there: it is a
 * background layer on the touch screen's engine, the one arenahud.cpp turns OFF
 * to keep it out of the match HUD's mirrored column - "the only layer covering all 256x192
 * and the only one scrolled, which is what identifies it rather than its index".
 * So the fork does not need to draw a starfield. It needs to keep that layer and
 * suppress the ones above it, which is arenahud's trick run in reverse.
 *
 * The pixels are then the cartridge's own starfield, scrolling as it always
 * does, and the rows land on it the way the game's rows do.
 *
 * RE-ASSERTED EVERY FRAME for the reason arenahud.cpp records: the guest owns
 * DISPCNT and rewrites it on any display change, so a one-shot disable holds
 * until the first time the game touches it and then never again.
 *
 * AND IT RESTORES WHAT IT TOOK, from the register it actually took it from -
 * see screenengine.h. Restoring the whole saved word would discard everything
 * the game changed about its own display while the menu was up.
 */

#ifndef LSW2_MENUBACKDROP_H
#define LSW2_MENUBACKDROP_H

/* DISPCNT bit 8+n enables background n; bit 12 is OBJ. The starfield is BG2,
 * identified by arenahud.cpp the same way. */
constexpr unsigned kLsw2BgEnableShift = 8u;
constexpr unsigned kLsw2StarfieldBg = 2u;

/* Everything on the touch screen EXCEPT the starfield: the tab artwork, the
 * portraits, the buttons. */
constexpr unsigned kLsw2MenuHiddenLayers =
    (1u << (kLsw2BgEnableShift + 0u)) |
    (1u << (kLsw2BgEnableShift + 1u)) |
    (1u << (kLsw2BgEnableShift + 3u)) |
    (1u << 12u);                       /* OBJ */

/* Once a frame, from the fork tick, before the frame is presented. Hides the
 * touch screen's non-starfield layers while a fork menu is open and puts them
 * back when it closes. Does nothing in the baseline. */
void lsw2_menubackdrop_tick();

#endif /* LSW2_MENUBACKDROP_H */
