/* The host's customization policy, which the design makes the host's call.
 *
 *   g++ -std=c++20 port/frontend/test_customization.cpp \
 *       port/frontend/customization_policy.cpp -o test_customization
 *
 * Only the FILTER is tested here. The rest of customization.cpp reads and writes
 * guest memory through the framework's bus, which a unit test has no business
 * linking - so the policy lives in its own translation unit and this covers the
 * part that is pure decision-making.
 */

#include "customization.h"

#include <cstdio>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/* A plausible set: a hat, a non-hat, an empty slot, another non-hat, another
 * hat. Part 228 is TopHat and 214 PirateHat; 100 and 55 are not headwear. */
/* SIX VALUES, NOT FIVE. These arrays are sized by kLsw2CustomSlots, so when
 * that constant went from 5 to 6 they kept compiling and silently zero-filled
 * the new slot - a test that stops exercising the thing it was widened for.
 * The sixth value is 0x13, inside the 0x11..0x19 range its applier switches
 * over (customization.h), so it is a real value for that slot rather than a
 * part id borrowed from the other five. */
const uint16_t kSaved[kLsw2CustomSlots] =
    {228, 100, kLsw2CustomNone, 55, 214, 0x13};

}  // namespace

int main() {
    /* --- HEADWEAR IS A RANGE, NOT EIGHT IDS ---------------------------------
     * This test file previously agreed with the code that eight ids were the
     * whole set. Slot 3 holds fifty-five, and the host's "Hats" policy stripped
     * the other forty-seven as though they were not hats. */
    check(lsw2_customization_is_headwear(177), "177 is the first headwear part");
    check(lsw2_customization_is_headwear(231), "231 is the last");
    check(!lsw2_customization_is_headwear(176), "176 is legs, not headwear");
    check(!lsw2_customization_is_headwear(232), "232 is an accessory");
    check(lsw2_customization_is_headwear(200) &&
              lsw2_customization_is_headwear(220),
          "ids the old eight-entry list missed are headwear too");

    /* THE EIGHT THAT USED TO BE THE WHOLE TEST are all inside the range - so
     * the old list was a subset, not a different answer. Kept as a check
     * because it is the thing that would break if the range were wrong. */
    const uint16_t extras_hats[] = {178, 187, 188, 202, 212, 214, 228, 229};
    for (uint16_t h : extras_hats)
        check(lsw2_customization_is_headwear(h),
              "an Extras hat is inside the headwear range");


    std::printf("customization policy\n");

    /* --- the headwear set ------------------------------------------------ */
    check(lsw2_customization_is_headwear(178), "BaseballCap is headwear");
    check(lsw2_customization_is_headwear(228), "TopHat is headwear");
    check(lsw2_customization_is_headwear(212), "Moustache is, because the game groups it there");
    check(!lsw2_customization_is_headwear(100), "an arbitrary part is not");
    check(!lsw2_customization_is_headwear(kLsw2CustomNone),
          "and neither is the empty sentinel");

    /* --- ALL keeps everything -------------------------------------------- */
    uint16_t out[kLsw2CustomSlots];
    lsw2_customization_filter(kSaved, out, Lsw2CustomPolicy::All);
    bool same = true;
    for (int i = 0; i < kLsw2CustomSlots; ++i) same = same && out[i] == kSaved[i];
    check(same, "ALL passes every slot through untouched");

    /* --- OFF is stock, and stock is the GAME's sentinel ------------------ */
    lsw2_customization_filter(kSaved, out, Lsw2CustomPolicy::Off);
    bool all_none = true;
    for (int i = 0; i < kLsw2CustomSlots; ++i)
        all_none = all_none && out[i] == kLsw2CustomNone;
    check(all_none, "OFF clears every slot to the game's own empty (0xFA)");

    /* Writing zero instead would be part id 0, not "no part" - the sentinel
     * was read off the two apply loops, which skip a slot holding 0xFA. */
    check(kLsw2CustomNone == 0x00FA, "and that sentinel is 0xFA, not zero");

    /* --- HATS keeps headwear and only headwear --------------------------- */
    lsw2_customization_filter(kSaved, out, Lsw2CustomPolicy::Hats);
    check(out[0] == 228, "HATS keeps a hat");
    check(out[1] == kLsw2CustomNone, "and clears a non-hat");
    check(out[2] == kLsw2CustomNone, "an already-empty slot stays empty");
    check(out[3] == kLsw2CustomNone, "another non-hat goes");
    check(out[4] == 214, "and a hat in the last slot survives");

    /* THE POINT OF FILTERING BY PART ID. The applier's call sites established
     * the five slots but never which one is the hat - the applier derives the slot from the
     * part - so a slot-indexed rule would have been invented. This asserts the
     * property that makes the id-based rule right: a hat is kept wherever it
     * sits, and moving it does not change the outcome. */
    const uint16_t moved[kLsw2CustomSlots] =
        {100, 228, 55, kLsw2CustomNone, 187, 0x15};
    lsw2_customization_filter(moved, out, Lsw2CustomPolicy::Hats);
    check(out[0] == kLsw2CustomNone && out[1] == 228 && out[4] == 187,
          "a hat is kept by what it IS, not by which slot it is in");

    /* --- aliasing, because the caller does it ---------------------------- */
    uint16_t inplace[kLsw2CustomSlots] = {228, 100, 55, 214, 187, 0x13};
    lsw2_customization_filter(inplace, inplace, Lsw2CustomPolicy::Hats);
    check(inplace[0] == 228 && inplace[1] == kLsw2CustomNone &&
              inplace[2] == kLsw2CustomNone && inplace[3] == 214 &&
              inplace[4] == 187,
          "filtering in place gives the same answer");

    /* --- THE WEAPON IS A NARROW RANGE, and out-of-range is not a weapon ---
     * The action-row setter 0x02004404 only maps rows 0x11..0x19; a
     * value outside that is written RAW to the action row and stamps a row with
     * no asset over the character's valid default weapon, leaving it UNARMED -
     * the bug seen in play, from a leftover 40. So anything outside the range is
     * treated as "no custom weapon". */
    check(lsw2_is_custom_weapon(0x11) && lsw2_is_custom_weapon(0x19),
          "0x11 and 0x19 are the ends of the customize-weapon range");
    check(lsw2_is_custom_weapon(0x15), "and a value between them is a weapon");
    check(!lsw2_is_custom_weapon(0x10) && !lsw2_is_custom_weapon(0x1A),
          "one below and one above the range are not weapons");
    check(!lsw2_is_custom_weapon(40),
          "40 - the leftover that caused the unarmed bug - is not a weapon");
    check(!lsw2_is_custom_weapon(kLsw2CustomNone) && !lsw2_is_custom_weapon(0),
          "none and zero are not weapons");

    /* --- NO HOST POLICY LEAVES A HUNTER UNARMED --------------------------
     * Under OFF and HATS the weapon slot is filtered to none; the applier then
     * SKIPS a non-weapon and the game's own default weapon stands, so the
     * hunter is stock AND armed. Under ALL a valid weapon passes through. This
     * asserts the filter half of that guarantee: OFF/HATS always leave a
     * non-weapon in the weapon slot, ALL keeps a valid one. */
    const uint16_t armed[kLsw2CustomSlots] = {228, 100, 55, 214, 187, 0x13};
    lsw2_customization_filter(armed, out, Lsw2CustomPolicy::Off);
    check(!lsw2_is_custom_weapon(out[5]),
          "OFF leaves no custom weapon -> default weapon kept, hunter armed");
    lsw2_customization_filter(armed, out, Lsw2CustomPolicy::Hats);
    check(!lsw2_is_custom_weapon(out[5]),
          "HATS strips the custom weapon too -> default weapon kept, armed");
    lsw2_customization_filter(armed, out, Lsw2CustomPolicy::All);
    check(lsw2_is_custom_weapon(out[5]) && out[5] == 0x13,
          "ALL keeps a valid custom weapon");

    /* --- null safety ------------------------------------------------------ */
    lsw2_customization_filter(nullptr, out, Lsw2CustomPolicy::Off);
    lsw2_customization_filter(kSaved, nullptr, Lsw2CustomPolicy::Off);
    check(true, "null arguments are ignored rather than crashing");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
