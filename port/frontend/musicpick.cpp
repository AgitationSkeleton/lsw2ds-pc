/* See musicpick.h. */

#include "musicpick.h"

#include "cheats.h"          /* lsw2_cheats_current_level, _state_kind */
#include "forksession.h"
#include "gearhop.h"         /* the live lobby switch, on the guest's thread */
#include "hostsettings.h"
#include "instancemute.h"   /* kLsw2MusicVolume */
#include "hudportrait.h"     /* kLsw2HudStep - the level state's update */
#include "levels.h"
#include "sessionroster.h"   /* lsw2_sessionroster_round */
#include "statelog.h"        /* kLsw2WorldSlot, kLsw2LevelStateKind */
#include "ui.h"

#include "state.h"

#include <chrono>
#include <cstdio>

namespace {

/* THE TWO ROUND DESTINATIONS, and only those - the lobby (level 9) has its own
 * record at a different address, so it keeps the arena's track whatever is
 * written here - the random pick applies to arena rounds, not the lobby. */
constexpr int kArenaMaps[] = {kLsw2ArenaJabbaLevel, kLsw2ArenaMosEisleyLevel};

/* Byte +1 of a level record is its music slot - the LDRB at 0x02093F74. */
constexpr uint32_t kRecordMusicSlot = 1;

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

/* The slot this module last left in the arena records; -1 until it has written
 * one. Starting at -1 rather than at the arena's own slot is what lets a run
 * that never selects RANDOM leave the records exactly as the ROM loaded them. */
int g_written = -1;

uint32_t record_of(int level) {
    const uint32_t rec =
        bus_device_read32(0, kLsw2LevelPermAddr + uint32_t(level) * 4u);
    return (rec >= kRamLow && rec < kRamHigh) ? rec : 0u;
}

void write_byte(uint32_t a, uint8_t v) {
    const uint32_t base = a & ~3u;
    const uint32_t sh = (a & 3u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

}  // namespace

int lsw2_music_pick(uint16_t seed) {
    /* One of the levels' battle tracks, flat - the arena's own among them
     * (musicpick.h). */
    return kLsw2MusicBattleSlots[seed % unsigned(kLsw2MusicBattleCount)];
}

namespace {

uint8_t read_byte(uint32_t a) {
    return uint8_t(bus_device_read32(0, a & ~3u) >> ((a & 3u) * 8u));
}

uint32_t set_entry(int set) {
    return kLsw2MusicSetTable + uint32_t(set) * kLsw2MusicSetStride;
}

/* DOES THE RUNNING GAME'S TABLE STILL SAY WHAT kLsw2MusicBattleSlots WAS READ
 * FROM? Every pooled slot must be some level set's battle slot distinct from
 * its calm one, and the spare set must be named by no level record. Asked once;
 * a "no" leaves RANDOM playing the arena's own music, and says why. */
bool table_agrees() {
    static int verdict = -1;
    if (verdict >= 0) return verdict == 1;
    bool ok = true;
    for (int i = 0; i < kLsw2MusicBattleCount && ok; ++i) {
        bool found = false;
        for (int s = 0; s < kLsw2MusicSlotCount && !found; ++s) {
            const uint8_t calm = read_byte(set_entry(s) + kLsw2MusicSetCalm);
            const uint8_t battle = read_byte(set_entry(s) + kLsw2MusicSetBattle);
            found = battle != calm && battle == kLsw2MusicBattleSlots[i];
        }
        ok = found;
    }
    for (int level = 0; level < 64 && ok; ++level) {
        const uint32_t rec = record_of(level);
        if (rec && (read_byte(rec + kRecordMusicSlot) == kLsw2MusicSpareSet ||
                    read_byte(rec + kRecordMusicSlot) == kLsw2MusicLobbySpareSet))
            ok = false;
    }
    verdict = ok ? 1 : 0;
    if (!ok)
        std::fprintf(stderr, "[fork] Music: the music-set table does not match "
                             "the battle tracks this build was made from - "
                             "RANDOM keeps the arena's own music\n");
    return ok;
}

/* The spare set as the ROM loaded it, so DEFAULT can put it back. */
uint8_t g_spare_saved[2] = {0, 0};
bool g_spare_touched = false;

void set_spare(uint8_t calm, uint8_t battle) {
    const uint32_t e = set_entry(kLsw2MusicSpareSet);
    if (!g_spare_touched) {
        g_spare_saved[0] = read_byte(e + kLsw2MusicSetCalm);
        g_spare_saved[1] = read_byte(e + kLsw2MusicSetBattle);
        g_spare_touched = true;
    }
    write_byte(e + kLsw2MusicSetCalm, calm);
    write_byte(e + kLsw2MusicSetBattle, battle);
}

}  // namespace

void lsw2_music_tick() {
    if (!ui_fork_build()) return;

    /* THE TRACK THE NEXT ARENA LOAD SHOULD PLAY. Anything short of a live
     * session, RANDOM, and an agreed round seed means the arena's own - which
     * is also how a host switching back to DEFAULT between rounds is undone. */
    const Lsw2ForkSession& session = lsw2_fork_session();
    const uint32_t seed32 = session.match_seed();
    /* EVERY ROUND ITS OWN TRACK. The match seed is chosen once per lobby, so on
     * its own it picked the same "random" track for every round of a session;
     * the pick is meant to happen upon round start, so it draws per round. The host numbers
     * each round in the round effect it posts when the statue select opens
     * (sessionroster.h), which every console applies at one dataset, before the
     * arena's load reads the byte below. (A first attempt seeded this from
     * 0x02101AD4, documented as a round counter; it is a sequence counter and
     * the pick changed dozens of times a round - statelog.h.) */
    const uint32_t round = lsw2_sessionroster_round();
    /* `want` is the SET the arena records name; `battle` the track the spare
     * set carries for a random round (-1: none). */
    int want = kLsw2MusicArenaSlot;
    int battle = -1;
    if (session.state() != Lsw2SessionState::Offline &&
        session.settings().music == Lsw2HostMusic::Random && seed32 != 0 &&
        table_agrees()) {
        battle = lsw2_music_pick(uint16_t(seed32 ^ (round * 0x9E3779B1u) ^
                                          (round * 0x9E3779B1u >> 16)));
        want = kLsw2MusicSpareSet;
    }

    /* g_written names the combination: the arena's set, or the spare set with
     * this battle track in it. */
    const int key = battle < 0 ? want : 1000 + battle;
    if (key == g_written) return;
    if (g_written < 0 && key == kLsw2MusicArenaSlot) return;   /* untouched */

    /* THE SPARE SET FIRST, so no arena load can find the records pointing at
     * it before it holds the pick; and put back as the ROM had it when a host
     * returns to DEFAULT. */
    if (battle >= 0)
        set_spare(uint8_t(battle), uint8_t(battle));
    else if (g_spare_touched)
        set_spare(g_spare_saved[0], g_spare_saved[1]);

    /* WRITTEN WHERE THE GAME LOOKS, BEFORE IT LOOKS - and no lifted code runs.
     *
     * The level constructor 0x02090F3C calls 0x0208FD24, which reads this byte
     * through 0x02093F6C and hands it to the music record (0x020B3ABC, then
     * variation 0 via 0x020B3718). That is the ONLY reader of the byte, so it
     * is consulted exactly once per arena load, and the seed is agreed at
     * round start - during the statue select, before the load begins. The
     * game's own level start therefore plays the picked track on every
     * machine, with nothing called into the guest.
     *
     * WHAT THIS REPLACED, measured over the host-level WH:
     * the previous version guest-called the selector 0x020B90E4 on "arrived in
     * an arena map", which is true from the load's FIRST frame - state -1, no
     * state object yet. Both consoles' calls ended parked in 0x020CEA74 (disable
     * interrupts, halt, loop: the SDK's terminate), the unwound call left its
     * return marker on the runtime's call stack, and neither console finished
     * loading the arena: the dataset counter stopped on the next line and never
     * moved again. It had never been seen before only because no earlier
     * two-console drive had a live fork session with RANDOM selected. */
    bool wrote = false;
    for (int level : kArenaMaps) {
        const uint32_t rec = record_of(level);
        if (!rec) continue;
        write_byte(rec + kRecordMusicSlot, uint8_t(want));
        wrote = true;
    }
    if (!wrote) return;     /* table not there yet - try again next tick */
    g_written = key;

    if (battle < 0)
        std::fprintf(stderr, "[fork] Music: arena maps back to their own "
                             "set %d\n", want);
    else
        std::fprintf(stderr,
                     "[fork] Music: RANDOM - round seed %u picked battle track "
                     "(slot) %d; the arena maps play it through spare set %d "
                     "(their own set is %d)\n",
                     unsigned(seed32), battle, kLsw2MusicSpareSet,
                     kLsw2MusicArenaSlot);
}

namespace {

/* THE GAME'S MUSIC RECORD and its start: the level path stores the set in
 * byte 0 (0x020B3ABC is `strb r1,[r0]`) and then calls 0x020B3718 on this
 * record, which applies variation 0 - 0x0208FD34..0x0208FD40. */
constexpr uint32_t kMusicRecord = 0x027E0BD0u;
constexpr uint32_t kMusicStart = 0x020B3718u;

/* Every distinct slot the levels' music sets use, calm or battle - the lobby's
 * RANDOM pool (a lobby is no fight, so the calm tracks belong in it too).
 * Ascending, read off the running game's table. */
int level_tracks(int* out, int cap) {
    bool seen[256] = {false};
    for (int s = 0; s < kLsw2MusicSlotCount; ++s) {
        seen[read_byte(set_entry(s) + kLsw2MusicSetCalm)] = true;
        seen[read_byte(set_entry(s) + kLsw2MusicSetBattle)] = true;
    }
    int n = 0;
    for (int slot = 0; slot < 29 && n < cap; ++slot)   /* 29: the slot table's rows */
        if (seen[slot]) out[n++] = slot;
    return n;
}

/* The lobby record's own set (2), read before this ever writes it. */
int g_lobby_own = -1;
int g_lobby_written = -1;
int g_lobby_track = -1;            /* RANDOM's current draw, -1 before one */
bool g_was_in_lobby = false;
uint32_t g_lobby_visits = 0;
uint32_t g_boot_salt = 0;

}  // namespace

void lsw2_lobby_music_tick() {
    if (!ui_fork_build()) return;

    /* THE LOBBY'S MUSIC AFTER A JOIN. The join prompt pauses the music
     * (0x020B918C sets the context's flag 0x01) and the wireless screens that
     * follow stop the paused sequence outright - measured: the SPU's output is
     * exactly zero from there on - without the sequence-end callback 0x020B9434
     * running, which is what would set flag 0x02, "the recorded slot is stale".
     * The cartridge never notices, because a stock join always loads a new level
     * with a different track. The fork returns everyone to the SAME lobby, whose
     * reload requests the slot the context still records as playing, and the
     * request function (0x020B90E4) treats that as already playing: silence for
     * the rest of the lobby. Entering the match screen (state 40), which host
     * and joiner both pass on the way back to the lobby, does here what the
     * callback would have: marks the slot stale, so the lobby's own request
     * restarts it. */
    {
        static int last_state = -2;
        const int now = lsw2_state_current();
        if (now == 40 && last_state != 40) {
            const uint32_t word = kLsw2MusicContext + 8u;   /* +8..+0xB, flags at +0xA */
            const uint32_t w = bus_device_read32(0, word);
            bus_device_write32(0, word, w | (0x02u << 16));
            std::fprintf(stderr, "[fork] music: the wireless screens stopped the "
                                 "lobby track - marked it stale (flag 0x02) so the "
                                 "lobby's reload restarts it\n");
        }
        last_state = now;
    }

    const uint32_t rec = record_of(kLsw2ArenaLobbyLevel);
    const uint32_t cantina = record_of(kLsw2CantinaLevel);
    if (!rec || !cantina) return;
    const bool in_lobby = lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel;
    if (g_lobby_own < 0) {
        /* NOT BEFORE THE LOBBY IS LIVE. The records are static data, and on the
         * first ticks the game has not finished unpacking its image: measured,
         * a first-tick read gave the lobby's set as 0 where the ROM says 2. */
        if (!in_lobby) return;
        g_lobby_own = read_byte(rec + kRecordMusicSlot);
        g_boot_salt = uint32_t(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
    if (g_was_in_lobby && !in_lobby) ++g_lobby_visits;
    g_was_in_lobby = in_lobby;

    const Lsw2ForkSession& session = lsw2_fork_session();
    const Lsw2LobbyMusic mode = session.settings().lobby_music;

    int set = g_lobby_own;
    int track = -1;
    if (mode == Lsw2LobbyMusic::Cantina) {
        set = read_byte(cantina + kRecordMusicSlot);
    } else if (mode == Lsw2LobbyMusic::Random && table_agrees()) {
        /* A NEW DRAW EACH TIME THE LOBBY IS LEFT, never while standing in it:
         * a session's round number (every console counts it alike, so all hear
         * one track) or, offline, this console's own visits. */
        if (!in_lobby || g_lobby_track < 0) {
            int pool[32];
            const int n = level_tracks(pool, 32);
            const uint32_t seed = session.state() != Lsw2SessionState::Offline
                ? session.match_seed() ^ (lsw2_sessionroster_round() * 0x9E3779B1u)
                : g_boot_salt ^ (g_lobby_visits * 0x9E3779B1u);
            if (n > 0) g_lobby_track = pool[(seed ^ (seed >> 16)) % uint32_t(n)];
        }
        if (g_lobby_track >= 0) {
            track = g_lobby_track;
            set = kLsw2MusicLobbySpareSet;
        }
    }

    const int key = track < 0 ? set : 1000 + track;
    if (key == g_lobby_written) return;
    if (g_lobby_written < 0 && key == g_lobby_own) {   /* untouched */
        g_lobby_written = key;
        return;
    }

    /* The spare set before the record that names it. */
    if (track >= 0) {
        const uint32_t e = set_entry(kLsw2MusicLobbySpareSet);
        write_byte(e + kLsw2MusicSetCalm, uint8_t(track));
        write_byte(e + kLsw2MusicSetBattle, uint8_t(track));
    }
    write_byte(rec + kRecordMusicSlot, uint8_t(set));
    g_lobby_written = key;
    std::fprintf(stderr, "[fork] Lobby Music: %s - the lobby's record names set "
                         "%d%s (its own is %d)\n",
                 mode == Lsw2LobbyMusic::Cantina ? "CANTINA"
                 : mode == Lsw2LobbyMusic::Random ? "RANDOM" : "HUNTER",
                 set, track >= 0 ? " carrying a random track" : "", g_lobby_own);

    /* STANDING IN THE LOBBY: switch now, the way the level path does - the set
     * into the music record's byte 0, then the game's own music start, run on
     * the guest's thread from the level's update. Otherwise the next lobby
     * load reads the record. */
    if (in_lobby && lsw2_cheats_state_kind(int(kLsw2WorldSlot)) ==
                        int(kLsw2LevelStateKind)) {
        write_byte(kMusicRecord, uint8_t(set));
        lsw2_gearhop_request_action_at(kMusicStart, kMusicRecord, 0u, 0u, 0u,
                                       kLsw2HudStep);
        std::fprintf(stderr, "[fork] Lobby Music: switched in place (set %d, "
                             "track %d)\n", set, track);
    }
}

void lsw2_music_watch_tick() {
    const uint32_t ctx = bus_device_read32(0, kLsw2MusicContext + 8u);   /* +8..+0xB */
    const int slot = int((ctx >> 8) & 0xFFu);
    const int flags = int((ctx >> 16) & 0xFFu);
    const uint32_t vw = bus_device_read32(0, kLsw2MusicVolume & ~3u);
    const int volume = int((vw >> ((kLsw2MusicVolume & 3u) * 8u)) & 0xFFu);
    static int last_slot = -2, last_flags = -2, last_volume = -2;
    if (slot == last_slot && flags == last_flags && volume == last_volume) return;
    last_slot = slot;
    last_flags = flags;
    last_volume = volume;
    std::fprintf(stderr, "[music] playing slot %d, flags %02X%s, volume %d "
                         "(state %d, level %d)\n",
                 slot, unsigned(flags), (flags & 0x20) ? " (suppressed)" : "",
                 volume, lsw2_state_current(), lsw2_cheats_current_level());
}
