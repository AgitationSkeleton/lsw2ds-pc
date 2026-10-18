/* See sessionroster.h. */

#include "sessionroster.h"

#include "cheats.h"          /* lsw2_cheats_party_actor, lsw2_cheats_state_kind */
#include "forksession.h"
#include "freeplaypick.h"    /* ALL's picks */
#include "gearhop.h"         /* the test death's pinned call */
#include "hudportrait.h"     /* kLsw2HudStep */
#include "hostsettings.h"
#include "lasthunter.h"      /* lsw2_hunter_index */
#include "lobbyrespawn.h"    /* kLsw2ActorPos - the test teleport */
#include "roster.h"          /* lsw2_roster_character */
#include "selectskip.h"      /* kLsw2SelPicked, kLsw2SelChosen, kLsw2SelSlots */
#include "statelog.h"        /* kLsw2StateSelect, kLsw2StateManager, lsw2_state_current */
#include "ui.h"
#include "viewmask.h"        /* kLsw2PumpCountWord - the game's own player count */
#include "vwh.h"
#include "whsession.h"       /* kLsw2ArenaLobbyState */

#include "state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void lsw2_arm9_FUN_020897f8(void);
extern "C" void lsw2_arm9_FUN_02091b88(void);

namespace {

/* The per-station save record the arena builds from is cheats.h's appearance
 * record (kLsw2AppearanceRecord, stride and gate beside it). Widths from the
 * disassembly: message 3 stores the character with STRH (0x02099C88) and the
 * gate with STRB (0x02099C94); the arena build reads them with LDRSH
 * (0x02089854) and LDRB (0x02089860). */
/* The statue each player is on (s8[4]) - message 3 turns it into a hunter
 * through the table at 0x020FFA00, which holds kLsw2HunterIds in order. */
constexpr uint32_t kSelIndex = 0x2B10u;

constexpr uint32_t kArenaBuild = 0x020897F8u;
constexpr uint32_t kBuildActor = 0x02091B88u;
constexpr uint32_t kRespawnReturn = 0x020381CCu;   /* the respawn's call */

/* An actor's death, its vtable's slot at +0x40: FUN_02004f7c calls
 * (**(*actor + 0x40))(actor, attacker) when a hit takes the last heart. */
constexpr uint32_t kActorDie = 0x40u;

constexpr uint16_t kKeep = 0xFFFFu;
constexpr int kPayloadBytes = 14;   /* rule, skip, salt u32, chars u16[4] */

struct Round {
    bool active = false;
    uint8_t rule = 0;
    uint32_t salt = 0;
    uint16_t chars[4] = {kKeep, kKeep, kKeep, kKeep};
    int spawn[4] = {0, 0, 0, 0};
};
Round g_round;
bool g_posted = false;     /* host: decided for the select now open */
uint32_t g_rounds = 0;     /* host: rounds decided this session */

uint8_t rd8(uint32_t a) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    return uint8_t((w >> ((a & 3u) * 8u)) & 0xFFu);
}

