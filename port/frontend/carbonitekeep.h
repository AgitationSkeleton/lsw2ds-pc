/* A CAPTURE TAKES THE CARBONITE, NOT THE CUSTOMIZATION. Capturing the flag
 * used to reset the carrier's customizations, weapon included. There were two
 * candidate fixes: put the carrier's real customizations back once the
 * Carbonite backpack is cleared, or take away the Carbonite backpack alone.
 *
 * WHAT THE GAME DOES. The capture's netgame message is handled at 0x020B208C
 * (the handler's dispatcher is interpreted: the lift of FUN_020B1248 stops at
 * 0x020B1268). It finds the carrier - party[[0x02107860] + 0x41] - and calls
 *
 *     020B20B8  bl 0x02006220 (carrier, 0)       and returns to 020B20BC
 *
 * then builds a fresh Carbonite at its spawn. 0x02006220 is the game's
 * RESET TO STOCK: for each of the five part slots it attaches the character's
 * stock part (the 0x020EEBB8 table, five bytes a character) through the gear
 * applier 0x020063D4, or stores 0xFA - and for slot 4 also clears the carry
 * flags +0x1DF and +0x207 - then sets +0x6C3 to the character's stock weapon
 * and arms it (0x02004404). In the cartridge the carrier was wearing stock
 * anyway, so this only takes the Carbonite off.
 *
 * WHAT THE PICKUP CHANGED, measured on a carrier (conn_ctfcust4, with a
 * carry probe): slot 4 = part 234 and +0x207 = 1, nothing else - the hat
 * (187) and the weapon (+0x6C3 = 17, a lightsaber) stay through the carry.
 * The capture then left hat 250 and weapon 26: the stock blaster. The write
 * watch named the writers: 0x0200626C and 0x0200645C, under 0x020B20BC.
 *
 * THE FIX, the second of those: only the carried slot is undone. The call
 * from the capture crosses shards and dispatches, so an override on 0x02006220
 * recognises it by its return address and performs the reset's own step for
 * slot 4 - the stock part through the gear applier, or 0xFA and the two carry
 * flags cleared - and returns to the handler. Every other call to the reset is
 * the game's, unchanged. It runs inside the handler, on every console at the
 * same dataset, and reads nothing but guest state, so a session stays in
 * lockstep. LSW2_CARBONITE_KEEP_OFF=1 is the control run's kill-switch. */

#ifndef LSW2_CARBONITEKEEP_H
#define LSW2_CARBONITEKEEP_H

#include <cstdint>

/* The reset to stock, and the capture's call of it. */
constexpr uint32_t kLsw2StockAppearance = 0x02006220u;
constexpr uint32_t kLsw2CaptureResetReturn = 0x020B20BCu;

/* The dispatch override on kLsw2StockAppearance (fork builds). */
void lsw2_carbonitekeep_override();

#endif /* LSW2_CARBONITEKEEP_H */
