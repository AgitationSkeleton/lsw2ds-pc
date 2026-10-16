/* Adaptive widescreen: widen the 3D viewport to match the window.
 *
 * WHAT THIS IS FOR. Stretching 256x192 to a wide window distorts everything;
 * letterboxing wastes the space. Widening the *viewport* instead shows more of
 * the world, which is what a modern source port is expected to do.
 *
 * IT IS ALSO THE ONE ENHANCEMENT THAT REACHES PAST PRESENTATION, and that is
 * worth being explicit about. port/ARCHITECTURE.md's rule for the host layer is
 * that it may change what the player experiences and must not change what the
 * game computes. Showing more world breaks that on purpose: the game culls
 * against a 256-wide frustum, so without a guest-side change the extra columns
 * fill with nothing, or worse with geometry popping in and out at the old
 * edges.
 *
 * So this is a TITLE PATCH - named, gated on the cartridge SHA-1, confined to
 * one file, and off by default. Not a checkbox that quietly rewrites the game.
 *
 * TWO HALVES, and only the first is game-independent:
 *
 *   1. PRESENTATION. Ask the 3D renderer for a wider raster
 *      (nds_gpu3d_set_output_width) and present the widened surface
 *      (nds_gpu2d_adaptive_framebuffer). The software rasteriser implements
 *      SetRenderWidth, so this works without the GL compute path.
 *
 *   2. CULLING, which IS needed - and this file has been wrong about it twice.
 *
 *      First it asserted culling would break the picture. Then a measurement
 *      showed level geometry rendering correctly across the full width, and
 *      the assertion was replaced with the opposite claim. That measurement
 *      was real but far too narrow: it compared STATIC SCENERY - walls, a bar,
 *      arches - which the hardware clips against the GPU's own volume, and
 *      that volume does follow the widened raster.
 *
 *      ACTORS do not. The game runs its own visibility test against the
 *      original 256-px screen bounds before it ever submits them, so objects
 *      left and right of centre pop in and out as they cross the old edges.
 *      Reported from play, which is where it was always going to show up: a
 *      paused interior scene has nothing moving through the boundary.
 *
 *      The framework solved the same problem for another title by widening
 *      the game's own frustum planes each frame (title_patches.cpp,
 *      patch_sm64ds_clipper). LSW2 needs the equivalent and does not have it
 *      yet, so the UI says so plainly.
 *
 *      The lesson, recorded because it cost two wrong claims: a measurement
 *      bounds what it measured. Static geometry says nothing about actors.
 */

#include "widescreen.h"

#include "screenengine.h"   /* kLsw2DispcntEngineA */

#include "gpu2d.h"
#include "gpu3d.h"
#include "state.h"          /* bus_device_read32 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kNativeWidth = 256;
constexpr int kNativeHeight = 192;

/* THE RENDERER'S CEILING, and it is not ours to choose.
 *
 * melonDS's software rasteriser sizes its scanline buffers at compile time -
 * GPU3D_Soft.h: MaxRenderWidth = 448, ScanlineWidth = MaxRenderWidth + 2 - and
 * GPU3D::SetRenderWidth clamps to the same figure, as does the framework's
 * nds_gpu3d_set_output_width. Asking for more is not "ambitious", it is
 * refused, and a refusal used to drop the picture all the way back to 4:3.
 *
 * 448 x 192 is 2.33:1. Every common display fits inside that with room to
 * spare: 16:9 wants 341 px, 16:10 wants 307. Only wider-than-21:9 setups reach
 * the ceiling at all, and there the picture simply stops widening rather than
 * misbehaving. Raising it would mean patching vendored GPL code and paying for
 * bigger buffers on every frame, to serve a shape almost nobody has - so the
 * ceiling is respected here and stated in the UI instead. */
constexpr int kMaxRenderWidth = 448;
constexpr int kMaxExtra = kMaxRenderWidth - kNativeWidth;   /* 192 */

