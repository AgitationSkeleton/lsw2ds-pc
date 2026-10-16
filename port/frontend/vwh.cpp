/* See vwh.h. */
#include "vwh.h"
#include "sessionroster.h"

#include "coophosts.h"          /* kLsw2HostTable / Stride / Status / Name / Ttl */
#include "coopjoin.h"           /* kLsw2WirelessOwner / Field */
#include "freeplaypick.h"       /* the ALL pick's end, an effect */
#include "sessionpause.h"       /* the online pause: departures unmark */
#include "levels.h"             /* kLsw2LevelRootAddr / ObjOffset */
#include "rngsync.h"            /* kLsw2RandState */
#include "scorescreen.h"        /* the lobby return, an effect */
#include "statelog.h"           /* lsw2_state_current */
#include "viewmask.h"           /* the WH context and its fields */
#include "whsession.h"          /* kLsw2CameraActorArray */
#include "../net/lsw2_net_bind.h"
#include "../net/lsw2_vwh.h"

#include "runtime_arm.h"
#include "state.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* fork.cpp - the Bounty Hunter party of one (no companion), by the dataset's members; called at the dataset
 * boundary in lockstep. */
void lsw2_fork_remove_companions(uint16_t members);

namespace {

/* The WH context and the fields of it this binding keeps coherent - named in
 * viewmask.h, which owns them (each is read from the cartridge's own
 * accessor). The rest are local to this binding. */
constexpr uint32_t kWhCtx = kLsw2SessionAuthority;
constexpr uint32_t kWhAid = kLsw2AuthorityActiveView;  /* u16, WH_GetCurrentAid */
constexpr uint32_t kWhBitmap = kLsw2AuthorityMask;     /* u16, WH_GetBitmap     */
constexpr uint32_t kWhState = kLsw2WhSystemState;      /* WH_GetSystemState     */
constexpr uint32_t kWhError = kLsw2WhErrorCode;        /* WH_GetLastError       */
constexpr uint32_t kWhChannel = 0x02u;     /* u16, WH_GetMeasureChannel */
constexpr uint32_t kWhBssDesc = kLsw2WhBssDesc;       /* ChildConnect's copy  */
constexpr uint32_t kWhShared = kLsw2WhSharedData;     /* what the game reads  */

enum : uint32_t {
    kStop = 0, kIdle = 1, kScanning = 2, kBusy = 3, kDataSharing = 5,
    kMeasureChannel = 7, kError = 9,
};
enum : uint32_t {
    kErrIllegalState = 3, kErrNoDataset = 5,
};

/* The channel this "radio" reports. A real one; nothing listens on it. */
constexpr uint16_t kChannel = 1;
/* A published browse row lives kLsw2HostRowTtl ticks; the tick below renews it
 * every frame while the list is open. */

uint16_t g_present = 0;        /* who is in the dataset the game last took */
bool g_child_connecting = false;
uint8_t g_entry = 1;           /* the game's own WM_SetEntry wish */
/* This console connected as the PARENT (ParentConnect) rather than a child. */
bool g_parent = false;
/* WM itself is up: set by a successful Initialize, cleared by WH_End (WM_End).
 * The WH system state cannot stand in for it - Reset and Finalize return to
 * IDLE with WM still initialized, and only WM_End brings it down. */
bool g_wm_up = false;

/* The host's seating of one joiner (lsw2_vwh_tick): 0 = waiting for its first
 * block, 1 = waiting for the game's join (prompt, 0x0A, reload) to leave and
 * return to the lobby, 2 = settling back in the lobby. */
struct Seating {
    int station = -1;
    int phase = 0;
    int frames = 0;
    bool left_lobby = false;
};
Seating g_seat;
int g_lobby_frames = 0;
uint16_t g_prev_members = 1;

/* How long one exchange may hold emulation for a dataset in flight (see
 * vwh_step_ds), and what holding has cost - reported with the status line. */
constexpr int kHoldBudgetMs = 250;
bool g_hold_suspended = false;
uint64_t g_holds = 0;
uint64_t g_hold_us = 0;
int g_hold_max_us = 0;

uint32_t rd32(uint32_t a) { return bus_device_read32(9, a); }
void wr32(uint32_t a, uint32_t v) { bus_device_write32(9, a, v); }
void wr16(uint32_t a, uint16_t v) { bus_device_write16(9, a, v); }

uint32_t wh_state() { return rd32(kWhCtx + kWhState); }

const char* state_name(uint32_t s) {
    switch (s) {
        case kStop: return "STOP";
        case kIdle: return "IDLE";
        case kScanning: return "SCANNING";
        case kBusy: return "BUSY";
        case kDataSharing: return "DATASHARING";
        case kMeasureChannel: return "MEASURECHANNEL";
        case kError: return "ERROR";
        default: return "?";
    }
}

/* `from` is the guest return address when a replaced method changes the state -
 * r14 at entry, the call site in the cartridge - and 0 from the fork tick. A
 * transition nobody expected is only diagnosable with its caller. */
void set_state(uint32_t s, uint32_t from) {
    const uint32_t was = wh_state();
    if (was == s) return;
    wr32(kWhCtx + kWhState, s);
    if (from)
        std::fprintf(stderr, "[vwh] WH %s -> %s (called from %08X)\n",
                     state_name(was), state_name(s), unsigned(from));
    else
        std::fprintf(stderr, "[vwh] WH %s -> %s\n", state_name(was),
                     state_name(s));
}

/* The call site of the replaced method now running. */
uint32_t caller() { return g_cpu.R[14]; }

/* WH_SetError's rule: an error is recorded unless the module is in ERROR. */
void set_error(uint32_t code) {
    if (wh_state() != kError) wr32(kWhCtx + kWhError, code);
}

void clear_host_table() {
    for (int i = 0; i < kLsw2HostSlots; ++i)
        wr32(kLsw2HostTable + uint32_t(i) * kLsw2HostStride + kLsw2HostTtl, 0);
}

/* One browse row: the host this console's session is connected to. The list
 * reads status, name and TTL (coophosts.h); the connect ignores the rest of the
 * record, because the session is already the connection. */
void publish_host_row() {
    const uint32_t rec = kLsw2HostTable;
    const char* name = lsw2_vwh_host_name();
    const size_t n = std::strlen(name);
    for (int c = 0; c < kLsw2HostNameMax; c += 2) {
        const uint16_t lo = uint16_t(c < int(n) ? uint8_t(name[c]) : 0);
        const uint16_t hi = uint16_t(c + 1 < int(n) ? uint8_t(name[c + 1]) : 0);
        wr16(rec + kLsw2HostName + uint32_t(c), uint16_t(lo | (hi << 8)));
    }
    wr16(rec + kLsw2HostStatus, 1u);
    wr32(rec + kLsw2HostTtl, kLsw2HostRowTtl);
    for (int i = 1; i < kLsw2HostSlots; ++i)
        wr32(kLsw2HostTable + uint32_t(i) * kLsw2HostStride + kLsw2HostTtl, 0);
}

void ret(uint32_t r0) {
    g_cpu.R[0] = r0;
    g_cpu.R[15] = g_cpu.R[14];
}

/* ---- the replaced methods (vtable 0x020FFD50) --------------------------- */

/* slot 0: WH_Initialize.
 *
 * THE CARTRIDGE'S OWN ORDER, INCLUDING ITS REFUSAL. 0x020AD438 resets the AID,
 * the connected bitmap and the error unconditionally, THEN asks WM to start
 * (0x020ADE64 -> 0x020D61C8), and WM refuses with ILLEGAL_STATE when it is
 * already up: WH_SetError records that, the call returns 0, and the system state
 * is not touched. The first version of this override skipped the refusal and
 * dropped a sharing console to IDLE - measured 2026-09-23, the host's match
 * ended the moment it began (DATASHARING -> IDLE, then the netgame's exchange
 * failed on the state and raised 15 -> 38) while the joiner played on. */
void vwh_initialize() {
    wr16(kWhCtx + kWhAid, 0);
    wr16(kWhCtx + kWhBitmap, 1);
    wr32(kWhCtx + kWhError, 0);
    if (g_wm_up) {
        std::fprintf(stderr, "[vwh] Initialize while WM is up (called from "
                     "%08X): refused, as WM refuses it\n", unsigned(caller()));
        set_error(kErrIllegalState);
        ret(0);
        return;
    }
    g_wm_up = true;
    g_present = 0;
    g_child_connecting = false;
    g_parent = false;
    set_state(kIdle, caller());
    ret(1);
}

uint32_t rd8(uint32_t a) { return (rd32(a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu; }
uint32_t rd16(uint32_t a) { return (rd32(a & ~3u) >> ((a & 2u) * 8u)) & 0xFFFFu; }

/* THE MATCH'S OWN OBJECT - FUN_0209b61c's single instance, registered here.
 * +0xB2 is the player mask its per-frame check (0x0209B974) samples
 * on the match's first frame and then compares against view record 0's +0x66;
 * a difference ends the session through 0x02072CD0 and requests 15 then 38 -
 * the game's "the players changed mid-match" guard. It runs on the host only
 * (it returns unless the view manager's +0x1C - this console's station - is 0),
 * and it is NOT the netgame's check, so that leaves its kind at 0. */
constexpr uint32_t kMatchObject = 0x02107860u;
constexpr uint32_t kMatchSavedMask = 0xB2u;

/* THE GUARD FOLLOWS A CHANGE THE SESSION MADE.
 *
 * The saved mask is cleared only by the match object's reset (0x0209B7CC), and
 * the only caller of that is the session's Finalize (0x02072BD4): within one
 * session the retail game never changes its players, so it samples once and
 * checks forever. The port changes players inside a session twice over -
 * a match already under way may be joined, and a disconnect is absorbed
 * without breaking the match - and measured: a joiner seated between rounds (members 7 -> F) and the host
 * ended round two as it began, state 38, with nothing out of step.
 *
 * So when the session's own membership reaches record 0 - and it reaches it at
 * one known moment, this dataset's copy of the host's block, whose +0x66 the
 * netgame copies into record 0 right after this exchange returns (0x020AF688) -
 * the guard is told to sample again, which is exactly what a fresh session does.
 * Every console clears it at the same dataset, so the state stays identical;
 * only the host's guard ever reads it. */
void follow_session_players(uint16_t incoming) {
    const uint32_t vm = rd32(kLsw2ViewManagerPtr);
    if (vm < 0x02000000u || vm >= 0x02400000u) return;
    const uint16_t current = uint16_t(rd16(vm + kLsw2ViewArray + kLsw2ViewMask));
    if (incoming == current) return;
    const uint32_t match = rd32(kMatchObject);
    if (match < 0x02000000u || match >= 0x02400000u) return;
    const uint16_t saved = uint16_t(rd16(match + kMatchSavedMask));
    if (saved == 0) return;
    wr16(match + kMatchSavedMask, 0);
    std::fprintf(stderr, "[vwh] the session's players changed %04X -> %04X: the "
                 "match's player-set guard samples them afresh (it had %04X)\n",
                 unsigned(current), unsigned(incoming), unsigned(saved));
}

/* WHY A SHARING CONSOLE IS BEING TAKEN DOWN, when the game itself does it.
 *
 * The netgame tick's failure path (0x020B0B3C) runs FUN_02072CD0 -> FUN_02072BC8,
 * which finalizes the WH (this override) BEFORE it requests 15 and 38 - so this
 * is the last moment the view manager still holds the evidence: the failure kind
 * FUN_020b0bdc stored at manager+8 (1 = the exchange failed, 2 = the cross-player
 * checksum FUN_020AF460 disagreed), each record's {frame, checksum}, and the
 * local 8-frame history the checksum is looked up in. Offsets from the
 * disassembly of FUN_020AF3E4 / FUN_020AF460: record +0x60 checksum word, +0x68
 * half, +0x6E frame; the outgoing record at +0x640; history at +0x6B4, 8 bytes
 * an entry, half then word. */
void report_netgame_failure(uint32_t from) {
    const uint32_t vm = rd32(kLsw2ViewManagerPtr);
    if (vm < 0x02000000u || vm >= 0x02400000u) return;
    const uint32_t kind = rd32(kLsw2ViewManagerPtr + 8u);
    const uint32_t match = rd32(kMatchObject);
    const int saved = (match >= 0x02000000u && match < 0x02400000u)
                          ? int(rd16(match + kMatchSavedMask)) : -1;
    std::fprintf(stderr,
                 "[vwh] the game is ending the session (finalize from %08X): "
                 "netgame check %u (%s); armed %u, own frame %u checksum "
                 "%08X/%04X; the match's saved player mask %04X\n",
                 unsigned(from), unsigned(kind),
                 kind == 1 ? "the exchange failed"
                 : kind == 2 ? "cross-player checksum mismatch"
                 : kind == 0 ? "not the netgame's check - see the saved mask"
                             : "?",
                 unsigned(rd8(vm + kLsw2NetCheckGate)), unsigned(rd8(vm + 0x6AEu)),
                 unsigned(rd32(vm + 0x6A0u)), unsigned(rd16(vm + 0x6A8u)),
                 unsigned(saved & 0xFFFF));
    for (int aid = 0; aid < kLsw2ViewRecords; ++aid) {
        const uint32_t rec = vm + kLsw2ViewArray + uint32_t(aid) * kLsw2ViewStride;
        std::fprintf(stderr,
                     "[vwh]   record %d: mask %04X frame %3u checksum %08X/%04X\n",
                     aid, unsigned(rd16(rec + kLsw2ViewMask)),
                     unsigned(rd8(rec + 0x6Eu)), unsigned(rd32(rec + 0x60u)),
                     unsigned(rd16(rec + 0x68u)));
    }
    for (uint32_t i = 0; i < 8u; ++i)
        std::fprintf(stderr, "[vwh]   history[%u]: checksum %08X/%04X\n",
                     unsigned(i), unsigned(rd32(vm + 0x6B8u + i * 8u)),
                     unsigned(rd16(vm + 0x6B4u + i * 8u)));
}

void wr8(uint32_t a, uint32_t v) {
    const uint32_t base = a & ~3u;
    const uint32_t sh = (a & 3u) * 8u;
    wr32(base, (rd32(base) & ~(0xFFu << sh)) | ((v & 0xFFu) << sh));
}

/* THE LOBBY TAKES ONE JOINER, THEN STOPS LOOKING - reopen it for the next.
 *
 * FUN_020B0CD4, the host's lobby manager, on owner *(0x02106D90+0xBB8):
 *
 *   mask changed, +0x10 == 0  -> +0x10 = the new AID; count; message 0x13
 *   mask changed, +0x10 != 0  -> its RESET path: end and restart the session
 *   +0x10 set, lobby idle, +0x08 == 0 -> request 35 (the Yes/No) + SetEntry(0)
 *   answer YES (+0x0D = 1), +0x08 == 0 -> message 0x0A mode 0: every console
 *                                        reloads the lobby with the new count;
 *                                        +0x08 = 1
 *
 * After the first join +0x10 still holds that AID and +0x08 is 1, so a third
 * player's AID would take the reset path - the end of everyone's session - and
 * no later join ever gets a prompt. Clearing the three fields at the dataset
 * boundary where the bitmap grows, before the game sees it, makes the next
 * join exactly the first one over again: the game's own broadcast, its own
 * reload, with N players. Host only - the joiners' owner is idle - and a no-op
 * for the first joiner, whose owner is still fresh.
 *
 * AND THE ANSWER IS YES, BECAUSE THE QUESTION CANNOT BE ASKED. The prompt's
 * own gate also wants the multi-view bit (0x40 of the session word 0x027E02A8)
 * clear, and in a lobby that already seated a player it is SET - read live on
 * the host: 0x00000840 ("clear in the cantina" was a single-player
 * measurement). That bit drives the behaviour broadcasts, so it is
 * not ours to clear. But whenever the prompt is skipped the manager falls
 * through to its answer check at 0x020B0E38, which asks only answer == YES,
 * not started, and a count above one - so setting the answer to YES (+0x0D = 1)
 * runs the game's own YES: the seed draw, message 0x0A mode 0, the disarm, and
 * every console's reload. The fork answers YES to every join prompt anyway
 * (lsw2_wh_answer_join_prompt_tick); this is the same answer without the
 * screen that cannot open. */
constexpr uint32_t kSessionWord = kLsw2GameStateBits;
constexpr uint32_t kMultiViewBit = 0x40u;

/* THE LOBBY MANAGER BACK TO IDLE ONCE A JOIN HAS RUN ITS COURSE.
 *
 * FUN_020B0CD4 records a joining station in owner+0x10 and, in the retail game,
 * never clears it - one join per session was all it had to handle. That field
 * also picks its reaction to the next membership change: with it zero, a player
 * who LEAVES takes the graceful path (store the new mask, recount, broadcast
 * message 0x13); with it set, the host takes the restart path (0x020B0F44):
 * reload the level, end the wireless session and start a new one - every
 * remaining console loses its link. Measured: the fourth console killed
 * mid-match, the host restarted, and both survivors ended in state 38. The
 * port wants the opposite: a departed player simply disappears from the HUD
 * and from gameplay. So once the seating is over, the field goes back to the value it
 * had before any join; the next join sets it again (and reopen_lobby_for_join
 * already clears it before one). The lobby manager is the host's alone (it
 * returns unless owner+0x07), so this is the host's write to make. */
void lobby_join_finished() {
    const uint32_t owner = rd32(kLsw2WirelessOwner + kLsw2WirelessOwnerField);
    if (owner < 0x02000000u || owner >= 0x02400000u) return;
    if (rd32(owner + 0x10u) == 0) return;
    wr32(owner + 0x10u, 0);
    std::fprintf(stderr, "[vwh] the lobby manager's join is complete - its "
                 "pending station cleared, so a player who leaves is counted "
                 "out rather than restarting the session\n");
}

/* THE SESSION'S DEPTH FITS ITS SLOWEST LINK - raised before the joiner who
 * needs it is seated.
 *
 * Measured, what a link slower than the depth costs: at an 80 ms round
 * trip and depth 2, every console ran at about 48 fps, the host waiting on the
 * far console's block every dataset. The depth cannot be fitted per client -
 * the game's own check needs it uniform (lsw2_vwh.cpp, release_admission) - so
 * the whole session deepens instead: the host posts the new depth as an
 * effect, every console raises its pipeline at the dataset that carries it,
 * and only then is the joiner seated, so its START carries the deeper value.
 * Returns true while that is still under way and the seating must wait.
 *
 * A host with nobody to agree with raises its own at once; and a change that
 * has not landed within five seconds is given up on - seating at the old depth
 * costs frame rate, which is better than a joiner who is never seated. */
int g_depth_asked = 0;
int g_depth_wait = 0;

bool deepen_for_next_joiner(Lsw2VwhSession* s) {
    /* A RAISE ALREADY POSTED IS SEEN THROUGH before anyone is seated, even if
     * the joiner's round trip has since come down. Measured 2026-09-23
     * (conn_int4b): the need dropped between two ticks, the joiner was seated
     * at the old depth with the raise still queued, the raise then landed at a
     * dataset before the joiner's first one - and one console ran a different
     * depth from the rest, the exact case the game's own check refuses. */
    if (g_depth_asked > s->delay() && g_depth_wait <= 5 * 60) {
        ++g_depth_wait;
        return true;
    }
    const int station = s->next_admission();
    const int need = station < 0 ? 0 : lsw2_vwh_delay_for_rtt_us(s->rtt_us(station));
    if (need <= s->delay() || g_depth_wait > 5 * 60) {
        if (need > s->delay())
            std::fprintf(stderr, "[vwh] the session did not deepen to %d in 5 s "
                         "- seating station %d at depth %d\n", need, station,
                         s->delay());
        g_depth_asked = 0;
        g_depth_wait = 0;
        return false;
    }
    ++g_depth_wait;
    if (s->members() == 1u) {
        s->raise_delay(need);
        std::fprintf(stderr, "[vwh] session depth -> %d for station %d's %d ms "
                     "round trip (the host is alone, nobody to agree with)\n",
                     s->delay(), station, s->rtt_us(station) / 1000);
        return false;
    }
    if (g_depth_asked < need) {
        const uint8_t d = uint8_t(need);
        if (lsw2_vwh_post_effect(kLsw2EffectDepth, &d, 1)) {
            g_depth_asked = need;
            std::fprintf(stderr, "[vwh] station %d's round trip is %d ms: the "
                         "session deepens from %d to %d datasets before it is "
                         "seated\n", station, s->rtt_us(station) / 1000,
                         s->delay(), need);
        }
    }
    return true;
}

/* THE HOST'S EFFECTS FOR ONE DATASET (vwh.h), in the order it posted them.
 * Called at the dataset boundary on every console with the same bytes. A type
 * this build does not know is reported once and skipped by its length - a newer
 * host must not be able to stop an older client's datasets. */
void apply_effects(const uint8_t* fx) {
    int off = 0;
    while (off + kLsw2VwhEffectHeader <= kLsw2VwhEffectBytes && fx[off] != 0) {
        const uint8_t type = fx[off];
        const int len = fx[off + 1];
        if (off + kLsw2VwhEffectHeader + len > kLsw2VwhEffectBytes) break;
        switch (type) {
        case kLsw2EffectLobbyReturn:
            lsw2_score_lobby_return_begin();
            break;
        case kLsw2EffectRoster:
            lsw2_sessionroster_apply(fx + off + kLsw2VwhEffectHeader, len);
            break;
        case kLsw2EffectTestKill:
            if (len >= 1)
                lsw2_sessionroster_test_kill(fx[off + kLsw2VwhEffectHeader]);
            break;
        case kLsw2EffectPickEnd:
            lsw2_freeplaypick_pregame_end();
            break;
        case kLsw2EffectTestTeleport:
            lsw2_sessionroster_test_teleport(fx + off + kLsw2VwhEffectHeader, len);
            break;
        case kLsw2EffectTestHit:
            lsw2_sessionroster_test_hit(fx + off + kLsw2VwhEffectHeader, len);
            break;
        case kLsw2EffectDepth:
            if (len >= 1) {
                Lsw2VwhSession* s = lsw2_vwh_session();
                const int was = s ? s->delay() : 0;
                if (s) s->raise_delay(fx[off + kLsw2VwhEffectHeader]);
                std::fprintf(stderr, "[vwh] session depth %d -> %d datasets, "
                             "applied at this dataset on every console\n", was,
                             s ? s->delay() : 0);
            }
            break;
        default: {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr, "[vwh] effect type %u is not one this build "
                             "applies - skipped\n", unsigned(type));
            }
            break;
        }
        }
        off += kLsw2VwhEffectHeader + len;
    }
}

void reopen_lobby_for_join(uint16_t grew) {
    /* THE ARENA'S CAPACITY, FIRST. The lobby is the cantina's co-op session, and
     * its session start (FUN_020B10F4) sets the player bound to 2; the manager
     * counts bitmap bits only below the bound, so a third and fourth player
     * sharing every dataset still counted as two - measured 2026-09-23, four
     * consoles in one match with the game running it as a two-player one and
     * two spectators with no actor. The arena's own multiplayer menus set 4
     * (0x0206F3EC, 0x0207161C), and the original game's arena capture reads 4
     * (viewmask.h). The host raises it here, before the game counts the new
     * member; message 0x13 carries it to every other console. */
    const uint32_t bound = rd32(kLsw2PumpCountWord + kLsw2PumpBound);
    if (int32_t(bound) < kLsw2PumpBoundValue) {
        wr32(kLsw2PumpCountWord + kLsw2PumpBound, uint32_t(kLsw2PumpBoundValue));
        std::fprintf(stderr, "[vwh] player bound %d -> %d (the arena's capacity)\n",
                     int(int32_t(bound)), kLsw2PumpBoundValue);
    }

    const uint32_t owner = rd32(kLsw2WirelessOwner + kLsw2WirelessOwnerField);
    if (owner < 0x02000000u || owner >= 0x02400000u) return;
    const uint32_t pending = rd32(owner + 0x10u);
    const uint32_t started = rd8(owner + 0x08u);
    if (!pending && !started) return;          /* a fresh lobby: the game's own */
    const bool multiview = (rd32(kSessionWord) & kMultiViewBit) != 0;
    wr32(owner + 0x10u, 0);
    wr8(owner + 0x08u, 0);
    wr8(owner + 0x0Du, multiview ? 1u : 0u);
    std::fprintf(stderr, "[vwh] AIDs %X joining a lobby that has seated before "
                 "(pending AID %u, started %u, multi-view %d) - reopened the "
                 "game's own join%s\n",
                 unsigned(grew), unsigned(pending), unsigned(started),
                 int(multiview), multiview ? ", answered YES" : "");
}

/* slot 1 */
void vwh_finalize() {
    if (wh_state() == kDataSharing) report_netgame_failure(caller());
    if (wh_state() == kIdle) { ret(0); return; }
    clear_host_table();
    g_child_connecting = false;
    set_state(kIdle, caller());
    ret(1);
}

/* slot 2 */
void vwh_reset() {
    clear_host_table();
    wr32(kWhCtx + kWhError, 0);
    g_child_connecting = false;
    set_state(kIdle, caller());
    ret(1);
}

/* slot 3: WH_ParentConnect(mode, tgid, channel, byte) */
void vwh_parent_connect() {
    const uint32_t mode = g_cpu.R[1];
    if (mode != 0 && mode != 2 && mode != 4) { ret(0); return; }
    g_parent = true;
    wr16(kWhCtx + kWhAid, 0);
    wr16(kWhCtx + kWhBitmap, 1);
    g_present = 0;
    Lsw2VwhSession* s = lsw2_vwh_session();
    std::fprintf(stderr, "[vwh] ParentConnect(mode %u): %s\n", unsigned(mode),
                 (s && s->hosting()) ? "hosting on the fork session"
                                     : "no lobby yet - a parent with no children");
    set_state(kDataSharing, caller());
    ret(1);
}

/* slot 4: WH_ChildConnect(mode, bssDesc, byte) */
void vwh_child_connect() {
    const uint32_t mode = g_cpu.R[1];
    const uint32_t desc = g_cpu.R[2];
    if (mode != 1 && mode != 3 && mode != 5) { ret(1); return; }
    g_parent = false;
    if (desc >= 0x02000000u && desc < 0x02400000u)
        for (uint32_t w = 0; w < 0xC0u; w += 4u)
            wr32(kWhBssDesc + w, rd32(desc + w));
    g_child_connecting = true;
    set_state(kBusy, caller());
    std::fprintf(stderr, "[vwh] ChildConnect(mode %u): joining over the fork "
                 "session\n", unsigned(mode));
    ret(1);
}

/* slot 5: the kick */
void vwh_kick() {
    const uint32_t aid = g_cpu.R[1] & 0xFFFFu;
    if (Lsw2VwhSession* s = lsw2_vwh_session()) s->kick(int(aid));
    const uint32_t bm = rd32(kWhCtx + (kWhBitmap & ~3u)) >> ((kWhBitmap & 2u) * 8u);
    wr16(kWhCtx + kWhBitmap, uint16_t((bm & 0xFFFFu) & ~(1u << (aid & 15u))));
    ret(0);
}

/* slot 6: WH_StepDS(send) */
void vwh_step_ds() {
    const uint32_t send = g_cpu.R[1];
    if (wh_state() != kDataSharing) { set_error(kErrIllegalState); ret(0); return; }

    Lsw2VwhSession* s = lsw2_vwh_session();
    if (!s) {
        /* A parent with no lobby: WM's NO_CHILD, which WH_StepDS reports as
         * success without touching the shared data - nobody present. */
        g_present = 0;
        ret(1);
        return;
    }
    if (!s->live()) { set_error(kErrNoDataset); ret(0); return; }

    /* ONE DATASET PER CONSOLE PER EMULATED FRAME - WM's one MP cycle a frame.
     *
     * Measured (a profiled two-console run): the netgame's tick (FUN_020b07e8) is a
     * do/while that runs another exchange whenever a view record still has work
     * flagged, and returns only when nothing does. Over the radio the second
     * exchange of a frame answers NO_DATASET, the game waits a vblank, and the
     * loop spreads over frames. A host that answered every call raced through
     * datasets many a frame - ~20 ms of emulation per frame in the statue
     * select, and a joiner it left permanently behind. */
    /* PER VBLANK, NOT PER LINE-0 FRAME. The game exchanges right after its
     * VBlank wait, 71 lines before a line-0 boundary; when the work before the
     * exchange varies around those ~4.5 ms, two consecutive VBlanks' exchanges
     * fall in one line-0 frame and the second is refused - an extra VBlank the
     * game waits on one console and not the other. WM releases one dataset per
     * VBlank, so that is the unit. (Measured: the two consoles made
     * the same shared-stream draws a step or three apart and the match's first
     * cross-player check failed. A refusal like this is ONE way a
     * console can lose a VBlank the other does not; the hold below covers the
     * other way, a dataset that arrives late.) */
    static uint64_t stepped_frame = ~uint64_t(0);
    const uint64_t frame = lsw2_vblank_ordinal();
    if (frame == stepped_frame) { set_error(kErrNoDataset); ret(0); return; }

    uint8_t block[kLsw2VwhBlockBytes];
    for (int i = 0; i < kLsw2VwhBlockBytes; i += 4) {
        const uint32_t w = rd32(send + uint32_t(i));
        block[i] = uint8_t(w);
        block[i + 1] = uint8_t(w >> 8);
        block[i + 2] = uint8_t(w >> 16);
        block[i + 3] = uint8_t(w >> 24);
    }
    uint8_t out[kLsw2VwhStations][kLsw2VwhBlockBytes];
    uint16_t present = 0, members = 0;

    /* THE GAME MUST NOT SEE THE NETWORK - it holds emulation instead.
     *
     * Measured: with every block delivered and every
     * dataset identical, the two consoles still made the SAME shared-stream
     * draws, in the same order, zero to three dataset steps apart - and the
     * match's first cross-player check failed. The game keeps time in VBlanks
     * (its frame loop waits for two per frame, 0x020013A4), and a console whose
     * exchange answers NO_DATASET waits another VBlank that the other console
     * does not. On hardware WM holds every console to one VBlank cadence and
     * that never happens; on PCs with their own clocks it happens whenever a
     * block is a millisecond late.
     *
     * So a missing dataset holds the emulator, in real time, until it arrives:
     * the guest never runs a VBlank the other consoles do not, and emulated
     * time stays identical on every machine - which is what a deterministic
     * lockstep needs. Bounded, and a peer that cannot deliver within the bound
     * is waited on the game's way (NO_DATASET) until it recovers, so a dead
     * link freezes the picture once, not on every frame. */
    uint8_t effects[kLsw2VwhEffectBytes];
    bool ok = s->step(block, out, &present, &members, true, effects);
    if (!ok && s->live() && !g_hold_suspended) {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(kHoldBudgetMs);
        while (!ok && s->live() && std::chrono::steady_clock::now() < deadline) {
            s->pump(1000);
            ok = s->step(block, out, &present, &members, false, effects);
        }
        const int held_us = int(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - start).count());
        ++g_holds;
        g_hold_us += uint64_t(held_us);
        if (held_us > g_hold_max_us) g_hold_max_us = held_us;
        if (!ok) {
            g_hold_suspended = true;
            std::fprintf(stderr, "[vwh] no dataset after holding %d ms - the "
                         "game waits on its own until the link recovers\n",
                         held_us / 1000);
        }
    }
    if (!ok) {
        set_error(kErrNoDataset);   /* the game waits a vblank and retries */
        ret(0);
        return;
    }
    if (g_hold_suspended) {
        g_hold_suspended = false;
        std::fprintf(stderr, "[vwh] datasets flowing again - holding resumed\n");
    }

