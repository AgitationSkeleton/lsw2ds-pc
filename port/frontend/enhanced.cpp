/* The guest half of the Enhanced control set's movement lever - see enhanced.h for the derivation
 * and enhanced_policy.cpp for the rules. This file is only the plumbing: read
 * the two settings, read the byte, ask the policy, write what it says. */

#include "enhanced.h"
#include "vwh.h"

#include "cheats.h"
#include "gearhop.h"
#include "netcount.h"        /* kLsw2NetCountObject, kLsw2NetLocalStation */
#include "viewmask.h"
#include "forksession.h"
#include "hostsettings.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

/* ------------------------------------------------------------------------- */
/* ENHANCED CONTROLS IN THE ARENA: the game's own per-player camera mode, sent the game's own
 * way, and then steered.
 *
 * WHAT THE ARENA'S TANK CONTROLS ACTUALLY ARE. Not actor+0x5DA, which the
 * offline rule below writes: in a networked arena that byte reads 0 or 1 (the
 * player's view), never the sentinel 4, so that rule never fires there. The
 * switch is the per-station MODE byte at camera + 0x208 - the table the notes
 * above call 0x02109E90 - which the arena's script sets to 9 for every player
 * (command 78). 0x02007420 reads it for the actor being moved: 9 diverts to the
 * tank handler 0x020075EC; any other playable value falls through to the
 * camera-relative path, `request + view yaw` (0x0204A5BC), where the yaw is the
 * one that player's own console published in its view record (+0x6A, written
 * at 0x0205EDC0 from its local camera). 0x02007408 and 0x0204A62C treat ONLY 9
 * as tank. The same byte picks this console's camera for its own station
 * (0x020A06A0).
 *
 * WHY MODE 4 AND NOT THE LOBBY'S 1. Measured on two consoles: with 1 the
 * movement was camera-relative and both consoles agreed on every position -
 * but the picture was a wall. Mode 1 follows a camera PATH from level data
 * (0x020A4830), and the arenas have none: the path pointer +0x21C is null, and
 * 0x020A97E0 read the point array through it - 64 unmapped reads a console,
 * the camera pinned to garbage. Mode 4 needs no level data (0x020A1B64): the
 * camera sits at the target plus the offset +0x264/+0x268/+0x26C rotated by the
 * yaw +0x272 (sin/cos from 0x020F88C4 at yaw >> 4) - the same formula mode 9
 * uses on its rest offset +0x250..+0x258 with heading + 0x8000. So mode 4 with
 * that offset and that yaw IS the arena's own framing, and its yaw is a lever
 * nothing else writes while +0x278 (the yaw animation) is clear. Movement in 4
 * is the camera-relative path, as in 1.
 *
 * WHY THE GAME SENDS THE MODE AND THE FORK DOES NOT WRITE IT. The byte decides
 * where a player moves, and positions are what the cross-player check sums: a
 * one-sided poke of this table ended a match in state 38, the same
 * poke on both consoles did not. The game has a replicated setter
 * for exactly this - 0x020A99BC(camera, value) - which, while the multi-view
 * bit is up, writes NOTHING locally and sends message 0x39 (this station,
 * value) to every player; every console, the sender included, applies it at
 * the same exchange (0x020B2CC4 -> 0x020A9A24). With the bit clear it writes
 * this console's own entry, which offline is all there is. So each console
 * asks the game to change ITS OWN player's mode, through the gear hop.
 *
 * WHY THE CAMERA'S OTHER FIELDS ARE THE FORK'S TO WRITE. Everything the camera
 * object holds apart from the mode table is this console's view: it reaches
 * the simulation only as the yaw the look-at step leaves in +0x42, which the
 * game publishes into this console's own view record and every console then
 * steers this player by, at the same dataset. So the orbit is an INPUT, like
 * the pad - carried by the game's own record - and writing it between frames
 * is as safe in lockstep as pressing a button.
 *
 * WHY EACH CONSOLE ONLY EVER ASKS FOR ITS OWN. The precedence rule - a client's
 * Off beats a host's Enhanced - is per player, and the setter can only name the
 * sender's own station. So the rule is enforced where it is decided. */
