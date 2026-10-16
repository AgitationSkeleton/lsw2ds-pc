#include "cheats.h"
#include "lasthunter.h"

#include <cstdint>

/* The hunter table, read out of the ROM at 0x020FFA00 and reproduced in its own
 * order. Greedo first, which is why he is the default hunter. */
const int kLsw2HunterIds[kLsw2HunterCount] = {
    20,  /* Greedo               */
     0,  /* 4-LOM                */
     7,  /* Boba Fett            */
     8,  /* Bossk                */
    13,  /* Dengar               */
    27,  /* IG-88                */
    34,  /* Lando Jabba's Palace */
    36,  /* Leia Bounty Hunter   */
    71,  /* Zuckuss              */
    29,  /* Jango Fett           */
};

/* THE SELECT SCREEN'S TWO HUNTER PAGES, read out of 0x020EDDB2 at pages 6 and
 * 7. Eight and two, and the padding on page 7's tail is the game's
 * own 0x64 "not available" sentinel - the same one the roster filter at
 * 0x0206F620 substitutes.
 *
 * Reproduced here rather than read from the guest for the same reason the
 * roster is: it is a ROM constant, and a lookup that needed a live guest would
 * answer "nowhere" whenever the game was between levels. */
/* cheats.h's kLsw2CharacterNone - 100, what the game writes where a
 * character id would go when there is no character. */

const uint8_t kHunterPages[2][kLsw2SelectPageSlots] = {
    /* page 6 */ { 20,  7, 29, 27,  0, 13,  8, 34 },
    /* page 7 */ { 36, 71, kLsw2CharacterNone, kLsw2CharacterNone,
                   kLsw2CharacterNone, kLsw2CharacterNone, kLsw2CharacterNone,
                   kLsw2CharacterNone },
};

namespace {

int g_last = kLsw2DefaultHunter;
bool g_dirty = false;

}  // namespace

bool lsw2_hunter_page_slot(int character_id, int* page, int* slot) {
    for (int p = 0; p < 2; ++p) {
        for (int s = 0; s < kLsw2SelectPageSlots; ++s) {
            if (kHunterPages[p][s] != character_id) continue;
            if (page) *page = kLsw2HunterPageFirst + p;
            if (slot) *slot = s;
            return true;
        }
    }
    return false;
}

int lsw2_select_character_at(int page, int slot) {
    if (page < kLsw2HunterPageFirst || page > kLsw2HunterPageLast) return -1;
    if (slot < 0 || slot >= kLsw2SelectPageSlots) return -1;
    const uint8_t id = kHunterPages[page - kLsw2HunterPageFirst][slot];
    return id == kLsw2CharacterNone ? -1 : int(id);
}

bool lsw2_is_bounty_hunter(int character_id) {
    return lsw2_hunter_index(character_id) >= 0;
}

int lsw2_hunter_index(int character_id) {
    for (int i = 0; i < kLsw2HunterCount; ++i)
        if (kLsw2HunterIds[i] == character_id) return i;
    return -1;
}

int lsw2_last_hunter() { return g_last; }

bool lsw2_set_last_hunter(int character_id) {
    if (!lsw2_is_bounty_hunter(character_id)) return false;
    if (character_id == g_last) return false;
    g_last = character_id;
    g_dirty = true;
    return true;
}

void lsw2_last_hunter_load(int character_id) {
    /* A FILE IS NOT A TRUSTED SOURCE, even the player's own. settings.json is
     * hand-editable and an id that is not in the table would travel straight
     * into the roster-page maths as a -1 index. Fall back rather than carry it. */
    g_last = lsw2_is_bounty_hunter(character_id) ? character_id
                                                 : kLsw2DefaultHunter;
    g_dirty = false;
}

bool lsw2_last_hunter_dirty() { return g_dirty; }
void lsw2_last_hunter_mark_clean() { g_dirty = false; }
