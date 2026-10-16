/* See dsname.h. Pure - no guest, no framework - so test_dsname.cpp can link it
 * alone, the same split hudportrait_policy.cpp and slotcolour_policy.cpp make. */

#include "dsname.h"

int lsw2_dsname_encode(const char* name, uint16_t out[kLsw2DsNicknameChars]) {
    for (int i = 0; i < kLsw2DsNicknameChars; ++i) out[i] = 0;

    int n = 0;
    if (name) {
        for (const char* p = name; *p && n < kLsw2DsNicknameChars; ++p) {
            /* PRINTABLE ASCII ONLY - the same allow-list lsw2_sanitise_name
             * applies on the way in. A byte outside it is skipped rather than
             * substituted, because a stand-in glyph the player never typed is
             * a name they did not choose. */
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20u || c > 0x7Eu) continue;
            out[n++] = static_cast<uint16_t>(c);
        }
    }

    /* Nothing survived: the sanitiser's own fallback, so no list ever shows a
     * blank row. Six characters, well under the cap. */
    if (n == 0) {
        static const char kFallback[] = "Player";
        for (const char* p = kFallback; *p; ++p)
            out[n++] = static_cast<uint16_t>(static_cast<unsigned char>(*p));
    }
    return n;
}
