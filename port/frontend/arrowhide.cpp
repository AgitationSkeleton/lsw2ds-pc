/* See arrowhide.h. */

#include "arrowhide.h"

#include "cheats.h"
#include "levels.h"
#include "ui.h"

#include "state.h"

#include <cstdio>
#include <cstdlib>

namespace {

/* The arrow's texture parameter (read off its polygons with gx_polygons) and
 * the mask that stands beside it in the source struct. Together they are the
 * signature: a lone
 * parameter match could be any object that draws the same texture, and the mask
 * four bytes earlier is what makes the hit the SOURCE STRUCT rather than a
 * coincidence. */
/* TWO TEXTURE PARAMETERS, ONE MARKER. Measured with the same method that found
 * the lobby's - gx_polygons on the child console standing in a running Mos
 * Eisley deathmatch: four polygons at x 128..150 y 61..75, exactly where the arrow
 * sits over the character, alpha 0x17 like the lobby's, attr 02178080 like the
 * lobby's - and texture 036B00 / palette 03DA / raw texparam 08036D60, where
 * the lobby's is 034160 / 0246 / 0803682C. One marker, whose texture differs
 * from level to level. The signature scan found exactly one struct at level 5 and it was
 * the LOBBY's leftover, so clearing it could never have touched the arena's
 * arrow. This is why. */
constexpr uint32_t kArrowTexParams[] = {
    0x0803682Cu,   /* the arena lobby, level 9                   */
    0x08036D60u,   /* the arenas, measured level 5                */
};
constexpr uint32_t kArrowMask = 0x001F78FFu;

/* Offsets within the struct, relative to its base. */
constexpr uint32_t kValue = 0x0Cu;
constexpr uint32_t kParam = 0x14u;

/* THE MASK SITS ONE WORD BELOW THE PARAMETER, and is written as a distance
 * rather than as its own `0x10` constant on purpose.
 *
 * `dup_constants` objected to a second name for 0x10 - statelog.h's
 * kLsw2StateArgC already has it - and it was right to. That gate's baseline is
 * frozen at thirteen entries for a reason it states plainly: sweep_orphans was
 * written with a large baseline and `lsw2_font_load` sat inside it for weeks
 * while the fork's menus drew no text at all. A gate whose baseline is where
 * the bugs live reports nothing.
 *
 * So the collision is removed rather than tolerated, and the relative form is
 * arguably the truer one anyway: what the struct read established is that these three
 * fields are ADJACENT in one struct, not that any of them lives at an
 * independently meaningful address. */
constexpr uint32_t kMaskBelowParam = 4u;

/* The polygon attribute's alpha field - the bits 01FFBC68 tests. */
constexpr uint32_t kAlphaBits = 0x001F0000u;

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

/* HOW MUCH RAM TO SWEEP PER FRAME.
 *
 * A whole-of-main-RAM scan is four megabytes of word reads through the bus
 * helper, and doing it in one frame would be a visible hitch on a console
 * already running at 0.44x real time. It is also unnecessary: the arrow is not
 * urgent, and a lobby lasts far longer than the second or so this takes spread
 * out. The scan resumes where it left off and wraps. */
constexpr uint32_t kScanBytesPerFrame = 0x6000u;

/* WHERE IT WAS FIRST FOUND, USED ONLY AS A HINT. Checking one address costs two
 * reads, and when it is right the scan never runs at all. When it is wrong -
 * a different build, a different boot - nothing is lost, because the hint is
 * validated by the same signature as every other candidate. This is a shortcut,
 * never an assumption. */
constexpr uint32_t kHintBase = 0x02207438u;

/* A TABLE, NOT ONE ADDRESS. An earlier run found the lobby's hide firing on
 * level 5 with the arrow still drawn, and could not say whether that was a stale
 * duplicate found first or a different bind - because a single cached base
 * cannot express "there are two of these". Eight is far more than any level
 * has been seen to carry; the cap exists so a corrupt heap full of the
 * signature cannot grow this without bound. */
constexpr int kMaxArrows = 8;
uint32_t g_bases[kMaxArrows];
int g_count = 0;
uint32_t g_scan = kRamLow;    /* where the amortised sweep is up to  */
bool g_swept_once = false;    /* a full pass has completed since the last reset */
bool g_hint_tried = false;

uint32_t read32(uint32_t a) { return bus_device_read32(0, a); }

/* Does this address carry the arrow's signature? Both fields, because either
 * alone is weak: the mask is a common enough word and the parameter could be
 * shared by another object that draws the same texture. */
/* Which of the marker's texture parameters this struct carries, or 0. The mask
 * is checked first because it is one read that rejects almost everything;
 * the parameter list is two entries, so the loop is not a cost. */
uint32_t arrow_param_at(uint32_t base) {
    if (base < kRamLow || base + kParam + 4u > kRamHigh) return 0;
    if (read32(base + kParam - kMaskBelowParam) != kArrowMask) return 0;
    const uint32_t p = read32(base + kParam);
    for (uint32_t known : kArrowTexParams)
        if (p == known) return p;
    return 0;
}

bool looks_like_the_arrow(uint32_t base) { return arrow_param_at(base) != 0; }

/* Record a newly seen struct and say so - once per struct, with its address
 * and the level it was found on, so the log carries the count. */
void remember(uint32_t base, int level) {
    for (int i = 0; i < g_count; ++i)
        if (g_bases[i] == base) return;
    if (g_count >= kMaxArrows) return;
    g_bases[g_count++] = base;
    std::fprintf(stderr,
                 "[fork] co-op arrow: struct #%d at %08X (parameter %08X, mask "
                 "%08X) on LEVEL %d\n",
                 g_count, unsigned(base), unsigned(arrow_param_at(base)),
                 unsigned(kArrowMask), level);
}

}  // namespace