    /* THE DATASET BOUNDARY. A fork change to simulation state that must still
     * happen while in lockstep happens HERE - every console is at the same step
     * of the game's frame when it consumes the same dataset - and never from
     * the between-frames tick (vwh.h, lsw2_vwh_lockstep). The Bounty Hunter
     * party of one (no companion) is the one such change: the arena spawns companions in the empty slots, and
     * the removal has to land at the same step on every machine. */
    if (lsw2_vwh_lockstep()) lsw2_fork_remove_companions(members);
    /* A paused player who left is not paused any more - at this dataset, on
     * every console (sessionpause.h). */
    lsw2_sessionpause_members(members);

    /* AND THE HOST'S EFFECTS FOR THIS DATASET - here, so they land at this
     * step on every console. */
    apply_effects(effects);

    /* THE HOST'S RECORD, before the netgame copies it: if the player mask it
     * carries is about to change record 0, the match's guard follows it. */
    if (present & 1u)
        follow_session_players(uint16_t(out[0][kLsw2ViewMask] |
                                        (out[0][kLsw2ViewMask + 1] << 8)));

    /* A NEW MEMBER IN THE BITMAP: let the game take it as its own join. */
    if (s->hosting()) {
        const uint16_t grew = uint16_t(members & ~g_prev_members);
        if (grew) reopen_lobby_for_join(grew);
        g_prev_members = members;
    }
    for (int aid = 0; aid < kLsw2VwhStations; ++aid) {
        if (!((present >> aid) & 1u)) continue;
        const uint32_t base = kWhShared + uint32_t(aid * kLsw2VwhBlockBytes);
        const uint8_t* b = out[aid];
        for (int i = 0; i < kLsw2VwhBlockBytes; i += 4)
            wr32(base + uint32_t(i), uint32_t(b[i]) | (uint32_t(b[i + 1]) << 8) |
                                         (uint32_t(b[i + 2]) << 16) |
                                         (uint32_t(b[i + 3]) << 24));
    }
    g_present = present;
    stepped_frame = frame;

