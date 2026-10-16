/* The live Free Play select widget, and who is allowed to know about it.
 *
 * WHY THIS FILE EXISTS. The widget's pointer was captured so the top-screen
 * portrait could follow a Free Play character change, and kept file-static in
 * partyportrait.cpp, because the portrait refresh was its only consumer. The
 * host settings menu gave it a second one: the host's character picker IS the
 * Free Play touch screen menu, so the fork has to read the host's selection out
 * of this same object.
 *
 * A second consumer reaching into partyportrait.cpp for a pointer that is not
 * about portraits is the kind of coupling that reads fine once and is wrong
 * the third time. selectwidget.h already owns this widget's field offsets, so
 * the widget itself belongs beside them.
 *
 * TWO NOTES FROM ONE CALL SITE, deliberately. dispatch_overrides.cpp tells
 * BOTH this file and partyportrait.cpp when the constructor returns, because
 * they want different things from the same pointer: this file wants the raw
 * address for as long as the widget lives, and the portrait refresh wants a
 * pointer it can RETIRE when the state moves on, which is portrait lifetime
 * logic and not a fact about the widget. Merging them would put the portrait's
 * retirement rule in the way of a reader that does not need it.
 */

#include "selectwidget.h"

#include "characterpick.h"

#include "state.h"

#include <cstdio>