/* THE COMPUTE RENDERER TAKES WHOLE 64-PIXEL COLUMNS, and nothing else.
 *
 * The framework's nds_gpu3d_use_compute_renderer (runner/src/gpu3d.cpp) refuses
 * any render width that is not a multiple of 64 - the width of one coarse
 * binning tile - and the refusal is not a clamp: the whole accelerated renderer
 * fails to start and the shell drops to the software rasteriser. A 16:9 display
 * asks for 341 px, rounded even to 340, so every launch on the most common
 * monitor shape was quietly running the SOFTWARE path - no internal resolution,
 * no texture upscale, and a rasteriser thread per instance on the CPU. With four
 * instances on one machine that thread is what the emulation threads were
 * queueing behind:
 *
 *     [gpu3d] compute render width 340 is unsupported
 *     [gl] no OpenGL 4.3 compute context; falling back to the software path
 *
 * The second line is the shell guessing at why; the first is the reason. So
 * under the accelerated renderer the width is snapped to this step before the
 * renderer exists. The four widths it allows - 256, 320, 384, 448 - are 4:3,
 * 5:3, 2:1 and 7:3; 16:9 lands on 320 (21 px short) rather than 384 (43 over). */
constexpr int kComputeWidthStep = 64;
bool g_accelerated = false;

bool g_enabled = false;
int g_current_width = kNativeWidth;
bool g_logged_unpatched = false;
int g_logged_refusal = 0;   /* the width we last complained about */

/* Whether this frame looks like a 3D world view rather than 2D artwork.
 * See lsw2_widescreen_note_frame. Starts false so the very first frames -
 * the publisher splash - are treated as artwork, which they are. */
bool g_locked = false;
bool g_world_view = false;
bool g_anchor_active = false;   /* what we last told the compositor */

/* THE ANCHOR BAND, and it is measured rather than chosen.
 *
 * The compositor buckets each element by where it sits: left of the band it
 * keeps its native x and so lands on the new left edge, right of the band it
 * moves out by the full extra width, inside the band it shifts by half and
 * stays centred.
 *
 * LSW2's top-screen HUD is a LEFT-ANCHORED CLUSTER - the character portrait and
 * the four hearts - measured at native x 9..112. The framework's default band
 * of 128 puts the left boundary at 64, which falls INSIDE that cluster: the
 * outermost heart bucketed left while the portrait and the other three bucketed
 * centre, and the HUD came apart with one heart stranded away from the rest.
 *
 * 32 puts the left boundary at 112, clear of the whole cluster, so the HUD
 * moves as one piece and keeps its internal spacing, while still leaving a
 * 32-px band down the middle for anything genuinely centred. If a centred
 * element is ever found to straddle 112..144, measure it and revisit this -
 * do not nudge it. */
constexpr int kAnchorBand = 32;

bool g_extend_active = false;   /* what we last told the compositor */

/* TWO DECISIONS, NOT ONE, and conflating them is what made the HUD fall apart.
 *
 * BACKGROUND EXTENSION asks: are the 2D layers the picture? On the title card
 * and the menus they are, and the backmost layer should reach the new edges so
 * the starfield fills the frame. During gameplay they are a HUD drawn over a 3D
 * world, and extending them tiles the tilemap - a second set of hearts appeared
 * at the right-hand edge. So: on for artwork, off for a world view.
 *
 * HUD ANCHORING asks: should corner elements move out to the new corners? For
 * LSW2 the answer is NO, and not by preference - by measurement. Its character
 * portrait is drawn by the 3D engine, so it moves with the widened viewport no
 * matter what the 2D compositor does. Anchoring the hearts to the window edge
 * therefore drags them away from the portrait they belong to: the hearts ended
 * up to the LEFT of the portrait, overlapping it, instead of to its right.
 * Leaving anchoring off keeps the whole HUD in its authored arrangement, just
 * inset from the corner rather than glued to it - which is the closest thing to
 * "preserved" available while one of its parts is 3D.
 *
 * LSW2_HUD_ANCHOR in the environment turns anchoring back on for anyone who
 * wants to re-measure this; it is not a setting because it is not a preference.
 *
 * Both are idempotent - the compositor is only told when an answer changes,
 * because this runs every frame. */