void wr8(uint32_t a, uint8_t v) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    const uint32_t sh = (a & 3u) * 8u;
    bus_device_write32(0, a & ~3u, (w & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

void wr16(uint32_t a, uint16_t v) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    const uint32_t sh = (a & 2u) * 8u;
    bus_device_write32(0, a & ~3u, (w & ~(0xFFFFu << sh)) | (uint32_t(v) << sh));
}

bool ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

int game_players() {
    uint32_t n = bus_device_read32(0, kLsw2PumpCountWord);
    if (n < 1u || n > uint32_t(kLsw2SelSlots)) n = 1u;
    return int(n);
}

/* The screen slot holding the statue select, and its object, or 0. */
uint32_t select_object() {
    for (int slot = 0; slot < 2; ++slot) {
        if (lsw2_cheats_state_kind(slot) != kLsw2StateSelect) continue;
        const uint32_t p = bus_device_read32(0, kLsw2StateManager + uint32_t(slot) * 4u);
        return ram(p) ? p : 0;
    }
    return 0;
}

/* A roll for a Random rule: a pure function of the rule, the station, that
 * station's spawn count and the round's salt - values every console holds
 * identically - through the offline roster's own pools and hash. */
int roll(uint8_t rule, int station, int spawn, uint32_t salt) {
    Lsw2HostSettings s;
    s.roster = Lsw2Roster(rule);
    return lsw2_roster_character(s, station, int((salt & 0xFFFFu) << 8) + spawn);
}

bool random_rule(uint8_t rule) {
    return Lsw2Roster(rule) == Lsw2Roster::RandomHunter ||
           Lsw2Roster(rule) == Lsw2Roster::RandomAll;
}

/* HOST: the round's characters, posted when the select opens. */
void decide() {
    const Lsw2HostSettings& hs = lsw2_fork_session().settings();
    /* Hunters: the game's own select decides the characters; the effect still
     * goes out, carrying the round's number and nothing else. */
    const bool skip = lsw2_roster_skips_select(hs.roster);
    const uint32_t salt = ++g_rounds;
    uint16_t chars[4] = {kKeep, kKeep, kKeep, kKeep};
    const int n = skip ? game_players() : 0;
    for (int i = 0; i < n; ++i) {
        int c = -1;
        if (hs.roster == Lsw2Roster::All) {
            /* WHAT THE PLAYER PICKED ON THE FREE PLAY GRID (freeplaypick.h),
             * as the host read it in the lobby before the round start. The
             * actor is no longer the answer by the time this runs: the round
             * start's Walkaround reload has rebuilt every party actor as
             * character 0 (measured in the select), so reading it
             * here gave every player character 0. The actor read is kept only
             * for a round the pick did not run in. */
            c = lsw2_freeplaypick_round_pick(i);
            const uint32_t actor = lsw2_cheats_party_actor(i);
            if (c < 0 && ram(actor)) c = rd8(actor + kLsw2ActorCharacter);
        } else if (hs.roster == Lsw2Roster::One) {
            c = hs.character;
        } else {
            c = roll(uint8_t(hs.roster), i, 0, salt);
        }
        if (c >= 0 && c < 0xFF) chars[i] = uint16_t(c);
    }
    uint8_t p[kPayloadBytes];
    p[0] = uint8_t(hs.roster);
    p[1] = skip ? 1u : 0u;   /* every rule but Hunters skips the select */
    for (int b = 0; b < 4; ++b) p[2 + b] = uint8_t(salt >> (8 * b));
    for (int i = 0; i < 4; ++i) {
        p[6 + 2 * i] = uint8_t(chars[i]);
        p[7 + 2 * i] = uint8_t(chars[i] >> 8);
    }
    if (!lsw2_vwh_post_effect(kLsw2EffectRoster, p, kPayloadBytes)) return;
    g_posted = true;
    std::fprintf(stderr, "[fork] roster %d for this round: %d %d %d %d - posted "
                 "for every console to apply at one dataset\n", int(hs.roster),
                 chars[0] == kKeep ? -1 : int(chars[0]),
                 chars[1] == kKeep ? -1 : int(chars[1]),
                 chars[2] == kKeep ? -1 : int(chars[2]),
                 chars[3] == kKeep ? -1 : int(chars[3]));
}

/* TEST ONLY - LSW2_TEST_KILL="station,seconds" on the host (for testing the
 * respawn re-roll). A Random rule re-rolls only at a respawn, and a drive
 * cannot script combat, so once the match (state 42) has run for `seconds` the
 * host posts one kLsw2EffectTestKill for `station` - once per round. Every
 * console then kills that actor at the same dataset. */
void test_kill_tick() {
    static const char* spec = std::getenv("LSW2_TEST_KILL");
    if (!spec) return;
    /* An optional third field repeats the kill every `seconds` instead of once,
     * so one round can show several respawns. */
    static int station = -1, seconds = 20, repeat = 0;
    if (station < 0 && std::sscanf(spec, "%d,%d,%d", &station, &seconds, &repeat) < 1)
        station = 1;
    static int frames = 0;
    static bool posted = false;
    if (lsw2_state_current() != kLsw2PreGameState) {
        frames = 0;
        posted = false;
        return;
    }
    if (repeat && posted && frames >= seconds * 60) {
        frames = 0;
        posted = false;
    }
    if (posted || ++frames < seconds * 60) return;
    const uint8_t p = uint8_t(station);
    if (!lsw2_vwh_post_effect(kLsw2EffectTestKill, &p, 1)) return;
    posted = true;
    std::fprintf(stderr, "[fork] TEST: posted a death for station %d, %d s into "
                 "the match\n", station, seconds);
}

/* TEST ONLY - LSW2_TEST_TELEPORT="station,x,y,z,seconds[;station,x,y,z,seconds
 * ...]" on the host: each entry is one kLsw2EffectTestTeleport, posted once per
 * round when the match has run that many seconds. A drive then reaches a spot
 * in the arena - a build pile, a lift, the flag and then a capture corner -
 * without steering the tank camera there. */
void test_teleport_tick() {
    struct Hop { int station, seconds; float x, y, z; bool posted; };
    static Hop hops[8];
    static int count = -1;
    if (count < 0) {
        count = 0;
        const char* spec = std::getenv("LSW2_TEST_TELEPORT");
        while (spec && *spec && count < 8) {
            Hop h{0, 10, 0.0f, 0.0f, 0.0f, false};
            if (std::sscanf(spec, "%d,%f,%f,%f,%d", &h.station, &h.x, &h.y, &h.z,
                            &h.seconds) >= 4)
                hops[count++] = h;
            spec = std::strchr(spec, ';');
            if (spec) ++spec;
        }
    }
    if (count == 0) return;
    static int frames = 0;
    if (lsw2_state_current() != kLsw2PreGameState) {
        frames = 0;
        for (int i = 0; i < count; ++i) hops[i].posted = false;
        return;
    }
    ++frames;
    for (int i = 0; i < count; ++i) {
        Hop& h = hops[i];
        if (h.posted || frames < h.seconds * 60) continue;
        uint8_t p[13];
        p[0] = uint8_t(h.station);
        const float v[3] = {h.x, h.y, h.z};
        for (int c = 0; c < 3; ++c) {
            const uint32_t f = uint32_t(int32_t(v[c] * 4096.0f));
            for (int b = 0; b < 4; ++b) p[1 + c * 4 + b] = uint8_t(f >> (b * 8));
        }
        if (!lsw2_vwh_post_effect(kLsw2EffectTestTeleport, p, int(sizeof p))) return;
        h.posted = true;
        std::fprintf(stderr, "[fork] TEST: posted a teleport of station %d to "
                     "%.2f,%.2f,%.2f, %d s into the match\n", h.station, h.x, h.y,
                     h.z, h.seconds);
    }
}

/* TEST ONLY - LSW2_TEST_HIT="station,attacker,type,seconds" on the host: one
 * kLsw2EffectTestHit, posted once per round when the match has run that many
 * seconds. See lsw2_sessionroster_test_hit. */
void test_hit_tick() {
    static const char* spec = std::getenv("LSW2_TEST_HIT");
    if (!spec) return;
    static int station = -1, attacker = 1, type = 1, seconds = 12;
    if (station < 0 &&
        std::sscanf(spec, "%d,%d,%d,%d", &station, &attacker, &type, &seconds) < 1)
        station = 0;
    static int frames = 0;
    static bool posted = false;
    if (lsw2_state_current() != kLsw2PreGameState) {
        frames = 0;
        posted = false;
        return;
    }
    if (posted || ++frames < seconds * 60) return;
    const uint8_t p[3] = {uint8_t(station), uint8_t(attacker), uint8_t(type)};
    if (!lsw2_vwh_post_effect(kLsw2EffectTestHit, p, 3)) return;
    posted = true;
    std::fprintf(stderr, "[fork] TEST: posted a hit on station %d from station "
                 "%d (damage type %d), %d s into the match\n", station, attacker,
                 type, seconds);
}

}  // namespace

