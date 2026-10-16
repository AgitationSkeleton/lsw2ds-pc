/* The online lobby's join and disconnect popups, drawn on the top screen.
 *
 * When a player leaves, a popup like the in-game time-limit ones says
 * "<Playername> has disconnected..."; when a player joins, the same kind of
 * popup says "<Playername> has joined the game!".
 *
 * WHY THE FORK DRAWS IT, measured after two routes through the game each
 * failed on screen:
 *
 *   - Script command 26 into the last script object seen: every console logged
 *     "popup raised", and no frame ever showed one.
 *   - The game's own message display, 0x020601AC(_, 0, string, -1, 4, 1), the
 *     call message 0x35's time-limit handler makes: the join popup appeared in
 *     the lobby, in the game's box (conn_dcpop) - but it takes the top screen's
 *     3D to 4:3 and hides the HUD while it stands, and in a networked arena the
 *     message system reads busy for the whole match (bit 0 of its halfword
 *     +0x880 at 0x021029E0 held 0 - the same test that gates the game's own
 *     60- and 30-second warnings), so every disconnect popup waited for ever.
 *
 * So the fork draws the box the game draws - dark, with a light rim, white
 * text in the game's own font, in the lower third of the top screen where the
 * game put the join popup - at present time, over the picture. It changes no
 * guest state, so it is safe in lockstep, and it shows in the lobby and in a
 * match alike. One popup at a time, each for three and a half seconds, the
 * next waiting its turn. */
#ifndef LSW2_POPUPHUD_H
#define LSW2_POPUPHUD_H

#include <cstdint>

/* Queue `text` to be shown (the popup sink - forkpopup.h). */
void lsw2_popup_hud_raise(const char* text);

/* Draw the current popup into `layer`, the top screen's overlay layer for this
 * frame (topoverlay.h), `width` x 192 - nothing when no popup is showing. */
void lsw2_popup_composite(uint32_t* layer, int width);

#endif /* LSW2_POPUPHUD_H */
