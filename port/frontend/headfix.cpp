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

namespace {

/* THE GEAR APPLIER'S ADDRESS IS NOT WRITTEN AGAIN HERE. gearapply.h exports it
 * as kLsw2GearApplier, and this file, partapply.cpp and that header each used
 * to carry their own copy of 0x020063D4 under their own name. One ROM fact
 * written down three times has a known failure shape: a correction lands
 * on one copy and the other two keep the old value in silence. */
constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;


int g_reports = 0;
constexpr int kReports = 4;

uint8_t read8(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

uint16_t read16(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint16_t((addr & 2u) ? (w >> 16) : (w & 0xFFFFu));
}

/* Halfword write through the containing word - the bus helpers this file
 * already uses are 32-bit, and the guest is stepped from this same thread
 * between frames so the other half cannot change underneath us. */
void write16(uint32_t addr, uint16_t v) {
    const uint32_t base = addr & ~3u;
    const uint32_t shift = (addr & 2u) * 8u;   /* 0 or 16 */
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base,
                       (w & ~(0xFFFFu << shift)) | (uint32_t(v) << shift));
}

}  // namespace

bool lsw2_headgear_covers(uint32_t part) {
    if (part < kLsw2HeadgearFirst || part > kLsw2HeadgearLast) return false;
    const uint32_t desc =
        kLsw2HeadgearTable + (part - kLsw2HeadgearFirst) * kLsw2HeadgearStride;
    return read8(desc + kLsw2HeadgearCovers) != 0;
}

bool lsw2_character_head_hidden_by_stock(int character) {
    if (character < 0 || character > 83) return false;
    const uint32_t stock = read8(kLsw2StockParts +
                                 uint32_t(character) * kLsw2StockStride +
                                 kLsw2StockHeadgear);
    return lsw2_headgear_covers(stock);
}

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

void lsw2_headfix_after_apply(uint32_t actor, uint32_t part) {
    if (!lsw2_ui_enhanced_heads()) return;
    if (actor < kRamLow || actor >= kRamHigh) return;

    /* Only a headgear part can have hidden a head, and only one that does NOT
     * cover leaves the head needing an attach the applier did not ask for. */
    if (part < kLsw2HeadgearFirst || part > kLsw2HeadgearLast) return;
    if (lsw2_headgear_covers(part)) return;

    /* AND ONLY FOR A CHARACTER WHOSE HEAD IS NEVER NORMALLY SEEN. For everyone
     * else the head is already attached and re-asserting the flag would ask for
     * an attach that the game did not think was needed - a change beyond what
     * the defect calls for, in a build whose rule is to match vanilla. */
    const int character = int(read8(actor + kLsw2ActorCharacter));
    if (!lsw2_character_head_hidden_by_stock(character)) return;

    /* THE HEAD IS A SUB-PART OF THE MAIN MODEL, and it is already there - it
     * just is not being drawn. This is the corrected mechanism; the old one
     * (set +0x6F3, call FUN_0200636C) reached a DIFFERENT subsystem and never
     * touched what the eye sees. See the header for the full derivation.
     *
     * MEASURED on Boba (character 7). The applier builds the head
     * into the MAIN MODEL's sub-part list at `*(actor+0xE0)+0x14`: u16[0] is a
     * visibility bitmask, u16[1..] are sub-part model ids. Sub-part 0 holds the
     * head model 0x0E31 in BOTH the covered (helmet) and the hatted case - so
     * the head geometry is present the whole time. The only difference is bit 0
     * of the bitmask: the covering-helmet build clears it (the helmet is drawn
     * instead), and swapping to a non-covering hat removes the helmet but never
     * re-shows the head, leaving a bare neck. Setting bit 0 is exactly what the
     * game's own head builder (FUN_0201c858) does when it shows a head; asserting
     * it here reveals the head that is already modelled.
     *
     * Both call sites reach this: the applier override for a Mix N Match hat,
     * and the gear-hop completion (gearhop.cpp) when the fork re-applies the
     * saved hat on a character switch - so the head is revealed however the hat
     * arrives. Idempotent: it writes only when bit 0 is clear. */
    /* The main model lives behind actor+0xE0; its sub-part id/visibility list is
     * +0x14 into it (measured - see headfix.h). Inline literals rather than
     * named constants because the values 0xE0 and 0x14 collide with unrelated
     * small offsets elsewhere, and the dup-constants baseline is deliberately
     * kept minimal. */
    const uint32_t holder = bus_device_read32(0, actor + 0xE0u);
    if (holder < kRamLow || holder >= kRamHigh) return;
    const uint32_t list = holder + 0x14u;

    const uint16_t head_model = read16(list + 2u);   /* sub-part 0's model id */
    if (head_model == 0u || head_model == 0xFFFFu) return;  /* no head model here */

    /* REBUILD THE HEAD GEOMETRY, do not merely flip a bit. Reverse-engineered
     * and runtime-verified: *(list) bit 0 is a pending-BUILD request, not a live
     * visibility flag. The old fix set it and the head stayed a bare neck,
     * because the helmet build tore down the head's renderable geometry and a bit
     * cannot rebuild it. So force the pending bit and run the game's OWN sub-part
     * commit FUN_0201C8A0(list, node) - it loops the pending sub-parts, builds
     * their geometry and attaches it to the model node at actor+0xD8. Through the
     * gear hop because the commit does DMA that needs the live scheduler, which a
     * between-frames host call does not have (gearhop.h); fire-and-
     * forget, nothing is done on completion. The head model already in sub-part 0
     * is the character's OWN - it is not overwritten, so every covered-head hunter
     * gets its own face. 0x0E31 is Boba/Jango's; others differ. */
    const uint16_t vis = read16(list);
    write16(list, uint16_t(vis | 1u));               /* request sub-part 0's rebuild */
    lsw2_gearhop_request_toggle(0x0201C8A0u, list, actor + 0xD8u, 0u);

    if (g_reports < kReports) {
        ++g_reports;
        std::fprintf(stderr,
                     "[fork] head rebuild queued: character %d wore non-covering "
                     "hat %u - committing head sub-part (model 0x%04X) to the "
                     "model node (mask %04X -> %04X)\n",
                     character, unsigned(part), unsigned(head_model),
                     unsigned(vis), unsigned(uint16_t(vis | 1u)));
    }
}