constexpr uint32_t kModeSetter = 0x020A99BCu;   /* (camera, value)            */
constexpr uint32_t kModeCamera = kLsw2LocalCamera;
constexpr uint32_t kModeTable = kModeCamera + 0x208u;   /* one byte a station */
constexpr uint8_t kModeTank = 9;       /* the arena's own                     */
constexpr uint8_t kModeOrbit = 4;      /* offset + yaw, no level data         */

/* The camera's fields, from the disassembly of 0x020A1B64 and 0x020A0C9C. */
constexpr uint32_t kCamTarget = 0x1D0u;        /* the actor it follows        */
constexpr uint32_t kCamRestHeight = 0x254u;    /* mode 9's offset, s32 20.12  */
constexpr uint32_t kCamRestDistance = 0x258u;
constexpr uint32_t kCamOffsetX = 0x264u;       /* mode 4's offset, s32 20.12  */
constexpr uint32_t kCamOffsetY = 0x268u;
constexpr uint32_t kCamOffsetZ = 0x26Cu;
constexpr uint32_t kCamYawTarget = 0x270u;     /* u16, what +0x272 animates to */
constexpr uint32_t kCamYaw = 0x272u;           /* u16, 0x10000 a full turn    */
constexpr uint32_t kCamYawAnimate = 0x278u;    /* s8, > 0 animates the yaw    */
constexpr uint32_t kActorHeading = 0xCAu;      /* u16                         */

/* THE ARENA'S FRAMING when its script set none: the constructor's rest offset
 * (0, 24, 40) in 20.12. */
constexpr int32_t kDefaultHeight = 24 << 12;
constexpr int32_t kDefaultDistance = 40 << 12;
/* The right stick's vertical range, as a camera height: from just over the
 * player's head to a high overview. */
constexpr int32_t kMinHeight = 6 << 12;
constexpr int32_t kMaxHeight = 70 << 12;

/* A REQUEST IN FLIGHT IS NOT REPEATED. The message is sent at this console's
 * next netgame tick and applied a pipeline later - a few frames. Three
 * quarters of a second is well past that, so a request still unanswered then
 * was lost (the game sends one message a record) and is asked again. */
constexpr int kModeRetryFrames = 45;

int g_mode_wait = 0;
/* Whether this console changed its own player's mode - the only case where
 * turning Enhanced off may put it back. The arena's 9 is the game's. */
bool g_mode_ours = false;
/* The mode-4 offset this console last set, so a reset by the game (0x020AA048
 * at a match start restores its own (30, 30, 30)) is seen and undone. */
int32_t g_orbit_height = 0;
int32_t g_orbit_distance = 0;

uint8_t guest_byte(uint32_t a) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    return uint8_t((w >> ((a & 3u) * 8u)) & 0xFFu);
}

uint16_t guest_half(uint32_t a) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    return uint16_t((w >> ((a & 2u) * 8u)) & 0xFFFFu);
}

void write_half(uint32_t a, uint16_t v) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    const uint32_t shift = (a & 2u) * 8u;
    bus_device_write32(0, a & ~3u, (w & ~(0xFFFFu << shift)) | (uint32_t(v) << shift));
}

void write_byte(uint32_t a, uint8_t v) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    const uint32_t shift = (a & 3u) * 8u;
    bus_device_write32(0, a & ~3u, (w & ~(0xFFu << shift)) | (uint32_t(v) << shift));
}

