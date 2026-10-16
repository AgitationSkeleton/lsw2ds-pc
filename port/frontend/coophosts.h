/* Filling the cartridge's own "nearby DS systems" table.
 *
 * The base recomp's lobby list should come from the master server and the LAN
 * while keeping the game's usual UI presentation. Reading the list screen showed
 * that this needs no drawing at all:
 *
 *   the list screen is state kind 0x22, and it reads its rows from a wireless
 *   manager through two vtable slots - and BOTH of those turn out to be
 *   constants over one fixed table:
 *
 *     0x020AD8DC  (slot 0x58)  ldr r0, =0x0210BF40 ; bx lr
 *     0x020AD8E8  (slot 0x5C)  count records with a non-zero TTL, sixteen of them
 *
 * So the table is at a FIXED ADDRESS and the count is DERIVED FROM IT. Nothing
 * has to be overridden to make the list show what the fork knows - the fork
 * writes records, and counting, drawing, scrolling and hit-testing them is
 * all the cartridge's own work.
 *
 * THE RECORD, as far as the list reads it:
 *
 *   +0x48  halfword  status; a record whose low two bits are 2 is SKIPPED
 *   +0x50  string    the host's name, capped at 30
 *   +0xC0  word      time-to-live; zero means "empty slot"
 *
 * THE TTL IS AGED ONCE A TICK by the browse phase, so a
 * published row has to be refreshed or it will fade out of the list while the
 * player is looking at it. Refreshing every frame is simplest and costs four
 * writes per row.
 *
 * WHAT THIS FILE DOES NOT DO. It does not touch the connect. Selecting a row
 * hands the RECORD POINTER to vtable slot 0x10 (`0x020ADBCC`), which is the
 * cartridge's own wireless connect and would try to reach a DS that is not
 * there. Overriding that is the other half of the master-server lobby list and
 * is deliberately not
 * here: a list that draws correctly and cannot join is a worse failure than an
 * empty one, because it looks like it works. Until the connect is redirected,
 * publishing is gated on an explicit call rather than happening by default.
 */

#ifndef LSW2_COOPHOSTS_H
#define LSW2_COOPHOSTS_H

#include <cstdint>

/* The cartridge's own table. Sixteen records, 0xC4 bytes each. */
constexpr uint32_t kLsw2HostTable = 0x0210BF40u;
constexpr uint32_t kLsw2HostStride = 0xC4u;
constexpr int kLsw2HostSlots = 16;

/* Field offsets within a record. */
constexpr uint32_t kLsw2HostStatus = 0x48u;
constexpr uint32_t kLsw2HostName = 0x50u;
constexpr uint32_t kLsw2HostTtl = 0xC0u;
constexpr int kLsw2HostNameMax = 30;
/* How long a published row lives without a refresh. The browse phase decrements
 * the TTL once a tick and drops the row at zero, so anything comfortably larger
 * than one frame works; a second's worth means a row survives a hitch. Both
 * writers of this table (coopjoin.cpp, the host-level WH in vwh.cpp) use it. */
constexpr uint32_t kLsw2HostRowTtl = 60;

/* THE PURE HALF: which of our lobbies goes in which slot, and what the status
 * and TTL should be. Split so the rules are testable without a ROM, the same
 * division customization_policy.cpp and lobbytimer.cpp make.
 *
 * `wanted` is how many lobbies we have; returns how many rows will be written,
 * which is `wanted` clamped to the table. Reporting the clamp is the point -
 * silently showing sixteen of twenty is the quiet truncation this project keeps
 * warning about. */
int lsw2_coophosts_rows(int wanted);

/* The status halfword a published row carries.
 *
 * NOT ZERO, and that is the whole reason this is a function. The list skips a
 * record whose `status & 3 == 2`, so any value avoiding that works - but which
 * value is a decision, and the honest one is the smallest that is plainly "not
 * the excluded state" rather than a number copied from a capture nobody has
 * taken. */
uint16_t lsw2_coophosts_status();

#endif /* LSW2_COOPHOSTS_H */