/* THE FILM EXTRAS - Old West, Old Time and extra 5.
 *
 * FUN_0208d478 puts a 256-wide image (0xEF0/0xEF1/0xEF2, table 0x020FF490) on
 * one of engine A's text BGs and blends it over the 3D: BLDCNT with that BG
 * as the first target and BG0 as the second (FUN_02053ff8(8, 1)), alpha mode,
 * EVA 6 of 16 - 4 for extra 5 (FUN_02054080). In a widened frame the 2D
 * layers stay native, so the film stopped at the old edges and the new
 * columns showed the 3D unfiltered. Naming that one layer to the compositor
 * (patch 0016) decodes it across the whole width - the tilemap wraps, which is
 * what the hardware would fetch past its viewport - and the compositor blends
 * those pixels exactly as it blends the native ones.
 *
 * The layer object is 0x021093A0, a BG handle like the HUD's: +0 the engine,
 * +4 the BG (FUN_020536a8 shows and hides it through those two), and +8 the
 * mode FUN_0208d478 stores - 0..2 a film, 3 the fade the HUD's hide asks for
 * (0x0208AD58). It is named only while that BG is on AND blended the way the
 * film blends it, so a screen that later reuses the BG for something else is
 * left native. */
constexpr uint32_t kFilmLayer = 0x021093A0u;
constexpr uint32_t kFilmLayerBg = 4u;
constexpr uint32_t kFilmLayerMode = 8u;
constexpr uint32_t kFilmModes = 3u;   /* 0..2 are the three films */
constexpr uint32_t kBldcntEngineA = kLsw2DispcntEngineA + 0x50u;
constexpr uint32_t kBlendSecondBg0 = 1u << 8;   /* BLDCNT 8-13: second targets */
constexpr uint32_t kBlendModeAlpha = 1u;
uint8_t g_film_mask = 0;   /* what we last told the compositor */

uint8_t film_layer_mask() {
    if (bus_device_read32(0, kFilmLayer) != 0u) return 0;   /* engine B */
    const uint32_t bg = bus_device_read32(0, kFilmLayer + kFilmLayerBg) & 0xFFu;
    const uint32_t mode =
        bus_device_read32(0, kFilmLayer + kFilmLayerMode) & 0xFFu;
    if (bg < 1u || bg > 3u || mode >= kFilmModes) return 0;
    const uint32_t dispcnt = bus_device_read32(0, kLsw2DispcntEngineA);
    const uint32_t bldcnt = bus_device_read32(0, kBldcntEngineA) & 0xFFFFu;
    const bool shown = ((dispcnt >> (8u + bg)) & 1u) != 0u;
    const bool blended = ((bldcnt >> bg) & 1u) != 0u &&
                         (bldcnt & kBlendSecondBg0) != 0u &&
                         ((bldcnt >> 6u) & 3u) == kBlendModeAlpha;
    return (shown && blended) ? uint8_t(1u << bg) : uint8_t(0);
}

void update_anchor(bool enabled, int width) {
    const bool wide = enabled && width > kNativeWidth;

    const uint8_t want_film = wide ? film_layer_mask() : uint8_t(0);
    if (want_film != g_film_mask) {
        g_film_mask = want_film;
        nds_gpu2d_set_adaptive_bg_extend_mask(want_film);
        std::fprintf(stderr, "[widescreen] film layer %s (BG mask %02X)\n",
                     want_film ? "extended to the full width" : "native",
                     unsigned(want_film));
    }

    static const bool anchor_allowed = std::getenv("LSW2_HUD_ANCHOR") != nullptr;
    const bool want_anchor = anchor_allowed && wide && g_world_view;
    if (want_anchor != g_anchor_active) {
        g_anchor_active = want_anchor;
        nds_gpu2d_set_adaptive_hud_anchor(want_anchor, kAnchorBand);
    }

    const bool want_extend = wide && !g_world_view;
    if (want_extend != g_extend_active) {
        g_extend_active = want_extend;
        nds_gpu2d_set_adaptive_bg_extend(want_extend);
        if (std::getenv("LSW2_WIDESCREEN_TRACE"))
            std::fprintf(stderr,
                         "[widescreen] background extension %s (width %d)\n",
                         want_extend ? "ON" : "off", width);
    }
}

}  // namespace

