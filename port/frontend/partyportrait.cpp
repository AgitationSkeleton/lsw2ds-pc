/* See partyportrait.h. The guest half. */

#include "partyportrait.h"
#include "gearhop.h"

#include "cheats.h"
#include "guestcall.h"
#include "selectwidget.h"
#include "state.h"
#include "statelog.h"
#include "ui.h"
#include "vwh.h"

#include <cstdio>
#include <cstdlib>

extern "C" void lsw2_arm9_FUN_020691b4(void);
extern "C" void lsw2_arm9_FUN_02068d74(void);

namespace {

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

/* cheats.h's kLsw2CharacterNone, which free_play_pages.json records as
 * `empty_slot`. */

uint32_t g_widget = 0;
/* THE STATE THE WIDGET WAS BUILT IN, so the pointer can be retired when the
 * game leaves that screen. See the guard in the tick. */
int g_widget_state = -1;
/* "not stamped yet" - distinct from any real state id, including the -1 the
 * state manager itself reports when there is no current state object. */
constexpr int kStateNotStamped = -2;
uint8_t g_before[kLsw2PartySlots] = {0};
bool g_have_before = false;
int g_reports = 0;
/* Set by the tick when the party changed and the refresh succeeded; drained
 * from inside guest execution. See the note at the deferral. */
bool g_redraw_pending = false;
/* A REFRESH THAT HAS BEEN ASKED FOR AND NOT YET DELIVERED.
 *
 * The baseline g_before is updated the moment a change is NOTICED, which is
 * before the refresh is attempted - so "changed" is true for exactly one tick.
 * That is fine when the call goes through and silently loses the refresh when
 * it cannot, which is the common case: the guest is usually
 * halted when this tick runs. The request therefore outlives the observation
 * that raised it, and is cleared only when the game has actually been told. */
bool g_refresh_wanted = false;
constexpr int kReports = 6;

uint8_t read8(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

uint32_t read32(uint32_t addr) { return bus_device_read32(0, addr); }

void write8(uint32_t addr, uint8_t v) {
    const uint32_t base = addr & ~3u;
    const uint32_t shift = (addr & 3u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFu << shift)) | (uint32_t(v) << shift));
}

/* The four slots' character ids, exactly as party_refresh reads them. */
void read_party(uint8_t* out) {
    for (int i = 0; i < int(kLsw2PartySlots); ++i) {
        out[i] = kLsw2CharacterNone;
        const uint32_t actor =
            read32(kLsw2LevelRootAddr + kLsw2PartyActors + uint32_t(i) * 4u);
        if (actor < kRamLow || actor >= kRamHigh) continue;
        out[i] = read8(actor + kLsw2ActorCharacter);
    }
}

}  // namespace

void lsw2_partyportrait_note_widget(uint32_t widget) {
    if (widget < kRamLow || widget >= kRamHigh) return;
    if (widget == g_widget) return;
    g_widget = widget;
    /* THE STATE IS NOT RECORDED HERE, and recording it here is what broke the
     * portrait following a Free Play switch.
     *
     * Reproduced on a single process. A Free Play switch
     * from Greedo to character 34 changed the actor and left the portrait
     * showing Greedo, with no "party refresh" line at all - the call was never
     * reached. Instrumenting the four early exits gave the order exactly:
     *
     *     [state] entered 48                            the lobby settles
     *     portrait refresh skipped: ids did not change   widget alive
     *     portrait refresh skipped: no widget captured   RETIRED
     *     last-played hunter is now id 34                the switch, too late
     *
     * The widget is constructed BEFORE the state it belongs to has settled, so
     * the state stamped on it here is the previous one. The tick's lifetime
     * guard then compares against that stale stamp, decides the game has left
     * the screen, and retires the pointer within a few frames - every time,
     * long before any character switch can need it.
     *
     * So the stamp is taken on the first TICK instead, by which time the state
     * is whatever the screen actually runs in. The guard keeps doing its job -
     * it still retires the pointer when the game genuinely leaves - it just
     * stops being wrong about where it started. */
    g_widget_state = kStateNotStamped;
    /* A NEW WIDGET IS A NEW BASELINE. The screen was just built, so whatever
     * the party is now is the state it was drawn with; treating it as a change
     * would refresh once on every open for no reason. */
    g_have_before = false;
}