    /* LSW2_RNG_TRACE=1 - WHERE THE SHARED STREAM IS, AT EVERY STEP.
     *
     * The cross-player check compares the byte the MT draw (FUN_02062D10)
     * leaves at kLsw2RandState on every draw; a mismatch means some
     * draw ran at a different simulation step on one console. The draw is a
     * direct call the lifter emits as plain C, so it cannot be hooked - but its
     * word pointer (+0xC) advances four bytes a draw, and the dataset index is
     * the same simulation step on every console under lockstep. So each line is
     * "how many draws happened since the previous step", and two consoles' logs
     * diff by dataset index to the first step that is not shared. */
    static const bool rng_trace = [] {
        const char* v = std::getenv("LSW2_RNG_TRACE");
        return v && *v == '1';
    }();
    if (rng_trace) {
        static uint32_t last_ptr = 0, last_byte = 0x100u;
        static int lines = 0;
        const uint32_t ptr = rd32(kLsw2RandState + 0xCu);
        const uint32_t byte = rd8(kLsw2RandState);
        if ((ptr != last_ptr || byte != last_byte) && lines < 8000) {
            ++lines;
            std::fprintf(stderr, "[rng] ds %u ptr %08X (%+d) byte %02X state %d\n",
                         unsigned(s->recv_cursor() - 1u), unsigned(ptr),
                         int(ptr - last_ptr) / 4, unsigned(byte),
                         lsw2_state_current());
        }
        last_ptr = ptr;
        last_byte = byte;
    }

