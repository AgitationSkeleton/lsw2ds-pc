/* SLOT COLOURS ON THE TOP SCREEN: THE PORTRAIT RING IN THE PLAYER'S OWN COLOUR.
 *
 * The top-screen portrait ring matches the player's slot: players 2, 3 and 4
 * get green, red and purple, and player 1 stays blue - consistent with the
 * touch screen's portrait column, where each player's ring already wears its
 * slot colour.
 *
 * WHY slotcolour.cpp NEVER DID IT. That module rewrites OAM palette banks, and
 * the top-screen ring is not a sprite: it is a 3D quad (hudportrait.h)
 * from the HUD's ring sprite [hud + 0x54], whose texture is resource 0x26C and
 * whose 16-colour palette object is at sprite + 0xD8. Every client's log said
 * so: "the MAIN engine's OAM has no sprite in the player-colour range".
 *
 * WHAT THE COLOURS ARE (conn_ring1, measured on a live joiner). The ring's
 * palette object keeps a RAM copy of its 16 colours (+0x4) and its place in
 * texture-palette VRAM (+0x8, low half, in 8-byte units: 0x6EC -> 0x3760). Those
 * 16 colours are EXACTLY the touch screen's blue player palette, OBJ bank 4, in
 * another order. The game's four player palettes - blue, green, red, purple,
 * the ones a match stages into OBJ banks 4..7 - sit uncompressed in the ROM at
 * kLsw2PlayerPalettesRom, with a fifth (yellow) between green and red.
 *
 * SO THE RING IS RECOLOURED WITH THE GAME'S OWN SLOT COLOURS: each of its
 * colours is found in the blue palette, and the same entry of the local slot's
 * palette replaces it. A ring colour not in the blue palette means this is not
 * the ring it was measured on, and nothing is written.
 *
 * THIS CONSOLE'S RING ONLY, and only the texture palette: a presentation change
 * (the guest never reads its uploaded palette back). Player 1 - and a console
 * with no session - gets the game's own colours back. Off with the Enhanced
 * "Player-colour ring" setting, like slotcolour.cpp. */

#ifndef LSW2_RINGCOLOUR_H
#define LSW2_RINGCOLOUR_H

#include <cstdint>

/* ROM byte offset of the five player palettes, 0x20 bytes each: blue, green,
 * yellow, red, purple. */
constexpr uint32_t kLsw2PlayerPalettesRom = 0x00CE63E0u;

/* Once a frame, from the fork's tick. */
void lsw2_ringcolour_tick();

#endif /* LSW2_RINGCOLOUR_H */