/* WHY THE REFRESH DID NOT RUN, said once per distinct reason.
 *
 * The defect reproduced cleanly on a single process: a Free Play switch from
 * Greedo to character 34 changed the actor and left the top-screen
 * portrait showing Greedo, and the log carried no "party refresh" line at all -
 * so the call is not failing, it is not being reached.
 *
 * This tick has four ways to return before it gets there and from outside they
 * are indistinguishable. Naming the one that fires is the difference between
 * knowing and guessing, and it is the rule this file's own investigation kept
 * proving: make the failing layer say WHICH condition tripped. */
void say_once(const char* why) {
    static const char* said = nullptr;
    if (said == why) return;
    said = why;
    std::fprintf(stderr, "[fork] portrait refresh skipped: %s\n", why);
}

void lsw2_partyportrait_tick() {
    if (!lsw2_ui_enhanced_portrait()) {
        say_once("the enhancement is off");
        return;
    }
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): the refresh is a guest call from the
     * between-frames tick, and in a shared lobby the party changes because the
     * other players' actors spawn, not because anyone used Free Play. After a
     * four-player match every console took that for a switch and refreshed the
     * rebuilt lobby's widget; the refresh fed the game's decompressor a source
     * that is not data (0x01FF8324 reading 0xEA137148 on all four) and the
     * geometry engine wedged on all four. The widget and the baseline are
     * dropped rather than kept, so nothing stale outlives the shared lobby -
     * the next Free Play screen captures afresh. */
    if (lsw2_vwh_lockstep()) {
        g_widget = 0;
        g_have_before = false;
        g_refresh_wanted = false;
        return;
    }
    if (g_widget < kRamLow || g_widget >= kRamHigh) {
        /* THE VALUE, NOT JUST THE VERDICT. The previous round of this
         * investigation could tell that the pointer had gone but not what it
         * had gone TO, and there are only three sites that zero it - all of
         * which log first, and none of which logged. Printing the value
         * separates "zeroed by one of those" from "never what we thought". */
        static uint32_t last = 1u;
        if (g_widget != last) {
            last = g_widget;
            std::fprintf(stderr,
                         "[fork] portrait refresh skipped: widget is %08X\n",
                         unsigned(g_widget));
        }
        return;
    }

    /* THE WIDGET DIES WITH ITS SCREEN, and nothing tells this file when.
     *
     * `note_widget` records the pointer when the Free Play screen is built and
     * there is no matching hook for the destructor, so without this the pointer
     * outlives the object. The party's character ids DO change while that
     * screen is closed - a level transition respawns the party, and
     * lasthunter_observe.cpp asks the game for a character at match start - so
     * the tick would find a difference, believe the screen was still up, and
     * call party_refresh and redraw_page through a freed pointer. An in-range
     * address test cannot see that: freed guest memory is still guest memory.
     *
     * THE SCREEN IS A STATE, so the state the widget was built in is the
     * lifetime bound, whatever that state's number turns out to be - this does
     * not need to know it. Once the game is somewhere else the pointer is
     * retired, and the next visit to Free Play captures a fresh one.
     *
     * Conservative in the safe direction: the worst case is refreshing one
     * frame less often, and a state change while the Free Play screen is still
     * up would mean the screen was not that state to begin with. */
    /* STAMP ON THE FIRST TICK, once the screen's state has settled. See
     * note_widget for why doing it at construction retired every widget. */
    if (g_widget_state == kStateNotStamped) {
        g_widget_state = lsw2_state_current();
        std::fprintf(stderr,
                     "[fork] Free Play widget %08X belongs to state %d\n",
                     unsigned(g_widget), g_widget_state);
    }

    if (lsw2_state_current() != g_widget_state) {
        say_once("the game left the state the widget was built in");
        g_widget = 0;
        g_have_before = false;
        return;
    }

    uint8_t now[kLsw2PartySlots];
    read_party(now);

    /* A HEARTBEAT, NOT A CHANGE LOG.
     *
     * Every diagnostic added to this file so far only fired on a change, and
     * every one of them was silent at exactly the moment that mattered - "the
     * ids did not change" said once and then nothing, which is
     * indistinguishable from the tick having stopped running. A log that only
     * speaks when something moves cannot tell you that nothing moved, or that
     * it was never asked.
     *
     * So this prints unconditionally, once a second, for a short while. It is
     * temporary scaffolding for the portrait refresh and should go once the
     * trigger is settled;
     * it is here rather than in a scratch script because the values it needs -
     * the widget, the stamped state, the live state, the party ids and the
     * actor - only exist together inside this function. */
    {
        static int beat = 0;
        static int beats = 0;
        if (++beat >= 60 && beats < 40) {
            beat = 0;
            ++beats;
            std::fprintf(stderr,
                         "[fork] portrait beat: widget %08X stamp %d live %d "
                         "party %u %u %u %u actor %08X char %d\n",
                         unsigned(g_widget), g_widget_state,
                         lsw2_state_current(),
                         unsigned(now[0]), unsigned(now[1]),
                         unsigned(now[2]), unsigned(now[3]),
                         unsigned(lsw2_cheats_party_actor(0)),
                         lsw2_cheats_party_character(0));
        }
    }

    const bool changed = lsw2_partyportrait_changed(
        now, g_before, int(kLsw2PartySlots), g_have_before);

    for (int i = 0; i < int(kLsw2PartySlots); ++i) g_before[i] = now[i];
    g_have_before = true;
    if (changed) g_refresh_wanted = true;
    if (!changed && !g_refresh_wanted) {
        /* THE IDS THEMSELVES, whenever they move.
         *
         * "did not change" is said once and then stays silent, so a switch that
         * this function fails to notice looks exactly like a switch it has not
         * been asked about yet. Printing the four ids on every change separates
         * them: if the player swaps character and these do not move, then
         * read_party is reading an array that does not track the Free Play
         * switch, and the comparison was never the problem. */
        static uint8_t seen[kLsw2PartySlots] = {0xFE, 0xFE, 0xFE, 0xFE};
        bool moved = false;
        for (int i = 0; i < int(kLsw2PartySlots); ++i)
            if (seen[i] != now[i]) moved = true;
        if (moved) {
            for (int i = 0; i < int(kLsw2PartySlots); ++i) seen[i] = now[i];
            /* BOTH SOURCES, IN THE SAME TICK. read_party and
             * lsw2_cheats_party_character read the same address by the same
             * arithmetic - kLsw2LevelRootAddr + kLsw2PartyActors + slot*4,
             * then actor + kLsw2ActorCharacter - and they are reporting
             * different characters: this said 20 while the customize probe
             * said 34 for the same switch. Two readings of one address cannot
             * both be right, so they are printed together rather than compared
             * across log lines written at different moments. */
            std::fprintf(stderr,
                         "[fork] party slots now %u %u %u %u (no refresh; "
                         "cheats says slot0 = %d, actor %08X)\n",
                         unsigned(now[0]), unsigned(now[1]),
                         unsigned(now[2]), unsigned(now[3]),
                         lsw2_cheats_party_character(0),
                         unsigned(lsw2_cheats_party_actor(0)));
        }
        return;
    }

    /* THE GAME'S OWN PAIR, IN THE GAME'S OWN ORDER. `0x02068598` runs
     * party_refresh then redraw_page and stops if the refresh returns zero;
     * doing anything else here would be inventing behaviour rather than
     * restoring it. */
    /* NOT READY IS NOT FAILED - measured, and here it was worse than a lost
     * call. The failure path below RETIRES THE WIDGET POINTER, so a single
     * frame where the guest happened to be halted - which is the normal state
     * between frames, when this tick runs - permanently disabled the portrait
     * refresh for the rest of the run. guestcall.h has the disassembly: every lifted
     * function refuses before its first instruction while the cpu is halted.
     *
     * AND RETURNING HERE ONLY WORKS BECAUSE THE REQUEST IS STICKY. g_before is
     * updated the moment a change is NOTICED, several lines above, so "changed"
     * is true for exactly one tick - deferring on that alone would drop the
     * refresh silently, which is the bug this guard is supposed to prevent
     * rather than a second copy of it. g_refresh_wanted carries it until the
     * game has actually been told. */
    if (!lsw2_guest_ready()) return;

    /* THE REQUEST IS SPENT HERE, whatever the call does next. It was attempted
     * with the guest awake, so this is a real answer rather than a deferral,
     * and leaving the flag raised would retry a genuinely failing call on every
     * frame for the rest of the run - the per-frame retry storm gearapply.h
     * records as the original freeze. */
    g_refresh_wanted = false;

    /* THROUGH THE HOP, NOT THE HOST SEAM.
     *
     * `lsw2_guest_call` can never run this: the machine's event queue was
     * measured EMPTY between frames, so a guest function that waits for an
     * interrupt cannot complete, and this one never returned once. The hop in
     * gearhop.cpp runs it on the guest's own thread where the queue is live -
     * the same mechanism that made customization re-apply after a Free Play
     * switch and the Jango/Boba head re-attach under headwear.
     *
     * THE ANSWER ARRIVES LATER, so honouring R[0] moves to the completion:
     * lsw2_partyportrait_refresh_returned below. */
    lsw2_gearhop_request_refresh(g_widget);
    return;
}

