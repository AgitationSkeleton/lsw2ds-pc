/* The guest half of the baseline build's co-op lobby list (lobbies listed by
 * the master server, shown in the cartridge's own browse screen).
 *
 * coophosts.h has the derivation and the pure rules. This is the part that
 * touches the guest: it fills the cartridge's own host table so the list draws
 * the fork's lobbies, and it answers the connect so choosing one joins over TCP.
 *
 * TWO PIECES, AND THE SECOND IS WHY THE FIRST WAS GATED:
 *
 *   PUBLISH   write records into 0x0210BF40. Needs no override at all - the
 *             table's address is a constant returned by vtable slot 0x58, and
 *             the count in slot 0x5C is DERIVED by walking the records' TTLs
 *             So whatever the fork puts there, the cartridge itself
 *             counts, draws, scrolls and hit-tests.
 *
 *   CONNECT   `0x020ADBCC` is vtable slot 0x10, and the list hands it the RECORD
 *             POINTER. Its real work is a WM connection to a DS that is not
 *             there, so it has to be replaced.
 *
 * WHY REPLACING IT IS SAFE HERE. `0x020ADBCC` serves several modes - the list
 * passes 5, the host path passes others - and normally reproducing the ones you
 * do not want would be the trap. In this port none of them can succeed: they all
 * end in the cartridge's wireless, which the recomp does not run. Replacing the
 * function therefore loses nothing that works.
 *
 * THIS USED TO CARRY A WARNING THAT "an override REPLACES and cannot defer to
 * the guest", AND THAT IS NOT THE WHOLE CONSTRAINT. The seam replaces, but it
 * can tail-call - and that changes what is reachable: setting `g_cpu.R[15]` to
 * a guest entry while R[14] still holds the caller's return address IS a
 * deferral. lsw2_guest_call packages the same capability and eight sites use
 * it.
 *
 * The false limit is worth naming because it is the fourth appearance of that
 * sentence and the second to invert its source: hostsettings.cpp used it to
 * call Music: RANDOM unbuildable, and strings.h used it to justify
 * reimplementing the decoder. A false limit that arrives wearing a citation
 * looks checked, and this one was citing the correction as the problem. The
 * conclusion here was right anyway - replacing 0x020ADBCC loses nothing - so
 * nothing changes but the reasoning.
 *
 * WHICH LOBBY A ROW IS, without a field. The fork could stash an index inside
 * the record, but it does not need to: the record's ADDRESS gives it, because
 * the table is a fixed base and a fixed stride. `(record - 0x0210BF40) / 0xC4`
 * is the row, and nothing in the guest can corrupt it.
 */

#ifndef LSW2_COOPJOIN_H
#define LSW2_COOPJOIN_H

#include <cstdint>

/* The wireless owner word. ONE COPY, and the two were not even spelled the
 * same way: coopjoin.cpp folded the field into the address as
 * `0x02106D90u + 0xBB8u` while diagnostics.cpp kept a separate base and
 * kWirelessOwnerField. Same word, and the folded form hid that they matched. */
/* THE CARTRIDGE'S "START A WIRELESS SESSION", 0x020B10A8.
 *
 * Moved up here from coopjoin.cpp because it now has two readers: the override
 * that REPLACES it, and whsession.cpp's bring-up which RUNS it. Declaring it
 * twice is how the two would drift, and dup_constants caught exactly that. */
constexpr uint32_t kLsw2WirelessStart = 0x020B10A8u;

/* THE CARTRIDGE'S "Browsing Wireless Game" SCREEN - the child's door, and the
 * counterpart of the session start above: 0x020B10A8 makes a PARENT, this
 * screen makes a CHILD. Its list's connect is 0x020ADBCC, which takes a
 * 0xC0-byte host record out of a scan.
 *
 * SHARED BECAUSE IT IS ONE FACT. It lived twice - `kListState` here for the
 * baseline's list publishing and `kLsw2BrowseState` in whsession.h for the
 * fork's role switch - and dup_constants flagged the pair by value. They are
 * not a coincidence of offsets like the +0xEC case; they are the same screen
 * reached for two reasons, so they get one name. */
constexpr int kLsw2BrowseState = 0x22;

/* THE CARTRIDGE'S "END A WIRELESS SESSION", 0x020B111C - the counterpart of
 * the start above, read off pause row 1 and decompiled whole:
 *
 *     0x020B111C(owner, 1) = a state request, then FUN_02072bc8, then
 *     owner[6] = owner[7] = owner[8] = owner[0xd] = 0
 *
 * and owner[6] = 0 is the exact inverse of the session start's owner[6] = 1,
 * which is how the cartridge records that a session is up.
 *
 * IT CANNOT BE CALLED FROM THE HOST, and that is why a clean "Shut Down Lobby"
 * return to single player resisted every attempt for so long. FUN_02072bc8 dispatches the WH object's vtable+0x50 -
 * WH_End - and if that returns zero it POLLS it in a do-while until it does
 * not. A synchronous call injected from the host can never come back out of
 * that loop. It has to run on the guest's own thread, which is what the
 * trampoline in whsession.cpp is for. */
constexpr uint32_t kLsw2WirelessEnd = 0x020B111Cu;

constexpr uint32_t kLsw2WirelessOwner = 0x02106D90u;
constexpr uint32_t kLsw2WirelessOwnerField = 0xBB8u;

/* Fill the cartridge's table from what the session knows, and keep the rows
 * alive - the browse phase ages each record's TTL once a tick, so a published
 * row that is not refreshed fades out while the player looks at it.
 *
 * Call once a frame. Silent in the fork build and while no lobbies are known. */
void lsw2_coopjoin_publish();

/* Install the connect override. Called from the same place as the rest of the
 * dispatch table, and BEFORE that function's fork-only return, because this is
 * the BASELINE's override - the baseline build's co-op lobby list.
 *
 * TAKES THE FORK FLAG rather than asking ui_fork_build(), which is always false
 * at registration time in both executables (dispatch_overrides.cpp says so in
 * the comment at the top of the calling function). Asking it there is what made
 * this install in the fork and never in the baseline - the exact reverse of
 * both halves of its own contract. */
void lsw2_coopjoin_install(bool fork_build);

/* Which published row a record address is, or -1 if it is not one of ours. */
int lsw2_coopjoin_row_for(uint32_t record);

#endif /* LSW2_COOPJOIN_H */