void lsw2_sessionroster_tick() {
    if (!ui_fork_build() || !lsw2_vwh_lockstep()) return;
    /* BACK IN THE LOBBY, THE ROUND IS OVER - on every console, before the next
     * round's arena build can read a table that no longer applies. */
    if (lsw2_state_current() == kLsw2ArenaLobbyState) {
        g_round.active = false;
        g_posted = false;
        return;
    }
    if (lsw2_vwh_hosting() && !g_posted && select_object() != 0) decide();
    if (lsw2_vwh_hosting()) test_kill_tick();
    if (lsw2_vwh_hosting()) test_teleport_tick();
    if (lsw2_vwh_hosting()) test_hit_tick();
}

void lsw2_sessionroster_apply(const uint8_t* p, int len) {
    if (!p || len < kPayloadBytes) return;
    Round r;
    r.active = true;
    r.rule = p[0];
    r.salt = uint32_t(p[2]) | (uint32_t(p[3]) << 8) | (uint32_t(p[4]) << 16) |
             (uint32_t(p[5]) << 24);
    for (int i = 0; i < 4; ++i)
        r.chars[i] = uint16_t(p[6 + 2 * i] | (p[7 + 2 * i] << 8));
    g_round = r;

    /* THE SELECT, COMPLETED AS PLAYERS WOULD: each present player marked as
     * having chosen - the statue of their character when it is a hunter, the
     * first statue otherwise - and the count the host's update tests. Its next
     * update sends message 3 and the round starts, on every console alike,
     * because this runs at the same dataset everywhere. */
    const uint32_t obj = p[1] ? select_object() : 0;
    if (obj) {
        const int n = game_players();
        for (int i = 0; i < n; ++i) {
            const int idx = r.chars[i] == kKeep ? 0 : lsw2_hunter_index(r.chars[i]);
            wr8(obj + kSelIndex + uint32_t(i), uint8_t(idx < 0 ? 0 : idx));
            wr8(obj + kLsw2SelChosen + uint32_t(i), 1u);
        }
        wr8(obj + kLsw2SelPicked, uint8_t(n));
    }
    std::fprintf(stderr, "[fork] roster %u applied at the dataset%s\n",
                 unsigned(r.rule), obj ? " - the statue select completes itself" : "");
}