    /* LSW2_STATE_TRACE=1 - WHERE TWO CONSOLES' GAME STATE FIRST PARTS.
     *
     * The shared stream only shows a divergence once it reaches a draw. This
     * hashes each party actor's first 0x800 bytes in 64-byte chunks at every
     * step of the match's first 150 datasets - the same dataset is the same
     * simulation step everywhere - so a diff of two consoles' lines names the
     * first step, actor and chunk that differ. */
    static const bool state_trace = [] {
        const char* v = std::getenv("LSW2_STATE_TRACE");
        return v && *v == '1';
    }();
    if (state_trace && lsw2_state_current() == 42) {
        static int steps = 0;
        if (steps < 150) {
            ++steps;
            for (uint32_t slot = 0; slot < 4u; ++slot) {
                const uint32_t actor = rd32(kLsw2CameraActorArray + slot * 4u);
                if (actor < 0x02000000u || actor >= 0x02400000u) continue;
                char line[40 * 5 + 64];
                int n = std::snprintf(line, sizeof line, "[st] ds %u a%u %08X",
                                      unsigned(s->recv_cursor() - 1u),
                                      unsigned(slot), unsigned(actor));
                for (uint32_t chunk = 0; chunk < 0x800u; chunk += 0x40u) {
                    uint32_t h = 2166136261u;
                    for (uint32_t w = 0; w < 0x40u; w += 4u) {
                        h ^= rd32(actor + chunk + w);
                        h *= 16777619u;
                    }
                    n += std::snprintf(line + n, sizeof line - size_t(n), " %04X",
                                       unsigned((h ^ (h >> 16)) & 0xFFFFu));
                }
                std::fprintf(stderr, "%s\n", line);
            }
        }
    }
    /* Membership, not presence: the station count derives from this and must
     * only change on a join or a leave. */
    wr16(kWhCtx + kWhBitmap, members);
    ret(1);
}

