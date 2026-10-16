/* CHARACTER CUSTOMIZATION IN A SHARED SESSION: each player's hat and weapon, on
 * every console.
 *
 * A player spawns in the Cantina lobby and sees the other connected players,
 * the host among them, wearing their customizations - and sees their own.
 *
 * WHY NOT THE SOLO PATH. customization.cpp dresses party slot 0 by writing the
 * game from the between-frames tick, and in a session that is two mistakes at
 * once: slot 0 is the HOST on a joiner, and a write from the tick lands at a
 * different step on every console - so it is gated off there.
 *
 * THE CARTRIDGE ALREADY NETWORKS THIS. Its customize menu's helpers 0x0207B240
 * (hat: actor, slot, part) and 0x0207B2C8 (weapon: actor, row) return at once if
 * the actor already wears it, and in a wireless session (bit 0x40 of 0x027E02A8)
 * they do not dress anything: they send netgame message 0x2E {station, slot,
 * part} or 0x2F {station, row}, and the handlers at 0x020B2864 / 0x020B2890 dress
 * party[station] on EVERY console at the same dataset. So a console only has to
 * press its own buttons: whenever its own player's actor is (re)built - the lobby
 * spawn, the arena spawn (whose enter clears every player's parts, 0x02098BD4),
 * a character switch - it calls those helpers for its saved set, filtered by the
 * host's Customization policy (lsw2_customization_effective), through the gear
 * hop. When the call runs does not matter; what it changes arrives as a message.
 *
 * Only this console's OWN station is ever dressed here; the others dress
 * themselves and their messages do the rest. */
#ifndef LSW2_SESSIONDRESS_H
#define LSW2_SESSIONDRESS_H

/* Once a frame from the fork tick. Silent outside a shared session. */
void lsw2_sessiondress_tick();

#endif /* LSW2_SESSIONDRESS_H */
