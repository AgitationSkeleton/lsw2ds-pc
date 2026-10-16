/* The top-screen portrait following a Free Play character change.
 *
 * THE DEFECT: after a Free Play character change, the top-screen portrait
 * stays stale until a tag to a partner and a tag back again. A
 * change from Greedo to Jango, for example, leaves the icon showing Greedo.
 *
 * The symptom carries the useful half of its own diagnosis. A tag DOES bring
 * the portrait up to date, so a refresh path exists; a Free Play switch just
 * never runs it - which makes the job reusing the tag's refresh, not painting
 * a portrait of our own.
 *
 * THE ASYMMETRY IS IN THE WIDGET'S OWN CODE. Both paths change the
 * party; only one tells anything about it:
 *
 *     0x02068598  input        BL party_refresh(widget, 0)
 *                              BL redraw_page(widget)          <- redrawn
 *     0x020687B4  slot_touched BL party_swap(widget, slot)
 *                              ... and nothing else
 *
 * And `party_refresh` is the right function rather than a layout pass that
 * happens to be missing: it walks the four player slots at
 * `0x0210930C + i*4 + 0x4C` and reads `[actor + 0x6BB]`, the character id the
 * portrait is drawn from.
 *
 * WHY THIS IS NOT AN OVERRIDE, checked before anything was written. Every
 * caller of all four functions is in the same generated bank - slot_touched
 * 1/1, party_swap 1/1, party_refresh 2/2, redraw_page 5/5 - so the recompiler
 * emits direct symbol calls and a dispatch override on any of them can never
 * fire. Three separate pieces of dead work paid for that habit.
 *
 * SO IT WATCHES AND CALLS, the shape `gearapply.h` uses. The four slots'
 * character ids are read once a frame - the same words `party_refresh` reads -
 * and when one changes the game's own two functions are called through
 * `lsw2_guest_call`, which invokes the lifted symbol directly and does not
 * depend on the dispatch table at all.
 *
 * That is deliberately not a reimplementation: it is the exact pair the d-pad
 * path already runs, in the same order, so the fix cannot diverge from the
 * behaviour it is copying.
 *
 * BOTH BUILDS: this is an Enhancement, not a fork-only feature. The bug is the
 * cartridge's, so the widget
 * capture that feeds this was moved out of the fork-only override table.
 */

#ifndef LSW2_PARTYPORTRAIT_H
#define LSW2_PARTYPORTRAIT_H

#include "cheats.h"
#include "levels.h"

#include <cstdint>

/* The game's own pair, in the order the working path calls them. */
constexpr uint32_t kLsw2PartyRefresh = 0x020691B4u;
constexpr uint32_t kLsw2RedrawPage = 0x02068D74u;

/* The player slot table party_refresh itself walks is levels.h's
 * kLsw2LevelRootAddr plus cheats.h's kLsw2PartyActors, and the character id
 * byte is kLsw2ActorCharacter. This file used to declare all four of those
 * facts itself - the root address as kLsw2PlayerSlots, the array offset as
 * kLsw2PlayerSlotActor, the count as kLsw2PlayerSlotCount and the byte as its
 * own kLsw2ActorCharacter. It is the same array cheats.cpp walks for the
 * last-played-hunter boot. */

/* Called from the flag-test override when the Free Play widget's constructor is
 * the caller - `lr == 0x02068384`, which nothing else arrives with. The pointer
 * is all this module needs and there is no other way to learn it. */
void lsw2_partyportrait_note_widget(uint32_t widget);

/* Once a frame from the fork tick, in both builds. Does nothing until a widget
 * has been seen, nothing while the party is unchanged, and nothing when the
 * enhancement is off. */
void lsw2_partyportrait_tick();