/* slot 8 */
void vwh_end_data_sharing() {
    wr32(kWhCtx + kWhError, 0);
    ret(1);
}

/* slot 9: WH_GetSharedDataAdr(aid) */
void vwh_shared_address() {
    const uint32_t aid = g_cpu.R[1] & 0xFFFFu;
    ret((aid < uint32_t(kLsw2VwhStations) && ((g_present >> aid) & 1u))
            ? kWhShared + aid * uint32_t(kLsw2VwhBlockBytes)
            : 0u);
}

/* slot 15: WH_StartMeasureChannel - there is no spectrum to measure */
void vwh_start_measure() {
    wr16(kWhCtx + kWhChannel, kChannel);
    set_state(kMeasureChannel, caller());
    ret(1);
}

/* slot 16: WH_StartScan(macFilter, channel) */
void vwh_start_scan() {
    set_state(kScanning, caller());
    if (lsw2_vwh_session()) publish_host_row();
    else clear_host_table();
    ret(1);
}

/* slot 17: WH_EndScan */
void vwh_end_scan() {
    if (wh_state() == kScanning) set_state(kIdle, caller());
    ret(1);
}

/* slot 19: WM_SetEntry(flag) - the game's own "room open to joiners" wish */
void vwh_set_entry() {
    const uint8_t want = uint8_t(g_cpu.R[1] & 1u);
    if (want != g_entry)
        std::fprintf(stderr, "[vwh] the game %s the room to new joiners\n",
                     want ? "opens" : "closes");
    g_entry = want;
    ret(1);
}

