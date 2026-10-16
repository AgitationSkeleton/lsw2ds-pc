/* See characterpick.h. */

#include "characterpick.h"

#include <cstdio>

const uint8_t kLsw2PickTable[kLsw2PickPages][kLsw2PickSlots] = {
    /* page  0 */ {  35,  37,  39,  40,  38,  26,  21,  24 },
    /* page  1 */ {  25,  10,  41,  47,  33,  56,  58,   2 },
    /* page  2 */ {  64,  60,  63,  61,  28,  62,  12,  65 },
    /* page  3 */ {  51,  44,  46,  43,  42,  14,  11,   1 },
    /* page  4 */ {  48,  52,  69,  70, 100, 100, 100, 100 },
    /* page  5 */ {   9,  32,  53,  54,  50,  18, 100, 100 },
    /* page  6 */ {  20,   7,  29,  27,   0,  13,   8,  34 },
    /* page  7 */ {  36,  71, 100, 100, 100, 100, 100, 100 },
    /* page  8 */ {  31,  16,  59,  67, 100, 100, 100, 100 },
    /* page  9 */ {  15,  66,  17,  23,   3,   4,   5,   6 },
    /* page 10 */ {  68, 100, 100, 100, 100, 100, 100, 100 },
    /* page 11 */ {  73,  74,  75, 100, 100, 100, 100, 100 },
};

namespace {

Lsw2CharacterNameFn g_name_fn = nullptr;

}  // namespace

void lsw2_set_character_name_source(Lsw2CharacterNameFn fn) { g_name_fn = fn; }

const char* lsw2_character_name(int character_id, char* buf, size_t n) {
    if (!buf || n == 0) return "";
    if (g_name_fn) return g_name_fn(character_id, buf, n);
    /* NO SOURCE INSTALLED. A blank row would read as a bug in the picker rather
     * than as "nobody has wired the decoder yet". */
    std::snprintf(buf, n, "Character %d", character_id);
    return buf;
}
