/* The Free Play character-select widget's field map.
 *
 * Read out of its constructor at 0x020682C4 and its tap handler at
 * 0x020687B4; each field below cites the instruction that identifies it.
 *
 * WHY THIS IS A HEADER AND NOT THREE MAGIC NUMBERS. Two files already reach
 * into this object - dispatch_overrides.cpp narrows the page bounds for the
 * hunters-only Free Play roster, and the boot-into-the-lobby path's apply will
 * set the page and the selection - and a third is coming. An
 * offset that appears in several files as a bare `+ 0x24u` is an offset nobody
 * can grep for when it turns out to be wrong, which is a failure this project
 * has recorded repeatedly with this ROM's structures.
 *
 * NOTHING HERE IS GUESSED. Every offset below is a store or a load in the
 * disassembly, with the instruction cited.
 */

#ifndef LSW2_SELECTWIDGET_H
#define LSW2_SELECTWIDGET_H

#include "cheats.h"
#include <cstdint>

/* The page currently shown.
 *     02068314  str  r3,[r4,#0x20]   = 0 at construction
 *     02068604  str  r1,[r4,#0x20]   written by the paging code */
constexpr uint32_t kLsw2SelectPage = 0x20u;

/* The page range, as a half-open interval [low, high).
 *     02068318  str  r3,[r4,#0x24]   = 0
 *     02068320  str  r2,[r4,#0x28]   = 0x0C, and the table has twelve pages
 *
 * HALF-OPEN IS READ, NOT ASSUMED: the increment at 0x0206860C wraps to `low`
 * when the new page equals `high`, and the decrement at 0x020685F0 wraps to
 * `high - 1` when it falls below `low`. */
constexpr uint32_t kLsw2SelectPageLow = 0x24u;
constexpr uint32_t kLsw2SelectPageHigh = 0x28u;

/* WHICH SCREEN THIS IS. Gates the confirm path: 0 takes the branch at
 * 0x02068964, anything else calls 0x02068B90.
 *     02068330  strb r3,[r4,#0x35]   = 0 at construction */
constexpr uint32_t kLsw2SelectMode = 0x35u;

/* The chosen entry within the page, or kLsw2SelectNone.
 *     0206833C  strb r0,[r4,#0x36]   = 0xFF at construction
 *     020685DC  strb r0,[r4,#0x36]   written by the caller after each tap
 *     02068908  ldrb r1,[r6,#0x36]   compared against the tapped entry */
constexpr uint32_t kLsw2SelectEntry = 0x36u;

/* The game's own "nothing chosen" value, from 0x0206833C. */
constexpr uint8_t kLsw2SelectNone = 0xFFu;

/* THE WIDGET'S OWN VTABLE, which is how it can be identified in RAM.
 *
 * The constructor writes TWO of them and the second one wins:
 *
 *     020682C8  ldr  r12,=0x020FE77C
 *     020682D0  str  r12,[r4,#0x00]      the base class
 *     020682E0  ldr  r0, =0x020FE790
 *     020682E8  str  r0, [r4,#0x00]      overwritten by the derived one
 *
 * So a live widget has 0x020FE790 at +0x00 and 0x020FE77C never appears there.
 * That is exactly the trap recorded for this project's object hunts - "a
 * derived constructor overwrites it, so search for a plausible vtable plus
 * known field constraints" - and here both values are in one function, so
 * there is no guessing about which. */
constexpr uint32_t kLsw2SelectVTable = 0x020FE790u;
constexpr uint32_t kLsw2SelectVTableBase = 0x020FE77Cu;

/* The page table the tap handler indexes, and its stride.
 *     020687C8  ldr  r2,->0x020EDDB2
 *     020687D0  add  r1,r2,r3,lsl #3      table + page * 8
 *     020687D8  ldrb r4,[r5,r1]           + entry
 *
 * Twelve pages of eight, which is where "one bounty hunter page is eight"
 * comes from - it is this stride, not a layout the fork chose. */
constexpr uint32_t kLsw2SelectPageTable = 0x020EDDB2u;
constexpr int kLsw2SelectPageStride = 8;
constexpr int kLsw2SelectPageCount = 12;

/* The padding that fills a short page's tail is cheats.h's
 * kLsw2CharacterNone - the same value the roster filter at 0x0206F620
 * substitutes for a locked hunter. */

/* THE TWO RANGES THE FORK PUTS ON THAT TABLE.
 *
 * The Free Play roster's is pages 6 and 7, "Bounty Hunters Page 1" and
 * "Page 2" by the page table's own titles (string 0x231 + page), and a read of
 * the table confirmed them independently: those sixteen slots hold exactly the
 * ten ids of the roster table at 0x020FFA00 and no others.
 *
 * The host settings menu's is the whole table, because the host's picker
 * offers any playable character, including R5-D4 and Gonk Droid - id 67 on
 * page 8 and id 68 on page 10, neither of them reachable inside the hunters
 * range.
 *
 * HERE RATHER THAN IN dispatch_overrides.cpp, which declared them when the
 * bounds were written once at construction and it was the only writer. Two
 * files apply a range now. */