/* slot 20: WH_End - WM_End, so WM is down and the next Initialize succeeds */
void vwh_end() {
    g_child_connecting = false;
    g_wm_up = false;
    set_state(kIdle, caller());
    ret(1);
}

/* slot 24: WM_GetLinkLevel - the DS signal icon (0-3) */
void vwh_link_level() {
    Lsw2VwhSession* s = lsw2_vwh_session();
    const bool linked = wh_state() == kDataSharing && s && s->live() &&
                        (s->members() & ~1u) != 0;
    const bool host_linked = wh_state() == kDataSharing && s && s->hosting() &&
                             (s->members() & ~1u) != 0;
    ret((linked || host_linked) ? 3u : 0u);
}

const NdsDispatchEntry kVwhOverrides[] = {
    { 0x020AD438u, 0, vwh_initialize,        nullptr },
    { 0x020AD5DCu, 0, vwh_finalize,          nullptr },
    { 0x020AD55Cu, 0, vwh_reset,             nullptr },
    { 0x020AD970u, 0, vwh_parent_connect,    nullptr },
    { 0x020ADBCCu, 0, vwh_child_connect,     nullptr },
    { 0x020ADC40u, 0, vwh_kick,              nullptr },
    { 0x020AD7F0u, 0, vwh_step_ds,           nullptr },
    { 0x020AD75Cu, 0, vwh_end_data_sharing,  nullptr },
    { 0x020AD884u, 0, vwh_shared_address,    nullptr },
    { 0x020ADAF0u, 0, vwh_start_measure,     nullptr },
    { 0x020ADB5Cu, 0, vwh_start_scan,        nullptr },
    { 0x020AD928u, 0, vwh_end_scan,          nullptr },
    { 0x020ADC88u, 0, vwh_set_entry,         nullptr },
    { 0x020ADD78u, 0, vwh_end,               nullptr },
    { 0x020AD534u, 0, vwh_link_level,        nullptr },
};

}  // namespace