void lsw2_arrowhide_tick() {
    if (!ui_fork_build()) return;

    /* AN OFF SWITCH, SO THERE CAN BE A CONTROL.
     *
     * `LSW2_ARROWHIDE_OFF=1` disables this entirely. It exists because "the
     * arrow is gone" is only meaningful against "the arrow was there", on the
     * same console under the same conditions - and the alternative control, a
     * screenshot from an older build, compares two things that differ in more
     * ways than the one being tested.
     *
     * It is also the bisection handle if this ever turns out to suppress
     * something it should not. Read once; absent means on, which is the
     * shipping behaviour. */
    static const bool off = [] {
        const char* e = std::getenv("LSW2_ARROWHIDE_OFF");
        return e && *e == '1';
    }();
    if (off) return;

    /* THE LOBBY ONLY, ORIGINALLY. The defect being fixed was co-op arrows
     * visible in the lobby, and an early reading took the marker for level-9
     * content, so this should never have had anything to do outside it. Gating
     * on the level anyway meant that if that reading was ever wrong, this could
     * not quietly start suppressing a marker somewhere nobody had decided to
     * hide it.
     *
     * THAT CAUTION WAS EARNED. The co-op arrow was then seen briefly in a
     * BOUNTY HUNTER level, and a capture of the child console during a
     * running Mos Eisley deathmatch (level 5) shows a green arrow above the
     * character. So "level-9 content" is very likely wrong.
     *
     * LSW2_ARROWHIDE_PROBE=1 answers that with evidence instead of a guess: it
     * lets the LOCATOR run at any level and REPORTS what it finds, and still
     * writes nothing outside the lobby. Finding the same signature at level 4
     * or 5 would prove the arrow is arena content too; finding nothing would
     * mean the thing on screen is a different marker and this module is not the
     * place to deal with it. Either way the shipping behaviour is unchanged
     * while the question is open - which is what the gate was for. */
    /* MEASURED, AND THE SCOPE IS NOW THE FORK'S THREE MAPS.
     *
     * The probe answered it on both consoles in a live deathmatch:
     *
     *   co-op arrow PROBE: the same signature is present on LEVEL 5, outside
     *   the lobby
     *
     * Same texture parameter, same mask, in the arena. The "level-9 content"
     * reading is wrong, and the lobby-only gate is why the arrow was still
     * visible in the arena after the lobby fix was considered complete.
     *
     * WHAT THIS IS AND IS NOT. The probe writes nothing outside the lobby, so
     * the arrow seen at level 5 is UNMODIFIED VANILLA behaviour - retail draws
     * that arrow in the bounty hunter arena. Hiding it there is therefore a
     * SECOND deliberate divergence from the ROM, made on that sighting and on
     * the port's premise that this is a deathmatch rather than a co-op
     * session. It is not a bug fix, and calling it one would misrepresent what
     * changed.
     *
     * THE THREE MAPS, NOT "ANY LEVEL". The original gate's caution was earned
     * and is kept: this still refuses to touch a marker on the story levels,
     * where the arrow is doing exactly the co-op job it was drawn for.
     * LSW2_ARROWHIDE_OFF=1 restores vanilla everywhere. */
    const int level = lsw2_cheats_current_level();
    const bool fork_map = (level == kLsw2ArenaLobbyLevel ||
                           level == kLsw2ArenaJabbaLevel ||
                           level == kLsw2ArenaMosEisleyLevel);
    static const bool probe = [] {
        const char* e = std::getenv("LSW2_ARROWHIDE_PROBE");
        return e && *e == '1';
    }();
    if (!fork_map && !probe) return;

    /* A LEVEL CHANGE INVALIDATES THE ADDRESS, and the signature check is not
     * enough on its own.
     *
     * MEASURED, and it is why widening the scope to the arenas did
     * nothing at first. The locator finds the arrow in the LOBBY and caches it.
     * On entering the arena the cached address STILL PASSES
     * looks_like_the_arrow() - the unloaded lobby's memory has not been
     * scribbled over yet, so it is still arrow-shaped - and the module happily
     * keeps clearing an alpha on a struct nobody is drawing from any more,
     * while the arena's own arrow renders untouched. The log said nothing,
     * because the "found it" line is one-shot, and only the SCREENSHOT showed
     * the arrow still on the character's head.
     *
     * So the level is remembered and a change drops the cache. That is the
     * file's own "do not trust a cached address" rule applied at the boundary
     * that actually moves things. */
    static int last_level = -999;
    if (level != last_level) {
        last_level = level;
        g_count = 0;
        g_scan = kRamLow;
        g_swept_once = false;
        g_hint_tried = false;
    }

    /* RE-VALIDATE BEFORE TRUSTING A CACHED ADDRESS. Levels load and unload and
     * the heap moves; an address that was the arrow's struct last lobby is not
     * automatically the arrow's struct this one. Two reads is a cheap price for
     * never writing into whatever happens to be there now - and "do not write
     * blind" is a rule this project wrote down after breaking it. */
    for (int i = 0; i < g_count;) {
        if (looks_like_the_arrow(g_bases[i])) { ++i; continue; }
        g_bases[i] = g_bases[g_count - 1];
        --g_count;
    }

    if (!g_hint_tried) {
        g_hint_tried = true;
        if (looks_like_the_arrow(kHintBase)) remember(kHintBase, level);
    }

    /* THE AMORTISED SWEEP, AND IT NO LONGER STOPS AT THE FIRST HIT.
     *
     * An earlier run could not tell "a stale duplicate is found first" from "the
     * arena binds the arrow another way", because the sweep broke out on its
     * first match and reported that one address. Now every match in a slice is
     * recorded, the sweep keeps going after a hit, and each newly seen struct
     * is printed with its address and level - so the log is a COUNT, which is
     * the measurement that was missing. Word-aligned, because the struct is. */
    const uint32_t end = (g_scan + kScanBytesPerFrame > kRamHigh)
                             ? kRamHigh
                             : g_scan + kScanBytesPerFrame;
    for (uint32_t a = g_scan; a + kParam + 4u <= end; a += 4u) {
        if (looks_like_the_arrow(a)) remember(a, level);
    }
    if (end >= kRamHigh) { g_scan = kRamLow; g_swept_once = true; }
    else                 { g_scan = end; }
    if (g_count == 0) return;

    /* THE PROBE STOPS HERE, on levels this module does not own. It reports and
     * returns without touching the struct, so a sighting somewhere unexpected
     * can be confirmed without changing what ships. This is how the arena was
     * established as arrow content in the first place. */
    if (!fork_map) {
        static int said_levels = 0;
        if (said_levels < 4 && g_swept_once) {
            ++said_levels;
            std::fprintf(stderr,
                         "[fork] co-op arrow PROBE: %d struct(s) carry the "
                         "signature on LEVEL %d, which this module does not "
                         "own. Reporting only - nothing is written here\n",
                         g_count, level);
        }
        return;
    }

    /* AND THE WRITE, TO EVERY STRUCT THAT CARRIES THE SIGNATURE, only when it
     * would change something. Re-applied rather than done once, because the
     * asset's own loader may refill a struct when a level reloads - but skipped
     * when the alpha is already zero, so the ordinary case is two reads per
     * struct and no write at all. */
    /* AND COUNT THE WRITES, because "the alpha is cleared" and "the alpha
     * STAYS cleared" are different claims. An earlier arena run cleared the only
     * struct there and the arrow drew anyway. If this line reports a write on
     * nearly every tick, the game is refilling the field each frame and a
     * once-per-tick poke is losing the race (the "external pokes lose the race"
     * pattern); if it reports one write and then none, the field is inert on
     * this level and the arrow is drawn from somewhere the signature does not
     * reach. Same run, two hypotheses told apart. */
    static int writes = 0, ticks = 0, said_level = -999;
    if (level != said_level) { said_level = level; writes = 0; ticks = 0; }
    for (int i = 0; i < g_count; ++i) {
        const uint32_t value = read32(g_bases[i] + kValue);
        if (value & kAlphaBits) {
            bus_device_write32(0, g_bases[i] + kValue, value & ~kAlphaBits);
            ++writes;
        }
    }
    if (++ticks % 600 == 0)
        std::fprintf(stderr,
                     "[fork] co-op arrow: %d alpha write(s) in the last 600 "
                     "ticks on LEVEL %d across %d struct(s)\n",
                     writes, level, g_count), writes = 0;
}