bool live_actor(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

/* This console's station as the game knows it, or -1 when the game does not
 * agree yet. In a session the station byte must be this console's session AID
 * and the multi-view bit up - the guard sessiondress.cpp measured the need
 * for: mid-join the byte can still read 0, and without the bit the setter
 * writes locally instead of sending, which on one console is the divergence
 * itself. Offline the bit must be DOWN, so the setter writes locally. */
int arena_station(bool session) {
    const uint8_t station =
        guest_byte(kLsw2NetCountObject + kLsw2NetLocalStation);
    if (station >= 4u) return -1;
    if (session && (int(station) != lsw2_vwh_local_aid() || !lsw2_vwh_multiview()))
        return -1;
    if (!session && lsw2_vwh_multiview()) return -1;
    return int(station);
}

/* FRAME THE ORBIT AS THE ARENA FRAMES ITS OWN CAMERA: mode 9's rest height and
 * distance, and the yaw mode 9 would use - heading + 0x8000, behind the player.
 * Written before the mode switch lands (mode 4 is the only reader of these), so
 * the first mode-4 frame is where mode 9's last one was. */
void frame_orbit(uint32_t actor) {
    int32_t height = int32_t(bus_device_read32(0, kModeCamera + kCamRestHeight));
    int32_t distance = int32_t(bus_device_read32(0, kModeCamera + kCamRestDistance));
    if (height <= 0 || height > kMaxHeight) height = kDefaultHeight;
    if (distance <= 0 || distance > (200 << 12)) distance = kDefaultDistance;
    const uint16_t yaw = uint16_t(guest_half(actor + kActorHeading) + 0x8000u);
    bus_device_write32(0, kModeCamera + kCamOffsetX, 0);
    bus_device_write32(0, kModeCamera + kCamOffsetY, uint32_t(height));
    bus_device_write32(0, kModeCamera + kCamOffsetZ, uint32_t(distance));
    write_byte(kModeCamera + kCamYawAnimate, 0);
    write_half(kModeCamera + kCamYawTarget, yaw);
    write_half(kModeCamera + kCamYaw, yaw);
    g_orbit_height = height;
    g_orbit_distance = distance;
}

/* Ours when the offset still reads what frame_orbit wrote (heights move with
 * the stick, so the height is compared against the last one written too). */
bool orbit_framed() {
    return g_orbit_distance != 0 &&
           bus_device_read32(0, kModeCamera + kCamOffsetX) == 0 &&
           int32_t(bus_device_read32(0, kModeCamera + kCamOffsetY)) == g_orbit_height &&
           int32_t(bus_device_read32(0, kModeCamera + kCamOffsetZ)) == g_orbit_distance;
}

/* THE ORBIT KEEPS OUT OF WALLS - the arena camera's own wall pull, run for
 * mode 4 too.
 *
 * Measured on two consoles: orbiting beside a wall in Mos Eisley put the
 * camera inside it - a screen of wall texture - because mode 4 places the
 * camera (0x020A1B64) and never tests the level, where mode 9 runs 0x020A1450
 * straight after its own placement: two raycasts through the level
 * (0x02092EB0) that pull the camera to the first surface between it and the
 * player, then aim it at the player's head. It reads nothing mode-9 specific -
 * the camera's position, its target and the look height +0x324 - so it is as
 * right for the orbit as for the chase camera.
 *
 * HOW IT IS RUN, and the way that failed first. An override on the placement
 * that called the placement and then the pull corrupted the frame (r5 left
 * holding the offset, an unmapped read in the leash, ~240 "fps" from a
 * guest that had stopped doing work): a lifted function can be suspended part
 * way by the scheduler and "return" to its C caller early, so an override may
 * call one only as its last act (the LB note on lifted bodies says the same).
 *
 * So the game's own mode-4 sequence is given the call instead. That sequence
 * (0x020A0784..0x020A07E8) is not lifted - the lift of 0x020A0440 ends at
 * 0x020A0458 - so the machine runs it from RAM and a changed instruction is
 * the instruction it runs (a guest write bumps the page's generation). Its
 * fifth call, 0x020A07C4 `BL 0x020A2530`, is the leash, which does nothing
 * unless +0x290 is set - off in both arenas (measured). While this port's
 * orbit is active that one word becomes `BL 0x020A1450`, with r0 already the
 * camera (`MOV r0, r5` before it); otherwise it is the game's own word. Only
 * rewritten when it holds the exact word expected, so a different image is
 * left alone. */
constexpr uint32_t kLeashCallSite = 0x020A07C4u;
constexpr uint32_t kLeashCall = 0xEB000759u;       /* BL 0x020A2530 */
constexpr uint32_t kWallPullCall = 0xEB000321u;    /* BL 0x020A1450 */

void orbit_walls(bool on) {
    const uint32_t now = bus_device_read32(0, kLeashCallSite);
    const uint32_t want = on ? kWallPullCall : kLeashCall;
    if (now == want || (now != kLeashCall && now != kWallPullCall)) return;
    bus_device_write32(0, kLeashCallSite, want);
    std::fprintf(stderr, "[fork] Enhanced: mode 4's leash call %s the arena camera's "
                 "wall pull\n", on ? "now runs" : "restored instead of");
}

void arena_mode_tick(bool session) {
    const int station = arena_station(session);
    if (station < 0) return;
    if (!lsw2_cheats_in_arena_match()) {
        g_mode_wait = 0;
        g_mode_ours = false;
        g_orbit_distance = 0;
        orbit_walls(false);
        return;
    }
    const uint32_t target = bus_device_read32(0, kModeCamera + kCamTarget);
    const uint8_t mode = guest_byte(kModeTable + uint32_t(station));
    const bool enhanced = lsw2_enhanced_effective(
        lsw2_fork_session().settings().controls == Lsw2HostControls::Enhanced,
        lsw2_ui_enhanced_controls());

    /* UNTIL THE SWITCH LANDS, the orbit follows the chase camera - re-framed
     * behind the player every frame - so the first mode-4 frame is where mode
     * 9's last one was. In mode 4 it is re-framed only when the game has reset
     * the offset: 0x020AA048 restores its own (30, 30, 30) at a match start. */
    if (live_actor(target)) {
        if (mode == kModeTank && enhanced) frame_orbit(target);
        else if (mode == kModeOrbit && !orbit_framed()) frame_orbit(target);
    }

    uint8_t want;
    if (enhanced && mode == kModeTank && live_actor(target)) {
        want = kModeOrbit;
    } else if (!enhanced && g_mode_ours && mode == kModeOrbit) {
        want = kModeTank;
    } else {
        g_mode_wait = 0;
        return;
    }
    if (g_mode_wait > 0) {
        --g_mode_wait;
        return;
    }
    if (!lsw2_gearhop_request_action(kModeSetter, kModeCamera, want, 0u, 0u))
        return;
    g_mode_wait = kModeRetryFrames;
    g_mode_ours = (want == kModeOrbit);
    orbit_walls(g_mode_ours);
    std::fprintf(stderr, "[fork] Enhanced: station %d's camera mode %u -> %u (%s)%s\n",
                 station, unsigned(mode), unsigned(want),
                 want == kModeTank ? "tank" : "free movement, orbit camera",
                 session ? ", sent as the game's own message 0x39 so every "
                           "console applies it at the same exchange"
                         : "");
}

/* WHICH ACTOR WE WROTE, not merely whether we wrote one.
 *
 * This began as a bool and that was a flag which can outlive the thing it
 * describes - the pattern this project has a name for. A round ends, the actor
 * is torn down, another is built at a different address, and a bare bool would
 * still say "this port chose that index" about a byte it had never touched. The
 * undo path acts on ownership, so a stale claim is a write.
 *
 * An address cannot go stale in the same way: a new actor is a different
 * pointer and simply does not match. Zero means "nothing of ours". */
uint32_t g_owner = 0;

}  // namespace