void lsw2_partyportrait_refresh_returned(uint32_t result) {
    /* R[0] IS THE REFRESH'S ANSWER, and the working path honours it: a zero
     * means it decided there was nothing to show, and acting over that would
     * be a change the cartridge never makes. */
    std::fprintf(stderr,
                 "[fork] party refresh RETURNED on the guest's thread, r0 %u"
                 " - this call had never once completed before\n",
                 unsigned(result));
    if (result == 0) return;

    /* THE REDRAW IS NOT CALLED FROM HERE, AND IS NO LONGER DEFERRED TO AN
     * OVERRIDE EITHER.
     *
     * It copies the portrait tiles by hardware DMA, and a call made between
     * frames has no scheduler to service the transfer - guestcall.h carries
     * the disassembly that named it. Calling it from this tick stalled the CPU
     * on a transfer nothing would advance, unwound mid-routine, and cost the
     * widget pointer on the way out.
     *
     * The answer to that was a deferred flag drained from a dispatch override,
     * and MEASUREMENT KILLED IT: every override was instrumented and a switch
     * driven, and the count of any override firing after the refresh saw the
     * new character was ZERO. A flag nothing drains is not a fix. A mode-byte
     * nudge replaced that and is dead too - partyportrait.h has both autopsies.
     *
     * THE CALL WAS PUT BACK ONCE MORE AND IT DAMAGED THE GAME - retaken after
     * lsw2_guest_call was fixed, because the original stall had
     * been measured through a seam that was refusing every host call and so
     * proved nothing about this routine. It proved it this time:
     *
     *     [guestcall] 02068D74 did not return: unwinding=1 r15=01FFD584
     *     [fork] redraw_page DID NOT RETURN - the widget page was not redrawn
     *     [guestcall] 020691B4 did not return: unwinding=1 ...
     *     [guestcall] 02004404 did not return: unwinding=1 ...
     *
     * r15 lands in the ITCM DMA routine, exactly where guestcall.h said it
     * would, so the DMA finding was right on its own merits. And the cost is
     * not just a call that fails: it stops HALF WAY THROUGH DRAWING, so the
     * select page is left with empty portrait circles, and it leaves the guest
     * unwinding, so every host call after it fails too - the refresh and the
     * weapon setter both went down with it in that same log.
     *
     * THE VISIBLE SYMPTOM SAID THE SAME THING: with this call in place, the
     * portraits on the BOTTOM screen refreshed on a change instead of the
     * top-screen one. That is the whole finding in one sentence. redraw_page
     * redraws the SELECT PAGE - the grid on the TOUCH screen - which is not
     * the top-screen portrait this feature is about, and running it does
     * damage without ever touching that portrait.
     *
     * THE DMA HALF OF THAT IS NOW FIXED, AND IT CHANGES NOTHING HERE.
     * guestcall.cpp learned to do what scheduler.cpp does when a
     * host call stalls a CPU on a transfer - advance it with nds_dma_run,
     * renew the cap, re-dispatch at `pc | thumb`. A call that used to die at
     * r15=0x01FFD584 now walks past the kick. So the first of the two reasons
     * above has gone away.
     *
     * THE SECOND HAS NOT, AND IT IS THE ONE THAT MATTERS. redraw_page redraws
     * the SELECT PAGE - the grid on the TOUCH screen - and this feature is
     * about the portrait on the TOP screen. Calling it now would work and
     * would reproduce, deliberately, the bottom-screen refresh described
     * above. A fixed seam makes the wrong function reachable; it does not make
     * it right.
     *
     * So it is still not called from here, and the reason is a measurement
     * rather than a preference. What the top-screen refresh still needs is in
     * partyportrait.h. */
    g_redraw_pending = true;
    lsw2_partyportrait_redraw_if_pending();

    if (g_reports < kReports) {
        ++g_reports;
        /* READ THE SLOTS HERE rather than carry the tick's snapshot. The
         * refresh's answer now arrives on a later frame, so the values the
         * request was made with are history; what is worth reporting is what
         * the party holds at the moment the game finished refreshing it. */
        std::fprintf(stderr,
                     "[fork] party portrait refreshed: slots %d %d %d %d "
                     "(widget %08X)\n",
                     lsw2_cheats_party_character(0),
                     lsw2_cheats_party_character(1),
                     lsw2_cheats_party_character(2),
                     lsw2_cheats_party_character(3), unsigned(g_widget));
    }
}

