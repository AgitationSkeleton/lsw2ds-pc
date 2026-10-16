/* See hudportrait.h. */

#include "hudportrait.h"

#include "gearhop.h"
#include "cheats.h"          /* kLsw2PartyActors */
#include "levels.h"          /* kLsw2LevelRootAddr */
#include "netcount.h"        /* kLsw2NetCountObject */
#include "partyportrait.h"   /* kLsw2PartyEmpty */
#include "ui.h"
#include "vwh.h"

#include "state.h"
#include "statelog.h"       /* kLsw2StateManager */

#include <cstdio>
#include <cstdlib>

namespace {

/* Main RAM, which is the only place these objects live. */
constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

/* THE FINGERPRINTS FUN_0208b76c WRITES. These are not guesses about a layout;
 * they are the literal constants that function stores into each sprite, and
 * they are what makes locating the HUD safe. */
constexpr uint32_t kFaceX0 = 0x00000000u, kFaceX1 = 0x00000000u;
constexpr uint32_t kFaceY0 = 0x00020000u, kFaceY1 = 0x00020000u;
constexpr uint32_t kRingX0 = 0x00000000u, kRingX1 = 0x00000800u;
constexpr uint32_t kRingY0 = 0x00026000u, kRingY1 = 0x00026000u;
constexpr uint32_t kSpriteAttr = 0x1Fu;

/* Sprite field offsets, from FUN_02019f74's own reads. */
constexpr uint32_t kSpriteX0 = 0xDCu;
constexpr uint32_t kSpriteX1 = 0xE0u;
constexpr uint32_t kSpriteY0 = 0xE4u;
constexpr uint32_t kSpriteY1 = 0xE8u;
constexpr uint32_t kSpriteAttrByte = 0xF4u;

/* The steps of one rebuild, in the order they must happen. */
constexpr int kStepDestroyFace = 0;
constexpr int kStepDestroyRing = 1;
constexpr int kStepBuild = 2;

uint32_t g_hud = 0;
uint8_t g_last_character = 0;
bool g_have_last = false;
uint8_t g_pending_character = 0;
/* The face sprite the baseline was taken for: the settling window takes the
 * baseline when a portrait the game built appears, not on every frame. */
uint32_t g_baseline_face = 0;

/* Reported once each, because a message every frame would bury the run. */
bool g_said_no_hud = false;
int g_reports = 0;
constexpr int kReports = 8;

uint32_t rd32(uint32_t addr) { return bus_device_read32(9, addr); }

uint8_t rd8(uint32_t addr) {
    const uint32_t w = bus_device_read32(9, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

bool in_ram(uint32_t a) { return a >= kRamLow && a < kRamHigh; }

/* Does this address hold a sprite with the given rectangle? The attribute byte
 * is checked too - it is 0x1F, the same 1F that shows up in the render list as
 * `attr 001F80C0` - so two independent fields have to agree. */
bool sprite_matches(uint32_t s, uint32_t x0, uint32_t x1, uint32_t y0,
                    uint32_t y1) {
    if (!in_ram(s) || (s & 3u) != 0u) return false;
    return rd32(s + kSpriteX0) == x0 && rd32(s + kSpriteX1) == x1 &&
           rd32(s + kSpriteY0) == y0 && rd32(s + kSpriteY1) == y1 &&
           rd8(s + kSpriteAttrByte) == kSpriteAttr;
}

bool is_face(uint32_t s) {
    return sprite_matches(s, kFaceX0, kFaceX1, kFaceY0, kFaceY1);
}

bool is_ring(uint32_t s) {
    return sprite_matches(s, kRingX0, kRingX1, kRingY0, kRingY1);
}

/* FIND THE HUD, THROUGH THE OBJECT THAT OWNS IT.
 *
 * RETRACTED: this used to be a search of all 4 MB of main RAM, on
 * the grounds that "there is no global holding this object that has been
 * found". There is, and the repo already had both halves of it:
 *
 *   - the state manager at 0x02102900 holds two slots. Slot 0 (+0)
 *     is the LEVEL state - class vtable 0x020FF478, the level index at +0x0C -
 *     and slot 1 (+4) is whatever state is on top of it (the Free Play state,
 *     vtable 0x020FF414, id 48, in the arena lobby).
 *   - the level class's frame step, slot 3 = FUN_0208cdb0, calls
 *     FUN_0208b0b8(state + 0x2C). The HUD is the LEVEL state's member at +0x2C.
 *
 * Read on the live machine in the arena lobby: slot 0 = 021DD944,
 * vtable 020FF478, level 9, and +0x2C+0x54 / +0x58 hold exactly the ring and
 * face fingerprints below; slot 1 = the Free Play state, with nothing there.
 * It is slot 0 and not lsw2_state_current_object() - the top state - that owns
 * the portrait.
 *
 * The search was not merely slow. It ran whenever the character had changed
 * and no HUD was found - and in the hunter statue select (state 41) the
 * character changes with no HUD on screen - so it re-ran EVERY FRAME there:
 * ~1M bus reads, measured at 260-280 ms of every second on BOTH linked
 * consoles, the largest single cost in a connected frame.
 *
 * The two sprite fingerprints still gate the answer: a level state whose +0x2C
 * does not hold the ring and the face is one with no portrait built yet, and
 * the caller retries next frame at the cost of a few reads. */
uint32_t find_hud() {
    const uint32_t level = rd32(kLsw2StateManager);
    if (!in_ram(level) || rd32(level) != kLsw2LevelStateVtable) return 0;
    const uint32_t hud = level + kLsw2LevelStateHud;
    const uint32_t ring = rd32(hud + kLsw2HudRingSlot);
    const uint32_t face = rd32(hud + kLsw2HudFaceSlot);
    if (!is_ring(ring) || !is_face(face)) return 0;
    std::fprintf(stderr,
                 "[fork] top-screen portrait: hud %08X = level state %08X + "
                 "0x2C (ring %08X, face %08X)\n",
                 unsigned(hud), unsigned(level), unsigned(ring), unsigned(face));
    return hud;
}

/* The face sprite the level's HUD holds right now, or 0: find_hud's reads
 * without its report, for the settling window's look on every frame. */
uint32_t hud_face_now() {
    const uint32_t level = rd32(kLsw2StateManager);
    if (!in_ram(level) || rd32(level) != kLsw2LevelStateVtable) return 0;
    const uint32_t hud = level + kLsw2LevelStateHud;
    const uint32_t face = rd32(hud + kLsw2HudFaceSlot);
    if (!is_face(face) || !is_ring(rd32(hud + kLsw2HudRingSlot))) return 0;
    return face;
}

/* The character the portrait is supposed to show, read exactly the way
 * FUN_0208aaf0 reads it. */
uint8_t current_character() {
    const uint8_t slot = rd8(kLsw2NetCountObject + kLsw2NetLocalStation);
    if (slot >= 4u) return kLsw2PartyEmpty;
    const uint32_t actor =
        rd32(kLsw2LevelRootAddr + kLsw2PartyActors + slot * 4u);
    if (!in_ram(actor)) return kLsw2PartyEmpty;
    return rd8(actor + kLsw2ActorCharacter);
}

/* Queue the destructor of one sprite: call [[sprite] + 4] with r0 = sprite. */
bool destroy_sprite(uint32_t sprite, int step) {
    if (!in_ram(sprite)) return false;
    const uint32_t vtable = rd32(sprite);
    if (!in_ram(vtable)) return false;
    const uint32_t dtor = rd32(vtable + 4u);
    /* The destructor is CODE, so it is not in the RAM range the objects are -
     * it is in the ROM's ARM9 image. A zero or wild value here means the vtable
     * read was wrong, and calling it would be a wild branch. */
    if (dtor < 0x01FF8000u || dtor >= 0x02400000u) return false;
    /* FROM THE HUD'S OWN STEP, not from whichever host asks first - see
     * kLsw2HudStep. The destructor is a deleting one (FUN_02019b54 frees the
     * texture, the palette and the sprite itself, whatever its second
     * argument), so where in the frame it runs is the difference between the
     * game's own teardown and a use-after-free. */
    const bool queued = lsw2_gearhop_request_call_at(dtor, sprite, 0u, 0u, step,
                                                     kLsw2HudStep);
    return queued;
}

}  // namespace

/* lsw2_hudportrait_should_rebuild lives in hudportrait_policy.cpp, so the
 * test can link the decision without the guest-memory code around it. */

void lsw2_hudportrait_tick() {
    if (!lsw2_ui_enhanced_portrait()) return;

    /* A KILL-SWITCH FOR ONE MEASUREMENT, not a setting. The goal it serves:
     * once a match is over, everyone goes back to the lobby. A measured run had
     * the return working on both consoles and the HOST then dying, and the host's
     * log puts the death between this module's two sprite destructors and its
     * rebuild after the level came back from the arena (the same three steps
     * that pass at boot). The player's setting for this enhancement lives in
     * settings.json, which a test drive leaves untouched, so this reads an
     * environment variable instead - the same shape as LSW2_ARROWHIDE_OFF.
     * With it set, the return runs with this module inert, and whether the
     * host survives says whether the teardown is the trigger. */
    static const bool off = std::getenv("LSW2_HUDPORTRAIT_OFF") != nullptr;
    if (off) return;

    /* IN A SHARED SESSION TOO. This
     * was off under lsw2_vwh_lockstep() beside its sibling on the touch screen
     * (partyportrait), which wedged all four consoles after a four-player
     * match. Two differences decide it, and neither holds here:
     *
     *  - WHAT TRIGGERS IT. The sibling reacted to the PARTY, and in a shared
     *    lobby the party changes whenever another player's actor spawns, so it
     *    fired on every console after every match. This reacts to one byte:
     *    the character of THIS console's own station (current_character()). A
     *    remote player's Free Play switch changes their actor, not this one.
     *    After a level change the settling window below re-takes the baseline,
     *    so a lobby return or an arena entry never counts as a switch.
     *  - WHERE IT RUNS. Nothing here touches the guest between frames. Each
     *    step (two destructors, then the game's own builder) is queued and
     *    runs on the guest's thread inside kLsw2HudStep, the HUD's own frame
     *    step, where the cartridge tears down player two's portrait itself
     *    (FUN_0208b54c).
     *
     * What it changes is this console's HUD, which is already different on
     * every console: each one shows its own player.
     *
     * ONLY ONCE THE GAME AGREES WHO THIS CONSOLE IS - sessiondress.cpp's rule.
     * Measured there: mid-join the game's station byte still read 0 on a
     * joiner, which would make the host's character look like this console's
     * own and rebuild the portrait for it. */
    if (lsw2_vwh_lockstep()) {
        const uint8_t station = rd8(kLsw2NetCountObject + kLsw2NetLocalStation);
        if (int(station) != lsw2_vwh_local_aid()) return;
    }

    /* A LEVEL CHANGE OPENS A SETTLING WINDOW in which this module never
     * rebuilds and keeps re-taking its baseline. The comment below already
     * said the rule: "a level load rebuilds the portrait correctly, and this
     * must not then fire on the next tick". Honouring it on the ONE tick the
     * level changed was not enough, and that is what killed the host after the
     * post-match lobby return, four runs out of four.
     *
     * WHY A WINDOW AND NOT A SINGLE TICK. Measured in run D: the level moves
     * to the lobby (9) while the local character still reads 0 - the actor is
     * being torn down and rebuilt across the load, and its character byte is
     * momentarily 0 - and only a handful of frames later does it settle at the
     * real hunter (20). A one-tick re-baseline captured the transient 0, then
     * saw "0 -> 20" the next frame and tore down the portrait the level load
     * had only just built, freeing a 64x64 texture the VBlank list still walked
     * (the crash site pc=020627EC). The game's own HUD init
     * needs those frames to finish; so does this. A window of about a second
     * and a half absorbs the transient and ends with the settled character as
     * the baseline, so no rebuild is ever queued for the game's own load.
     *
     * A Free Play switch INSIDE a level - the only thing this module exists for
     * - happens after the window has opened and still fires once it closes. */
    constexpr int kSettleFrames = 90;
    static int last_level = -999;
    static int settle = 0;
    const int level_now = lsw2_cheats_current_level();
    if (level_now != last_level) {
        last_level = level_now;
        settle = kSettleFrames;
        g_hud = 0;   /* the HUD is rebuilt with the level; relocate on demand */
        g_baseline_face = 0;
    }

    const uint8_t now = current_character();
    if (settle > 0) {
        --settle;
        /* THE BASELINE IS WHAT THE GAME BUILT THE PORTRAIT FOR, taken once,
         * when a new portrait appears - not whatever the actor reads on every
         * frame of the window.
         *
         * Re-taking it every frame absorbed a real switch that landed inside
         * the window, and one always does after a join: the joiner arrives
         * rebuilt as Greedo, the game builds Greedo's portrait, and the
         * last-played hunter is restored by the game's own switch 30 frames in
         * (lasthunter_observe.cpp). The window then settled on the hunter as
         * "already shown", and the joiner kept Greedo's face for the whole
         * session.
         *
         * Taken when the face appears, the baseline is the character the
         * builder read in that frame (FUN_0208aaf0), so a switch after it is
         * seen as one once the window closes. If the face appears while the
         * actor is still mid-rebuild, the worst case is one rebuild the portrait
         * did not need, after the window - the same rebuild a Free Play switch
         * makes, not the teardown-during-build the host died of. An empty read
         * is ignored so a torn-down actor does not become the baseline. */
        const uint32_t face = hud_face_now();
        if (face != 0u && face != g_baseline_face && now != kLsw2PartyEmpty) {
            g_baseline_face = face;
            g_last_character = now;
            g_have_last = true;
            /* AND NEVER A TEARDOWN WITHIN HALF A SECOND OF THE BUILD, however
             * late in the window the game built it: the host's crash was a
             * portrait freed a frame after its build, with its texture still
             * on the VBlank list. */
            constexpr int kAfterBuildFrames = 30;
            if (settle < kAfterBuildFrames) settle = kAfterBuildFrames;
        }
        return;
    }
    if (!lsw2_hudportrait_should_rebuild(now, g_last_character, g_have_last)) {
        /* The baseline still has to be established, and updated when the game
         * itself changes the character (a level load rebuilds the portrait
         * correctly, and this must not then fire on the next tick). */
        if (now != kLsw2PartyEmpty) {
            g_last_character = now;
            g_have_last = true;
        }
        return;
    }

    if (g_hud == 0 || !is_ring(rd32(g_hud + kLsw2HudRingSlot))) {
        /* THE CACHE IS RE-VALIDATED, NOT TRUSTED. The HUD is destroyed and
         * rebuilt across a level change, so a pointer that was right once can
         * name freed memory later - and freed guest memory is still guest
         * memory, so an in-range test cannot see it. Requiring the ring sprite
         * to still be there is the same check that found it. */
        g_hud = find_hud();
    }
    if (g_hud == 0) {
        if (!g_said_no_hud) {
            g_said_no_hud = true;
            std::fprintf(stderr,
                         "[fork] top-screen portrait: the HUD could not be "
                         "located, so character %u will not be shown. This is "
                         "a miss, not a quiet skip\n",
                         unsigned(now));
        }
        return;
    }

    const uint32_t face = rd32(g_hud + kLsw2HudFaceSlot);
    const uint32_t ring = rd32(g_hud + kLsw2HudRingSlot);

    /* THE ORDER IS THE GAME'S ORDER: both sprites released, both slots cleared,
     * and only then rebuilt. The clearing happens on the ring's completion
     * rather than here, because the destructors run on later frames and a slot
     * zeroed early would leave the queued call holding the only pointer.
     *
     * AND THE PHASE IS THE GAME'S PHASE: every step is pinned
     * to kLsw2HudStep, the HUD's own frame step, because taken from an actor's
     * update the same three steps killed the host after a lobby return - twice.
     * The sequence had been right and the place had not. */
    g_pending_character = now;
    const bool a = destroy_sprite(face, kStepDestroyFace);
    const bool b = destroy_sprite(ring, kStepDestroyRing);
    if (!a || !b) {
        std::fprintf(stderr,
                     "[fork] top-screen portrait: could not queue the teardown "
                     "for character %u (face %08X %s, ring %08X %s) - NOT "
                     "rebuilding, because a half-torn-down portrait is worse "
                     "than a stale one\n",
                     unsigned(now), unsigned(face), a ? "ok" : "refused",
                     unsigned(ring), b ? "ok" : "refused");
        return;
    }

    g_last_character = now;
    g_have_last = true;
    /* UNCAPPED, because the capped line above was silent when it mattered.
     * In one measured run the host died right after the return's rebuild began
     * and the hop's own per-call reports had already spent their cap on the
     * match's gear, so the log could not say whether the destructors had even
     * been taken. A rebuild happens once per character change; a line each is
     * not noise. The goal: the post-match return to the lobby ends with a
     * live host, and the chain that kills it must be legible in one read. */
    std::fprintf(stderr,
                 "[fork] top-screen portrait: character %u -> rebuilding hud "
                 "%08X: teardown queued (face %08X, ring %08X)\n",
                 unsigned(now), unsigned(g_hud), unsigned(face), unsigned(ring));
}

bool lsw2_hudportrait_step_still_live(int step, uint32_t a0) {
    const uint32_t level = rd32(kLsw2StateManager);
    if (!in_ram(level) || rd32(level) != kLsw2LevelStateVtable) return false;
    const uint32_t hud = level + kLsw2LevelStateHud;
    switch (step) {
    case kStepDestroyFace: return rd32(hud + kLsw2HudFaceSlot) == a0;
    case kStepDestroyRing: return rd32(hud + kLsw2HudRingSlot) == a0;
    case kStepBuild:
        return a0 == hud && rd32(hud + kLsw2HudFaceSlot) == 0u &&
               rd32(hud + kLsw2HudRingSlot) == 0u;
    default: return false;
    }
}

void lsw2_hudportrait_step_dropped(int step) {
    std::fprintf(stderr, "[fork] top-screen portrait: step %d dropped - the HUD "
                 "it was for is gone (a level reload); the new one was built by "
                 "the game for the character it found\n", step);
    g_hud = 0;
}

void lsw2_hudportrait_step_returned(int step, uint32_t result) {
    /* Each step's completion is said out loud, uncapped, for the reason given
     * at the teardown line: the sequence spans frames and the host has died
     * inside it, so which step it died after must be readable from the log. */
    std::fprintf(stderr, "[fork] top-screen portrait: step %d returned (r0 %08X)\n",
                 step, unsigned(result));
    if (step != kStepDestroyRing) return;
    if (g_hud == 0) return;

    /* Both sprites are gone; clear the slots the way FUN_0208b54c does, then
     * ask the game to build the portrait again for the new character. */
    bus_device_write32(9, g_hud + kLsw2HudFaceSlot, 0u);
    bus_device_write32(9, g_hud + kLsw2HudRingSlot, 0u);

    const bool queued = lsw2_gearhop_request_call_at(
        kLsw2PortraitBuild, g_hud, g_pending_character, 0u, kStepBuild,
        kLsw2HudStep);
    if (!queued) {
        std::fprintf(stderr,
                     "[fork] top-screen portrait: the rebuild for character %u "
                     "was REFUSED after the teardown - the icon is now empty "
                     "rather than stale, and that is worth saying out loud\n",
                     unsigned(g_pending_character));
    }
}
