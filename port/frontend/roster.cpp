/* See roster.h. */

#include "roster.h"

#include "characterpick.h"
#include "lasthunter.h"

bool lsw2_roster_assigns_character(Lsw2Roster roster) {
    switch (roster) {
    case Lsw2Roster::One:
    case Lsw2Roster::RandomHunter:
    case Lsw2Roster::RandomAll:
        return true;
    case Lsw2Roster::Hunters:
    case Lsw2Roster::All:
        break;
    }
    return false;
}

bool lsw2_roster_skips_select(Lsw2Roster roster) {
    return roster != Lsw2Roster::Hunters;
}

uint32_t lsw2_roster_mix(int station, int spawn, uint32_t salt) {
    /* A SMALL INTEGER HASH, not a generator with state.
     *
     * State would have to be kept in step across four machines, which is the
     * problem this design exists to avoid. A pure function of (station, spawn)
     * needs nothing kept in step at all - every machine computes the same
     * answer from values the lockstep already agrees on.
     *
     * This is one round of the finaliser from MurmurHash3, which is public
     * domain and chosen for a property that matters here: adjacent inputs
     * produce unrelated outputs. Station 0 spawn 1 and station 1 spawn 1 must
     * not land on the same character just because the inputs are close, and a
     * naive `(station * 31 + spawn)` does exactly that once it is taken modulo a
     * small roster.
     */
    uint32_t h = uint32_t(station) * 0x9E3779B9u ^
                 uint32_t(spawn) * 0x85EBCA6Bu ^ salt;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

namespace {

/* Different salts so the two modes do not walk the same sequence - a host
 * switching from Random Hunter to Random All mid-lobby would otherwise hand
 * everyone a character at the same index in a different table, which looks like
 * the setting did nothing for whoever happened to land on the same one. */
constexpr uint32_t kHunterSalt = 0x48554E54u;   /* 'HUNT' */
constexpr uint32_t kAllSalt = 0x414C4C43u;      /* 'ALLC' */

/* The offered characters, flattened once. The picker's table is pages of eight
 * with padding; a uniform roll wants a dense list, and building it every spawn
 * would be sixty-five comparisons for a value that never changes. */
struct AllCharacters {
    int ids[kLsw2PickPages * kLsw2PickSlots];
    int count = 0;

    AllCharacters() {
        for (int page = 0; page < kLsw2PickOfferedPages; ++page)
            for (int slot = 0; slot < kLsw2PickSlots; ++slot) {
                const uint8_t id = kLsw2PickTable[page][slot];
                if (id != kLsw2CharacterNone) ids[count++] = int(id);
            }
    }
};

const AllCharacters& all_characters() {
    static const AllCharacters table;
    return table;
}

}  // namespace

int lsw2_roster_character(const Lsw2HostSettings& s, int station, int spawn) {
    switch (s.roster) {
    case Lsw2Roster::One:
        /* A further setting, Character, unhides below the roster row with
         * Greedo as its default - everyone plays as the host's one pick, so
         * this ignores both the station and the spawn. */
        return s.character;

    case Lsw2Roster::RandomHunter: {
        const uint32_t h = lsw2_roster_mix(station, spawn, kHunterSalt);
        return kLsw2HunterIds[h % uint32_t(kLsw2HunterCount)];
    }

    case Lsw2Roster::RandomAll: {
        /* Every character except the three custom character slots - the
         * rule's own exclusion, and the same page the picker leaves out. The flattened
         * table already omits it, so there is nothing to subtract here. */
        const AllCharacters& all = all_characters();
        if (all.count <= 0) return -1;
        const uint32_t h = lsw2_roster_mix(station, spawn, kAllSalt);
        return all.ids[h % uint32_t(all.count)];
    }

    case Lsw2Roster::Hunters:
    case Lsw2Roster::All:
        /* The player chooses. */
        break;
    }
    return -1;
}