/* LET THE GAME DO IT - the cartridge's own refresh, one byte away.
 *
 * MEASURED. 0x02068598, the select widget's update handler, ends in
 * this block, and three separate paths fall into it every time it runs:
 *
 *     02068748  LDRB  r0, [r4, #0x35]    the MODE field
 *     0206874C  CMP   r0, #0
 *     02068750  LDMEQ sp!, {...pc}       mode == 0 -> return, no refresh
 *     0206875C  BL    0x020691B4         party_refresh
 *     0206876C  BL    0x02068D74         redraw_page
 *
 * So the game refreshes the portrait on every screen EXCEPT the one where mode
 * is zero - and selectwidget.h records mode 0 as the Free Play select. That is
 * the stale-portrait bug, in the cartridge, in one comparison: tagging a
 * partner refreshes because that screen is not mode 0, and a Free Play switch
 * does not because this one is.
 *
 * WHICH MAKES THIS THE RIGHT KIND OF FIX. The intended approach is to work out
 * which refresh a tag sets off and set that same refresh off on the switch,
 * instead of painting the portrait in the port. This is that: hold mode non-zero for a frame and the
 * game runs its own refresh and its own redraw, in its own order, from inside
 * its own update - which is also the only context where the redraw's DMA can
 * complete. No host call, no reimplementation.
 *
 * RESTORED IMMEDIATELY, because mode is load-bearing elsewhere: it also picks
 * the confirm path at 0x020687D4, so leaving it flipped would send a tap to the
 * wrong screen's handler. One frame is enough - the handler runs every frame -
 * and the window is closed on the very next tick. */
void lsw2_partyportrait_redraw_if_pending() {
    if (!g_redraw_pending) return;
    g_redraw_pending = false;

    static bool said = false;
    if (said) return;
    said = true;
    std::fprintf(stderr,
                 "[fork] portrait: the party changed and the refresh ran, and "
                 "nothing here can redraw the top screen yet - see "
                 "partyportrait.h\n");
}