int lsw2_widescreen_max_extra() { return kMaxExtra; }

void lsw2_widescreen_lock(bool locked) { g_locked = locked; }
bool lsw2_widescreen_locked() { return g_locked; }

/* IS THIS A WORLD VIEW OR IS IT ARTWORK? The answer decides whether the HUD
 * anchor may run, and getting it wrong is very visible either way.
 *
 * WHAT THE ANCHOR DOES. When the frame is widened, the compositor pins the
 * left part of the native 2D image to the new left edge, the right part to the
 * new right edge, and leaves a band in the middle - so a HUD drawn in the
 * corners stays in the corners instead of drifting inward. That is exactly
 * right for the portrait and hearts drawn over the Cantina.
 *
 * WHY IT CANNOT ALWAYS BE ON. On a screen where the 2D layers ARE the picture -
 * the title card, the publisher splash, any full-width menu art - the same
 * rule cuts the image into three pieces and slides them apart. The LEGO Star
 * Wars II title logo came apart into "LEGO STAR W", "TA", and "ARS II" with
 * gaps between them. No choice of band width fixes that: art that spans the
 * full width will always straddle whatever boundaries are picked.
 *
 * THE TEST. The two cases differ in something we can measure: a world view has
 * a 3D layer covering most of the frame, artwork has almost none. So sample the
 * rasterised 3D surface and ask how much of it was actually drawn. This reads
 * only the host's own render output - no guest state, no ROM addresses, nothing
 * that could go stale - and it is self-checking: if the 3D layer really is
 * covering the screen, anchoring is what the player wants.
 *
 * The thresholds are deliberately apart. A single figure would let a frame
 * hovering near it flip the HUD between anchored and centred every few frames,
 * which reads as the HUD twitching. Coverage has to climb past 55% to turn
 * anchoring on and fall below 35% to turn it off. */
void lsw2_widescreen_note_frame() {
    if (!g_enabled || g_current_width <= kNativeWidth) {
        g_world_view = false;
        return;
    }

    /* Five scanlines spread down the frame. Sampling every line would be 192
     * times the work to answer the same question; sampling one would be fooled
     * by a letterboxed cutscene bar or a full-width HUD strip. */
    static const int kRows[] = {24, 64, 96, 128, 168};
    const int width = g_current_width;
    long drawn = 0;
    for (int row : kRows) {
        const uint32_t* line = nds_gpu3d_wide_line(row);
        if (!line) continue;
        for (int x = 0; x < width; ++x) {
            /* The 3D surface stores coverage in the top bits; zero alpha means
             * no polygon was rasterised into this pixel. */
            if (((line[x] >> 24) & 0x1Fu) != 0u) ++drawn;
        }
    }

    const double coverage =
        double(drawn) / double(width * int(sizeof kRows / sizeof *kRows));
    if (coverage > 0.55) g_world_view = true;
    else if (coverage < 0.35) g_world_view = false;
}

int lsw2_widescreen_width_for(int window_w, int window_h, int max_extra) {
    if (window_w <= 0 || window_h <= 0) return kNativeWidth;

    /* How much wider than 4:3 the window is, expressed in guest pixels at the
     * height the guest actually renders. */
    const double want = double(window_w) / double(window_h);
    const double native = double(kNativeWidth) / double(kNativeHeight);
    if (want <= native) return kNativeWidth;

    int width = int(kNativeHeight * want + 0.5);

    /* Even, so the extra columns split evenly either side and the native
     * viewport stays centred - the adaptive compositor assumes that. */
    width &= ~1;
    const int extra = std::clamp(max_extra, 0, kMaxExtra);
    width = std::clamp(width, kNativeWidth, kNativeWidth + extra);

    /* NEAREST WHOLE COLUMN UNDER THE ACCELERATED RENDERER, ties down, and never
     * past the player's own ceiling. See kComputeWidthStep. */
    if (g_accelerated) {
        const int step = kComputeWidthStep;
        int snapped = ((width - kNativeWidth + step / 2 - 1) / step) * step +
                      kNativeWidth;
        while (snapped > kNativeWidth + extra) snapped -= step;
        width = std::max(snapped, kNativeWidth);
    }
    return width;
}

