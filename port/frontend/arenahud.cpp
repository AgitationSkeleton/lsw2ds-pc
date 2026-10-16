/* See arenahud.h. */

#include "arenahud.h"

#include "cheats.h"
#include "forksession.h"
#include "hiddenbgs.h"
#include "objsprite.h"
#include "screenengine.h"
#include "ui.h"

#include "gpu2d.h"
#include "vram.h"
#include "sessionpause.h"
#include "topoverlay.h"

#include <cstdio>

namespace {

constexpr int kW = 256;
constexpr int kH = 192;

/* "Engine B - the touch screen. Engine A is the top one" and a hardcoded
 * 0x04001000 both lived here until 2026-08-31. Which engine drives which screen
 * is POWCNT1 bit 15's business and the game moves it - see screenengine.h - so
 * both the OAM read and the starfield mask below resolve it per frame.
 *
 * THE MASK MATTERS MORE THAN THE READ, because it changes a picture. Aimed at
 * the wrong engine, it would take a layer off the screen the PLAYER IS LOOKING
 * AT while leaving the starfield in the pixels being mirrored - the visible
 * half of the bug and the broken half of the feature, from one constant. */

/* THE STARFIELD IS TWO LAYERS, BG2 AND BG3 - measured in a match with a
 * starfield probe: engine B had BGCNT 0x1E02 on BG2 and 0x1F01 on
 * BG3, both maps covering all 768 cells, the same two words the loading screen
 * draws its starfield with (whsession.cpp's join-screen note). An earlier
 * reading named BG2 alone; with BG2 off, BG3's stars were still behind every mirrored
 * portrait, in the CPU capture and in the window. So both are hidden.
 *
 * HIDDEN AT RENDER TIME, NOT BY POKING DISPCNT. This cleared DISPCNT's BG2 bit
 * from the between-frames tick, re-asserted every frame: a write into the
 * guest's own display register, from outside the frame, in a lockstep match.
 * Patch 0016's mask (hiddenbgs.h) leaves the layers out of engine B's picture
 * and touches nothing the game can read. */
constexpr uint8_t kStarfieldBgs = (1u << 2) | (1u << 3);

/* THE SLOT COLUMN, from config/bounty_hud_layout.json's measurements of a real
 * in-match frame. The circle is 40x40 at (5,5) with a 48-pixel pitch; the
 * counters sit to its right, so the mirrored strip is wider than the circle. */
constexpr int kSlotOriginY = 5;
constexpr int kSlotPitch = 48;
constexpr int kStripX = 0;
/* Circle plus the skull and its number, and no further: the radar panel's
 * left bezel starts at x 96 (kRadarCx's measurement), and at 112 every strip
 * carried sixteen columns of it - grey border and radar interior - beside the
 * digits (seen in the window once the HUD reached it). */
constexpr int kStripW = 96;
constexpr int kStripH = 48;
constexpr int kSlots = 4;

/* Palettes 4..7 are the four players' colours - a palette swap on shared tiles
 * rather than a tint. Which player a palette means was an open question;
 * the logging below is what settles it. */
constexpr int kFirstSlotPalette = 4;

/* The radar's source window on the touch screen - MEASURED, 2026-08-30.
 *
 * THIS WAS DISPUTED AND THE CAPTURE THAT SETTLES IT WAS ALREADY IN THE REPO.
 * The note here said "what settles it is one in-match capture of the bottom
 * screen", and out/work/gpudump_arena/inst0/screen1.png
 * IS that capture: the same arena frame bounty_hud_layout.json's sprite list
 * was read from. Nobody had looked at the picture.
 *
 * WHAT IT SHOWS. The radar is a bordered panel on the right of the screen. On
 * scanline y=72 its grey border begins at x=96 and the dark blue interior at
 * x=106; the right border ends at x=247. Down column x=171 the interior ends at
 * y=132 and the bottom border at y=139, with the top border at y=8. So:
 *
 *     panel x 96..247, y 8..139   -   152 wide, 132 tall, centred (171, 73)
 *
 * THE OLD VALUE WAS ESSENTIALLY RIGHT and the alternative was wrong. (169, 71)
 * is two pixels from the measured centre; (127, 79) sits inside the panel's
 * left third, and a 96-wide window there really would have missed the right
 * half, exactly as the old note feared. The reason to keep the old number
 * until a frame was looked at was sound, and looking at the frame is what
 * changes it.
 *
 * THE SIZE IS NOW A MEASUREMENT TOO. It was "a choice - wide enough for the
 * dial, short of the buttons"; 96 cropped 25 pixels off each side of the dial
 * and 20 off the bottom. The copy is a SQUARE, and the largest square that
 * fits inside a 152x132 panel is 132 - so the window is the panel's full
 * height, centred, and takes the dial whole rather than its middle.
 *
 * Centre y is 74 rather than the measured 73 so that 132 rows land exactly on
 * y 8..139, the panel's own rows, instead of one row of black above it.
 *
 * THE COPY IS THE WHOLE PANEL NOW, NOT A SQUARE: the square copy cut off the
 * radar's leftmost pixels. The largest square in a 152x132 panel left
 * ten columns off each side - the bezel's left edge among them - and nothing
 * required a square: the copy can keep the panel's own shape. */
constexpr int kRadarX = 96, kRadarY = 8, kRadarW = 152, kRadarH = 132;

/* THE DS SIGNAL METER'S IDENTITY IN OAM - the one arena-HUD element that MOVES.
 *
 * The mirrored HUD puts the DS signal meter in the top screen's bottom right.
 * Originally this was mirrored from a hand-set rectangle behind an on/off
 * setting, and on every install that had ever saved settings.json the setting
 * read OFF and the box 24x24 - the corrected defaults never reached anybody,
 * and the meter was not on any player's screen. The rectangle was never the
 * right shape for it anyway: the game does not draw the meter at one place. In
 * out/work/archive/gpudump_3matches (a real two-console session, 13,677
 * frames) the same sprite sits at (164,156) through the match, at (217,159) on
 * the score screen and at (238,4) elsewhere, and is absent in the lobby.
 *
 * WHAT IDENTIFIES IT is not where it is but what it is: the only OBJ on the
 * touch screen that is 16x16, 16-colour, palette 0. 465 sampled in-match
 * frames of the melonDS capture carry exactly one such sprite and it is
 * always this one; the same entry decodes into signal_bars_oam.png,
 * and gpudump_arena/inst0/screen1.png shows the antenna-and-bars glyph inside
 * its box, between the green button and the camera icon. So the HUD asks the
 * game's own OAM where the meter is this frame and copies that box - the same
 * move read_rows() makes for the portraits, and the reason no setting is
 * needed: "the game owns the pixels".
 *
 * NOT THE TILE NUMBER. The first version of this fingerprint also required
 * tile 0, which is what the melonDS dumps showed, and in a live match on the
 * port it matched NOTHING for a whole round (HUD on, level 5, no
 * `signal meter located` line). The game's sub-engine shadow OAM was
 * then read during that match (0x02103A98): entry 0 was x=164 y=156, 16x16,
 * 16-colour, palette 0 - the meter, at the position the melonDS dumps
 * show - with TILE 24. A tile index is where the game's
 * loader happened to place the glyph's character data, an allocation detail
 * that differs between a dump and a run and can differ between levels; it is
 * not what the sprite is. Palette, size and depth are, and among the
 * seventeen live sprites of that frame they were unique to this one. */
constexpr int kMeterPalette = 0;
constexpr int kMeterSize = 16;

/* WHERE THEY GO ON THE TOP SCREEN. The portrait column goes lower left and
 * the radar upper right; the exact insets are a choice. Two pixels off each edge so nothing sits
 * flush against a border that a widescreen letterbox might crop. */
constexpr int kInset = 2;

/* THE HUD'S LAYER is kLsw2HudLayerScale times the frame in each direction
 * (topoverlay.h); every destination below is in its pixels. */
constexpr int kLayerH = kH * kLsw2HudLayerScale;

bool g_layer_off = false;
bool g_reported = false;

/* Copy a rectangle from the touch frame into the destination - the ART, not the
 * backdrop behind it.
 *
 * RETRACTED: an earlier note read "NO COLOUR KEY, AND THE MOCK-UP IS WHY ...
 * with the starfield layer off, the backdrop IS that panel". In play the copy
 * left an opaque black backdrop down the left side. The black
 * column was the touch screen's backdrop, copied as if it were art. A colour key
 * was rightly refused - it cannot tell backdrop black from the black in the art
 * - so the renderer now says which pixels showed the backdrop (patch 0018,
 * nds_gpu2d_backdrop_mask) and those are left transparent. */
/* THE SAME COPY, RESAMPLED - the arena HUD's "HUD scale" slider.
 *
 * `arena_hud_scale` used to be a slider over nothing: this composite declined
 * to scale at all, with the note "Declining is better than guessing at a scale
 * factor". That note was about the WIDENED top screen, where the HUD's source
 * rectangles are DS pixels measured on a 256-wide frame and a scale factor
 * between the two would have to be guessed. It was never about the player
 * asking for a bigger HUD, which is not a guess but a request.
 *
 * The native-width refusal below is untouched and still correct. What changes
 * is that at native width the destination rectangle may now be a multiple of
 * the source.
 *
 * NEAREST NEIGHBOUR, because the source is a DS framebuffer of hard-edged
 * sprite art and a filter would blur exactly the edges the player is reading.
 *
 * AND IT IS EXACTLY THE OLD PATH AT 1:1. `dw == w && dh == h` takes the
 * original loop, so a player who never touches the slider gets the same pixels
 * as before rather than a resample that happens to round back.
 *
 * FROM THE TOUCH SCREEN'S PIXELS, INTO THE HUD'S LAYER (topoverlay.h), `lw`
 * wide and kLayerH tall, and OPAQUE where there is art: alpha 0xFF is what says
 * "the HUD is here" to both present paths. The source comes first, as it always
 * did - a source-check script reads `blit_scaled(touch,` as the
 * proof that the column is a copy. */
void blit_scaled(const uint32_t* src, const uint8_t* backdrop, int sx, int sy,
                 int w, int h, uint32_t* layer, int lw, int dx, int dy, int dw,
                 int dh) {
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return;
    for (int row = 0; row < dh; ++row) {
        const int dyy = dy + row;
        if (dyy < 0 || dyy >= kLayerH) continue;
        const int syy = sy + (dw == w && dh == h ? row : row * h / dh);
        if (syy < 0 || syy >= kH) continue;
        for (int col = 0; col < dw; ++col) {
            const int dxx = dx + col;
            if (dxx < 0 || dxx >= lw) continue;
            const int sxx = sx + (dw == w ? col : col * w / dw);
            if (sxx < 0 || sxx >= kW) continue;
            if (backdrop[syy * kW + sxx]) continue;
            layer[dyy * lw + dxx] = 0xFF000000u | src[syy * kW + sxx];
        }
    }
    lsw2_top_overlay_hud_touched(dy, dy + dh);
}

/* The player's size (UiSettings::arena_hud_scale), in HUD-layer pixels,
 * clamped to the slider's own range. At 0.5 a touch pixel is one layer pixel. */
int scaled(int v) {
    const float s = lsw2_ui_arena_hud_scale();
    const float f = s < 0.5f ? 0.5f : (s > 1.5f ? 1.5f : s);
    const int out = int(float(v * kLsw2HudLayerScale) * f + 0.5f);
    return out < 1 ? 1 : out;
}

/* WHICH ROW HOLDS WHICH PLAYER.
 *
 * The touch column is a live scoreboard sorted by score, and the mirrored
 * version must NOT be: it keeps players in a fixed order rather than
 * reordering them by who is winning. So the row a player occupies changes during a match and
 * cannot be used as an identity.
 *
 * The palette can. Reading the OAM entry for each row's 32x32 body gives its
 * palette bank, and bounty_hud_layout.json calls that "the player-colour
 * system". `rows[p]` ends up holding the row currently showing player p, or -1.
 *
 * THE INFERENCE WAS FLAGGED, AND IS NOW CONFIRMED - from a recording
 * rather than a live match. It was flagged because every frame then to
 * hand showed palettes "4, 5, 6, 7 in slot order", which is what a capture at
 * tied scores looks like whether the palette follows the player or the row.
 * The match with unequal scores existed all along:
 * out/work/archive/gpudump_3matches spans three matches in 6868 captured
 * frames, and its state.json files carry the decoded sub-engine OAM, so the
 * question is answerable by reading them.
 *
 * The mapping is not constant. 4193 frames give {0:0, 1:1, 2:2, 3:3} and 461
 * frames, f026710 to f031310, give {0:1, 1:0, 2:2, 3:3} - palette 5 in the top
 * row and palette 4 in the second. The column re-sorts, and the palette travels
 * with the player.
 *
 * So `player = palette - 4` is not merely defensible, it is REQUIRED: for those
 * 461 frames the top row belonged to player 2, and reading the column
 * positionally would have put the wrong player's strip on screen throughout.
 *
 * THE MODULO TEST BELOW EARNS ITS KEEP TOO. The reorder is a slide, not a swap:
 * one frame before it settles the two bodies are at y=20 and y=38, one frame
 * after it ends they are at y=11 and y=50, and in between they sit exactly on
 * the row origins. Rejecting a sprite whose (y - kSlotOriginY) is not a
 * multiple of kSlotPitch is what stops a mid-slide frame from reporting a row
 * that is not there - six frames of that capture correctly yield only the two
 * players who were standing still. */
void read_rows(int* rows) {
    for (int p = 0; p < kSlots; ++p) rows[p] = -1;

    const uint8_t* oam = nds_vram_renderer_oam(lsw2_screen_engine(kLsw2ScreenTouch));
    if (!oam) return;

    int seen[kSlots];
    for (int i = 0; i < kSlots; ++i) seen[i] = -1;

    for (int i = 0; i < kLsw2OamEntries; ++i) {
        Lsw2ObjSprite s;
        if (!lsw2_obj_decode(oam, i, &s) || s.disabled) continue;
        if (s.width != 32 || s.height != 32) continue;
        if (s.x < 4 || s.x > 6) continue;

        const int dy = s.y - kSlotOriginY;
        if (dy < 0 || dy % kSlotPitch != 0) continue;
        const int row = dy / kSlotPitch;
        if (row < 0 || row >= kSlots) continue;

        const int player = s.palette - kFirstSlotPalette;
        if (player < 0 || player >= kSlots) continue;

        rows[player] = row;
        seen[row] = player;
    }

    if (!g_reported) {
        g_reported = true;
        /* ONE LINE, AND IT IS THE MEASUREMENT THE PALETTE QUESTION NEEDS. Printed with
         * the row order so a match at unequal scores says immediately whether
         * the palettes travelled with their players or stayed put. */
        std::fprintf(stderr,
                     "[fork] arena HUD rows top-to-bottom carry players "
                     "%d %d %d %d (palette - 4)\n",
                     seen[0], seen[1], seen[2], seen[3]);

        /* AND THE RADAR WINDOW, printed where somebody is already looking at
         * this HUD. The anchor question this used to ask is SETTLED - see
         * kRadarCx - so the line now states what is being sampled and what the
         * panel measured, which is still worth one glance: if the mirrored
         * radar has a black border on any side, the window has drifted off the
         * panel and these two rectangles will say which way. */
        std::fprintf(stderr,
                     "[fork] radar copied from (%d,%d) %dx%d; the panel "
                     "measures x 96..247, y 8..139\n",
                     kRadarX, kRadarY, kRadarW, kRadarH);

        /* EVERY SPRITE ON THE TOUCH SCREEN, ONCE - aimed at one missing feature
         * rather than being a general dump.
         *
         * The mirrored HUD later gained the DS signal meter, placed in the
         * bottom-right corner of the top screen.
         *
         * IT IS BUILT, AND THIS PARAGRAPH ONCE SAID OTHERWISE. It
         * read "NOT built, because nobody knows where it is: the only measured
         * frame came from a SINGLE-PLAYER replay". The meter was identified on
         * 2026-08-30 as sprite 0, 16x16 at (164,156) - bounty_hud_layout.json's
         * one bottom-right entry with a position and no role, and the drawn
         * glyph in gpudump_arena/inst0/screen1.png sits one pixel inside that
         * box on every side. hud_ping_show defaults to TRUE.
         *
         * THIS WAS THE THIRD COPY OF THAT CLAIM. Two were in ui.h and both were
         * corrected - the second only after the first correction fixed the
         * wrong block. Each time the search was "grep ui.h", and this one lives
         * in another file entirely. Grep the PORT for a claim, not the file you
         * happen to be reading.
         *
         * THE DUMP BELOW STAYS, on its own merits rather than as a hunt for the
         * meter. Any sprite listed here that the layout file does not account
         * for is still worth seeing, and a networked match still shows sprites
         * a single-player replay cannot.
         *
         * Aimed instrumentation is what settled the save gate after three wrong
         * guesses. Same move. */
        std::fprintf(stderr, "[fork] touch-screen sprites this frame:\n");
        for (int i = 0; i < kLsw2OamEntries; ++i) {
            Lsw2ObjSprite obj;
            if (!lsw2_obj_decode(oam, i, &obj) || obj.disabled) continue;
            std::fprintf(stderr,
                         "[fork]   obj %2d at (%3d,%3d) %dx%d pal %d\n",
                         i, obj.x, obj.y, obj.width, obj.height, obj.palette);
        }
    }
}

/* Where the game is drawing the signal meter this frame, from the touch
 * screen's OAM. False when it is not drawing one, which is the honest answer
 * for a frame with no meter rather than a guess at where one would be.
 *
 * Reads the OAM again rather than threading it out of read_rows(): the image
 * is a pointer into the renderer's copy and the second decode costs 128 entry
 * reads, which is nothing beside the blits below. */
bool find_signal_meter(int* x, int* y, int* tile) {
    const uint8_t* oam = nds_vram_renderer_oam(lsw2_screen_engine(kLsw2ScreenTouch));
    if (!oam) return false;

    for (int i = 0; i < kLsw2OamEntries; ++i) {
        Lsw2ObjSprite s;
        if (!lsw2_obj_decode(oam, i, &s) || s.disabled || s.affine) continue;
        if (s.colour256 || s.width != kMeterSize || s.height != kMeterSize) continue;
        if (s.palette != kMeterPalette) continue;
        if (x) *x = s.x;
        if (y) *y = s.y;
        if (tile) *tile = s.tile;
        return true;
    }
    return false;
}

/* The station this machine plays as, from the session rather than assumed.
 *
 * IT WAS HARDCODED TO ZERO, which is right for a host and wrong for everyone
 * else: a client at station 2 would have hidden the HOST's portrait and shown
 * its own, which is the one portrait already on the top screen. */
int local_station() {
    return lsw2_fork_session().local_station();
}

}  // namespace