uint32_t lsw2_sessionroster_round() { return g_round.active ? g_round.salt : 0u; }

void lsw2_sessionroster_next_match() {
    if (!g_round.active) return;
    g_round.salt += 0x9E3779B1u;   /* every bit moves: music and the rolls */
    std::fprintf(stderr, "[fork] new match on the same map: round %08X\n",
                 unsigned(g_round.salt));
}

void lsw2_sessionroster_arena_build() {
    bool any = false;
    for (int i = 0; i < 4; ++i) any = any || g_round.chars[i] != kKeep;
    if (g_round.active && any) {
        /* THE ROUND'S CHARACTERS, INTO THE RECORDS THE BUILD IS ABOUT TO READ -
         * after message 3 wrote the statues' hunters there, which is what lets
         * One's Gonk Droid or ALL's lobby choice arrive as itself. */
        for (int i = 0; i < 4; ++i) {
            g_round.spawn[i] = 0;
            if (g_round.chars[i] == kKeep) continue;
            const uint32_t rec =
                kLsw2AppearanceRecord + uint32_t(i) * kLsw2AppearanceStride;
            wr16(rec, g_round.chars[i]);
            wr8(rec + kLsw2AppearanceGate, 1u);
        }
        std::fprintf(stderr, "[fork] roster: the arena builds with %d %d %d %d\n",
                     g_round.chars[0] == kKeep ? -1 : int(g_round.chars[0]),
                     g_round.chars[1] == kKeep ? -1 : int(g_round.chars[1]),
                     g_round.chars[2] == kKeep ? -1 : int(g_round.chars[2]),
                     g_round.chars[3] == kKeep ? -1 : int(g_round.chars[3]));
    }
    /* THE GAME'S OWN BUILD, as the override's last act (a lifted body may be
     * suspended part way, so nothing may follow it). */
    g_cpu.R[15] = kArenaBuild;
    lsw2_arm9_FUN_020897f8();
}

