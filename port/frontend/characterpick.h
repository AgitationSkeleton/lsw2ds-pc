/* The host settings menu's character picker.
 *
 * When the host's character setting is 'One', a second row appears beneath
 * it, 'Character:', defaulting to Greedo. Activating that row opens the Free
 * Play picker so the host can choose the one character every player uses -
 * any playable character, including odd picks like R5-D4 or Gonk Droid - and
 * the choice is confirmed inside that picker.
 *
 * IT IS THE FREE-PLAY TOUCH SCREEN MENU - the cartridge's own widget on the
 * bottom screen, not a look-alike.
 *
 * AN EARLIER VERSION OF THIS FILE ARGUED THE OPPOSITE AT LENGTH AND THE
 * ARGUMENT WAS WRONG. It concluded that the picker was the fork's own menu,
 * not the cartridge's widget - from reading "pseudo-freeplay menu" as a menu
 * that merely resembles the Free Play one, and from "pressing A" having no
 * path in the cartridge widget (its three button tests are L, R and B, and
 * confirmation is a double tap via `+0x36 == entry`).
 *
 * Both observations are still true and the conclusion drawn from them was not.
 * The double tap is not evidence AGAINST the cartridge widget - it is a
 * confirm named precisely because that is how that widget confirms. Reading
 * "pseudo-" as "not the real one" was the error, and it produced a top-screen
 * menu where the bottom-screen one was wanted.
 *
 * WHAT THAT REOPENS, stated plainly rather than left for the next reader to
 * rediscover:
 *
 *   - The conflict this file claimed to have retired is REAL again. The lobby's
 *     hunters-only clamp holds the Free Play widget to the two Bounty Hunter
 *     pages in the arena lobby, and the host's picker must reach any playable
 *     character, R5-D4 and Gonk Droid included. The clamp has to stand down
 *     while the host is picking. `forced` in dispatch_overrides.cpp is where
 *     that lives.
 *   - The double-tap confirm comes free, because it is the widget's own.
 *   - "Pressing A" IS SETTLED AND THE ANSWER IS TO DROP IT. The picker uses the
 *     normal Free Play touch-menu controls and logic, and nothing else.
 *
 *     So the picker is the cartridge's Free Play touch screen with ITS OWN
 *     controls: L and R page, a double tap confirms, B backs out. The fork
 *     invents no input path. "Pressing A on it" means the HOST MENU's
 *     Character row being activated - the fork's own menu, where A is
 *     already the activate button - and it opens the widget. Nothing more.
 *
 *     That also means this file must not grow an A-confirm. A control the
 *     cartridge does not have is exactly the kind of invention the port has
 *     been wrong to make before.
 *
 * ~~THE CODE BELOW STILL BUILDS THE FORK'S OWN MENU.~~ RETRACTED:
 * the fork-drawn list is gone. The Character row now hands the cartridge's
 * own Free Play grid to the host (freeplaypick.h), replacing the individual
 * character list the one-character picker used to draw. This file keeps the
 * table and the name source, which the host menu's Character row and the
 * roster still read.
 *
 * ------------------------------------------------------------------------
 * THE WIDGET, READ OUT OF THE ROM SO THE NEXT PASS IMPLEMENTS RATHER THAN
 * RE-DERIVES. Everything here came from the ROM image.
 *
 * IT IS ALREADY ON SCREEN, which is the part that makes this small. The lobby
 * puts the Free Play select on the touch screen in the arena lobby, so the host
 * does not need the fork to summon anything: activating the Character row
 * means "stand the clamp down and start listening", not "open a screen".
 * lsw2_hle_fork_hunters_only already does the first half.
 *
 * HOW IT IS BUILT, for whoever does need to summon it. 0x020682C4 constructs
 * the widget and has THREE callers - 0x02087700, 0x0208792C and 0x02087B94 -
 * all the same shape: a parent screen object with fields at +0x28, +0x68 and
 * +0xC8, storing the result into its own +0xC4. It is a TAB of the touch
 * screen, not a standalone screen, so "opening" it means moving the parent's
 * tab rather than calling a constructor.
 *
 * THE FIELDS THE PICK LIVES IN, from the widget's disassembly and selectwidget.h:
 *
 *     +0x20   the current page          (kLsw2SelectPage)
 *     +0x24   the low page bound        (kLsw2SelectPageLow)
 *     +0x28   the high page bound       (kLsw2SelectPageHigh)
 *     +0x35   which confirm runs: 0 -> 0x02068964, else -> 0x02068B90
 *     +0x36   THE SELECTED ENTRY, or 0xFF for none
 *
 * THE CONFIRM IS THE DOUBLE TAP, at 0x020687B4:
 *
 *     02068908  ldrb r1,[r6,#0x36]    the previous selection
 *     0206890C  and  r0,r5,#0xFF      the entry just tapped
 *     02068910  cmp  r1,r0
 *     02068918  ldmnefd sp!,{...,pc}  DIFFERENT entry: highlight and return
 *     0206891C  ...                   SAME entry twice: confirm
 *
 * So a first tap writes +0x36 and returns; a second tap on the same entry
 * falls through. freeplaypick.cpp now sees that second tap before it
 * confirms - the tap handler asks 0x02086534 whether the character is
 * unlocked first - and takes the character id from that very query.
 *
 * AND THE POINTER IS ALREADY CAPTURED. dispatch_overrides.cpp notes the widget
 * for the party portrait when `lr == kFreePlayCtorReturn`, and hands it to
 * lsw2_partyportrait_note_widget. That pointer is file-static in
 * partyportrait.cpp with no accessor, because the portrait was its only
 * consumer. A second consumer means it wants a neutral home rather than an
 * accessor reaching into the portrait module - that is a small refactor and it
 * is the first thing the implementing pass should do, not an afterthought.
 *
 * WHAT IT SHARES WITH THE REAL SCREEN is the Free Play layout itself:
 * twelve pages of eight, from the cartridge's own table at 0x020EDDB2, paged
 * left and right. The fork does not invent an ordering.
 *
 * THE DOUBLE-TAP NOTE BELOW IS SUPERSEDED BY THE CORRECTION ABOVE, and is
 * kept rather than deleted because it is the reasoning that has to be undone
 * and the next reader should see why it was wrong, not just that it was.
 *
 * Two ways to confirm were wanted: pressing A, or double-tapping the
 * character icon. The (wrong) reasoning above established that this was the
 * fork's own menu, and the fork's menus are composited into SCREEN 0 - the top
 * screen - by frontend.cpp's present path, alongside the arena HUD and the
 * lobby countdown. The DS's touch panel is the bottom screen. There is no tap
 * to double on a top-screen menu, so the double-tap alternative had nowhere
 * to land.
 *
 * Under that reading the intent was met: the host could confirm, with the
 * button named first and the one that works on that menu. What was NOT met
 * was the literal "or", and the only way to meet it was to move the picker to
 * the bottom screen - which would take it out of the menu system every other
 * fork screen uses, and put it back on the touch surface the arena HUD
 * replaces during a match.
 *
 * That trade was left as a design decision rather than made here: a
 * bottom-screen picker is a deliberate change of shape and not a small
 * addition. It is the change that was eventually made (freeplaypick.h). */