namespace {

/* The last widget the Free Play constructor built, or 0.
 *
 * NOT CLEARED ON A STATE CHANGE, unlike partyportrait.cpp's copy. A reader here is expected
 * to check that the address is still sane and that the situation still makes
 * sense - lsw2_selectwidget_entry() does both - because "the widget the game
 * last built" is a fact with a longer life than any one screen. */
uint32_t g_widget = 0;

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

uint8_t read_byte(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

}  // namespace

namespace {

/* DOES THIS ADDRESS STILL HOLD A SELECT WIDGET?
 *
 * This is the whole reason the host picker once never worked. With
 * "Bounty Hunters Page 1" on screen and a portrait visibly
 * ring-highlighted, the port reported `page 6, entry -1` - nothing selected,
 * while something plainly was. The page reading 6 was not evidence of anything
 * either: the fork's own range writer had put it there, so that number was our
 * own write read back out of whatever object we happened to be holding.
 *
 * The note below fires from a save-flag question inside the Free Play
 * constructor, which happens ONCE. The widget the player actually taps is
 * rebuilt when the Character tab is chosen, through a path that never asks
 * that question - so the captured address goes stale and the fork keeps
 * reading a dead object. Scanning RAM live found the real one at 0x021DFA04
 * with entry 0x01, the exact portrait that had been tapped, while the fork
 * held 0x021FDFA8.
 *
 * READING THE RIGHT FIELDS OF THE WRONG OBJECT LOOKS EXACTLY LIKE READING THE
 * RIGHT OBJECT AND FINDING NOTHING. So the pointer is no longer trusted: it is
 * a hint, checked before every use. */
bool looks_like_the_widget(uint32_t a) {
    if (a < kRamLow || a >= kRamHigh) return false;
    if (bus_device_read32(0, a) != kLsw2SelectVTable) return false;

    /* THE VTABLE ALONE IS NOT ENOUGH. The scan that found this turned up four
     * matches and three were literal-pool words inside the code region, whose
     * "fields" were addresses. The field constraints are what separate an
     * object from a coincidence. */
    const uint32_t page = bus_device_read32(0, a + kLsw2SelectPage);
    const uint32_t lo = bus_device_read32(0, a + kLsw2SelectPageLow);
    const uint32_t hi = bus_device_read32(0, a + kLsw2SelectPageHigh);
    if (hi > uint32_t(kLsw2SelectPageCount)) return false;
    if (lo > hi) return false;
    if (page >= uint32_t(kLsw2SelectPageCount)) return false;

    const uint8_t entry = read_byte(a + kLsw2SelectEntry);
    if (entry != kLsw2SelectNone && entry >= uint8_t(kLsw2SelectPageStride))
        return false;
    return true;
}

bool has_a_selection(uint32_t a) {
    return read_byte(a + kLsw2SelectEntry) != kLsw2SelectNone;
}

/* THE SHAPE TEST IS NOT ENOUGH EITHER, and the first version of this file
 * learned that the hard way. It validated the hint, the hint PASSED, and
 * nothing was re-found - because the fork's own range writer had written page 6
 * and bounds [6,8) into the stale object, so it looked exactly like a live
 * widget. Our own writes making the wrong object look right, one level down
 * from the bug this was written to fix.
 *
 * Both addresses really are select widgets - same vtable, same plausible
 * fields. Shape cannot separate them. What separates them is that the player is
 * tapping ONE of them, so that one has a selection and the other does not. */

/* THE SCAN IS NOT USED, AND BOTH REASONS ARE WORTH KEEPING.
 *
 * IT IS TOO EXPENSIVE. Four megabytes of word reads through bus_device_read32
 * is not a per-frame cost and is barely a twice-a-second one: at every 30
 * frames it made the game unresponsive enough to hang a driving script.
 *
 * AND IT DID NOT WORK. With the character select open and a portrait tapped,
 * the port reported:
 *
 *     [fork] widget scan: hint 021FDFA8 plausible, scan found 00000000
 *
 * while a scan of the same RAM over the debug socket, with the same vtable and
 * the same field constraints, found exactly one widget - 021DFA04, bounds
 * [6,8), entry 0x07. Same memory, same predicate, different answers, so the
 * difference is in the read path (bus_device_read32 here against the debug
 * surface's bus_debug_read8 there) and that is not yet understood.
 *
 * Reporting a scan that cannot find what is demonstrably there would be worse
 * than not scanning, so it does neither. What survives is the VALIDATION,
 * which is cheap and correct: the hint is checked before every use instead of
 * being trusted. The identification work is kept because it is right - the
 * vtable pair came out of the constructor and the live address was confirmed
 * live - and finding the widget without a full scan is the open problem.
 *
 * A WINDOWED SEARCH WAS TRIED NEXT AND FAILED THE SAME WAY, so the size of the
 * scan is NOT the explanation. The addresses are stable across runs - hint
 * 021FDFA8, live 021DFA04, 0x1E5A4 apart - so a +/-0x40000 window round the
 * hint covers the live widget with room to spare. It was searched, at one pass
 * per thirty frames, and found nothing, while a socket scan of the same RAM in
 * the same run found 021DFA04 with entry 0x07.
 *
 * AND THEN THE READ-PATH THEORY FELL OVER, which is why it is written here
 * rather than acted on. THREE lsw2_bountyhunter.exe processes were found alive
 * at once: `pkill -f` does not kill a Windows process from an MSYS shell, only
 * `taskkill //F //IM` does, so every "relaunch" left the previous one running.
 * They all try to bind the same debug port, so a socket scan reaches whichever
 * holds it - very possibly a STALE process - while the log being read belongs
 * to the newest.
 *
 * Two different processes have two different heaps. That alone produces exactly
 * the symptom the read-path theory was invented to explain: an address that is
 * a live widget over here and nothing over there. So "bus_device_read32 cannot
 * see what the debug surface can" is NOT established - it may simply be that
 * the two were never looking at the same game. The comparison has not been
 * repeated with exactly one instance running, and until it is, the scan's
 * failure is unexplained rather than attributed to the read path.
 *
 * WHAT HAS NOT BEEN TRIED, and is the better idea: the three constructor
 * callers all store the widget at PARENT + 0xC4 (0x02087700 in 0x02087390,
 * 0x0208792C in 0x02087844, 0x02087B94 in 0x02087A54). A pointer read out of
 * the field the game itself keeps is correct by construction, costs one read,
 * and needs no search at all. Finding the parent once is the remaining work. */

/* The live widget, or 0. One word read in the common case. */
uint32_t live_widget() {
    /* THE HINT, CHECKED - not trusted. See looks_like_the_widget for what a
     * trusted hint cost: the port read `entry -1` off a dead object while a
     * portrait was visibly selected on screen, and the page it also read was
     * the fork's own range-writer value echoing back.
     *
     * When the hint fails this returns 0 and the callers answer "no widget",
     * which is honest. It is not yet the same as "the widget the player is
     * using" - see the note above about the scan. */
    return looks_like_the_widget(g_widget) ? g_widget : 0;
}

}  // namespace

void lsw2_selectwidget_note(uint32_t widget) {
    if (widget < kRamLow || widget >= kRamHigh) return;

    /* SAY WHEN THE POINTER MOVES, and say who moved it.
     *
     * Two runs now have shown the fork tracking the selection correctly, and
     * BOTH times the negative test said the new code was not responsible - the
     * constructor's capture happened to be the right object in that build. A
     * result that survives removing its own cause is heap luck, and the only
     * way to tell the two apart is to watch the POINTER rather than the
     * selection it produces.
     *
     * If the tap capture is doing anything, this prints a change from the
     * constructor's address to another one when the player taps. If it prints
     * nothing, the constructor was already right and the override is
     * insurance rather than a fix - which is worth knowing either way. */
    if (widget != g_widget) {
        std::fprintf(stderr, "[fork] select widget pointer %08X -> %08X\n",
                     unsigned(g_widget), unsigned(widget));
    }
    g_widget = widget;
}

int lsw2_selectwidget_page() {
    const uint32_t g_widget = live_widget();
    if (!g_widget) return -1;
    /* A WORD, not a byte: selectwidget.h's page and its two bounds are read
     * and written as 32-bit by the game (0x02068314 `str r3,[r4,#0x20]`), and
     * the fork's own clamp writes them the same way. */
    return int(bus_device_read32(0, g_widget + kLsw2SelectPage));
}

int lsw2_selectwidget_entry() {
    const uint32_t g_widget = live_widget();
    if (!g_widget) return -1;

    /* +0x36 IS A BYTE AND 0xFF MEANS NOTHING IS SELECTED.
     *
     * The confirm path reads it: `ldrb r3,[r6,#0x36]` then
     * `cmp r3,#0xFF` to decide whether there is a previous selection to
     * un-highlight. So the sentinel is the game's own, not a choice made here,
     * and it is the same 0xFF the select table uses for a padding slot - which
     * is why this returns -1 rather than passing 0xFF along to a caller that
     * would have to know that. */
    const uint8_t entry = read_byte(g_widget + kLsw2SelectEntry);
    if (entry == kLsw2SelectNone) return -1;
    return int(entry);
}

bool lsw2_selectwidget_set_pages(uint32_t low, uint32_t high) {
    const uint32_t g_widget = live_widget();
    if (!g_widget) return false;

    /* A RANGE THAT CANNOT HOLD A PAGE IS A CALLER BUG, and writing it would
     * leave the widget on a page the arrows can never leave - the wrap at
     * 0x020685F0 subtracts one from the high bound, so an empty range hands the
     * paging code high-1 < low and it stays there. Refuse rather than brick the
     * screen. */
    if (low >= high || high > uint32_t(kLsw2SelectPageCount)) return false;

    /* WORDS, not bytes: 0x02068318 and 0x02068320 write both bounds with `str`,
     * and 0x02068314 the page. Device 9 to match dispatch_overrides.cpp's
     * writes - and device 0 above reaches the same memory, since the runner's
     * bus_device_* select ARM7 only for a cpu of exactly 7 and ARM9 for
     * everything else. Noted because the two numbers look like a disagreement
     * and are not one. */
    const uint32_t cur_low = bus_device_read32(9, g_widget + kLsw2SelectPageLow);
    const uint32_t cur_high =
        bus_device_read32(9, g_widget + kLsw2SelectPageHigh);
    const uint32_t page = bus_device_read32(9, g_widget + kLsw2SelectPage);
    const bool page_ok = page >= low && page < high;

    if (cur_low == low && cur_high == high && page_ok) return false;

    bus_device_write32(9, g_widget + kLsw2SelectPageLow, low);
    bus_device_write32(9, g_widget + kLsw2SelectPageHigh, high);
    if (!page_ok) bus_device_write32(9, g_widget + kLsw2SelectPage, low);
    return true;
}