void lsw2_widescreen_set_accelerated(bool accelerated) {
    g_accelerated = accelerated;
}

bool lsw2_widescreen_apply(bool enabled, int window_w, int window_h,
                           int max_extra) {
    const int want = enabled
        ? lsw2_widescreen_width_for(window_w, window_h, max_extra)
        : kNativeWidth;

    /* Locked: the width is whatever was established before the renderer
     * started. Reporting success is correct - nothing failed, the answer is
     * simply already decided - and it keeps the caller from retrying a change
     * the renderer will refuse for the rest of the session.
     *
     * AND THE ENABLED STATE IS LOCKED WITH IT. This used to copy the live
     * checkbox into g_enabled and hand it to update_anchor, while the 3D
     * raster stayed at the width the lock had fixed. Unticking "Adaptive
     * widescreen" under OpenGL therefore switched the 2D background extension
     * off against a raster still 448 wide: the world view stayed wide and
     * every artwork screen grew black bars either side of its 256 columns -
     * half a setting, applied to the half that could move. The UI already
     * tells the player the choice takes effect on restart; the compositor now
     * behaves that way too, by following the state that was established when
     * the width was, not the checkbox. */
    if (g_locked) {
        update_anchor(g_enabled, g_current_width);
        return true;
    }

    /* THE ANCHOR IS RE-EVALUATED EVERY FRAME, not only when the width changes.
     * Whether the scene is a world view or artwork changes constantly while the
     * width does not, and an early return here used to leave the anchor stuck
     * in whatever state the last resize left it in. */
    update_anchor(enabled, want);

    if (want == g_current_width && enabled == g_enabled) return true;

    /* STEP DOWN RATHER THAN GIVE UP. The clamp above should already keep every
     * request inside what the renderer accepts, but the ceiling lives in
     * vendored code and could move; if a width is refused anyway, walk down to
     * the widest one that is taken. Falling straight back to 256 was the old
     * behaviour and it was the worst possible answer: the player asked for a
     * wider view and silently got a letterboxed 4:3 picture, which reads as
     * "the setting does nothing" rather than "the renderer said no". */
    int got = 0;
    for (int try_w = want; try_w >= kNativeWidth; try_w -= 2) {
        if (nds_gpu3d_set_output_width(uint16_t(try_w))) { got = try_w; break; }
    }

    if (got == 0) {
        /* Not even native was accepted: the renderer is in a state the
         * presenter cannot reason about, so say so rather than draw garbage. */
        if (g_logged_refusal != want) {
            g_logged_refusal = want;
            std::fprintf(stderr,
                         "[widescreen] renderer refused every width from %d "
                         "down to %d; staying at %d\n",
                         want, kNativeWidth, g_current_width);
        }
        g_enabled = false;
        return false;
    }

    if (got != want && g_logged_refusal != want) {
        /* Only once per distinct request - this runs every frame, and the old
         * version filled the log with one line per frame. */
        g_logged_refusal = want;
        std::fprintf(stderr,
                     "[widescreen] renderer refused width %d; using %d\n",
                     want, got);
    }

    g_current_width = got;
    g_enabled = enabled;
    update_anchor(enabled, got);

    if (enabled && got > kNativeWidth && !g_logged_unpatched) {
        /* The second half of this line used to warn that geometry would cull at
         * the old edges. Measuring it showed otherwise, and the UI
         * was corrected while this was left asserting the opposite - a log line
         * is a claim like any other. */
        g_logged_unpatched = true;
        std::fprintf(stderr, "[widescreen] 3D raster widened to %d px\n", got);
    }
    return true;
}


int lsw2_widescreen_width() { return g_current_width; }
bool lsw2_widescreen_frustum_patched() {
    /* Kept, and still false, because no frustum patch exists - nothing was
     * changed in the guest. It is no longer used to drive a warning, since
     * measuring showed there is nothing to warn about; see the file header. */
    return false;
}
bool lsw2_widescreen_enabled() { return g_enabled && g_current_width > kNativeWidth; }