const NdsDispatchEntry* lsw2_vwh_overrides(unsigned* count) {
    if (!lsw2_vwh_enabled()) {
        if (count) *count = 0;
        return nullptr;
    }
    if (count) *count = unsigned(sizeof kVwhOverrides / sizeof *kVwhOverrides);
    return kVwhOverrides;
}

void lsw2_vwh_tick() {
    if (!lsw2_vwh_enabled()) return;
    Lsw2VwhSession* s = lsw2_vwh_session();
    const uint32_t st = wh_state();

    /* A joining console becomes a member when the host's START arrives. */
    if (g_child_connecting && s && !s->hosting() && s->live()) {
        g_child_connecting = false;
        wr16(kWhCtx + kWhAid, uint16_t(s->aid()));
        set_state(kDataSharing, 0);
        std::fprintf(stderr, "[vwh] joined as AID %d (pipeline depth %d)\n",
                     s->aid(), s->delay());
    }

    /* The browse screen ages the row once a tick; keep it alive. */
    if (st == kScanning && s) publish_host_row();

    /* VIEW RECORD 0's PLAYER MASK, whenever it moves - the value the match's
     * player-set guard compares (kMatchObject), so a guard that fires can be
     * lined up against the dataset at which the mask it saw changed. */
    if (s && s->live()) {
        static int last = -1;
        const uint32_t vm = rd32(kLsw2ViewManagerPtr);
        const int now = (vm >= 0x02000000u && vm < 0x02400000u)
                            ? int(rd16(vm + kLsw2ViewArray + kLsw2ViewMask)) : -1;
        if (now != last) {
            std::fprintf(stderr, "[vwh] view record 0 mask %04X -> %04X at dataset "
                         "%u (state %d)\n", unsigned(last & 0xFFFF),
                         unsigned(now & 0xFFFF), unsigned(s->recv_cursor()),
                         lsw2_state_current());
            last = now;
        }
    }

    /* WHERE THE SESSION IS, every ten seconds while sharing: the dataset index,
     * who is connected, and how far a client trails the host's commits - the
     * three numbers that tell a healthy link from a starving one. */
    static int beat = 0;
    if (st == kDataSharing && s && s->live() && ++beat >= 600) {
        beat = 0;
        std::fprintf(stderr, "[vwh] AID %d dataset %u members %X backlog %d | "
                     "held %llu times, %llu ms in all, longest %d ms | "
                     "effects pending %d B\n",
                     s->aid(), unsigned(s->recv_cursor()), unsigned(s->members()),
                     s->backlog(), (unsigned long long)g_holds,
                     (unsigned long long)(g_hold_us / 1000u), g_hold_max_us / 1000,
                     s->pending_effect_bytes());
    }

    /* THE HOST SEATS ONE JOINER AT A TIME, AND ONLY IN THE LOBBY.
     *
     * The cartridge's lobby manager (FUN_020B0CD4) takes a joiner by seeing a
     * new AID in the bitmap, raising its Yes/No prompt, broadcasting message
     * 0x0A and reloading the lobby on every console with the new count. It does
     * that for ONE joiner; a second AID arriving mid-sequence takes its reset
     * path. So a connected station waits in the session's queue until the host
     * has sat in the lobby for a second with no join under way, is seated
     * (START), and the next one waits until this one's reload has come back to
     * the lobby. A joiner who connects mid-round is therefore seated at the next
     * lobby - its console keeps its own lobby until then (whsession.cpp holds
     * the game-side join until START). */
    if (s && s->hosting()) {
        const int state = lsw2_state_current();
        /* NOT DURING ALL'S PRE-GAME PICK, although it runs in the lobby's own
         * screen state: the pick IS the round start. A joiner seated in it
         * would reload every console's lobby under the grid, and its own
         * console, which never saw the round start, would ignore the pick's
         * end - staying in the lobby while the rest go to the select. It is
         * seated at the next lobby, as a joiner who arrives mid-match is. */
        const bool lobby = state == kLsw2ArenaLobbyState &&
                           !lsw2_freeplaypick_pregame_active();
        g_lobby_frames = lobby ? g_lobby_frames + 1 : 0;
        if (g_seat.station >= 0) {
            ++g_seat.frames;
            const bool shares = ((s->members() >> g_seat.station) & 1u) != 0;
            if (g_seat.phase == 0 && shares) {
                g_seat.phase = 1;
                g_seat.frames = 0;
            } else if (g_seat.phase == 1) {
                /* THE RELOAD, not merely leaving the lobby: the first join also
                 * passes through the prompt (35) and back before message 0x0A
                 * sends everyone through 40 and a load. Measured: counting the
                 * 35 -> 48 bounce as done released the next joiner early. */
                if (state == kLsw2StateSessionStart || state == -1)
                    g_seat.left_lobby = true;
                else if (state == kLsw2ArenaLobbyState && g_seat.left_lobby) {
                    g_seat.phase = 2;
                    g_seat.frames = 0;
                }
            } else if (g_seat.phase == 2 && g_seat.frames >= 60) {
                std::fprintf(stderr, "[vwh] station %d is seated: the lobby "
                             "reloaded with members %X\n", g_seat.station,
                             unsigned(s->members()));
                lobby_join_finished();
                g_seat = Seating();
            }
            if (g_seat.station >= 0 && g_seat.frames > 60 * 30) {
                std::fprintf(stderr, "[vwh] station %d: no progress in 30 s at "
                             "seating phase %d - moving on\n", g_seat.station,
                             g_seat.phase);
                lobby_join_finished();
                g_seat = Seating();
            }
        } else if (s->waiting_admissions() > 0 && g_lobby_frames >= 60 &&
                   s->admission_measured() && !deepen_for_next_joiner(s)) {
            g_seat = Seating();
            g_seat.station = s->release_admission();
            const int rtt = s->rtt_us(g_seat.station);
            const int need = lsw2_vwh_delay_for_rtt_us(rtt);
            std::fprintf(stderr, "[vwh] seating station %d - the lobby takes one "
                         "joiner at a time (%d more waiting); round trip %d ms, "
                         "session depth %d%s\n", g_seat.station,
                         s->waiting_admissions(), rtt < 0 ? -1 : rtt / 1000,
                         s->delay_for(g_seat.station),
                         need > s->delay_for(g_seat.station)
                             ? " - this link needs more, so frames will wait on it"
                             : "");
        }
    }

    /* LSW2_ITCM_WATCH=1 - NAME THE MOMENT ITCM CHANGES.
     *
     * The four-player Game Settings crash ran code at 0x01FF8AEC that is not
     * the boot image's instruction there (a conditional branch became a call
     * into 0x03BB8B07), and this ROM has no ARM9 overlays - so something wrote
     * into ITCM, most likely through a null pointer (ITCM is mirrored from 0).
     * Snapshot it once in the lobby, compare every tick, and report the first
     * differing words with the state they changed in. Diagnostic only. */
    static const bool itcm_watch = [] {
        const char* v = std::getenv("LSW2_ITCM_WATCH");
        return v && *v == '1';
    }();
    if (itcm_watch) {
        constexpr uint32_t kItcm = 0x01FF8000u, kItcmWords = 0x55C0u / 4u;
        static uint32_t* snap = nullptr;
        static bool reported = false;
        if (!snap && lsw2_state_current() == kLsw2ArenaLobbyState) {
            snap = new uint32_t[kItcmWords];
            for (uint32_t i = 0; i < kItcmWords; ++i) snap[i] = rd32(kItcm + i * 4u);
            std::fprintf(stderr, "[itcm] snapshot taken (%u words)\n", unsigned(kItcmWords));
        } else if (snap && !reported) {
            int shown = 0;
            for (uint32_t i = 0; i < kItcmWords; ++i) {
                const uint32_t now = rd32(kItcm + i * 4u);
                if (now == snap[i]) continue;
                if (shown < 12)
                    std::fprintf(stderr, "[itcm] %08X was %08X now %08X (state %d)\n",
                                 unsigned(kItcm + i * 4u), unsigned(snap[i]),
                                 unsigned(now), lsw2_state_current());
                ++shown;
            }
            if (shown) {
                reported = true;
                std::fprintf(stderr, "[itcm] %d word(s) changed\n", shown);
            }
        }
    }

    /* LSW2_LEVEL_LIST_DUMP=1 - the level's actor list (level + 0x394, nodes
     * {actor, next}) on every state change: the four-player Game Settings walk
     * (FUN_0208926C) met a garbage node that no two-player run shows. */
    static const bool list_dump = [] {
        const char* v = std::getenv("LSW2_LEVEL_LIST_DUMP");
        return v && *v == '1';
    }();
    if (list_dump) {
        static int last_state = -999;
        const int state = lsw2_state_current();
        if (state != last_state) {
            last_state = state;
            const uint32_t level = rd32(kLsw2LevelRootAddr + kLsw2LevelObjOffset);
            std::fprintf(stderr, "[list] state %d level %08X:", state, unsigned(level));
            if (level >= 0x02000000u && level < 0x02400000u) {
                uint32_t node = rd32(level + 0x394u);
                for (int i = 0; i < 12 && node; ++i) {
                    const bool ok = node >= 0x02000000u && node < 0x02400000u;
                    const uint32_t actor = ok ? rd32(node) : 0;
                    std::fprintf(stderr, " [%08X a=%08X]", unsigned(node), unsigned(actor));
                    if (!ok) break;
                    node = rd32(node + 4u);
                }
            }
            std::fprintf(stderr, "\n");
        }
    }

    /* LSW2_DEEP_TRACE_54: whsession.cpp arms the branch ring as Game Settings
     * opens; two seconds later it is switched off again (the ring forces the
     * slow bus path for every access). */
    static const bool deep54 = [] {
        const char* v = std::getenv("LSW2_DEEP_TRACE_54");
        return v && *v == '1';
    }();
    static int deep_left = -1;
    if (deep54 && g_runtime_deep_trace) {
        if (deep_left < 0) deep_left = 120;
        if (--deep_left == 0) {
            runtime_set_deep_trace(0);
            std::fprintf(stderr, "[fork] deep trace OFF\n");
        }
    }

    /* A HOST HELD UP BY ONE MEMBER, said once a second while it lasts - under
     * lockstep a frozen host screen is always waiting on somebody, and this
     * names who and for how long (it is kicked at kLsw2VwhStallSteps). */
    static uint32_t said_at = 0;
    if (s && s->hosting() && s->waiting_on() >= 0 && s->stall_steps() >= 60 &&
        s->stall_steps() % 60 == 0 && s->stall_steps() != said_at) {
        said_at = s->stall_steps();
        std::fprintf(stderr, "[vwh] host waiting on AID %d for %u frames\n",
                     s->waiting_on(), unsigned(s->stall_steps()));
    }

    /* The session ended under a sharing console: say so the way the radio
     * would, so the game's wait loop exits and its own handling runs. */
    if (st == kDataSharing && !s && !g_parent) {
        set_error(kErrIllegalState);
        set_state(kError, 0);
    }
}