void lsw2_headfix_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): rebuilding a head on one console is a divergence. */
    if (lsw2_vwh_lockstep()) return;
    /* THE SAME REVEAL, ONCE A FRAME, so it catches the case lsw2_headfix_after_
     * apply misses: the FIRST time a non-covering hat is worn, the sub-part list
     * may not hold the head model yet when the apply fires, so after_apply skips
     * it (its head_model guard) - in play the head appeared on SUBSEQUENT
     * changes but not the initial one. Re-checking every frame reveals it as soon
     * as the model is registered, regardless of the apply's timing. Idempotent:
     * writes only when bit 0 is clear, and only for one of the nine wearing
     * headwear that does NOT cover, so a helmet still hides the head. */
    if (!ui_fork_build() || !lsw2_ui_enhanced_heads()) return;

    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (actor < kRamLow || actor >= kRamHigh) return;

    const int character = int(read8(actor + kLsw2ActorCharacter));
    if (!lsw2_character_head_hidden_by_stock(character)) return;   /* one of the 9 */

    /* Slot 3 is the headwear. A covering hat/helmet must keep the head hidden,
     * or the head would clip through it. */
    const uint16_t hat = read16(actor + kLsw2ActorSlotTable + 3u * 2u);
    if (lsw2_headgear_covers(hat)) return;

    /* ONCE PER (character, hat) STATE, NOT PER FRAME. The reveal now rebuilds
     * geometry through the gear hop (see lsw2_headfix_after_apply), which must not
     * be queued sixty times a second. after_apply handles the apply moment; this
     * is the safety net for a reveal that apply's timing missed, and it fires only
     * when the character or their hat has just changed to a reveal-needed state. */
    static int last_character = -1;
    static uint16_t last_hat = 0xFFFFu;
    if (character == last_character && hat == last_hat) return;
    last_character = character;
    last_hat = hat;

    const uint32_t holder = bus_device_read32(0, actor + 0xE0u);
    if (holder < kRamLow || holder >= kRamHigh) return;
    const uint32_t list = holder + 0x14u;

    const uint16_t head_model = read16(list + 2u);
    if (head_model == 0u || head_model == 0xFFFFu) return;

    const uint16_t vis = read16(list);
    write16(list, uint16_t(vis | 1u));               /* request sub-part 0's rebuild */
    lsw2_gearhop_request_toggle(0x0201C8A0u, list, actor + 0xD8u, 0u);

    static int reports = 0;
    if (reports < kReports) {
        ++reports;
        std::fprintf(stderr,
                     "[fork] head rebuild queued (tick): character %d, headwear "
                     "%u non-covering - committing head sub-part (0x%04X) to the "
                     "model node (mask %04X -> %04X)\n",
                     character, unsigned(hat), unsigned(head_model),
                     unsigned(vis), unsigned(uint16_t(vis | 1u)));
    }
}