/* THE ENHANCED CONTROL SET'S CAMERA: the stick turns the game's own orbit.
 *
 * RETRACTED. This wrote the stick's yaw into the VIEW RECORD's
 * +0x6A, on the argument that between frames nothing else writes it. Nothing
 * else does - but the record is what the game SENDS about a camera, rebuilt
 * from the camera every frame (0x0205EDA0) and overwritten by the next
 * exchange's copy (0x020AF688, a memcpy, which is why a STRH scan missed it).
 * It never turned the picture, and in a session it would have been a
 * local-only change to what the other consoles steer this player by.
 *
 * What turns the picture is the camera: in the game's mode 4 (see the mode
 * block above), the yaw +0x272 places it around the player and the offset's
 * height +0x268 raises it, and the look-at step then leaves the new view yaw
 * in +0x42 for the game to publish. This runs between frames - before the next
 * camera update at 0x0205ED88 - so a turn made here is in the very next frame's
 * picture and the very next record. Only while this console's own station is
 * in mode 4 and the orbit is framed as ours: in mode 9 the yaw is the chase
 * camera's to decide. */
void lsw2_enhanced_camera_step() {
    const int dx = lsw2_ui_take_camera_delta();
    const int dy = lsw2_ui_take_camera_delta_y();
    if (dx == 0 && dy == 0) return;
    if (!ui_fork_build() || !lsw2_ui_enhanced_controls()) return;
    if (!lsw2_cheats_in_arena_match()) return;
    const int station = arena_station(lsw2_vwh_lockstep());
    if (station < 0) return;
    if (guest_byte(kModeTable + uint32_t(station)) != kModeOrbit || !orbit_framed())
        return;

    if (dx != 0) {
        /* WRAPPED, NOT CLAMPED: the yaw is a full turn in a halfword. Mirrored
         * into the animation target, which the game would otherwise animate
         * the yaw back toward were the animation ever re-armed. */
        const uint16_t yaw = uint16_t(guest_half(kModeCamera + kCamYaw) + dx);
        write_half(kModeCamera + kCamYaw, yaw);
        write_half(kModeCamera + kCamYawTarget, yaw);
    }
    if (dy != 0) {
        /* One stick unit is 1/512 of a world unit of height: full deflection
         * is 0.5 units a frame, 30 a second - the whole range in two. */
        int32_t h = g_orbit_height + dy * 8;
        if (h < kMinHeight) h = kMinHeight;
        if (h > kMaxHeight) h = kMaxHeight;
        g_orbit_height = h;
        bus_device_write32(0, kModeCamera + kCamOffsetY, uint32_t(h));
    }
}