void lsw2_sessionroster_spawn() {
    if (g_round.active && random_rule(g_round.rule) &&
        g_cpu.R[14] == kRespawnReturn) {
        const int slot = int(g_cpu.R[1]);
        if (slot >= 0 && slot < 4) {
            const int c = roll(g_round.rule, slot, ++g_round.spawn[slot], g_round.salt);
            if (c >= 0) {
                std::fprintf(stderr, "[fork] roster: station %d respawns as %d "
                             "(rule %u, its spawn %d this round, was %u)\n",
                             slot, c, unsigned(g_round.rule),
                             g_round.spawn[slot], unsigned(g_cpu.R[2]));
                g_cpu.R[2] = uint32_t(c);
            }
        }
    }
    g_cpu.R[15] = kBuildActor;
    lsw2_arm9_FUN_02091b88();
}

void lsw2_sessionroster_test_teleport(const uint8_t* p, int len) {
    if (!p || len < 13) return;
    const uint32_t actor = lsw2_cheats_party_actor(p[0]);
    int32_t v[3];
    for (int i = 0; i < 3; ++i)
        v[i] = int32_t(uint32_t(p[1 + i * 4]) | (uint32_t(p[2 + i * 4]) << 8) |
                       (uint32_t(p[3 + i * 4]) << 16) | (uint32_t(p[4 + i * 4]) << 24));
    if (ram(actor))
        for (int i = 0; i < 3; ++i)
            bus_device_write32(0, actor + kLsw2ActorPos + uint32_t(i) * 4u, uint32_t(v[i]));
    std::fprintf(stderr, "[fork] TEST: station %d's actor %08X placed at "
                 "%.2f,%.2f,%.2f at this dataset\n", int(p[0]), unsigned(actor),
                 v[0] / 4096.0, v[1] / 4096.0, v[2] / 4096.0);
}

void lsw2_sessionroster_test_hit(const uint8_t* p, int len) {
    if (!p || len < 3) return;
    /* The hit handler, the vtable slot combat reaches (hatstay.h). */
    constexpr uint32_t kHitHandler = 0x02004F7Cu;
    const uint32_t actor = lsw2_cheats_party_actor(p[0]);
    const uint32_t attacker = lsw2_cheats_party_actor(p[1]);
    const bool queued = ram(actor) && ram(attacker) &&
        lsw2_gearhop_request_action_at(kHitHandler, actor, attacker, 0u, p[2],
                                       kLsw2HudStep);
    std::fprintf(stderr, "[fork] TEST: station %d's actor %08X is hit by station "
                 "%d's %08X (damage type %d) at this dataset - %s\n", int(p[0]),
                 unsigned(actor), int(p[1]), unsigned(attacker), int(p[2]),
                 queued ? "queued for the HUD step" : "NOT queued");
}

void lsw2_sessionroster_test_kill(int station) {
    const uint32_t actor = lsw2_cheats_party_actor(station);
    const uint32_t vtable = ram(actor) ? bus_device_read32(0, actor) : 0u;
    const uint32_t die = ram(vtable) ? bus_device_read32(0, vtable + kActorDie) : 0u;
    const bool queued = die != 0u &&
        lsw2_gearhop_request_action_at(die, actor, actor, 0u, 0u, kLsw2HudStep);
    std::fprintf(stderr, "[fork] TEST: station %d's actor %08X dies at this "
                 "dataset (vtable +0x40 = %08X) - %s\n", station, unsigned(actor),
                 unsigned(die), queued ? "queued for the HUD step" : "NOT queued");
}
