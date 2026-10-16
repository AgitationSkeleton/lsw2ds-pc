/* The view mask's arithmetic, pinned against the ROM it was transcribed from.
 *
 * These checks exist because the port spent three attempts writing player
 * counts the game recomputes, and because `enhanced.h` claimed the resolver's
 * argument was an array subscript. The subscript reading passes every test you
 * would write with a full session and fails the moment a mask has a gap - so
 * the gap cases below are the point of the file, not an edge. */

#include "viewmask.h"

#include <cstdio>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok: %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL: %s\n", what);
    }
}

int main() {
    /* --- index 0 never consults the mask -----------------------------------
     * `020AF728: CMP r1,#0 / BNE` falls through, so record 0 comes back even
     * from an empty session. */
    check(lsw2_viewmask_resolve(0x0, 0) == 0, "view 0 is record 0 even at mask 0");
    check(lsw2_viewmask_resolve(0xF, 0) == 0, "view 0 is record 0 at mask 0xF");

    /* --- negative is the authority's to answer, not ours ------------------- */
    check(lsw2_viewmask_resolve(0xF, -1) == kLsw2ViewNotFound,
          "a negative index is refused rather than guessed");

    /* --- the contiguous case, where a subscript reading LOOKS right -------- */
    check(lsw2_viewmask_resolve(0xF, 1) == 1, "mask 0xF: view 1 is record 1");
    check(lsw2_viewmask_resolve(0xF, 2) == 2, "mask 0xF: view 2 is record 2");
    check(lsw2_viewmask_resolve(0xF, 3) == 3, "mask 0xF: view 3 is record 3");

    /* --- THE GAP, where it does not -----------------------------------------
     * Mask 0b1101: players in records 0, 2 and 3. The game's view 1 is the
     * SECOND player present, which is record 2. Subscripting would hand back
     * record 1, which holds nobody. */
    check(lsw2_viewmask_resolve(0xD, 1) == 2,
          "mask 0xD: view 1 is record 2, not record 1");
    check(lsw2_viewmask_resolve(0xD, 2) == 3, "mask 0xD: view 2 is record 3");
    check(lsw2_viewmask_resolve(0xD, 3) == kLsw2ViewNotFound,
          "mask 0xD: view 3 does not exist");

    /* Two players in the high records: view 1 is record 3. */
    check(lsw2_viewmask_resolve(0xC, 1) == 3, "mask 0xC: view 1 is record 3");

    /* --- the popcount the count derivation performs ------------------------ */
    check(lsw2_viewmask_count(0xF, 4) == 4, "0xF over 4 slots counts 4");
    check(lsw2_viewmask_count(0xD, 4) == 3, "0xD over 4 slots counts 3");
    check(lsw2_viewmask_count(0x1, 4) == 1, "a lone bit counts 1");
    check(lsw2_viewmask_count(0x0, 4) == 0, "an empty mask counts 0");

    /* THE BOUND IS THE SLOT COUNT, not the mask's width. `[0x020FEB88+4]`
     * bounds the loop, so bits above it do not count - which is what makes the
     * word worth reading rather than assuming 4. */
    check(lsw2_viewmask_count(0xF, 2) == 2, "0xF over 2 slots counts only 2");
    check(lsw2_viewmask_count(0xF, 0) == 0, "no slots counts nothing");

    /* --- record addresses, including the fallback that is not element 4 ---- */
    const uint32_t mgr = 0x02200000u;
    check(lsw2_viewmask_record(mgr, 0) == mgr + 0x240u, "record 0 sits at +0x240");
    check(lsw2_viewmask_record(mgr, 1) == mgr + 0x2B0u, "record 1 is one stride on");
    check(lsw2_viewmask_record(mgr, 3) == mgr + 0x390u, "record 3 is three strides on");

    /* `0x240 + 4*0x70` is `0x400`. The game uses `0x440`. */
    check(lsw2_viewmask_record(mgr, kLsw2ViewNotFound) == mgr + 0x440u,
          "the fallback is +0x440, not the stride's +0x400");
    check(lsw2_viewmask_record(mgr, 4) == mgr + 0x440u,
          "an out-of-range record falls back too");

    /* --- the mask a session implies ---------------------------------------
     * Composed from the stations the transport handed out, never from the
     * count, because those disagree the moment a station is not contiguous. */
    {
        const int none[3] = {-1, -1, -1};
        check(lsw2_viewmask_compose(0, none, 0) == 0x1u,
              "alone at station 0 is mask 0x1");
        check(lsw2_viewmask_compose(2, none, 0) == 0x4u,
              "alone at station 2 is mask 0x4, not 0x1");

        const int two[3] = {1, -1, -1};
        check(lsw2_viewmask_compose(0, two, 1) == 0x3u,
              "stations 0 and 1 are mask 0x3");

        /* THE CASE A COUNT CANNOT EXPRESS. Two players at 0 and 2: a
         * count-derived mask would be 0x3, seating station 1 (nobody) and
         * leaving station 2 unseated. */
        const int gapped[3] = {2, -1, -1};
        check(lsw2_viewmask_compose(0, gapped, 1) == 0x5u,
              "stations 0 and 2 are mask 0x5, not the count's 0x3");

        /* A row whose station has not arrived yet is not seated. */
        const int pending[3] = {1, -1, 3};
        check(lsw2_viewmask_compose(0, pending, 3) == 0xBu,
              "a row with no station reported is skipped");

        /* Out of range is refused rather than shifted off the end. */
        const int wild[3] = {9, -4, 1};
        check(lsw2_viewmask_compose(0, wild, 3) == 0x3u,
              "stations outside 0..3 are ignored");
        check(lsw2_viewmask_compose(-1, two, 1) == 0x2u,
              "an unknown local station seats only the peers");
        check(lsw2_viewmask_compose(0, nullptr, 3) == 0x1u,
              "a null roster is not walked");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