int lsw2_vwh_backlog() {
    Lsw2VwhSession* s = lsw2_vwh_session();
    return (s && !s->hosting() && s->live()) ? s->backlog() : 0;
}

bool lsw2_vwh_seating() {
    const Lsw2VwhSession* s = lsw2_vwh_session();
    if (!lsw2_vwh_enabled() || !s || !s->hosting()) return false;
    return g_seat.station >= 0 || s->waiting_admissions() > 0;
}

bool lsw2_vwh_hosting() {
    const Lsw2VwhSession* s = lsw2_vwh_session();
    return lsw2_vwh_enabled() && s && s->hosting();
}

int lsw2_vwh_local_aid() {
    const Lsw2VwhSession* s = lsw2_vwh_session();
    return (lsw2_vwh_enabled() && s && s->live()) ? s->aid() : -1;
}

bool lsw2_vwh_multiview() { return (rd32(kSessionWord) & kMultiViewBit) != 0; }

bool lsw2_vwh_post_effect(uint8_t type, const uint8_t* payload, int len) {
    Lsw2VwhSession* s = lsw2_vwh_session();
    if (!lsw2_vwh_enabled() || !s || !s->hosting() || type == 0) return false;
    if (len < 0 || len > kLsw2VwhEffectBytes - kLsw2VwhEffectHeader) return false;
    if (len > 0 && !payload) return false;
    uint8_t rec[kLsw2VwhEffectBytes];
    rec[0] = type;
    rec[1] = uint8_t(len);
    if (len > 0) std::memcpy(rec + kLsw2VwhEffectHeader, payload, size_t(len));
    return s->post_effect(rec, len + kLsw2VwhEffectHeader);
}

bool lsw2_vwh_lockstep() {
    if (!lsw2_vwh_enabled()) return false;
    const Lsw2VwhSession* s = lsw2_vwh_session();
    if (!s || !s->live() || wh_state() != kDataSharing) return false;
    const uint16_t m = s->members();
    return (m & uint16_t(m - 1u)) != 0;   /* two or more stations */
}