#ifndef LSW2_CHARACTERPICK_H
#define LSW2_CHARACTERPICK_H

#include <cstddef>
#include "cheats.h"
#include <cstdint>

/* The cartridge's select table, reproduced from 0x020EDDB2. Twelve
 * pages of eight; 0x64 is the game's own "not available" padding. */
constexpr int kLsw2PickPages = 12;
constexpr int kLsw2PickSlots = 8;
/* cheats.h's kLsw2CharacterNone - 100, what the game writes where a
 * character id would go when there is no character. */
extern const uint8_t kLsw2PickTable[kLsw2PickPages][kLsw2PickSlots];

/* THE LAST PAGE IS THE THREE CUSTOM SLOTS, and the picker does not offer it.
 *
 * Read, not assumed: page 11 holds exactly three entries, they are the three
 * highest ids in the whole table (73, 74, 75), and the three custom character
 * slots are exactly what Random All excludes. Three of three is a strong match
 * and it is still a match rather than a proof, so it is stated here where it
 * can be challenged.
 *
 * Why exclude them from the host's pick when the requirement only excludes
 * them from Random All: an empty custom slot is not a character, and a host who
 * chose one would put every player in a match as something the cartridge has
 * not been given. The pick is "any playable character", and an unfilled
 * slot is not one. */
constexpr int kLsw2PickCustomPage = 11;
constexpr int kLsw2PickOfferedPages = kLsw2PickCustomPage;

/* WHERE THE NAMES COME FROM.
 *
 * The cartridge's own, decoded at string id `0x1CB + id` - sixty-five
 * names the game already holds, which is sixty-five fewer chances to misspell
 * one than a table here.
 *
 * BUT THE DECODER NEEDS A LIVE GUEST, and this file must not. So the lookup is
 * a SEAM, the same shape forkpopup.h uses for its popup sink: the frontend
 * installs the real decoder at startup, and until it does - or in a test - the
 * default writes a truthful placeholder. A first attempt called the decoder
 * directly and would have dragged the entire guest runtime onto the test's link
 * line. */
using Lsw2CharacterNameFn = const char* (*)(int character_id, char* buf,
                                            size_t n);
void lsw2_set_character_name_source(Lsw2CharacterNameFn fn);
const char* lsw2_character_name(int character_id, char* buf, size_t n);

#endif /* LSW2_CHARACTERPICK_H */