/* ASK THE GAME TO REDRAW, by holding one byte non-zero for a frame.
 *
 * ---- superseded, kept because it is why this is not a guest call ----------
 * This used to call redraw_page through lsw2_guest_call and had to be drained
 * from a dispatch override, because redraw_page copies tiles by hardware DMA
 * and a call made between frames has no scheduler to service the transfer -
 * it stalls the CPU mid-routine and unwinds (guestcall.h has the
 * disassembly).
 *
 * THAT DESIGN WAS KILLED BY MEASUREMENT, 2026-09-01. Every registered override
 * was instrumented to report when it runs and a character switch was driven.
 * Whole-run counts: script_command 12, flag_test_array_c 5, state_message 3,
 * string_decode 2, script_enqueue_wrapper 2 - and the count of any override
 * firing AFTER the refresh saw the new character was ZERO. They all run during
 * boot and lobby setup and then stop. Both wired drain points, the save-flag
 * query and the gear applier, were therefore dead, and a third would have been
 * dead the same way. The port has no per-frame guest hook.
 * --------------------------------------------------------------------------
 *
 * ---- and the mode nudge that replaced it was TRIED AND FAILED, same day ---
 * partyportrait.cpp carries the disassembly of 0x02068598's tail: the update
 * handler refreshes and redraws only when the widget's mode byte at +0x35 is
 * non-zero, and selectwidget.h records mode 0 as the Free Play select. That
 * looked like the cartridge's bug in one comparison, and the fix looked like
 * one byte: hold mode non-zero for a frame and let the game run its own pair
 * from inside its own update, where the DMA works.
 *
 * IT RAN AND IT CHANGED NOTHING. Driven live, twice:
 *
 *     [fork] portrait: holding select mode non-zero ... (mode was 0)
 *     [fork] party portrait refreshed: slots 34 100 100 100
 *
 * so the byte really is 0 there, the hold really happened, and the refresh
 * really saw Jango - and a capture of the top screen still showed the Greedo
 * portrait. Repeated with the hold widened to 60 frames: unchanged. Two
 * different windows, same result, so this is not a timing miss.
 * --------------------------------------------------------------------------
 *
 * WHY, AND IT WAS ALREADY WRITTEN DOWN IN THIS TREE. slotcolour.h records the
 * running port reporting, with the ring and hearts plainly on the top screen:
 *
 *     POWCNT1 = 0x820F           swap bit set -> engine A IS the top screen
 *     engine A DISPCNT=0x00010358    BG0=1 BG1=1 BG2=0 BG3=0 OBJ=0
 *     OAM main: 0 live sprites
 *
 * The top screen's HUD is a BG LAYER. redraw_page is the select widget's own
 * page redraw - the grid of portraits on the TOUCH screen - so running it was
 * never going to move the top-screen portrait, and neither the DMA problem nor
 * the missing per-frame hook was ever the thing standing in the way. Three
 * rounds of work went into delivering a call that would not have fixed this
 * even if it had landed perfectly.
 *
 * THE OPEN QUESTION IS NOW A DIFFERENT ONE: which routine writes the top
 * screen's portrait BG tiles, and what makes it run. That is a search for a
 * VRAM writer, not for a scheduler hook - instrument the BG write path to
 * report the guest r15 when a write lands in the portrait's tile region, drive
 * a switch, and let the writer name itself. Note that tagging a
 * partner DOES refresh it, so the routine exists and something on the tag path
 * calls it.
 *
 * **AND THE PORTRAIT HAS NO BG TILES, SO THAT SEARCH WOULD FIND NOTHING.**
 * Measured 2026-09-05 on the running port, in the lobby at state 48.
 *
 *   - POWCNT1 = 0x820F, so engine A is the top screen, as slotcolour.h said.
 *   - Engine A DISPCNT = 0x00011358: mode 0 with bit 3 SET, so BG0 IS THE 3D
 *     LAYER; BG1 and OBJ are the only other things enabled.
 *   - Engine A's whole 2KB BG map was read and scanned. It holds exactly
 *     SIXTEEN non-background cells - rows 3-4, columns 6-13, alternating tile
 *     pairs 0x42/0x43 and 0x4A/0x4B. That is the HEARTS. Nothing else.
 *   - Engine A's OAM reads attribute0 = 0x0200 on every entry, which is the
 *     hidden encoding, so no sprite is drawing it either.
 *
 * So the round character icon is neither a BG tile nor a sprite, which is an
 * earlier investigation's conclusion arrived at from the other direction: IT
 * IS 3D. A hunt
 * for the routine that writes its tiles is a hunt for something that does not
 * exist, and the paragraph above is kept only so nobody re-derives it.
 *
 * AND THE PARTY ARRAY BEING STALE IS NOT THE ANSWER EITHER - an earlier
 * measurement already eliminated it. Measuring a Free Play switch shows the array at
 * levelRoot + 0x4C still naming the old actor while the switch dressed a new
 * one, which looks exactly like the cause and is not: that measurement poked five
 * references to the live actor, one at a time with a capture between, and the
 * portrait did not refresh. That measurement was repeated here by accident,
 * which is reason enough to write it down.
 *
 * WHAT IS LEFT, then, is the 3D submission: what geometry is drawn for that
 * icon each frame, and which field tells it whose head to use. Not tiles, not
 * sprites, not the actor pointer.
 *
 * Until then this only records that the party changed, once, so the log does
 * not imply a fix that is not there. */
void lsw2_partyportrait_redraw_if_pending();

/* THE PARTY REFRESH'S ANSWER, delivered when the hop's call comes back.
 *
 * `0x020691B4` had never once returned before this: it
 * was called through `lsw2_guest_call`, which runs between frames where the
 * machine's event queue is EMPTY, so a function that waits for an
 * interrupt can never finish. gearhop.cpp now runs it on the guest's own
 * thread, and because that answer arrives on a later frame the caller cannot
 * read R[0] inline - it is handed here instead. */
void lsw2_partyportrait_refresh_returned(uint32_t result);

/* ---------------------------------------------------------------- the policy
 *
 * Split out so `test_partyportrait.cpp` can pin "when should the refresh run"
 * without a ROM or a guest, the same split customization_policy.cpp makes. */

/* Should a refresh be requested? True when any slot's character differs from
 * what was last seen AND a previous set was recorded - the first observation
 * establishes the baseline rather than firing, because at that point nothing
 * has changed yet and refreshing would be a guess dressed as a fix. */
/* THE VALUE AN UNFILLED PARTY SLOT HOLDS.
 *
 * hostsettings.h already names it from the other side - "the game's own 'not
 * available' padding (0x64)" - and every log line this file has ever printed
 * shows it: `party 20 100 100 100` is one seated character and three empty
 * seats.
 *
 * It is named here because the rule below now has to tell an empty slot from a
 * chosen one, and a bare 100 in that comparison would be exactly the kind of
 * unexplained constant this tree keeps deleting. */
constexpr uint8_t kLsw2PartyEmpty = 100u;

bool lsw2_partyportrait_changed(const uint8_t* now, const uint8_t* before,
                                int count, bool have_before);

#endif /* LSW2_PARTYPORTRAIT_H */
