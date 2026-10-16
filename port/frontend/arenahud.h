/* The match HUD, drawn into the game's own frame.
 *
 * WHAT IT IS. Not an imgui overlay: it copies, from the touch screen, the
 * portraits of the remaining three players (kept in the touch screen's order,
 * NOT re-sorted by who is winning) into the bottom-left corner, each with its
 * white and red skull/carbonite counts next to it, plus the radar at the top
 * right. The setting is toggled through
 * imgui, but the presentation is in-game.
 *
 * THE RULE: the switch lives in imgui, the pixels come from the game. `gamemenu_render.h`
 * already argued this for the fork's menus and named this HUD as its
 * counter-example; all three of its reasons apply here unchanged.
 *
 * WHAT MAKES IT CHEAP. The pieces are mirrored from the touch screen rather than
 * rebuilt, so the counters, the mode-dependent icons and the radar all stay
 * exactly what the game drew. The two findings that matter are:
 *
 *   - the radar bezel is a BACKGROUND, not a sprite, so a sprites-only mirror
 *     would leave a hole where it should be;
 *   - and bg2 is the starfield - the only layer covering all 49152 pixels, and
 *     the only one scrolling - so switching that one layer off leaves the HUD
 *     on a plain backdrop with nothing else disturbed.
 *
 * Switching it off is acceptable because the port already hides that screen
 * during a match. Nobody is looking at it.
 */

#ifndef LSW2_ARENAHUD_H
#define LSW2_ARENAHUD_H

#include <cstdint>

/* Once a frame, before the composite. Owns the starfield layer: turns it off
 * while the HUD is up and puts it back the moment it is not. */
void lsw2_arena_hud_tick();

/* Draw the HUD into `layer`, the top screen's overlay layer for this frame
 * (topoverlay.h): `width` x 192, as wide as the frame being presented, with the
 * pieces anchored to its corners. Draws nothing when the HUD is not active, so
 * the present path can call it unconditionally.
 *
 * THE SLOT-COLOUR RING'S SECOND HALF IS SATISFIED HERE, BY CONSTRUCTION, and
 * that is worth writing down because it looks like missing work.
 *
 * The portrait ring taking the player's slot colour has two halves. The first
 * is the POV player's own ring, which slotcolour.cpp does by rewriting an OAM
 * palette field. The second is that this HUD's mirrored column must draw the
 * remaining three players in their respective colours too - the HOST among
 * them whenever someone else is the POV player - with the fork's own rendering honouring that
 * rather than assuming it.
 *
 * There is no code for that, and none is needed: this column is a PIXEL COPY.
 * `blit_scaled(touch, ...)` takes each row straight out of the sub engine's
 * finished framebuffer, and the cartridge has already drawn that row in its
 * player's palette. The colours arrive because they were never removed.
 *
 * SO THE THING TO PROTECT IS THE COPY. If this ever synthesises portraits -
 * drawing its own boxes, or blitting one row and tinting it - the other
 * players' slot colours break silently, because nothing about the column's
 * LAYOUT would look wrong. A source sweep under tools/ guards the blit for
 * that reason.
 *
 * The reverse is also true and is why slotcolour.cpp must not be pointed at
 * the sub engine: those nineteen sprites are this column's source, and the
 * HUD's read_rows() identifies players by exactly the palette bank a rewrite there
 * would overwrite.
 *
 * `layer` is the HUD's own layer (topoverlay.h, lsw2_top_overlay_hud_begin),
 * `width` wide and 192 * kLsw2HudLayerScale tall - twice the frame's size, so a
 * half-size HUD keeps every touch pixel. Only the art is copied: the pixels the
 * touch screen showed as backdrop stay transparent (patch 0018's mask). */
void lsw2_arena_hud_composite(uint32_t* layer, int width);

/* Whether the HUD is drawing right now - the touch panel asks, because the
 * HUD REPLACES it during a match rather than sitting beside it. */
bool lsw2_arena_hud_active();

#endif /* LSW2_ARENAHUD_H */