bool lsw2_arena_hud_active() {
    /* THREE CONDITIONS, AND ALL THREE ARE PART OF THE FEATURE.
     *
     * The fork, because the baseline plays as it always did. The toggle,
     * because the arena HUD is an ImGui enhancement setting. And a match,
     * because it applies during gameplay only - the lobby keeps its touch screen, which in
     * the fork is the Free Play character select. */
    return ui_fork_build() && lsw2_ui_arena_hud_enabled() &&
           lsw2_cheats_in_arena_match() &&
           /* THIS CONSOLE'S PAUSE MENU IS ON THE TOUCH SCREEN: hiding it
            * would leave a menu nobody can see (sessionpause.h). */
           !lsw2_sessionpause_local_menu_up();
}

void lsw2_arena_hud_tick() {
    const bool want = lsw2_arena_hud_active();

    /* AND IT SAYS WHY IT IS OFF, once, and again whenever the answer changes.
     *
     * The arena HUD was long taken for unimplemented and is not - it is built,
     * wired into the present path, and DEFAULTS OFF, as an ImGui enhancement
     * setting may, which makes "not implemented" and
     * "not switched on" look identical from a capture. Three conditions decide
     * it and none of them was visible from outside. */
    {
        static int last = -1;
        const int now = int(want);
        if (now != last) {
            last = now;
            std::fprintf(stderr,
                         "[fork] arena HUD: %s (fork %d, setting %d, "
                         "on an arena map %d, level %d)\n",
                         want ? "ON" : "off", int(ui_fork_build()),
                         int(lsw2_ui_arena_hud_enabled()),
                         int(lsw2_cheats_in_arena_match()),
                         lsw2_cheats_current_level());
        }
    }

    if (want) {
        /* THE TOUCH SCREEN LOSES ITS STARFIELD WHILE THE HUD IS UP, which is
         * acceptable because that screen is HIDDEN for the match:
         * the mirrored pieces then sit on the plain backdrop the design mock-up
         * shows, instead of on stars.
         *
         * THE ENGINE IS RESOLVED EVERY FRAME, because the game can swap which
         * engine drives the touch screen (screenengine.h). The keeper only
         * tells the renderer when the answer changes. */
        const int engine = lsw2_screen_engine(kLsw2ScreenTouch);
        lsw2_hidden_bgs_set(Lsw2HiddenBgOwner::ArenaStarfield, engine,
                            kStarfieldBgs);
        /* AND WHICH OF ITS PIXELS ARE BACKDROP, for the copy (blit_scaled). */
        nds_gpu2d_set_backdrop_mask(engine, true);
        nds_gpu2d_set_backdrop_mask(engine ^ 1, false);
        if (!g_layer_off) {
            g_layer_off = true;
            std::fprintf(stderr,
                         "[fork] arena HUD on; touch-screen starfield (BG2, BG3) "
                         "left out of engine %c's picture\n",
                         engine == 0 ? 'A' : 'B');
        }
        return;
    }

    if (g_layer_off) {
        lsw2_hidden_bgs_set(Lsw2HiddenBgOwner::ArenaStarfield, -1, 0u);
        nds_gpu2d_set_backdrop_mask(0, false);
        nds_gpu2d_set_backdrop_mask(1, false);
        g_layer_off = false;
        g_reported = false;
        std::fprintf(stderr, "[fork] arena HUD off; starfield restored\n");
    }
}

