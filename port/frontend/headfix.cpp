/* See headfix.h. */

#include "headfix.h"
#include "vwh.h"

#include "probehooks.h"

#include "partyportrait.h"

#include "cheats.h"
#include "customization.h"
#include "gearapply.h"
#include "gearhop.h"
#include "partslot.h"
#include "state.h"
#include "ui.h"

#include <cstdio>


extern "C" void lsw2_arm9_FUN_020063d4(void);

/* THE GEAR APPLIER'S ADDRESS IS NOT WRITTEN AGAIN HERE. gearapply.h exports it
 * as kLsw2GearApplier, and this file, partapply.cpp and that header each used
 * to carry their own copy of 0x020063D4 under their own name. One ROM fact
 * written down three times has a known failure shape: a correction lands
 * on one copy and the other two keep the old value in silence. */

void lsw2_headfix_gear_applier() {
    /* THE PARTY PORTRAIT'S DEFERRED REDRAW, DRAINED HERE TOO.
     *
     * It is already drained from the save-flag override, and that was not
     * enough: measured, the refresh ran ("party portrait refreshed: slots 34
     * 100 100 100") and the redraw never did, because that override only fires
     * at particular moments and none of them followed the switch.
     *
     * THIS ONE WAS ADDED BECAUSE IT FIRES WHEN THE GAME DRESSES AN ACTOR,
     * which a character switch causes - the new character has to be built and
     * clothed - so the redraw would land within a frame or two of the change
     * that needs it, from inside guest execution where the DMA can complete.
     *
     * **THAT IS FALSE, AND IT WAS MEASURED FALSE.** A counter on
     * this override, in a run where the probe verified itself live and three
     * other hooks reported first calls, recorded NOT ONE call across a real
     * character switch - a switch the same log confirms happened, from
     * character 20 to character 7.
     *
     * The reason is four lines further down this file's own header: 0x020063D4
     * has 80 callers, 79 of them dispatch, and the one that does NOT is
     * 0x02006220 - the stock-appearance applier. Dressing a freshly switched
     * character is exactly what the stock applier does, so the one path that
     * matters here is the one path the dispatch table cannot see.
     *
     * SO THIS DRAIN POINT IS INERT ON A CHARACTER SWITCH. The portrait's redraw
     * is not getting a second chance from here, and the comment below about "two
     * drain points" removing a timing dependence overstates what this one
     * provides.
     *
     * TWO DRAIN POINTS IS NOT REDUNDANCY BY ACCIDENT. The flag is cleared by
     * whichever runs first, so a second site costs one bool test and removes
     * the dependence on any single override's timing - which is the thing that
     * has gone wrong at every step of this feature. */
    lsw2_partyportrait_redraw_if_pending();

    /* DOES THIS FIRE ON A CHARACTER SWITCH? The comment above says it does and
     * the portrait's redraw already leans on it, but customization auto-apply and
     * the head reveal are about to lean on it much harder, and six hook
     * candidates have been refuted by measurement after being picked by reading.
     * Counted, not assumed. */
    lsw2_probe_hook_rate("gear_applier");

    const uint32_t actor = g_cpu.R[0];
    const uint32_t part = g_cpu.R[1];

    /* THE GAME'S OWN APPLIER FIRST, unchanged and complete. R[15] names the
     * entry because the lifted function uses it to pick a landing pad; R[14] is
     * left alone so it returns to whoever called it. */
    g_cpu.R[15] = kLsw2GearApplier;
    lsw2_arm9_FUN_020063d4();

    lsw2_headfix_after_apply(actor, part);

    /* AND REMEMBER THE PLAYER'S CHOICE. This override fires for the game's own
     * dispatching callers - the customize panel among them - so a hat applied
     * through Mix N Match is captured here, before the game clears the staging
     * the per-frame learn-back would otherwise read. Gated to the lobby and to
     * real wearable parts inside the function. */
    lsw2_customization_observe_panel_apply(part);
}

/* RETRACTED: THE HEAD REBUILD. Both entry points below used to force the
 * head sub-part's pending-build bit in the main model's list
 * (*(actor+0xE0)+0x14) and queue the game's sub-part commit FUN_0201C8A0
 * through a deferred guest call. Two findings retire it:
 *
 *   - IT DOES NOT WORK. In play, Boba and Jango wearing a non-covering hat
 *     still show a bare neck after the commit runs, so the premise - that
 *     the head is still modelled in sub-part 0 and only needs re-committing -
 *     does not hold for what the eye sees.
 *   - IT CORRUPTED THE HEAP IN SESSIONS. On a host, applying a joining
 *     player's outfit during the lobby's teardown and rebuild queued the
 *     commit for an actor that was freed before the deferred call ran; the
 *     commit then built geometry into freed memory, the allocator's lists
 *     were corrupted, and the host crashed as the second player arrived.
 *
 * Left as no-ops so their callers (the gear applier override and the
 * per-frame tick) keep their shape for a working mechanism. The lead for one
 * is the gear applier's own kind-5 arm, FUN_02010C54, which is what builds a
 * head in the game's own flow. */
void lsw2_headfix_after_apply(uint32_t actor, uint32_t part) {
    (void)actor;
    (void)part;
}

void lsw2_headfix_tick() {
}