constexpr uint32_t kLsw2SelectHunterPageLow = 6u;
constexpr uint32_t kLsw2SelectHunterPageHigh = 8u;
constexpr uint32_t kLsw2SelectAllPageLow = 0u;

/* THE LAST PAGE IS THE CHARACTER CREATOR'S THREE CUSTOM SLOTS, AND THE "ANY
 * CHARACTER" RANGE STOPS SHORT OF IT.
 *
 * Page 11 holds ids 73/74/75 - the three highest in the table and the game's
 * three custom-character slots (characterpick.h's kLsw2PickCustomPage).
 * The fork's "any character" range excludes it, so the high bound is 11 (the
 * page count minus the custom page), NOT 12.
 *
 * WHY, and this is deliberate and LIFTABLE: a custom character is authored on
 * one console's save and has no presence on another's, so offering one in a
 * pre-game pick or the host's One picker would put a peer in a match as something
 * its cartridge has never been given - it "probably wouldn't network well". Until
 * that is proven safe over the wire, the custom page is out of reach. To lift the
 * restriction later, set this back to kLsw2SelectPageCount (and revisit
 * characterpick.h's kLsw2PickOfferedPages, which excludes the same page for the
 * same reason). This is the ONE place the "any character" high bound is decided. */
constexpr uint32_t kLsw2SelectCustomPage = uint32_t(kLsw2SelectPageCount) - 1u;
constexpr uint32_t kLsw2SelectAllPageHigh = kLsw2SelectCustomPage;   /* = 11 */

static_assert(kLsw2SelectHunterPageHigh - kLsw2SelectHunterPageLow == 2,
              "the hunters-only Free Play range is exactly two pages");
static_assert(kLsw2SelectHunterPageHigh <= kLsw2SelectAllPageHigh,
              "the hunters range has to sit inside the table");
static_assert(kLsw2SelectAllPageHigh < uint32_t(kLsw2SelectPageCount),
              "the custom-character page is excluded from the any-character range");

/* --- THE LIVE WIDGET -------------------------------------------------------
 *
 * The host settings menu's character picker IS the Free Play touch screen
 * menu, so the fork uses the widget the cartridge already built rather than
 * drawing one. The host's choice is the widget's own confirming tap, caught in
 * freeplaypick.cpp; these accessors are what the port still reads off the
 * widget directly.
 *
 * The pointer is captured where the constructor returns - see
 * dispatch_overrides.cpp's kFreePlayCtorReturn - and lives here rather than in
 * partyportrait.cpp, which held it only because the top-screen portrait
 * following a Free Play switch was its first consumer.
 * selectwidget.cpp says why there are two notes from one call site. */

/* Remember the widget the Free Play constructor just built. Ignores an address
 * outside main RAM. */
void lsw2_selectwidget_note(uint32_t widget);

/* The page it is showing, or -1 when there is no live widget. */
int lsw2_selectwidget_page();

/* The selected entry within that page, or -1 for none - which is the game's
 * own kLsw2SelectNone rather than a sentinel invented here. */
int lsw2_selectwidget_entry();

/* PUT A PAGE RANGE ON THE LIVE WIDGET. Half-open, the way the constructor
 * writes [0, 12) and the way the paging code wraps against it.
 *
 * THE BOUNDS ARE A LIVE PROPERTY AND WERE BEING TREATED AS A CONSTRUCTION-TIME
 * ONE. The hunters-only clamp was written once, at the Free Play constructor's
 * return, and nothing wrote them again for the life of the widget. That is fine
 * for the Free Play roster and it silently broke the host's picker: the widget
 * is built when the lobby's touch screen appears, so by the time the host opens
 * the Character row it is already cut to pages 6 and 7 - and R5-D4 (page 8) and
 * Gonk Droid (page 10), the two picks the any-character range exists for,
 * cannot be reached at all. ui.cpp standing the hunters-only
 * flag down on that screen could not help, because nothing re-read the flag.
 *
 * Idempotent, and it says whether it wrote: a range that is already applied
 * costs three reads and returns false, so a caller may run it every frame.
 *
 * THE CURRENT PAGE IS PULLED INSIDE the new range, for the reason the
 * constructor path already had - the paging code wraps against the bounds but
 * never corrects a page that starts outside them, so narrowing without this
 * leaves the widget on a page its own arrows cannot leave. Widening never
 * needs it, and asking costs one read either way. */
bool lsw2_selectwidget_set_pages(uint32_t low, uint32_t high);

#endif /* LSW2_SELECTWIDGET_H */