void lsw2_arena_hud_composite(uint32_t* layer, int width) {
    if (!layer || width <= 0 || !lsw2_arena_hud_active()) return;

    /* ANY WIDTH, ANCHORED TO THE FRAME'S OWN CORNERS.
     *
     * This declined every frame that was not 256 wide, on the reasoning that
     * the source rectangles are DS pixels measured on a 256-wide screen. They
     * are - on the TOUCH screen, which is never widened. What depends on the
     * top screen's width is only where each piece lands, and each lands in a
     * corner: the column lower left, the radar upper right, the meter lower
     * right, all measured from the edges of the frame actually presented. The
     * adaptive widescreen moves the game's own HUD out to the widened edges the
     * same way (gpu2d.cpp's HUD anchor). With the refusal in place, a player
     * with adaptive widescreen on had the touch screen hidden and no HUD at all. */
    const uint32_t* touch = nds_gpu2d_framebuffer(1);
    if (!touch) return;
    /* NOTHING UNTIL THE RENDERER HAS SAID WHAT IS BACKDROP: the frame the HUD
     * turns on in was drawn before it asked, and without the mask the black
     * panel would show for that frame. */
    const uint8_t* backdrop = nds_gpu2d_backdrop_mask(1);
    if (!backdrop) return;

    /* THE WINDOW'S CORNERS, NOT THE FRAME'S - anchored to the frame, the HUD
     * did not stay inside the visible screen. In a window narrower than the widened frame the
     * frame's edge columns are off the window, and a radar anchored to them
     * was cut off at the right. In HUD-layer pixels. */
    int vx0 = 0, vx1 = 0;
    lsw2_top_overlay_visible(&vx0, &vx1);
    const int left = vx0 * kLsw2HudLayerScale;
    const int right = vx1 * kLsw2HudLayerScale;
    const int inset = kInset * kLsw2HudLayerScale;

    int rows[kSlots];
    read_rows(rows);

    /* THE OTHER THREE, IN PLAYER ORDER. Slot 0 is this machine - the top screen
     * already carries its portrait and hearts, which is why the column shows
     * the other three players rather than all four.
     *
     * EACH PLAYER OWNS ITS PLACE IN THE COLUMN, AND KEEPS IT WHEN OTHERS LEAVE.
     *
     * This used to advance the column position only when a portrait was
     * actually drawn, so an absent player closed the gap and everyone below
     * shifted up. That reads well until you compare it with what a disconnect
     * should look like: the departed player simply disappears from the HUD
     * and from gameplay.
     *
     * SIMPLY disappears. If the remaining portraits jump up a slot the moment
     * someone drops, what the player sees is the HUD rearranging itself - which
     * is the same visual event as somebody's score suddenly belonging to a
     * different face. The seat now advances whether or not anyone is sitting in
     * it, so a departure leaves a hole and nothing else moves.
     *
     * It is the same reason the portraits are in player order rather than score
     * order (never reordered by who is winning): position in
     * this column is an identity, not a ranking. */
    int seat = 0;
    for (int player = 0; player < kSlots; ++player) {
        if (player == local_station()) continue;

        const int row = rows[player];
        if (row >= 0) {           /* -1 is "nobody in that slot" */
            /* ANCHORED BOTTOM-LEFT, so a scaled column grows upward from
             * the corner it belongs in rather than off the screen. */
            const int sw = scaled(kStripW), sh = scaled(kStripH);
            blit_scaled(touch, backdrop, kStripX, row * kSlotPitch, kStripW,
                        kStripH, layer, width, left + inset,
                        kLayerH - (kSlots - 1 - seat) * sh - inset, sw, sh);
        }
        ++seat;
    }

    /* THE RADAR, from the measured constants and nothing else - 2026-09-12.
     *
     * This read three settings (lsw2_ui_hud_radar) that existed to settle the
     * anchor by dragging sliders in a running match. The anchor is settled -
     * kRadarCx above has the scanlines - and what the settings then did was
     * keep the PRE-measurement (169,71,96) alive in every settings.json that
     * had ever been saved, so the port kept drawing the cropped dial the
     * comment above says was wrong. A measurement of the game's own frame is
     * not a preference; it is a constant, and it lives here. */
    /* ANCHORED TOP-RIGHT, the radar's corner, so growing it moves
     * the far edge inward rather than off-screen. */
    const int rw = scaled(kRadarW), rh = scaled(kRadarH);
    blit_scaled(touch, backdrop, kRadarX, kRadarY, kRadarW, kRadarH,
                layer, width, right - rw - inset, inset, rw, rh);

    /* The arena HUD's signal meter, BOTTOM-RIGHT: the DS ping/signal sprite
     * moves to the top screen's bottom-right corner as well.
     *
     * LOCATED, NOT CONFIGURED. See kMeterPalette for what identifies the
     * sprite and why a fixed rectangle was the wrong shape. The box copied is the
     * sprite's own 16x16 at wherever the game put it this frame; when the game
     * is not drawing it, nothing is drawn here either. That is the same rule
     * as the portrait column: the HUD moves what the cartridge draws and
     * invents nothing.
     *
     * ONE CAVEAT THAT SURVIVES from the old note. gpudump_arena is a
     * single-player replay, so the glyph measured there is the meter's idle
     * appearance; a networked match may light more bars. That changes the
     * picture inside the box, which is copied live, not the box. */
    int px = 0, py = 0, ptile = 0;
    if (find_signal_meter(&px, &py, &ptile)) {
        /* SAY WHERE IT WAS FOUND, once per position. LSW2_PRESENT_SHOT sees
         * the composited frame but cannot say which sprite fed the corner;
         * this line is the traced value a capture can be checked against. The
         * tile is REPORTED, not required - see kMeterPalette. */
        static int said_x = -1, said_y = -1;
        if (px != said_x || py != said_y) {
            said_x = px; said_y = py;
            std::fprintf(stderr,
                         "[fork] signal meter located: touch OAM tile %d pal %d "
                         "%dx%d at (%d,%d) -> bottom right\n",
                         ptile, kMeterPalette, kMeterSize, kMeterSize,
                         px, py);
        }
        const int pd = scaled(kMeterSize);
        blit_scaled(touch, backdrop, px, py, kMeterSize, kMeterSize,
                    layer, width, right - pd - inset, kLayerH - pd - inset, pd,
                    pd);
    }
}