int lsw2_enhanced_camera_yaw(int index) {
    if (index < 0 || index >= 4) return -1;

    const uint32_t mgr = bus_device_read32(0, kLsw2ViewManagerPtr);
    if (mgr < 0x02000000u || mgr >= 0x02400000u) return -1;

    /* THROUGH THE GAME'S OWN RESOLUTION, not by subscripting.
     *
     * This line used to be `mgr + 0x240 + index * 0x70`, which is what
     * `0x020AF718` computes only after it has turned a positive index into a
     * record by walking the mask. For index 0 the two agree, which is why the
     * probe never caught it; for any other index they agree only when the
     * players occupy records 0..n-1 with no gap. */
    const int mask = lsw2_viewmask_guest();
    const int record =
        lsw2_viewmask_resolve(mask < 0 ? 0u : unsigned(mask), index);
    const uint32_t cam = lsw2_viewmask_record(mgr, record);
    const uint32_t addr = cam + kLsw2ViewYaw;

    /* A HALFWORD, read through its word. `+0x6A` is even but not word aligned,
     * so the bus's 32-bit read straddles it - the same width discipline the
     * actor's view index needed. */
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return int((w >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

namespace {

/* The probe's state: armed by the button, checked on the following frame. */
int g_probe = 0;              /* 0 idle, 1 armed, 2 written-and-waiting */
int g_probe_wrote = 0;

/* THE WRITE, kept private. Nothing but the probe may call it until the
 * experiment says a written yaw survives at all. */
void camera_write_yaw(int index, uint16_t v) {
    const uint32_t mgr = bus_device_read32(0, kLsw2ViewManagerPtr);
    if (mgr < 0x02000000u || mgr >= 0x02400000u) return;

    /* The same resolution the reader uses - a probe that writes a record the
     * reader is not reading would report "overwritten" forever. */
    const int mask = lsw2_viewmask_guest();
    const int record =
        lsw2_viewmask_resolve(mask < 0 ? 0u : unsigned(mask), index);
    const uint32_t addr = lsw2_viewmask_record(mgr, record) + kLsw2ViewYaw;
    const uint32_t base = addr & ~3u;
    const uint32_t sh = (addr & 2u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFFFu << sh)) | (uint32_t(v) << sh));
}

void camera_probe_tick() {
    if (g_probe == 0) return;

    const int now = lsw2_enhanced_camera_yaw(0);

    if (g_probe == 1) {
        /* THE SAME ARENA GATE lsw2_enhanced_camera_step needs, and for a
         * sharper reason: this probe exists to answer "does a written yaw
         * survive the camera's own update", and the recordings show the view
         * manager stays allocated in the lobby after a match. Run there, the
         * probe would write into a camera nothing is updating and report THE
         * WRITE HELD - a false yes to the exact question it was built to ask.
         * Its own message already said "try it during a match"; now it is
         * enforced rather than requested. */
        if (!lsw2_cheats_in_arena_match()) {
            g_probe = 0;
            std::fprintf(stderr,
                         "[fork] camera probe: not on an arena map. The view "
                         "manager outlives a match, so a result from the lobby "
                         "would be a false yes.\n");
            return;
        }
        if (now < 0) {
            g_probe = 0;
            std::fprintf(stderr,
                         "[fork] camera probe: no multi-view array - nothing to "
                         "write to. Try it during a match.\n");
            return;
        }
        /* A QUARTER TURN. Large enough to see and to tell apart from the
         * camera's own drift; binary angles, so it wraps for free. */
        g_probe_wrote = (now + 0x4000) & 0xFFFF;
        camera_write_yaw(0, uint16_t(g_probe_wrote));
        g_probe = 2;
        std::fprintf(stderr,
                     "[fork] camera probe: %d -> %d, checking next frame\n",
                     now, g_probe_wrote);
        return;
    }

    g_probe = 0;
    if (now == g_probe_wrote) {
        std::fprintf(stderr,
                     "[fork] camera probe: THE WRITE HELD (%d) - the Enhanced right "
                     "stick is buildable\n", now);
    } else {
        std::fprintf(stderr,
                     "[fork] camera probe: overwritten (wrote %d, read %d) - "
                     "the camera recomputes its yaw\n", g_probe_wrote, now);
    }
}

}  // namespace

void lsw2_enhanced_probe_camera() {
    if (g_probe == 0) g_probe = 1;
}

void lsw2_enhanced_tick() {
    /* THE FORK'S FEATURE. The Enhanced control set belongs to the Bounty
     * Hunter arena; the baseline build has nothing of the kind, and its co-op
     * is the vanilla game with a lobby list in front of it. Changing how the
     * base game walks is out of scope for either build. */
    if (!ui_fork_build()) return;
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): the movement mode is simulation
     * state, and slot 0 is the HOST's actor on a joiner - so in a
     * session the mode travels as the game's own message instead (above). */
    if (lsw2_vwh_lockstep()) {
        arena_mode_tick(true);
        return;
    }
    arena_mode_tick(false);

    camera_probe_tick();

    const bool in_arena = lsw2_cheats_in_arena_match();

    /* NO ACTOR, NO OWNERSHIP. Leaving the arena tears the player actor down, so
     * the next one starts from whatever the game gives it - and if the flag
     * survived, the first frame of the next match would look like an actor this
     * port had already changed and could "restore". */
    if (!in_arena) {
        g_owner = 0;
        return;
    }

    const bool enhanced = lsw2_enhanced_effective(
        lsw2_fork_session().settings().controls == Lsw2HostControls::Enhanced,
        lsw2_ui_enhanced_controls());

    /* SLOT 0 IS THE PLAYER. The Enhanced control set is the local player's -
     * the other slots in an arena match are remote stations simulating their
     * own movement on their own machines, and the party pin keeps them out
     * anyway. */
    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (!actor) return;                      /* no actor yet this frame */

    const int current = lsw2_cheats_party_view_index(0);
    if (current < 0) return;

    const int want = lsw2_enhanced_view_write(in_arena, enhanced, current,
                                              g_owner == actor);
    if (want < 0) return;

    if (!lsw2_cheats_set_party_view_index(0, want)) return;
    g_owner = (want == kLsw2ViewLocal) ? actor : 0;

    /* SAY IT ONCE PER CHANGE, not once per frame. The policy only returns a
     * value when the byte is about to actually change, so this line is already
     * rare - it fires when a match starts and when the player toggles. */
    std::fprintf(stderr,
                 "[fork] Enhanced view index %d -> %d  (%s movement)\n",
                 current, want,
                 want == kLsw2ViewSentinel ? "tank" : "camera-relative");
}
