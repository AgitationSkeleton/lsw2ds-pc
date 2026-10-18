/* See ui.h. */

#include "ui.h"
#include "instancemute.h"   /* the Debug tab's second-instance mutes */
#include "widescreen.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "backends/imgui_impl_opengl3.h"

#include "arenahud.h"
#include "cheats.h"
#include "customization.h"
#include "lasthunter.h"
#include "enhanced.h"
#include "countboot.h"
#include "partapply.h"
#include "forksession.h"
#include "fork.h"
#include "glpresent.h"
#include "levels.h"
#include "selectwidget.h"
#include "settings_store.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool g_visible = false;
/* Which imgui renderer backend is live. Derived once from whether ui_init was
 * given an SDL_Renderer, so it can never disagree with the window that was
 * actually created. */
bool g_gl_backend = false;
double g_fps = 0.0;
uint64_t g_frames = 0;
uint64_t g_underruns = 0;
void* g_touch_texture = nullptr;
SDL_Rect g_floating_rect{};
bool g_floating_shown = false;

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;

/* THE BINDING TABLE, and the single place the DS bit layout is written down on
 * the host side. Defaults are the common emulator convention so muscle memory
 * from any DS emulator carries over. */
UiBinding g_bindings[] = {
    {"A",           1u << 0,  SDL_SCANCODE_Z,         SDL_CONTROLLER_BUTTON_A},
    {"B",           1u << 1,  SDL_SCANCODE_X,         SDL_CONTROLLER_BUTTON_B},
    {"X",           1u << 10, SDL_SCANCODE_S,         SDL_CONTROLLER_BUTTON_X},
    {"Y",           1u << 11, SDL_SCANCODE_A,         SDL_CONTROLLER_BUTTON_Y},
    {"L",           1u << 9,  SDL_SCANCODE_Q,         SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"R",           1u << 8,  SDL_SCANCODE_W,         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"Start",       1u << 3,  SDL_SCANCODE_RETURN,    SDL_CONTROLLER_BUTTON_START},
    {"Select",      1u << 2,  SDL_SCANCODE_BACKSPACE, SDL_CONTROLLER_BUTTON_BACK},
    {"D-pad Up",    1u << 6,  SDL_SCANCODE_UP,        SDL_CONTROLLER_BUTTON_DPAD_UP},
    {"D-pad Down",  1u << 7,  SDL_SCANCODE_DOWN,      SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"D-pad Left",  1u << 5,  SDL_SCANCODE_LEFT,      SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {"D-pad Right", 1u << 4,  SDL_SCANCODE_RIGHT,     SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    /* The free camera, bindable from the input screen: no DS bit - the
     * frontend turns a held one into the same camera motion as the right stick
     * at full deflection (frontend.cpp, sample_camera), so a keyboard player
     * can orbit too. The pad's analog stick needs no row. */
    {"Camera Left",  0u,      SDL_SCANCODE_J,         -1},
    {"Camera Right", 0u,      SDL_SCANCODE_L,         -1},
    {"Camera Up",    0u,      SDL_SCANCODE_I,         -1},
    {"Camera Down",  0u,      SDL_SCANCODE_K,         -1},
};
constexpr int kBindingCount = int(sizeof g_bindings / sizeof *g_bindings);

const char* g_unlock_note = nullptr;  /* what the unlock button last did */
int g_capture_index = -1;   /* which binding is waiting for a key */
bool g_capture_pad = false; /* waiting for a pad button rather than a key */
/* The Panel scale slider moved this frame: ui_draw resizes the floating touch
 * panel to the preset and clears this. A request rather than a value, for the
 * same reason UiSettings::applied_scale is - the remembered dragged size is
 * what the panel normally follows, and only an explicit move should beat it. */
bool g_panel_resize = false;

/* THE FLOATING TOUCH PANEL ONLY COMES IN THE DS SCREEN'S SHAPE.
 *
 * The floating touch-screen panel is constrained to the DS's 4:3 aspect with
 * no stretching, as LEGO Battles' top-screen panel is (legobattles-decomp port/runner/src/lb_overlay.cpp,
 * top_screen_size_cb; this is that callback, fitted to this imgui).
 *
 * The panel resized to any shape and the picture was FITTED inside it, so every
 * shape but 4:3 was part picture and part bare panel. Even the Panel scale
 * preset was off: it added the title bar but not WindowPadding, so at 1x the
 * content region was 240x176 and the picture 234.7x176.
 *
 * So the WINDOW is constrained, because that is where the empty space came from.
 * imgui runs this callback on every size it is about to use - the re-check at
 * the top of every Begin (which is how a remembered size reaches it on the first
 * frame) and every drag of a grip or border (CalcWindowSizeAfterConstraint in
 * imgui.cpp) - and it returns a size whose content region is the picture.
 *
 * WHAT SURROUNDS THE PICTURE, from the live style so the UI scale is honoured:
 * the title bar, GetFrameHeight() (imgui sizes it FontSize + 2 * FramePadding.y),
 * and WindowPadding on each side. Image() adds nothing here: this imgui (1.91.5)
 * pads an image only for a visible border colour, and none is passed.
 *
 * WHOLE STEPS. imgui truncates the size to whole pixels and at 4:3 only a width
 * that is a multiple of 4 has a whole-pixel height, so the size is a count of
 * 4x3 steps; native is 64.
 *
 * WHICH AXIS DRIVES. An edge drag follows that edge; imgui sets the resize cursor
 * for the grip or border it holds before it asks for the size, so the cursor
 * names it. A corner drag puts the corner at the 4:3 point nearest the mouse -
 * the projection onto the diagonal, which cannot oscillate the way "whichever
 * axis moved more" does (LB measured 392x317 / 324x266 alternating). No change
 * at all - the per-frame re-check, and so a remembered size - keeps the largest
 * picture that fits inside, and never grows the panel by itself.
 *
 * THE FLOOR AND THE CAP are applied in steps too, because a per-axis clamp
 * afterwards is what would bend the ratio: at least quarter size (and imgui's
 * WindowMinSize, which it applies after this), at most the host window less
 * 8 px, skipped while SDL reports a minimised window as 0x0.
 *
 * IT MUST STAY A PURE FUNCTION OF ITS INPUTS: imgui also calls it for an
 * auto-fit estimate it throws away, so state kept here would advance on calls
 * that resize nothing. */
void touch_panel_size_cb(ImGuiSizeCallbackData* data) {
    const ImGuiStyle& style = ImGui::GetStyle();
    constexpr int kSteps = 64;   /* 256x192 = 64 steps of 4x3 */
    constexpr float kUw = float(kScreenWidth / kSteps);
    constexpr float kUh = float(kScreenHeight / kSteps);

    const float edge_w = 2.0f * style.WindowPadding.x;
    const float edge_h = ImGui::GetFrameHeight() + 2.0f * style.WindowPadding.y;

    const ImVec2 want = data->DesiredSize;
    const ImVec2 have = data->CurrentSize;
    const float kx = (want.x - edge_w) / kUw;   /* the desired size, in steps */
    const float ky = (want.y - edge_h) / kUh;
    const bool moved_x = std::fabs(want.x - have.x) >= 0.5f;
    const bool moved_y = std::fabs(want.y - have.y) >= 0.5f;

    float k;
    if (!moved_x && !moved_y) {
        k = std::floor(std::min(kx, ky) + 0.001f);
    } else {
        const float corner = (kx * kUw * kUw + ky * kUh * kUh) /
                             (kUw * kUw + kUh * kUh);
        const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
        if (cursor == ImGuiMouseCursor_ResizeEW)
            k = kx;
        else if (cursor == ImGuiMouseCursor_ResizeNS)
            k = ky;
        else if (cursor == ImGuiMouseCursor_ResizeNWSE ||
                 cursor == ImGuiMouseCursor_ResizeNESW)
            k = corner;
        else
            /* Not a mouse drag (keyboard resize, the auto-fit estimate, the
             * Panel scale preset): follow whichever component changed. */
            k = !moved_y ? kx : !moved_x ? ky : corner;
        k = std::floor(k + 0.5f);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float lim_w = vp->WorkSize.x - 8.0f;
    const float lim_h = vp->WorkSize.y - 8.0f;
    if (lim_w > 0.0f && lim_h > 0.0f) {
        const float kmax = std::floor(std::min((lim_w - edge_w) / kUw,
                                               (lim_h - edge_h) / kUh));
        if (k > kmax) k = kmax;
    }
    float kmin = std::ceil(float(kSteps) * 0.25f);
    kmin = std::max(kmin, std::ceil((style.WindowMinSize.x - edge_w) / kUw));
    kmin = std::max(kmin, std::ceil((style.WindowMinSize.y - edge_h) / kUh));
    if (k < kmin) k = kmin;

    data->DesiredSize = ImVec2(k * kUw + edge_w, k * kUh + edge_h);
}

void video_tab(UiSettings& s) {
    ImGui::TextUnformatted("Presentation");
    ImGui::Separator();

    ImGui::Checkbox("Preserve aspect ratio", &s.preserve_aspect);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Letterbox rather than stretch.\n\n"
            "This changes how the frame is SHOWN. It never changes the\n"
            "projection the game produced - the guest still renders 256x192\n"
            "and still believes it is on a DS.");
    }

    ImGui::BeginDisabled(!s.preserve_aspect);
    ImGui::Checkbox("Integer scale", &s.integer_scale);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Snap to whole multiples of the guest pixel so every\n"
                          "one is the same size. Leaves a wider border.");
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextUnformatted("Widescreen");
    ImGui::Separator();
    ImGui::Checkbox("Adaptive widescreen", &s.adaptive_widescreen);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Widen the 3D viewport to match the window instead of\n"
            "letterboxing, so more of the world is visible.\n\n"
            "Under OpenGL the width is fixed when the renderer starts,\n"
            "so this takes effect on the next launch.");
    }
    /* SAY "RESTART" BESIDE THE CHECKBOX, not under the slider. Under OpenGL
     * the raster width is settled before the compute renderer exists and
     * cannot move afterwards (widescreen.cpp locks it), so a click here is a
     * request for the next launch. The note used to sit at the bottom of the
     * slider block, where a player who only ever touched the checkbox would
     * not read it and would report the checkbox as doing nothing. */
    if (lsw2_widescreen_locked()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(applies on restart)");
    }
    ImGui::BeginDisabled(!s.adaptive_widescreen);
    /* The ceiling comes from the renderer, not from a number picked here. A
     * slider that offers widths the rasteriser refuses looks like a setting
     * that does nothing. */
    const int max_extra = lsw2_widescreen_max_extra();
    if (s.max_extra_width > max_extra) s.max_extra_width = max_extra;
    ImGui::SliderInt("Max extra width", &s.max_extra_width, 0, max_extra, "%d px");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "How far past the native 256 px the 3D raster may widen.\n\n"
            "The width actually used is whatever makes the picture fill\n"
            "the window exactly; this is only the ceiling. Lower it if a\n"
            "very wide view shows scenery the level was never built for.\n\n"
            "The rasteriser cannot go past 448 px total, which is 2.33:1 -\n"
            "wider than 21:9. On a window wider than that the picture stops\n"
            "growing and the remainder stays letterboxed.");
    }
    ImGui::TextDisabled("raster now: %d x 192", lsw2_widescreen_width());
    if (lsw2_widescreen_locked()) {
        /* The width cannot follow the window on this path, so say what it is
         * following instead. Without this the bars on a differently-shaped
         * window look like a bug rather than a consequence. */
        ImGui::TextDisabled("%s", "fixed for this session: sized to the display");
    }
    ImGui::EndDisabled();
    if (s.adaptive_widescreen) {
        /* THIS TEXT HAS BEEN WRONG TWICE, so it now says exactly what was seen.
         *
         * It first warned that scenery would cull, from reasoning. A paused
         * scene at 256 and 448 px then showed level geometry rendering fine
         * across the full width, so the warning was replaced with the opposite
         * claim. Both were too broad: static geometry is clipped by the GPU,
         * whose volume follows the widened raster, while ACTORS are tested by
         * the game against the original screen bounds and vanish at the old
         * edges. That only shows up in play, which is where it was reported.
         *
         * So the warning is back, narrowed to what actually breaks. See
         * widescreen.cpp for the fix this is waiting on. */
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
            "OBJECTS to the left and right of centre pop in and out");
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
            "until they are near the middle. Level geometry is fine.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Rendering quality");
    ImGui::Separator();

    /* WHY THE UI SAYS "RESTART" AND MEANS IT. The internal scale is compiled
     * into the compute shaders and sizes every framebuffer, so the framework
     * refuses to change it under a live renderer; and SDL_WINDOW_OPENGL cannot
     * be added to a window that already exists. A slider that appeared to work
     * and quietly did nothing would be worse than one that says so. */
    const bool gl_live = lsw2_gl_active();
    ImGui::Checkbox("OpenGL renderer", &s.opengl_renderer);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Render the 3D layer on the GPU and present it directly.\n\n"
            "This is what makes the two settings below possible: the extra\n"
            "detail they produce exists only in a GPU texture and never\n"
            "reaches the CPU framebuffer the software path presents from.\n\n"
            "Needs OpenGL 4.3. If it is unavailable the port falls back to\n"
            "the software path and says so on startup.");
    }

    ImGui::BeginDisabled(!s.opengl_renderer);
    ImGui::SliderInt("3D resolution", &s.internal_resolution, 1, 4, "%dx");
    /* A KNOWN DEFECT, SAID OUT LOUD. Either of the two settings below above 1x
     * turns on the compute renderer's HD compositing path, and that path
     * renders the Arena Lobby's hologram as a solid white shape instead of a
     * translucent one. Bisected: 1x/1x is correct, 2x/1x and 1x/2x both break
     * it, and the software renderer is correct at any setting - so it is the
     * HD path, not the upscaling ratio.
     *
     * Shown here rather than buried in a document because the alternative is a
     * player turning on a quality setting and quietly getting a broken picture. */
    if (s.opengl_renderer &&
        (s.internal_resolution > 1 || s.texture_upscale > 1)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
            "Known: above 1x, the Cantina hologram renders as a");
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
            "solid white shape. Set both to 1x to avoid it.");
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Render the 3D layer at this multiple of 256x192.\n\n"
            "This is the setting that fixes chunkiness at large window\n"
            "sizes. The DS renders 256x192, so scaling up magnifies every\n"
            "polygon edge; rendering at 3x means there is real detail to\n"
            "show instead of bigger pixels.\n\n"
            "The guest is unaffected - it still reads the native surface.");
    }
    /* NAMED FOR WHAT IT DRIVES. This combo feeds nds_texture_upscale_set_factor
     * - an xBR resample of each decoded texture at 2x or 4x - and it was
     * labelled "Texture filtering", which is the name of the checkbox below
     * that does something else. A player reading "filtering" here and
     * "Linear filtering" there had two controls with one name. */
    const char* upscales[] = {"Off", "2x", "4x"};
    int upscale_index = s.texture_upscale >= 4 ? 2 : s.texture_upscale >= 2 ? 1 : 0;
    if (ImGui::Combo("Texture upscale", &upscale_index, upscales, 3))
        s.texture_upscale = upscale_index == 2 ? 4 : upscale_index == 1 ? 2 : 1;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Resample each decoded DS texture once (xBR), when it is\n"
            "first used. Restart to apply.\n\n"
            "Worth turning on alongside a higher 3D resolution: without it\n"
            "the geometry sharpens but the textures on it stay blocky.");
    }
    ImGui::EndDisabled();

    /* Report what is ACTUALLY running next to what is selected, so a failed
     * OpenGL start is visible here and not only in a startup log nobody reads. */
    if (s.opengl_renderer != gl_live) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
            gl_live ? "Running on OpenGL - restart to go back to software."
                    : "Running on software - restart to apply, or OpenGL 4.3");
        if (!gl_live)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
                "is unavailable here (the startup log says which).");
    } else {
        ImGui::TextDisabled("now: %s%s", gl_live ? "OpenGL" : "software",
                            gl_live ? "" : " (3D resolution unavailable)");
    }

    ImGui::Spacing();
    ImGui::Checkbox("Linear filtering", &s.linear_filter);
    /* WHAT IT REACHES, said where the player can read it. The SDL present
     * path honours this on both screens (SDL_SetTextureScaleMode) and the
     * floating panel's GL texture does too (lsw2_gl_touch_texture). The
     * OpenGL TOP screen does not: nds_compute_host_present_top samples
     * integer textures with texelFetch inside its own shader and offers no
     * filter switch, so under the default renderer this checkbox changes the
     * touch screen only. Saying so here is the difference between a setting
     * that is honest about its reach and one that looks broken. */
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off is nearest-neighbour, which is usually what you\n"
                          "want for a DS: this is pixel art.\n\n"
                          "Under OpenGL the top screen is always nearest - the\n"
                          "GPU presenter samples exact texels - so this applies\n"
                          "to the touch screen and the floating panel.");
    }
    if (gl_live) {
        ImGui::SameLine();
        ImGui::TextDisabled("(touch screen only under OpenGL)");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Gameplay window size");
    ImGui::Separator();
    ImGui::TextDisabled("Applies to the top screen only; the touch window keeps "
                        "its own size.");
    ImGui::SliderInt("Scale", &s.window_scale, 1, 6, "%dx");
    ImGui::SameLine();
    if (ImGui::Button("Apply")) s.applied_scale = s.window_scale;
    ImGui::TextDisabled("%d x %d per screen",
                        kScreenWidth * s.window_scale,
                        kScreenHeight * s.window_scale);

    ImGui::Spacing();
    ImGui::TextUnformatted("Presets");
    struct { const char* name; int scale; } presets[] = {
        {"1x  256x192",  1}, {"2x  512x384",  2}, {"3x  768x576",  3},
        {"4x 1024x768",  4}, {"5x 1280x960",  5}, {"6x 1536x1152", 6},
    };
    for (int i = 0; i < 6; ++i) {
        if (i % 3) ImGui::SameLine();
        if (ImGui::Button(presets[i].name, ImVec2(118, 0))) {
            s.window_scale = presets[i].scale;
            s.applied_scale = presets[i].scale;
        }
    }
    ImGui::TextWrapped(
        "The window stays freely resizable; a preset just snaps it to an exact "
        "multiple so every guest pixel is the same size.");
}

void input_tab(UiSettings& s) {
    ImGui::TextUnformatted("Analog stick");
    ImGui::Separator();
    ImGui::Checkbox("Stick as D-pad", &s.stick_as_dpad);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "The DS has no analog input, so a stick can only drive the same\n"
            "four direction bits the D-pad does - there is nothing finer to\n"
            "send. This is what every DS emulator does.\n\n"
            "The pad's buttons were read from the start and its sticks were\n"
            "not read at all, which is why a controller only half-worked.");
    }
    ImGui::BeginDisabled(!s.stick_as_dpad);
    ImGui::SliderInt("Deadzone", &s.stick_deadzone, 5, 90, "%d%%");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "How far the stick must move before it counts as a direction.\n"
            "Raise it if the character wanders on its own - a worn stick\n"
            "rests off centre.");
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextUnformatted("Enhancements");
    ImGui::Separator();
    /* The optional enhancements, and they belong in BOTH builds - they are
     * specified for both, and the baseline's customize panel applies hats
     * through the same function. */
    ImGui::Checkbox("Player-colour ring", &s.enhanced_slot_colour);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Recolour the top-screen portrait ring to this player's\n"
            "colour, in the hunter lobby and the co-op netgame.");
    }

    ImGui::Checkbox("Portrait follows character", &s.enhanced_portrait);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Changing character on the Free Play screen leaves the top-screen\n"
            "portrait showing the old one until you tag a partner and back.\n\n"
            "The touch path swaps the party without asking the game to re-read\n"
            "it; the d-pad path does. On, the same refresh runs either way.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Enhanced arena controls");
    ImGui::Separator();
    ImGui::Checkbox("Enhanced controls", &s.enhanced_controls);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Free camera on the right stick and ordinary movement on the\n"
            "left, in arena gameplay only - the lobby keeps its usual\n"
            "controls.\n\n"
            "Turning this OFF always wins: you get tank controls even in a\n"
            "match whose host has Enhanced enabled. The reverse is not true -\n"
            "a host who disallows it still disallows it.");
    }
    ImGui::BeginDisabled(!s.enhanced_controls);
    ImGui::Checkbox("Invert camera X", &s.right_stick_invert_x);
    /* LIVE SINCE 2026-09-23, and the greyed version's reason retracted.
     *
     * This was disabled with a measured reason: the VIEW RECORD at
     * [0x021092F4] + 0x240 carries one angle, the yaw at +0x6A, and the game's
     * wire sync (0x0205EDA0 -> 0x0205EDC0) sends one halfword - so there was no
     * pitch to invert. Every word of that is still true of the record. The
     * record was the wrong object: it is what the game SENDS about a camera,
     * not the camera. The enhanced camera is now the game's own mode 4 on the
     * local camera object (enhanced.cpp), which sits at an offset from the
     * player - and the offset has a height, which the stick's vertical axis
     * moves. That is what this inverts. */
    ImGui::Checkbox("Invert camera Y", &s.right_stick_invert_y);
    ImGui::SliderInt("Camera deadzone", &s.right_stick_deadzone, 5, 90, "%d%%");
    ImGui::SliderInt("Camera turn speed", &s.right_stick_speed_x, 25, 600, "%d%%");
    ImGui::SliderInt("Camera height speed", &s.right_stick_speed_y, 25, 600, "%d%%");
    ImGui::EndDisabled();
    /* SAY WHAT IS LIVE, and say it precisely - this is the copy the player
     * reads. It said the checkbox gives movement "the camera's heading instead
     * of the ship's" by writing the actor's view index, and that the stick
     * writes the view record's yaw. Neither ever moved anything in an arena:
     * the index is never the sentinel there, and the record is overwritten by
     * the next exchange (enhanced.cpp has the measurements). The checkbox now
     * switches the arena's per-player camera mode through the game's own
     * message, and the stick steers that camera. */
    ImGui::TextDisabled("In arena matches: the left stick or d-pad moves you");
    ImGui::TextDisabled("relative to the camera, and the right stick (or the");
    ImGui::TextDisabled("Camera bindings below) turns it around you and raises");
    ImGui::TextDisabled("or lowers it. The lobby keeps the usual controls.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Buttons");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Click a binding to change it, then press the key or button you want. "
        "Escape cancels.");
    ImGui::Spacing();

    if (ImGui::BeginTable("bindings", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Gamepad");
        ImGui::TableHeadersRow();

        for (int i = 0; i < kBindingCount; ++i) {
            UiBinding& b = g_bindings[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.action);

            ImGui::TableNextColumn();
            const bool waiting_key = (g_capture_index == i && !g_capture_pad);
            const char* key_label = waiting_key
                ? "press a key..."
                : (b.scancode >= 0 ? SDL_GetScancodeName(SDL_Scancode(b.scancode))
                                   : "unbound");
            if (ImGui::Button(*key_label ? key_label : "unbound",
                              ImVec2(-FLT_MIN, 0))) {
                g_capture_index = i;
                g_capture_pad = false;
            }

            ImGui::TableNextColumn();
            const bool waiting_pad = (g_capture_index == i && g_capture_pad);
            char pad_label[32];
            if (waiting_pad) {
                std::snprintf(pad_label, sizeof pad_label, "press a button...");
            } else if (b.pad_button >= 0) {
                const char* n = SDL_GameControllerGetStringForButton(
                    SDL_GameControllerButton(b.pad_button));
                std::snprintf(pad_label, sizeof pad_label, "%s", n ? n : "?");
            } else {
                std::snprintf(pad_label, sizeof pad_label, "unbound");
            }
            if (ImGui::Button(pad_label, ImVec2(-FLT_MIN, 0))) {
                g_capture_index = i;
                g_capture_pad = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults")) {
        static const int keys[kBindingCount] = {
            SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_S, SDL_SCANCODE_A,
            SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_RETURN,
            SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
            SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
        for (int i = 0; i < kBindingCount; ++i) g_bindings[i].scancode = keys[i];
        g_capture_index = -1;
    }

    ImGui::Spacing();
    /* ONE LINE, AND A TRUE ONE. The second line here said right-stick camera
     * control was "planned for the Bounty Hunter fork", forty lines below the
     * controls that drive it. A stale promise under a working control reads
     * as the control not working. */
    ImGui::TextDisabled("The stylus is the mouse, on the touch screen or the "
                        "floating panel.");
}

void touch_tab(UiSettings& s) {
    /* THE LOBBY CHARACTER SELECT LIVES HERE, not on the Multiplayer tab.
     *
     * This group was on Multiplayer and it is not a networking setting -
     * it decides what the LOWER SCREEN SHOWS in the lobby, which is what
     * this tab is for. The Multiplayer tab holds only settings that have
     * no sensible in-game home - the player name and the master server
     * address - so anything else sitting on it was miscategorised by that
     * rule as well as by feel. */
    ImGui::Spacing();
    ImGui::SeparatorText("Lobby character select");
    /* A flag for whether the select shows all characters or only the bounty
     * hunters, defaulting to only bounty hunters. It is not specified as a game
     * menu row - so by the rule above it lives here. It is not a match setting;
     * the host's own Roster row is, and that one is in the game's host menu. */
    ImGui::Checkbox("Free Play select",
                    &s.bh_freeplay_select);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "The lobby's touch screen shows the Free Play character select,\n"
            "so a hunter can be picked and customized before joining.\n\n"
            "Turning it off restores the stock Party Members tab - which is\n"
            "the way back if the roster screen misbehaves in the lobby.");
    }
    /* GREYED WHILE A SESSION IS UP, because the host owns this then, and a
     * control that still moves while being ignored is worse than one that
     * refuses. See lsw2_hle_fork_hunters_only() for the precedence. */
    const bool host_owns_roster = lsw2_fork_session().in_session();
    ImGui::BeginDisabled(!s.bh_freeplay_select || host_owns_roster);
    ImGui::Checkbox("Bounty Hunters only", &s.bh_roster_only);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            host_owns_roster
              ? "The host's Roster setting decides this during a session.\n\n"
                "A client cannot widen its own roster - that would mean picking\n"
                "from twelve pages while everyone else picks from two."
              : "Restricts the select to the two Bounty Hunters pages, cycling\n"
                "between them in both directions.\n\n"
                "Off shows all twelve character pages.");
    }
    if (host_owns_roster) {
        ImGui::SameLine();
        ImGui::TextDisabled("(host decides)");
    }

    ImGui::Spacing();

    /* The arena match HUD on the top screen. Grouped with the touch settings
     * because it REPLACES them:
     * during a match the floating panel is not drawn at all. */
    ImGui::TextUnformatted("Match HUD on the top screen");
    ImGui::Separator();
    ImGui::Checkbox("Arena HUD overlay", &s.arena_hud_overlay);
    /* THE TOOLTIP SITS ON THE CHECKBOX IT DESCRIBES. It used to be sixty lines
     * down, after the player-count bootstrap button - the probe buttons were inserted
     * between the checkbox and its IsItemHovered, so imgui attached this text
     * to the last button drawn and the checkbox had none. Same shape as a
     * comment drifting away from its code, with the same fix. */
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "During an arena match, mirrors the other players' column to the\n"
            "lower left, the radar to the top right and the signal meter to\n"
            "the bottom right, and hides the touch screen.\n\n"
            "Only during a match - the lobby keeps its touch screen, which\n"
            "is where the character select lives.\n\n"
            "Copied from the real touch screen, in player order rather than\n"
            "score order - the game draws them, this only moves them.");
    }

    /* NO SOURCE-RECTANGLE SLIDERS ANY MORE.
     *
     * Two tree nodes stood here: "Radar source" (centre and size) and "Ping
     * meter source" (an on/off box plus its rectangle). They were built to
     * settle where on the touch screen each element lives, by dragging a
     * slider during a match, and both questions have been settled by
     * measurement since - so what the sliders did in practice was PERSIST the
     * pre-measurement values. Every install that had ever saved carried radar
     * (169,71,96) and the meter switched off in settings.json, the corrected
     * defaults never reached it, and the tooltip above described a meter the
     * player was not seeing. That is exactly the kind of control to avoid:
     * one that looks like a setting and is not.
     *
     * The design rule for the HUD: the switch lives in imgui, the pixels come
     * from the game. arenahud.cpp now locates the signal meter from the game's own OAM
     * entry on every frame and mirrors the radar's measured panel as a constant,
     * so the toggle above is the whole of what this tab has to offer for the
     * arena HUD. The
     * old settings keys are left untouched in the file by the store. */
    ImGui::BeginDisabled(!s.arena_hud_overlay);
    ImGui::SliderFloat("HUD size", &s.arena_hud_scale, 0.5f, 1.5f, "%.2fx");
    ImGui::EndDisabled();
    if (s.arena_hud_overlay) {
        ImGui::TextDisabled("The floating touch panel is hidden during matches.");
    }

    /* THE THREE PROBE BUTTONS THAT SAT HERE ARE ON THE DEBUG TAB NOW. They
     * are guest-call probes that print a log line - the camera yaw, the gear
     * applier, the count bootstrap - and this tab is for what the lower screen
     * shows. A probe between the HUD toggle and the touch panel is a control
     * in the wrong category. */
    ImGui::Spacing();

    ImGui::TextWrapped(
        "The DS touch screen normally gets its own window. It can instead "
        "float inside this one as a movable panel.");
    ImGui::Spacing();

    ImGui::Checkbox("Floating touch panel", &s.floating_touch);
    ImGui::BeginDisabled(!s.floating_touch);
    /* A MOVED SLIDER IS A RESIZE REQUEST. The panel remembers the size it was
     * dragged to and that memory outranked this slider on every frame after
     * the first, so once the panel had been shown the slider changed nothing
     * (see the note over SetNextWindowSize in ui_draw). Moving it now asks
     * for the preset size, the same way the window Scale slider's Apply does
     * for the top window. */
    if (ImGui::SliderFloat("Panel scale", &s.floating_touch_scale, 0.5f, 3.0f,
                           "%.2fx"))
        g_panel_resize = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Resizes the panel to this multiple of 256x192.\n"
                          "Dragging its corner afterwards is remembered too.");
    }
    ImGui::Checkbox("Hide touch window", &s.floating_touch_only);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled(
        "The panel is clickable: clicks map back to stylus coordinates through\n"
        "the same transform used to draw it.");
}

void debug_tab(UiSettings& s) {
    ImGui::Text("%.1f FPS", g_fps);
    ImGui::Text("%llu frames presented", (unsigned long long)g_frames);
    if (g_underruns)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "%llu audio underruns", (unsigned long long)g_underruns);
    else
        ImGui::TextDisabled("no audio underruns");

    ImGui::Spacing();
    ImGui::Checkbox("ImGui metrics", &s.show_metrics);

    /* Several clients on one machine, heard once. */
    ImGui::Spacing();
    ImGui::TextUnformatted("When another copy of the game is already running");
    ImGui::Separator();
    ImGui::Checkbox("Mute music", &s.mute_music_second_instance);
    ImGui::SameLine();
    ImGui::Checkbox("Mute all sound", &s.mute_sound_second_instance);
    ImGui::TextDisabled("%s", lsw2_instance_is_secondary()
                                  ? "This window is a second copy - these apply to it."
                                  : "This window is the first copy - these do not apply to it.");

    /* THE FORK'S GUEST-CALL PROBES, moved here from the Touch tab.
     *
     * Each is one call into the cartridge's own code, armed by the button and
     * fired from the fork tick, and each reports with a log line rather than
     * changing anything a player sees. That is a debugging instrument, so it
     * lives on the tab that says so. Fork only, because every one of them
     * refuses in the baseline and a button that only prints "fork build only"
     * is noise. Labels are the noun; the tooltip carries the explanation. */
    if (ui_fork_build()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Probes");
        ImGui::Separator();
        if (ImGui::Button("Probe camera yaw")) lsw2_enhanced_probe_camera();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "During a match, writes a quarter turn into the camera's yaw\n"
                "and reports next frame whether it stuck. Outside a match it\n"
                "refuses and says so.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Probe gear applier")) lsw2_partapply_arm();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("One call to the game's own part attacher, and a\n"
                              "log line saying whether it returned.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Seat players")) lsw2_countboot_arm();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Runs the cartridge's own player-count bootstrap\n"
                              "with the session's real count.");
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Remote control");
    ImGui::Separator();
    ImGui::TextWrapped(
        "This build serves a debug surface on 127.0.0.1:19842 - framebuffer "
        "capture, memory and register reads, and input injection.");
    ImGui::TextDisabled("tools/pilot.py shot out.png");
    ImGui::TextDisabled("tools/pilot.py press start");
}

/* The fork's networking configuration, and for now ONLY the
 * configuration - nothing here connects to anything yet. It exists first
 * because the settings have to be persisted and shown before the transport is
 * written, and because the master server host is already known.
 *
 * FORK-ONLY, deliberately. The base recomp is meant to stay close to the
 * original game, so the baseline gets no Multiplayer tab at all rather than a
 * tab full of controls that do nothing. */
void multiplayer_tab(UiSettings& s) {
    /* ONE FIELD WIDTH FOR THE WHOLE FORM, so nothing on it is misaligned.
     *
     * imgui draws a label to the RIGHT of its widget, so with no explicit
     * width every field takes its own and the labels - "Player name",
     * "Host", "Port", "Host or IP", "Listen port" - start at five different
     * x positions down a single column. Pinning one width lines them all up.
     * Sized in font units rather than pixels so it survives the UI scale
     * settings this panel also offers. */
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.0f);
    /* WHAT BELONGS ON THIS TAB, and nothing else.
     *
     * The imgui Multiplayer tab is limited to settings with no sensible
     * in-game home: the player/DS name, the master server address, and
     * whatever else the design does not place in the game's own UI.
     *
     * So: the name, the directory address, the listen port. Every match
     * setting - Roster, Extras, Controls, Customization, Music, LOBBY TIME -
     * is an IN-GAME menu row and is deliberately absent here. */
    Lsw2ForkSession& session = lsw2_fork_session();

    ImGui::TextDisabled("Name and server address only. Match settings are in the");
    ImGui::TextDisabled("game, on the host menu.");
    ImGui::Separator();

    ImGui::InputText("Player name", s.mp.player_name, sizeof s.mp.player_name);
    ImGui::TextDisabled("Shown to other players and on the master server list.");

    ImGui::Spacing();
    ImGui::SeparatorText("Master server");
    ImGui::InputText("Host", s.mp.master_host, sizeof s.mp.master_host);
    ImGui::InputInt("Port", &s.mp.master_port);
    /* A port outside the legal range is not a preference, it is a typo, and
     * clamping here means nothing downstream has to check it. Port 0 is legal
     * to bind but meaningless to connect to, so the floor is 1. */
    s.mp.master_port = std::clamp(s.mp.master_port, 1, 65535);

    ImGui::Spacing();
    if (ImGui::Button("Use 127.0.0.1")) {
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host, "127.0.0.1");
        s.mp.master_port = 28553;
    }
    ImGui::SameLine();
    if (ImGui::Button("Use lsw2server.viosarcade.xyz")) {
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host,
                      "lsw2server.viosarcade.xyz");
        s.mp.master_port = 80;
    }
    ImGui::TextDisabled("Local for testing on this machine; the real host otherwise.");

    ImGui::Spacing();
    ImGui::SeparatorText("Direct connect");
    /* THE OTHER HALF OF THE 2026-08-27 DECISION. The room code is entered in
     * the game, on a d-pad, because four characters is what a d-pad can do
     * without an invented keyboard. A full address is not, so it lives here and
     * the in-game screen offers it as a second way in. */
    ImGui::InputText("Host or IP", s.mp.direct_host, sizeof s.mp.direct_host);
    ImGui::InputInt("Host port", &s.mp.direct_port);
    s.mp.direct_port = std::clamp(s.mp.direct_port, 1, 65535);
    ImGui::TextDisabled("Offered in the game's Join screen as \"Join <address>\".");

    ImGui::Spacing();
    ImGui::SeparatorText("This machine");
    ImGui::InputInt("Listen port", &s.mp.listen_port);
    s.mp.listen_port = std::clamp(s.mp.listen_port, 1, 65535);
    ImGui::TextDisabled("The port other players connect to when you host.");
    ImGui::Spacing();
    ImGui::Text("Will connect to:  %s:%d", s.mp.master_host, s.mp.master_port);

    /* PUSH THE SETTINGS DOWN EVERY FRAME rather than on a button.
     *
     * A tab that needs an Apply press is a tab someone forgets to press, and
     * the values are three copies rather than a computation - pushing them is
     * cheaper than explaining when they take effect. */
    session.configure(s.mp.player_name, s.mp.master_host, s.mp.master_port,
                      s.mp.listen_port);
    session.configure_direct(s.mp.direct_host, s.mp.direct_port);


    ImGui::SeparatorText("Session");
    switch (session.state()) {
    case Lsw2SessionState::Offline:
        ImGui::TextUnformatted("Offline.");
        ImGui::TextDisabled("Host or join from the arena menu inside the game.");
        break;
    case Lsw2SessionState::Hosting:
        ImGui::Text("Hosting on port %d - %d/4 players", s.mp.listen_port,
                    session.player_count());
        if (session.console().room_code[0])
            ImGui::Text("Room code: %s", session.console().room_code);
        else
            ImGui::TextDisabled("No room code - the directory has not issued one.");
        break;
    case Lsw2SessionState::Joining:
        ImGui::TextUnformatted("Connecting...");
        break;
    case Lsw2SessionState::Joined:
        ImGui::Text("Joined - %d/4 players", session.player_count());
        break;
    case Lsw2SessionState::InMatch:
        ImGui::TextUnformatted("In a match.");
        break;
    }
    if (session.last_error() && *session.last_error())
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s",
                           session.last_error());
    ImGui::PopItemWidth();
}

/* THE BASELINE'S CO-OP TAB - the baseline's online co-op configuration.
 *
 * The surface is divided explicitly, and the division is the reverse of the
 * fork's in one place. This tab holds the master server address (by default
 * the fork's), connecting directly by IP, and the room code - and the room
 * code of the current lobby is shown on this imgui tab, concealed until a
 * checkbox is ticked, where the Bounty Hunter fork shows it in the game. In
 * the baseline, a co-op lobby is hosted or joined through the pause menu or
 * through a console entity in the Cantina's central hub.
 *
 * So the tab is CONFIGURATION ONLY, and deliberately so: hosting and joining
 * happen on the cartridge's own screens, which the pause menu and the
 * Cantina's console entity lead to. Adding an imgui Host button here would
 * give a control with no requirement behind it, in the one build that exists to leave the game's UI presentation as
 * it is.
 *
 * WHAT IS SHARED WITH THE FORK, and why that is not laziness: the master
 * address defaults to the same one, the transport is the same, and the
 * settings file is already shared between the two launchers. Two copies of
 * these four fields would be two things to keep in step for no gain. */
void coop_tab(UiSettings& s) {
    /* THE SAME ONE FIELD WIDTH the fork's tab uses.
     *
     * The fork's Multiplayer tab was misaligned, and this tab has the
     * identical shape - "Player name", "Host", "Port", "Host or IP",
     * "Listen port" down one column, each taking its own width so every
     * label starts somewhere different. Fixing one build and not the other
     * would leave the same defect waiting in the baseline. */
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.0f);
    Lsw2ForkSession& session = lsw2_fork_session();

    ImGui::TextDisabled("Online co-op. Games are started and joined in the game -");
    ImGui::TextDisabled("from the Cantina's console, or the pause menu - the way");
    ImGui::TextDisabled("the cartridge does it. This tab is the address book.");
    ImGui::Separator();

    ImGui::InputText("Player name", s.mp.player_name, sizeof s.mp.player_name);
    ImGui::TextDisabled("Shown to other players and on the master server list.");

    ImGui::Spacing();
    ImGui::SeparatorText("Master server");
    ImGui::InputText("Host", s.mp.master_host, sizeof s.mp.master_host);
    ImGui::InputInt("Port", &s.mp.master_port);
    s.mp.master_port = std::clamp(s.mp.master_port, 1, 65535);
    ImGui::Spacing();
    if (ImGui::Button("Use 127.0.0.1")) {
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host, "127.0.0.1");
        s.mp.master_port = 28553;
    }
    ImGui::SameLine();
    if (ImGui::Button("Use lsw2server.viosarcade.xyz")) {
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host,
                      "lsw2server.viosarcade.xyz");
        s.mp.master_port = 80;
    }
    ImGui::TextDisabled("The same directory the Bounty Hunter fork uses. Co-op");
    ImGui::TextDisabled("lobbies are listed separately from arena ones.");

    ImGui::Spacing();
    ImGui::SeparatorText("Direct connect");
    ImGui::InputText("Host or IP", s.mp.direct_host, sizeof s.mp.direct_host);
    ImGui::InputInt("Host port", &s.mp.direct_port);
    s.mp.direct_port = std::clamp(s.mp.direct_port, 1, 65535);
    ImGui::TextDisabled("Offered in the game's own join list as a direct entry.");

    ImGui::Spacing();
    ImGui::SeparatorText("This machine");
    ImGui::InputInt("Listen port", &s.mp.listen_port);
    s.mp.listen_port = std::clamp(s.mp.listen_port, 1, 65535);
    ImGui::TextDisabled("The port other players connect to when you host.");

    session.configure(s.mp.player_name, s.mp.master_host, s.mp.master_port,
                      s.mp.listen_port);
    session.configure_direct(s.mp.direct_host, s.mp.direct_port);

    ImGui::Spacing();
    ImGui::SeparatorText("Room code");
    /* THE ONE PLACE THE BASELINE DIFFERS FROM THE FORK BY DESIGN. The fork puts
     * its code on an in-game console screen; the baseline's goes here,
     * behind a checkbox that starts unticked. */
    ImGui::Checkbox("Show room code", &s.coop_show_room_code);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Hidden by default: a code on screen is a code anyone watching\n"
            "a stream or a screenshot can join with.");
    }
    if (!s.coop_show_room_code) {
        ImGui::TextDisabled("Hidden.");
    } else if (session.state() == Lsw2SessionState::Offline) {
        ImGui::TextDisabled("No lobby - host one from the game first.");
    } else if (session.console().room_code[0]) {
        ImGui::Text("Room code: %s", session.console().room_code);
        /* SAY WHAT THE TAB IS SHOWING, once per code. LSW2_PRESENT_SHOT sees
         * the DS frames and not this overlay, so the requirement that the code
         * be shown on the imgui Multiplayer tab behind a checkbox had no instrument
         * that could see it - a drive could prove the checkbox was wired and
         * the code was issued, and still not that this text was ever drawn.
         * This line is drawn from the same branch as the text, so it is only
         * printed when the text is, which is the traced value that was
         * missing. */
        static char said[8] = {0};
        if (std::strncmp(said, session.console().room_code, sizeof said - 1) != 0) {
            std::strncpy(said, session.console().room_code, sizeof said - 1);
            said[sizeof said - 1] = '\0';
            std::fprintf(stderr, "[ui] Multiplayer tab is drawing \"Room code: "
                                 "%s\" (checkbox on)\n", said);
        }
    } else {
        ImGui::TextDisabled("The directory has not issued one.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Session");
    ImGui::Text("Advertising as:  %s", session.kind());
    switch (session.state()) {
    case Lsw2SessionState::Offline:
        ImGui::TextUnformatted("Offline.");
        break;
    case Lsw2SessionState::Hosting:
        ImGui::Text("Hosting on port %d - %d/4 players", s.mp.listen_port,
                    session.player_count());
        break;
    case Lsw2SessionState::Joining:
        ImGui::TextUnformatted("Connecting...");
        break;
    case Lsw2SessionState::Joined:
        ImGui::Text("Joined - %d/4 players", session.player_count());
        break;
    case Lsw2SessionState::InMatch:
        ImGui::TextUnformatted("In a game.");
        break;
    }
    if (session.last_error() && *session.last_error())
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s",
                           session.last_error());

    /* SAY WHAT IS AND IS NOT KNOWN, WHICH IS NOT WHAT THIS USED TO SAY.
     *
     * It read: "No way in yet - the Cantina console and pause-menu entries are
     * not built", on the reasoning that the two co-op entry points (the
     * Cantina console and the pause menu) are the cartridge's own and
     * "neither has been located".
     *
     * The baseline does not need the port to BUILD those entries. The
     * baseline's co-op uses the cartridge's own list UI and keeps the usual
     * UI presentation; the port's job is to fill that list and answer the
     * connect. lsw2_coopjoin_publish writes the cartridge's host table every
     * frame in both builds, and lsw2_coopjoin_install registers the connect
     * override - which was once installed on the wrong side of the fork gate
     * and never reached the baseline at all. Both entry points lead to that
     * one list, so both are covered by covering it.
     *
     * WHAT IS GENUINELY UNKNOWN is whether a baseline session works end to
     * end, because no capture reaches the co-op screen - and that is the same
     * end-to-end run the rest of the baseline's online co-op is waiting on.
     * Saying "not built" claimed
     * something worse and more specific than the truth, in a place the PLAYER
     * reads. */
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
        "Wired but unproven: the lobby list and connect are in the");
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
        "baseline, through the game's own screens. No session tested yet.");
    ImGui::PopItemWidth();
}

/* Set from the executable name at startup - see ui_set_fork_build. */
bool g_fork_build = false;

void cheats_tab(UiSettings&) {
    ImGui::TextWrapped(
        "The baseline runs the game as shipped. Nothing here is applied unless "
        "you press it, and nothing is applied at startup.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Levels");
    ImGui::Separator();

    if (!lsw2_cheats_available()) {
        ImGui::TextDisabled("Waiting for the game to build its state - this is");
        ImGui::TextDisabled("normal during boot and on the title screen.");
        return;
    }

    /* TWO READINGS THAT ANSWER DIFFERENT QUESTIONS, and this used to claim
     * otherwise. The first is the REQUEST the state machine builds from; the
     * second is the state object the game's ordinary entry path installs. A
     * warp writes the first and not the second, so they differ after warping -
     * which the old code reported in red as "the addresses need re-deriving".
     * That fired during entirely normal use. */
    const int live = lsw2_cheats_current_level();
    const int check = lsw2_cheats_current_level_via_state();
    const char* live_name = "unknown";
    for (int i = 0; i < kLsw2LevelCount; ++i)
        if (kLsw2Levels[i].id == live) live_name = kLsw2Levels[i].name;

    if (live >= 0) ImGui::Text("Currently: %s  (id %d)", live_name, live);
    else ImGui::TextDisabled("Currently: unreadable");
    if (live != check) {
        ImGui::TextDisabled("The installed level state says %d - it differs", check);
        ImGui::TextDisabled("after a warp, because a warp sets the request");
        ImGui::TextDisabled("and lets the game build from it.");
    }

    ImGui::Spacing();
    static int selected = 0;
    /* Names carry their confidence, because most of them are DERIVED from the
     * level's own asset names rather than confirmed. Presenting a guess in the
     * same typeface as a fact is how a guess becomes a fact by accident. */
    if (ImGui::BeginCombo("Level", kLsw2Levels[selected].name)) {
        for (int i = 0; i < kLsw2LevelCount; ++i) {
            const bool chosen = (i == selected);
            ImGui::PushID(i);
            if (ImGui::Selectable(kLsw2Levels[i].name, chosen)) selected = i;
            ImGui::SameLine();
            if (kLsw2Levels[i].source[0] == 'c')
                ImGui::TextDisabled("confirmed");
            else if (kLsw2Levels[i].source[0] == 'd')
                ImGui::TextDisabled("derived from its assets");
            else
                ImGui::TextDisabled("unidentified");
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const bool can_warp = lsw2_cheats_can_warp();
    ImGui::BeginDisabled(!can_warp);
    if (ImGui::Button("Warp")) {
        lsw2_cheats_warp(kLsw2Levels[selected].id);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (!can_warp) {
        /* Disabled rather than failing quietly, and with the reason, because
         * "the button did nothing" is the least useful thing a UI can say. */
        ImGui::TextWrapped(
            "Warp is unavailable right now: the game is not in a level state. "
            "It works from inside the game, not from the menus.");
    } else {
        ImGui::TextWrapped(
            "Warp uses the game's own state-request queue - it fills in a "
            "request and the game performs the transition itself, on its own "
            "thread, rather than anything being called from outside.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Unlock flags");
    ImGui::Separator();

    /* THIS SECTION USED TO EXPLAIN WHY THERE WAS NOTHING HERE. It said the
     * flags were unlocated and that only another save file could separate the
     * categories. Both statements were true when written and the second was
     * wrong as a conclusion - the whole system was found from the cheat
     * codes instead. The old text is gone rather than left as a caveat, because
     * a UI that hedges about a solved problem teaches the reader to distrust
     * the parts that are right. */
    uint32_t flags_a[kLsw2FlagWords] = {};
    uint32_t flags_b[kLsw2FlagWords] = {};
    const bool have_flags = lsw2_cheats_read_flags(flags_a, flags_b);

    if (!have_flags) {
        ImGui::TextDisabled("Unavailable - the game has not built its context.");
    } else {
        ImGui::Text("A  %08X %08X %08X %08X",
                    flags_a[0], flags_a[1], flags_a[2], flags_a[3]);
        ImGui::Text("B  %08X %08X %08X %08X",
                    flags_b[0], flags_b[1], flags_b[2], flags_b[3]);
        ImGui::Spacing();
        if (ImGui::Button("Copy")) {
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "A %08X %08X %08X %08X\nB %08X %08X %08X %08X\n",
                          flags_a[0], flags_a[1], flags_a[2], flags_a[3],
                          flags_b[0], flags_b[1], flags_b[2], flags_b[3]);
            ImGui::SetClipboardText(buf);
        }
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Those two are 0x02104D90 + 0x44CC and + 0x44DC, and a capture "
            "showed them reading identically on a fully-unlocked save and an "
            "empty one. That refuted them - but enumerating the "
            "accessors then found NINE flag arrays, so the capture had ruled out "
            "two candidates out of nine, not all of them.");
        ImGui::Spacing();
        if (ImGui::TreeNode("All nine flag arrays")) {
            uint32_t all[kLsw2FlagArrayCount * kLsw2FlagWords] = {0};
            if (lsw2_cheats_read_all_flag_arrays(all)) {
                for (int a = 0; a < kLsw2FlagArrayCount; ++a) {
                    ImGui::Text("+0x%04X  %08X %08X %08X %08X",
                                kLsw2FlagArrays[a].offset,
                                all[a * kLsw2FlagWords + 0],
                                all[a * kLsw2FlagWords + 1],
                                all[a * kLsw2FlagWords + 2],
                                all[a * kLsw2FlagWords + 3]);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", kLsw2FlagArrays[a].note);
                }
                if (ImGui::Button("Copy all nine")) {
                    /* THE POINT OF THIS WHOLE PANEL. Take this on a fresh save
                     * and on a fully-unlocked one; the array that differs is
                     * the unlock state, and unlocking everything from the
                     * start follows immediately. */
                    std::string dump;
                    char line[96];
                    for (int a = 0; a < kLsw2FlagArrayCount; ++a) {
                        std::snprintf(line, sizeof line,
                                      "+%04X %08X %08X %08X %08X\n",
                                      kLsw2FlagArrays[a].offset,
                                      all[a * kLsw2FlagWords + 0],
                                      all[a * kLsw2FlagWords + 1],
                                      all[a * kLsw2FlagWords + 2],
                                      all[a * kLsw2FlagWords + 3]);
                        dump += line;
                    }
                    ImGui::SetClipboardText(dump.c_str());
                }
            } else {
                ImGui::TextUnformatted("not available yet - the game has not "
                                       "built its context.");
            }
            ImGui::TreePop();
        }
        ImGui::Spacing();
        /* NO BUTTONS YET, and for a reason that is nearly the opposite of the
         * old one. The mechanism is known; what is not known is which array
         * means what, because the dispatcher sets one or the other depending on
         * a byte in the roster table and that byte has not been read out.
         *
         * The honest route is to capture these two lines from a save that has
         * the codes entered and use the captured pair - measured rather than
         * reasoned. That is what this readout is for. */
        /* THE BASELINE'S OPT-IN, chosen over unlocking by default: the base
         * port stays close to the original game and this is a button
         * someone presses. The fork applies the same writes automatically once
         * its lobby is up (fork.cpp). */
        if (ImGui::Button("Unlock everything")) {
            if (lsw2_cheats_unlock_everything())
                g_unlock_note = "written - open the terminal's Characters tab "
                                "to check for names rather than ????";
            else
                g_unlock_note = "not available yet - the game has not built "
                                "its context";
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Sets OWNED and SEEN for every character and vehicle id the\n"
                "game ships, and marks all 22 terminal Extras as bought.\n\n"
                "Both roster arrays, because the Characters tab tests SEEN\n"
                "first and draws \"????\" when it is clear - so OWNED alone\n"
                "would give a roster of question marks.\n\n"
                "Does NOT switch Extras on: that is a different array and\n"
                "belongs to the host's match settings.");
        }
        if (g_unlock_note) ImGui::TextWrapped("%s", g_unlock_note);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
            "The array identification is strong but not settled.");
        ImGui::TextWrapped(
            "Four lines of evidence say +0x44CC/+0x44DC are the roster state, "
            "against one capture that says they do not differ "
            "between saves. If the button above does nothing visible, that "
            "capture was right and this needs a save-to-save comparison.");
        ImGui::TextWrapped(
            "One array IS the Extras, corroborated four ways: +0x4518, "
            "indices 0..19. The unlocks are not identified yet. Expand the "
            "list above on a fresh save and on a fully-unlocked one and copy "
            "both: the array that differs is the answer, and the buttons can "
            "be written the same day.");
    }
}

}  /* namespace */

bool ui_init(SDL_Window* window, SDL_Renderer* renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   /* we persist our own settings, not imgui's */

    /* ONLY THE TITLE BAR MOVES A WINDOW.
     *
     * imgui's default is that a window can be dragged from anywhere inside it.
     * For a settings panel that is fine. For the floating touch screen it is
     * not: the whole point of that panel is that you click ON it, and every
     * such click was dragging the window around instead of reaching the game.
     * The panel was effectively untouchable. */
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui::StyleColorsDark();

    /* LSW2_UI_TAB is for screenshotting the UI from a script, and on its own it
     * could not do that: it preselects a tab in a window that starts hidden, so
     * nothing was ever drawn to capture. Setting it also opens the menu, which
     * is what "screenshot this tab" has to mean. */
    if (SDL_getenv("LSW2_UI_TAB")) g_visible = true;

    g_gl_backend = (renderer == nullptr);
    if (g_gl_backend) {
        /* 1.30 is the floor imgui's GL3 backend accepts, and it is well below
         * the 4.3 the compute renderer already required to get here - so this
         * cannot be the thing that fails. */
        if (!ImGui_ImplSDL2_InitForOpenGL(window, lsw2_gl_context()))
            return false;
        if (!ImGui_ImplOpenGL3_Init("#version 130")) return false;
        return true;
    }
    if (!ImGui_ImplSDL2_InitForSDLRenderer(window, renderer)) return false;
    if (!ImGui_ImplSDLRenderer2_Init(renderer)) return false;
    return true;
}

void ui_shutdown() {
    if (g_gl_backend) ImGui_ImplOpenGL3_Shutdown();
    else ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

bool ui_handle_event(const SDL_Event& event) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    if (!g_visible) return false;
    const ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
            return io.WantCaptureKeyboard;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION: case SDL_MOUSEWHEEL: {
            /* THE PANEL'S PICTURE IS THE GAME'S, not the UI's. imgui reports it
             * wants the mouse for anything over one of its windows, which
             * includes the touch screen it is drawing - so with the settings
             * menu open the panel went dead. Hand back any event inside the
             * panel's image so it becomes a stylus touch. Dragging still works
             * because the title bar is outside this rectangle. */
            if (g_floating_shown && event.type != SDL_MOUSEWHEEL) {
                int mx = 0, my = 0;
                if (event.type == SDL_MOUSEMOTION) { mx = event.motion.x; my = event.motion.y; }
                else { mx = event.button.x; my = event.button.y; }
                if (mx >= g_floating_rect.x && my >= g_floating_rect.y &&
                    mx < g_floating_rect.x + g_floating_rect.w &&
                    my < g_floating_rect.y + g_floating_rect.h)
                    return false;
            }
            return io.WantCaptureMouse;
        }
        default:
            return false;
    }
}

void ui_begin_frame() {
    if (g_gl_backend) ImGui_ImplOpenGL3_NewFrame();
    else ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

/* THE FLAGS THE REST OF THE PORT READS.
 *
 * Declared here rather than beside their accessors because ui_draw - which is
 * what keeps them current - is above those, and a mirror that cannot see the
 * variable it mirrors into is a compile error rather than a subtle one. */
static bool g_fork_wants_roster = true;
static bool g_fork_hunters_only = true;
static bool g_arena_hud_enabled = false;
static bool g_enhanced_controls = true;   /* Enhanced controls, default on   */
static bool g_mute_music_second = false;  /* Debug, second-instance mute     */
static bool g_mute_sound_second = false;  /* Debug, second-instance mute     */
static bool g_enhanced_portrait = true;   /* Portrait follows, both builds   */
static bool g_enhanced_slot_colour = true;/* Player-colour ring, both builds */
static bool g_rs_invert_x = false;        /* Enhanced controls camera        */
static bool g_rs_invert_y = false;        /* the orbit camera's height       */
static int  g_rs_deadzone = 20;
static int  g_rs_speed_x = 250;
static int  g_rs_speed_y = 150;
static float g_arena_hud_scale = 0.5f;   /* Arena HUD overlay           */
static int  g_camera_delta = 0;
static int  g_camera_delta_y = 0;
/* No radar or meter rectangle mirrors: those settings are retired (ui.h). The
 * mirrors here used to start at the PRE-measurement values - (169,71,96), meter
 * off, 24x24 - and were only corrected by ui_draw's per-frame copy, so the
 * first frame of every HUD drew the old rectangles. Gone with the settings. */

void ui_draw(UiSettings& s) {
    g_floating_shown = false;

    /* MIRROR THE FLAGS EVERY FRAME, not only at load.
     *
     * These three are read from outside the UI - by the dispatch override that
     * forces the Free Play roster, and by the arena HUD that draws into the DS
     * frame - and they were being copied out of the settings struct in
     * ui_load_settings ALONE. So toggling any of their checkboxes did nothing
     * until the next restart, which reads as a dead control rather than a
     * delayed one.
     *
     * ui_draw runs every frame with the live struct, and this sits above the
     * `!g_visible` early-out so closing the panel does not freeze the mirror at
     * whatever it was when the panel was last open. */
    g_fork_wants_roster = s.bh_freeplay_select;
    g_fork_hunters_only = s.bh_roster_only;
    g_arena_hud_enabled = s.arena_hud_overlay;
    g_enhanced_controls = s.enhanced_controls;
    g_mute_music_second = s.mute_music_second_instance;
    g_mute_sound_second = s.mute_sound_second_instance;
    g_enhanced_portrait = s.enhanced_portrait;
    g_enhanced_slot_colour = s.enhanced_slot_colour;
    g_rs_invert_x = s.right_stick_invert_x;
    g_rs_invert_y = s.right_stick_invert_y;
    g_rs_deadzone = s.right_stick_deadzone;
    g_rs_speed_x = s.right_stick_speed_x;
    g_rs_speed_y = s.right_stick_speed_y;
    g_arena_hud_scale = s.arena_hud_scale;

    /* NO LOADING OVERLAY. There was one - "Bounty Hunter / Skipping boot..." -
     * and it is gone: no imgui popup says the boot is being skipped or
     * fast-forwarded; the game simply launches. A caption announcing a wait
     * is an admission the wait exists. The fork should not
     * need to explain itself; it should arrive. lsw2_fork_boot_status() is
     * still exposed for stderr diagnostics, and nothing draws it. */

    /* THE ARENA HUD OVERLAY IS NOT DRAWN HERE ANY MORE.
     *
     * It used to be two imgui-blitted crops of the touch screen. That was the
     * wrong layer: the HUD overlay is not done through imgui - its setting is
     * toggled in imgui, but its presentation is in the game.
     *
     * So it composites into the DS frame from the present path - arenahud.cpp,
     * and gamemenu_render.h's opening argument for why that is the right place.
     * What is left here is the toggle and the fact that the HUD REPLACES the
     * floating touch panel rather than sitting beside it. */
    const bool arena_hud = lsw2_arena_hud_active();

    /* The floating touch panel is drawn even with the menu hidden - it is part
     * of the game view, not part of the configuration UI. */
    /* The overlay REPLACES this panel, so the same scope has to gate both -
     * otherwise turning the overlay on hides the touch screen everywhere while
     * only drawing the HUD in a match. */
    if (s.floating_touch && g_touch_texture && !arena_hud) {
        /* REMEMBERED GEOMETRY WINS OVER THE SCALE PRESET. Zero means the
         * player has never moved or resized the panel, so the preset applies;
         * once they have, that is what they meant. FirstUseEver rather than
         * Always, or the panel could not be dragged at all - it would snap back
         * to the saved position every frame. */
        const bool have_saved = s.touch_panel_w > 1.0f && s.touch_panel_h > 1.0f;
        /* The preset is the PICTURE at that scale, so the padding and the title
         * bar go around it (touch_panel_size_cb then rounds to whole steps). */
        const ImGuiStyle& style = ImGui::GetStyle();
        const float preset_w = kScreenWidth * s.floating_touch_scale
                               + 2.0f * style.WindowPadding.x;
        const float preset_h = kScreenHeight * s.floating_touch_scale
                               + ImGui::GetFrameHeight()
                               + 2.0f * style.WindowPadding.y;
        /* THE SLIDER WINS ON THE FRAME IT MOVES. FirstUseEver plus the
         * remembered size meant "Panel scale" was consulted exactly once per
         * install - the first time the panel was ever shown - and the
         * read-back below then remembered whatever imgui produced, so every
         * later move of the slider changed a number nobody looked at. An
         * explicit move is applied with Always for that one frame and the
         * read-back records it, so a drag afterwards still sticks. */
        if (g_panel_resize) {
            ImGui::SetNextWindowSize(ImVec2(preset_w, preset_h), ImGuiCond_Always);
            g_panel_resize = false;
        } else {
            const float w = have_saved ? s.touch_panel_w : preset_w;
            const float h = have_saved ? s.touch_panel_h : preset_h;
            ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
        }
        ImGui::SetNextWindowPos(
            ImVec2(s.touch_panel_x > 0 ? s.touch_panel_x : 24.0f,
                   s.touch_panel_y > 0 ? s.touch_panel_y : 24.0f),
            ImGuiCond_FirstUseEver);
        /* Wide open, so the callback sees each size exactly as imgui proposes
         * it; read during Begin, so nothing has to outlive this call. */
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                            ImVec2(FLT_MAX, FLT_MAX),
                                            touch_panel_size_cb, nullptr);
        if (ImGui::Begin("Touch screen", nullptr,
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse)) {
            /* THE PICTURE IS THE CONTENT REGION, not a fit inside it:
             * touch_panel_size_cb makes the region whole 4x3 steps, and drawing
             * AT the region rather than at a fit recomputed from it means no
             * float rounding can open a 1-px strip along either edge. */
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float dw = avail.x, dh = avail.y;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)g_touch_texture, ImVec2(dw, dh));
            g_floating_rect = SDL_Rect{int(pos.x), int(pos.y), int(dw), int(dh)};
            g_floating_shown = true;

            /* READ BACK WHERE IT ENDED UP, every frame. Cheaper than tracking
             * drag and resize events separately, and it cannot miss one. */
            const ImVec2 wpos = ImGui::GetWindowPos();
            const ImVec2 wsize = ImGui::GetWindowSize();
            s.touch_panel_x = wpos.x;  s.touch_panel_y = wpos.y;
            s.touch_panel_w = wsize.x; s.touch_panel_h = wsize.y;
        }
        ImGui::End();
    }

    if (!g_visible) return;

    ImGui::SetNextWindowSize(ImVec2(430, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("LEGO Star Wars II - Settings", nullptr)) {
        /* SAY WHICH BUILD THIS IS. The two executables are identical to look
         * at and share one settings file, so without this the only way to tell
         * them apart is to notice which tabs are missing. */
        if (g_fork_build) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                               "Bounty Hunter fork");
            ImGui::SameLine();
            ImGui::TextDisabled("(saves to bountyhunter.sav)");
            ImGui::Separator();
        }
        if (ImGui::BeginTabBar("tabs")) {
            /* LSW2_UI_TAB preselects a tab once, so the UI can be screenshotted
             * from a script. imgui has no remote clicking, and "it compiles" is
             * not evidence that a panel renders - this UI has already had a
             * panel that was correctly placed, correctly sized and entirely
             * black. */
            static const char* want = SDL_getenv("LSW2_UI_TAB");
            static bool selected = false;
            auto flags = [&](const char* name) {
                if (selected || !want || SDL_strcasecmp(want, name) != 0)
                    return ImGuiTabItemFlags_None;
                selected = true;
                return ImGuiTabItemFlags_SetSelected;
            };
            if (ImGui::BeginTabItem("Video", nullptr, flags("Video")))
                { video_tab(s);  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Input", nullptr, flags("Input")))
                { input_tab(s);  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Touch", nullptr, flags("Touch")))
                { touch_tab(s);  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Debug", nullptr, flags("Debug")))
                { debug_tab(s);  ImGui::EndTabItem(); }
            /* The fork has no cheats tab. Not greyed out - absent.
             * A disabled control still advertises the feature, and the point
             * of the fork is that unlocks are not the player's to grant: they
             * are granted wholesale at startup (everything unlocked from the
             * start) and the match rules come from the host's settings menu. */
            if (!g_fork_build &&
                ImGui::BeginTabItem("Cheats", nullptr, flags("Cheats")))
                { cheats_tab(s); ImGui::EndTabItem(); }
            /* BOTH BUILDS HAVE A MULTIPLAYER TAB NOW, and they are different
             * tabs. The baseline has online co-op; the fork's tab carries
             * the lobby character select's roster flags and points at in-game
             * menus, the baseline's carries the room code, which the baseline
             * shows OUT of the game. Same name, because to a player it is the same
             * question. */
            if (ImGui::BeginTabItem("Multiplayer", nullptr,
                                    flags("Multiplayer"))) {
                if (g_fork_build) multiplayer_tab(s);
                else              coop_tab(s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Esc or F1 closes this. The game keeps running behind it.");
    }
    ImGui::End();

    if (s.show_metrics) ImGui::ShowMetricsWindow(&s.show_metrics);
}

void ui_render(SDL_Renderer* renderer) {
    ImGui::Render();
    if (g_gl_backend) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    else ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void ui_set_fork_build(bool fork) {
    g_fork_build = fork;
    /* THE ONE PLACE THE BUILD IS KNOWN, so it is the one place the session's
     * directory label is set. The baseline runs online co-op over the same
     * directory the fork uses, and a baseline lobby
     * advertised as `bounty` would show up in the fork's browser and be joined
     * by someone expecting an arena. See Lsw2ForkSession::set_coop. */
    lsw2_fork_session().set_coop(!fork);
}

/* Answers the dispatch override, which runs before the frontend exists and so
 * cannot read UiSettings. Defaults to true, the specified default, and the
 * settings load overwrites it once the file has been read. */

/* THE HOST OWNS THE ROSTER WHILE A MATCH IS ON. Anything else is a cheat.
 *
 * Being able to toggle the Free Play menus freely from here, instead of through
 * the lobby host's controls when starting or configuring a lobby, would allow
 * cheating.
 *
 * It is the same precedence the Enhanced controls already follow:
 * a client may restrict itself, never widen itself. These two checkboxes decide
 * which character pages the Free Play select offers, so a client that ticked
 * "Bounty Hunters only" off during a Hunters match would be picking from all
 * twelve pages while everyone else picked from two.
 *
 * So while a session is up, the HOST'S Roster setting decides, and the local
 * checkbox is ignored rather than merged - a client's "off" must not widen the
 * roster, and there is no case where it should. Offline the checkbox is the
 * player's own business and behaves as before. */
bool lsw2_hle_fork_wants_roster() { return g_fork_wants_roster; }

bool lsw2_hle_fork_hunters_only() {
    /* THE HOST'S PICKER OUTRANKS THE CLAMP, and this is the interaction a
     * later correction reopened.
     *
     * The lobby roster flag narrows the Free Play select to the two Bounty
     * Hunter pages in the arena lobby. The host settings menu's character
     * picker uses THAT SAME SCREEN - the Free Play touch screen menu - and must
     * reach every playable character, unlikely ones such as R5-D4 and Gonk
     * Droid included. Two features want opposite things from one widget, and the tie-breaks by WHO IS ASKING: the host choosing
     * the match's one character is not a player choosing their own hunter.
     *
     * characterpick.h previously argued this conflict away by claiming the
     * picker was the fork's own menu and never touched the widget. That was
     * wrong, so the conflict is real and this is where it resolves.
     *
     * Keyed on the session's screen rather than a new flag, so it is true for
     * however the picker is opened - the fork-drawn one today, the cartridge's
     * widget once it moves - and false again the moment the host leaves it. */
    if (lsw2_fork_session().screen() == Lsw2ForkScreen::Character)
        return false;

    /* ONLY ONCE THE HOST'S RULES ARE HERE. A joiner is in the session a moment
     * before the host's rules arrive, and until then settings() holds the
     * joiner's OWN rules - measured (conn_roster3): a joiner whose own Roster
     * was All widened its grid to every page, then narrowed it again when it
     * adopted the host's Hunters. "A client may restrict itself, never widen
     * itself": until the rules arrive, the player's own checkbox decides. */
    if (lsw2_fork_session().in_session() &&
        lsw2_fork_session().session_rules_known())
        return lsw2_fork_session().settings().roster == Lsw2Roster::Hunters;
    return g_fork_hunters_only;
}
bool lsw2_hle_fork_select_range(unsigned* low, unsigned* high) {
    /* THE FORK'S ROSTER WISH IS WHAT MAKES THIS WIDGET THE FORK'S BUSINESS.
     * Without it the Free Play select is the cartridge's own screen and gets
     * the cartridge's own full range, which is what the baseline build and a
     * fork with the flag off both want. */
    if (!lsw2_hle_fork_wants_roster()) return false;

    /* The restricted select is only shown while in the Arena lobby map. The
     * constructor-time writer has always had this condition; stating it here
     * is what stops the per-frame writer from quietly not having it. */
    if (lsw2_cheats_current_level() != kLsw2ArenaLobbyLevel) return false;

    const bool hunters = lsw2_hle_fork_hunters_only();
    if (low) *low = hunters ? kLsw2SelectHunterPageLow : kLsw2SelectAllPageLow;
    if (high)
        *high = hunters ? kLsw2SelectHunterPageHigh : kLsw2SelectAllPageHigh;
    return true;
}

bool lsw2_ui_arena_hud_enabled() { return g_arena_hud_enabled; }
bool lsw2_ui_enhanced_controls() { return g_enhanced_controls; }
bool lsw2_ui_mute_music_second_instance() { return g_mute_music_second; }
bool lsw2_ui_mute_sound_second_instance() { return g_mute_sound_second; }
bool lsw2_ui_enhanced_portrait() { return g_enhanced_portrait; }
bool lsw2_ui_enhanced_slot_colour() { return g_enhanced_slot_colour; }
bool lsw2_ui_right_stick_invert_x() { return g_rs_invert_x; }
int  lsw2_ui_right_stick_deadzone() { return g_rs_deadzone; }
int  lsw2_ui_right_stick_speed_x() { return g_rs_speed_x; }
int  lsw2_ui_right_stick_speed_y() { return g_rs_speed_y; }
float lsw2_ui_arena_hud_scale() { return g_arena_hud_scale; }

void lsw2_ui_push_camera_delta(int dx) { g_camera_delta += dx; }
void lsw2_ui_push_camera_delta_y(int dy) { g_camera_delta_y += dy; }
bool lsw2_ui_right_stick_invert_y() { return g_rs_invert_y; }

int lsw2_ui_take_camera_delta_y() {
    const int d = g_camera_delta_y;
    g_camera_delta_y = 0;
    return d;
}

int lsw2_ui_take_camera_delta() {
    /* TAKEN, NOT READ. The pad pushes every frame and the camera consumes once;
     * leaving the value behind would apply the same nudge again on any frame
     * the fork tick ran twice or the pad did not. */
    const int d = g_camera_delta;
    g_camera_delta = 0;
    return d;
}

bool ui_fork_build() { return g_fork_build; }

UiBinding* ui_bindings(int* count) {
    if (count) *count = kBindingCount;
    return g_bindings;
}

bool ui_capturing_binding() { return g_capture_index >= 0; }

void ui_offer_captured_key(int scancode) {
    if (g_capture_index < 0 || g_capture_pad) return;
    /* Escape cancels rather than binding - otherwise the one key everybody
     * presses to back out becomes permanently bound to something. */
    if (scancode != SDL_SCANCODE_ESCAPE)
        g_bindings[g_capture_index].scancode = scancode;
    g_capture_index = -1;
}

void ui_offer_captured_pad(int button) {
    if (g_capture_index < 0 || !g_capture_pad) return;
    g_bindings[g_capture_index].pad_button = button;
    g_capture_index = -1;
}

void ui_toggle() { g_visible = !g_visible; }
bool ui_visible() { return g_visible; }

void ui_set_stats(double fps, uint64_t frames, uint64_t underruns) {
    g_fps = fps; g_frames = frames; g_underruns = underruns;
}

void ui_set_touch_texture(void* texture_handle) {
    g_touch_texture = texture_handle;
}

bool ui_floating_touch_rect(SDL_Rect* out) {
    if (!g_floating_shown) return false;
    if (out) *out = g_floating_rect;
    return true;
}

/* SETTINGS PERSISTENCE. See settings_store.h for why this is a store and not a
 * serialisation of the struct - the short version is that the baseline and the
 * fork share one file and neither may assume its own key set is complete.
 *
 * The store is kept between load and save so a key this build does not
 * understand survives a round trip. It is a file-scope object rather than
 * something threaded through the API because ui_save_settings is called from
 * shutdown, which has no path back to whatever loaded it. */
SettingsStore g_store;
std::string g_store_path;

void ui_load_settings(UiSettings& s, const char* path) {
    g_store_path = lsw2_settings_path(path);
    if (!g_store.load(g_store_path)) {
        std::fprintf(stderr, "[settings] could not read %s; using defaults\n",
                     g_store_path.c_str());
    }

    /* MIGRATE THE OLD .ini ON FIRST RUN, so nobody's bindings are lost to the
     * format change. Only when the JSON has nothing in it - once the JSON
     * exists it is the authority, and re-importing would undo later edits. */
    if (!g_store.has("window_scale") && path && *path) {
        std::string ini = path;
        const size_t n = ini.size();
        if (n > 5 && ini.compare(n - 5, 5, ".json") == 0)
            ini.replace(n - 5, 5, ".ini");
        if (ini != g_store_path) g_store.import_ini(ini);
    }

    g_store.begin_tracking();

    s.preserve_aspect      = g_store.get_bool("preserve_aspect", s.preserve_aspect);
    s.integer_scale        = g_store.get_bool("integer_scale", s.integer_scale);
    s.window_scale         = int(g_store.get_number("window_scale", s.window_scale));
    s.linear_filter        = g_store.get_bool("linear_filter", s.linear_filter);
    s.floating_touch       = g_store.get_bool("floating_touch", s.floating_touch);
    s.floating_touch_scale = float(g_store.get_number("floating_touch_scale",
                                                      s.floating_touch_scale));
    s.floating_touch_only  = g_store.get_bool("floating_touch_only", s.floating_touch_only);
    s.mute_music_second_instance =
        g_store.get_bool("debug_mute_music_second_instance", s.mute_music_second_instance);
    s.mute_sound_second_instance =
        g_store.get_bool("debug_mute_sound_second_instance", s.mute_sound_second_instance);
    s.enhanced_portrait    = g_store.get_bool("enhanced_portrait", s.enhanced_portrait);
    s.enhanced_slot_colour = g_store.get_bool("enhanced_slot_colour", s.enhanced_slot_colour);
    s.adaptive_widescreen  = g_store.get_bool("adaptive_widescreen", s.adaptive_widescreen);
    s.max_extra_width      = int(g_store.get_number("max_extra_width", s.max_extra_width));
    s.opengl_renderer      = g_store.get_bool("opengl_renderer", s.opengl_renderer);
    s.internal_resolution  = int(g_store.get_number("internal_resolution", s.internal_resolution));
    s.texture_upscale      = int(g_store.get_number("texture_upscale", s.texture_upscale));
    s.stick_as_dpad        = g_store.get_bool("stick_as_dpad", s.stick_as_dpad);
    s.stick_deadzone       = int(g_store.get_number("stick_deadzone", s.stick_deadzone));
    {
        const std::string h = g_store.get_string("mp_direct_host", s.mp.direct_host);
        std::snprintf(s.mp.direct_host, sizeof s.mp.direct_host, "%s", h.c_str());
    }
    s.mp.direct_port = int(g_store.get_number("mp_direct_port", s.mp.direct_port));

    /* THE NAME AND THE DIRECTORY ADDRESS, which had never been persisted.
     *
     * Only the direct-connect pair was. So a player who set their name, or
     * pointed the fork at their own master server, got it back as "Maclunkey"
     * and play.viosarcade.xyz on the next launch - and both of these are the
     * imgui tab's whole job: the Multiplayer tab of the imgui menu is where
     * the player's name lives (Maclunkey unless changed), and both builds let
     * the master server address be changed. A setting that does not survive a restart is not
     * configurable in any useful sense. */
    {
        const std::string name = g_store.get_string("mp_player_name",
                                                    s.mp.player_name);
        std::snprintf(s.mp.player_name, sizeof s.mp.player_name, "%s",
                      name.c_str());
        /* A STORED HOST THAT IS THE OLD DEFAULT IS NOT A CHOICE.
         *
         * ui.h's default moved from play.viosarcade.xyz to 127.0.0.1 on
         * 2026-09-01, and settings are persisted, so every machine that has
         * ever opened this menu has the old value written down and would keep
         * getting it - the new default would reach nobody who had already run
         * the port, which is everybody reporting the problem.
         *
         * So exactly one string is treated as "never chosen": the previous
         * default, byte for byte.
         *
         * AND IT HAPPENS ONCE, which is the whole difference between a
         * migration and a bug. Without the marker this would re-apply on every
         * load, so a user who deliberately typed play.viosarcade.xyz back in
         * would find it silently reverted at the next start - the setting would
         * stop being a setting. The marker is written whether or not the value
         * was replaced, because "this install has been through the change" is
         * what is being recorded, not "something was rewritten". */
        std::string host = g_store.get_string("mp_master_host",
                                              s.mp.master_host);
        if (g_store.get_number("mp_master_host_localised", 0) == 0) {
            if (host == "play.viosarcade.xyz") host = s.mp.master_host;
            g_store.set_number("mp_master_host_localised", 1);
        }
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host, "%s",
                      host.c_str());
    }
    s.mp.master_port = int(g_store.get_number("mp_master_port", s.mp.master_port));
    /* AND ONCE MORE, THE OTHER WAY (2026-10-16): the hosted directory is live
     * on Cloudflare, so the old default - 127.0.0.1:28553, written into every
     * settings file that was ever saved - becomes the new one. Exactly that
     * pair, exactly once, under its own marker: an address anybody typed in is
     * theirs, and so is 127.0.0.1 typed back in afterwards. */
    if (g_store.get_number("mp_master_cloud", 0) == 0) {
        if (std::strcmp(s.mp.master_host, "127.0.0.1") == 0 &&
            s.mp.master_port == 28553) {
            std::snprintf(s.mp.master_host, sizeof s.mp.master_host,
                          "lsw2server.viosarcade.xyz");
            s.mp.master_port = 80;
        }
        g_store.set_number("mp_master_cloud", 1);
    }
    s.mp.listen_port = int(g_store.get_number("mp_listen_port", s.mp.listen_port));

    /* CLAMPED, because settings.json is hand-editable and a port outside the
     * range would be handed straight to bind() or connect(). */
    s.mp.master_port = std::clamp(s.mp.master_port, 1, 65535);
    s.mp.listen_port = std::clamp(s.mp.listen_port, 1, 65535);
    s.arena_hud_overlay    = g_store.get_bool("arena_hud_overlay", s.arena_hud_overlay);
    s.arena_hud_scale      = float(g_store.get_number("arena_hud_size", s.arena_hud_scale));

    /* Customization has to survive character switches and every move between
     * a level and an arena - which outlives one run, so it lives in the file. One
     * key per slot rather than a packed string: a slot that fails to parse then
     * falls back to "no part" on its own instead of corrupting the other four. */
    {
        uint16_t slots[kLsw2CustomSlots];
        char key[32];
        for (int i = 0; i < kLsw2CustomSlots; ++i) {
            std::snprintf(key, sizeof key, "custom_slot_%d", i);
            slots[i] = uint16_t(g_store.get_number(key, kLsw2CustomNone));
        }
        lsw2_customization_load(slots);
        /* The two overlay Extras (moustache/sunglasses) persist alongside the
         * slots - after load(), which clears the dirty flag this must not set. */
        lsw2_customization_set_overlays(
            g_store.get_bool("custom_moustache", false),
            g_store.get_bool("custom_sunglasses", false));
    }
    /* The player starts as their last-played Bounty Hunter. Stored as the game's
     * own character id, and validated on the way in - settings.json is
     * hand-editable and an id outside the roster table would travel into the
     * page maths as a -1 index. */
    lsw2_last_hunter_load(int(g_store.get_number("bh_last_hunter",
                                                 kLsw2DefaultHunter)));
    /* Settings and customization are persistent, which is the reason the
     * fork's Save Game may be hidden at all. The
     * host's ten rows are settings by any reading, and they were being
     * default-constructed every run. Clamped after loading because the file is
     * hand-editable and these cast straight to enums that index value arrays. */
    if (ui_fork_build()) {
        Lsw2HostSettings& h = lsw2_fork_session().settings();
        h.visible_master = g_store.get_bool("host_visible_master", h.visible_master);
        h.visible_lan = g_store.get_bool("host_visible_lan", h.visible_lan);
        h.roster = Lsw2Roster(int(g_store.get_number("host_roster", double(int(h.roster)))));
        h.character = int(g_store.get_number("host_character", h.character));
        h.controls = Lsw2HostControls(int(g_store.get_number("host_controls", double(int(h.controls)))));
        h.customization = Lsw2HostCustomization(int(g_store.get_number("host_customization", double(int(h.customization)))));
        h.music = Lsw2HostMusic(int(g_store.get_number("host_music", double(int(h.music)))));
        h.lobby_music = Lsw2LobbyMusic(int(g_store.get_number("host_lobby_music", double(int(h.lobby_music)))));
        h.lobby_time = Lsw2LobbyTime(int(g_store.get_number("host_lobby_time", double(int(h.lobby_time)))));
        lsw2_host_extras_from_mask(h, uint32_t(g_store.get_number("host_extras", 0)));
        lsw2_host_settings_clamp(h);
    }
    s.bh_freeplay_select   = g_store.get_bool("bh_freeplay_select", s.bh_freeplay_select);
    s.bh_roster_only       = g_store.get_bool("bh_roster_only", s.bh_roster_only);
    g_fork_wants_roster    = s.bh_freeplay_select;
    g_fork_hunters_only    = s.bh_roster_only;
    g_arena_hud_enabled    = s.arena_hud_overlay;
    s.coop_show_room_code  = g_store.get_bool("coop_show_room_code", s.coop_show_room_code);
    s.enhanced_controls    = g_store.get_bool("enhanced_controls", s.enhanced_controls);
    /* hud_radar_x/y/size and hud_ping_show/x/y/w/h are NOT read any more -
     * the settings are retired (ui.h) and reading them back is exactly how the
     * pre-measurement values outlived every correction. The store leaves the
     * old keys in the file untouched, so nothing is lost if this is reversed. */
    s.right_stick_invert_x = g_store.get_bool("right_stick_invert_x", s.right_stick_invert_x);
    s.right_stick_invert_y = g_store.get_bool("right_stick_invert_y", s.right_stick_invert_y);
    s.right_stick_deadzone = std::clamp(int(g_store.get_number(
                                 "right_stick_deadzone", s.right_stick_deadzone)), 5, 90);
    s.right_stick_speed_x = std::clamp(int(g_store.get_number(
                                "right_stick_speed_x", s.right_stick_speed_x)), 25, 600);
    s.right_stick_speed_y = std::clamp(int(g_store.get_number(
                                "right_stick_speed_y", s.right_stick_speed_y)), 25, 600);
    /* MIRRORED AFTER THE READS, not before them. The right-stick mirrors used
     * to be copied three lines above the loads that filled them, so the first
     * frame's stick sampling ran on the defaults and the file's values only
     * arrived with the first ui_draw. One frame, but a frame in which the
     * order of two blocks was the whole difference between a setting that is
     * loaded and one that is loaded eventually. */
    g_enhanced_controls    = s.enhanced_controls;
    g_mute_music_second    = s.mute_music_second_instance;
    g_mute_sound_second    = s.mute_sound_second_instance;
    g_enhanced_portrait    = s.enhanced_portrait;
    g_enhanced_slot_colour = s.enhanced_slot_colour;
    g_rs_invert_x = s.right_stick_invert_x;
    g_rs_invert_y = s.right_stick_invert_y;
    g_rs_deadzone = s.right_stick_deadzone;
    g_rs_speed_x = s.right_stick_speed_x;
    g_rs_speed_y = s.right_stick_speed_y;
    g_arena_hud_scale = s.arena_hud_scale;
    s.window_w             = int(g_store.get_number("window_w", s.window_w));
    s.window_h             = int(g_store.get_number("window_h", s.window_h));
    s.touch_window_w       = int(g_store.get_number("touch_window_w", s.touch_window_w));
    s.touch_window_h       = int(g_store.get_number("touch_window_h", s.touch_window_h));
    s.touch_panel_x        = float(g_store.get_number("touch_panel_x", s.touch_panel_x));
    s.touch_panel_y        = float(g_store.get_number("touch_panel_y", s.touch_panel_y));
    s.touch_panel_w        = float(g_store.get_number("touch_panel_w", s.touch_panel_w));
    s.touch_panel_h        = float(g_store.get_number("touch_panel_h", s.touch_panel_h));

    /* CLAMPED ON LOAD, not only in the UI. A hand-edited file is a supported
     * way to fix a bad binding, and a hand-edited file is also how an
     * out-of-range value gets in. */
    s.window_scale        = std::clamp(s.window_scale, 1, 6);
    s.internal_resolution = std::clamp(s.internal_resolution, 1, 4);
    s.texture_upscale     = std::clamp(s.texture_upscale, 1, 4);
    s.stick_deadzone      = std::clamp(s.stick_deadzone, 5, 90);

    std::snprintf(s.mp.player_name, sizeof s.mp.player_name, "%s",
                  g_store.get_string("mp.player_name", s.mp.player_name).c_str());
    /* THE SAME ONE-TIME LOCALISATION as the mp_master_host site above, under
     * the dotted key this second reader uses. Both keys exist because the store
     * was renamed once and both spellings are still read; a migration applied
     * to only one of them would be undone by whichever ran last. */
    {
        std::string host = g_store.get_string("mp.master_host",
                                              s.mp.master_host);
        if (g_store.get_number("mp_master_host_localised", 0) == 0) {
            if (host == "play.viosarcade.xyz") host = s.mp.master_host;
            g_store.set_number("mp_master_host_localised", 1);
        }
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host, "%s",
                      host.c_str());
    }
    s.mp.master_port = std::clamp(int(g_store.get_number("mp.master_port",
                                                        s.mp.master_port)), 1, 65535);
    s.mp.listen_port = std::clamp(int(g_store.get_number("mp.listen_port",
                                                        s.mp.listen_port)), 1, 65535);

    /* PUSH THE MULTIPLAYER TAB'S VALUES ONCE, HERE, as well as every frame
     * the tab is drawn.
     *
     * The per-frame push lives in the tab's draw function, which only runs
     * while that tab is OPEN. So a player who set their name, restarted, and
     * went straight to hosting without reopening the tab would have hosted as
     * "Maclunkey" - the name was in settings.json and in `s`, and had simply
     * never reached the session.
     *
     * IN BOTH BUILDS, AND AFTER EVERY SPELLING OF THE KEY. This push used to
     * sit inside the fork-only block above, so the baseline - whose co-op tab
     * is the same six fields and whose sessions are started from the game's
     * own Cantina console and pause menu, never from this tab - kept the
     * name and directory address it had loaded and never handed them to the
     * session until the tab happened to be opened. And it ran before the
     * dotted `mp.*` keys were read, so whichever spelling was read last was
     * not what the session got. Last thing before the bindings, so what is
     * pushed is what `s` will hold. */
    {
        Lsw2ForkSession& session = lsw2_fork_session();
        session.configure(s.mp.player_name, s.mp.master_host, s.mp.master_port,
                          s.mp.listen_port);
        session.configure_direct(s.mp.direct_host, s.mp.direct_port);
    }

    for (int i = 0; i < kBindingCount; ++i) {
        char key[96];
        std::snprintf(key, sizeof key, "bind.%s.key", g_bindings[i].action);
        g_bindings[i].scancode = int(g_store.get_number(key, g_bindings[i].scancode));
        std::snprintf(key, sizeof key, "bind.%s.pad", g_bindings[i].action);
        g_bindings[i].pad_button = int(g_store.get_number(key, g_bindings[i].pad_button));
    }
}

void ui_save_settings(const UiSettings& s, const char* path) {
    /* The path is taken from the load when there was one. Recomputing it from
     * the argument would be equivalent today and would quietly diverge the
     * moment a caller passes something different, which is exactly how the port
     * ended up with two settings files before. */
    const std::string target = g_store_path.empty() ? lsw2_settings_path(path)
                                                    : g_store_path;

    g_store.set_bool("preserve_aspect", s.preserve_aspect);
    g_store.set_bool("integer_scale", s.integer_scale);
    g_store.set_int("window_scale", s.window_scale);
    g_store.set_bool("linear_filter", s.linear_filter);
    g_store.set_bool("floating_touch", s.floating_touch);
    g_store.set_number("floating_touch_scale", s.floating_touch_scale);
    g_store.set_bool("floating_touch_only", s.floating_touch_only);
    g_store.set_bool("adaptive_widescreen", s.adaptive_widescreen);
    g_store.set_int("max_extra_width", s.max_extra_width);
    g_store.set_bool("opengl_renderer", s.opengl_renderer);
    g_store.set_int("internal_resolution", s.internal_resolution);
    g_store.set_int("texture_upscale", s.texture_upscale);
    g_store.set_bool("stick_as_dpad", s.stick_as_dpad);
    g_store.set_int("stick_deadzone", s.stick_deadzone);
    g_store.set_string("mp_direct_host", s.mp.direct_host);
    g_store.set_number("mp_direct_port", s.mp.direct_port);
    g_store.set_string("mp_player_name", s.mp.player_name);
    g_store.set_string("mp_master_host", s.mp.master_host);
    g_store.set_number("mp_master_port", s.mp.master_port);
    g_store.set_number("mp_listen_port", s.mp.listen_port);
    g_store.set_bool("arena_hud_overlay", s.arena_hud_overlay);
    g_store.set_number("arena_hud_size", s.arena_hud_scale);

    /* ONLY WHEN IT CHANGED, the same shape as the last-hunter write below.
     * The flag was already being maintained and already being cleared here;
     * nothing asked it, so all six keys were rewritten on every save whether
     * or not the player had ever opened the customize screen. */
    if (lsw2_customization_dirty()) {
        const uint16_t* slots = lsw2_customization_saved();
        char key[32];
        for (int i = 0; i < kLsw2CustomSlots; ++i) {
            std::snprintf(key, sizeof key, "custom_slot_%d", i);
            g_store.set_number(key, slots[i]);
        }
        g_store.set_bool("custom_moustache", lsw2_customization_moustache());
        g_store.set_bool("custom_sunglasses", lsw2_customization_sunglasses());
        lsw2_customization_mark_clean();
    }
    if (lsw2_last_hunter_dirty()) {
        g_store.set_number("bh_last_hunter", lsw2_last_hunter());
        lsw2_last_hunter_mark_clean();
    }
    /* ONLY THE FORK MAY WRITE THESE, and the reason is one line in launch.sh:
     * "ONE SETTINGS FILE for both launchers and both modes". A baseline run
     * never opens the host menu, so its session holds defaults - and writing
     * them unconditionally, as the first version of this did, meant one
     * baseline launch silently reset every host setting the player had
     * configured in the fork. The load above is guarded for the same reason:
     * there is nothing there for the baseline to want.
     *
     * ui_fork_build() is valid here because frontend.cpp sets it from LSW2_FORK
     * before it calls ui_load_settings(). */
    /* NOT WHILE JOINED TO SOMEBODY ELSE'S LOBBY.
     *
     * A client adopts the HOST's match rules into the same struct - that is how
     * the host's settings reach each client as it joins - so while
     * joined, `settings()` holds another player's configuration, not this
     * player's. Writing it here would mean joining one game permanently
     * replaced your own host settings with theirs, and you would find out the
     * next time you hosted.
     *
     * Skipping the write leaves the file holding what the player last chose,
     * which is exactly right: they have not changed it, they have borrowed
     * someone else's for a match. */
    const bool borrowed_rules =
        lsw2_fork_session().state() == Lsw2SessionState::Joined ||
        lsw2_fork_session().state() == Lsw2SessionState::Joining;

    if (ui_fork_build() && !borrowed_rules) {
        const Lsw2HostSettings& h = lsw2_fork_session().settings();
        g_store.set_bool("host_visible_master", h.visible_master);
        g_store.set_bool("host_visible_lan", h.visible_lan);
        /* NOR A TEST PIN. LSW2_TEST_ROSTER and LSW2_TEST_MUSIC (fork.cpp) pin
         * these two in the session every frame so a drive can reach a rule; a
         * process run with one must not save it as the player's choice. The
         * drives' settings copies carry host_music 1 because a copy was taken
         * from a file a pinned run had written. */
        if (!std::getenv("LSW2_TEST_ROSTER"))
            g_store.set_number("host_roster", int(h.roster));
        g_store.set_number("host_character", h.character);
        g_store.set_number("host_controls", int(h.controls));
        g_store.set_number("host_customization", int(h.customization));
        if (!std::getenv("LSW2_TEST_MUSIC"))
            g_store.set_number("host_music", int(h.music));
        g_store.set_number("host_lobby_music", int(h.lobby_music));
        g_store.set_number("host_lobby_time", int(h.lobby_time));
        g_store.set_number("host_extras", double(lsw2_host_extras_mask(h)));
    }
    g_store.set_bool("bh_freeplay_select", s.bh_freeplay_select);
    g_store.set_bool("bh_roster_only", s.bh_roster_only);
    g_store.set_bool("coop_show_room_code", s.coop_show_room_code);
    g_store.set_bool("enhanced_controls", s.enhanced_controls);
    g_store.set_bool("debug_mute_music_second_instance", s.mute_music_second_instance);
    g_store.set_bool("debug_mute_sound_second_instance", s.mute_sound_second_instance);
    g_store.set_bool("enhanced_portrait", s.enhanced_portrait);
    g_store.set_bool("enhanced_slot_colour", s.enhanced_slot_colour);
    /* The eight hud_radar and hud_ping keys are neither read nor written now;
     * see the load side. They persist in the file as untouched keys. */
    g_store.set_bool("right_stick_invert_x", s.right_stick_invert_x);
    g_store.set_bool("right_stick_invert_y", s.right_stick_invert_y);
    g_store.set_int("right_stick_deadzone", s.right_stick_deadzone);
    g_store.set_int("right_stick_speed_x", s.right_stick_speed_x);
    g_store.set_int("right_stick_speed_y", s.right_stick_speed_y);
    g_store.set_int("window_w", s.window_w);
    g_store.set_int("window_h", s.window_h);
    g_store.set_int("touch_window_w", s.touch_window_w);
    g_store.set_int("touch_window_h", s.touch_window_h);
    g_store.set_number("touch_panel_x", s.touch_panel_x);
    g_store.set_number("touch_panel_y", s.touch_panel_y);
    g_store.set_number("touch_panel_w", s.touch_panel_w);
    g_store.set_number("touch_panel_h", s.touch_panel_h);

    /* WRITTEN BY BOTH BUILDS even though only the fork shows the tab. The store
     * would preserve them anyway; writing them keeps the two builds' files
     * identical in content, so a diff between them means something. */
    g_store.set_string("mp.player_name", s.mp.player_name);
    g_store.set_string("mp.master_host", s.mp.master_host);
    g_store.set_int("mp.master_port", s.mp.master_port);
    g_store.set_int("mp.listen_port", s.mp.listen_port);

    /* THE BINDINGS. Stored as scancodes rather than names because
     * SDL_GetScancodeFromName does not round-trip every name, and a binding
     * that silently reverts is worse than none. */
    for (int i = 0; i < kBindingCount; ++i) {
        char key[96];
        std::snprintf(key, sizeof key, "bind.%s.key", g_bindings[i].action);
        g_store.set_int(key, g_bindings[i].scancode);
        std::snprintf(key, sizeof key, "bind.%s.pad", g_bindings[i].action);
        g_store.set_int(key, g_bindings[i].pad_button);
    }

    const std::vector<std::string> kept = g_store.untouched_keys();
    if (!g_store.save(target)) {
        std::fprintf(stderr, "[settings] FAILED to write %s\n", target.c_str());
        return;
    }
    if (!kept.empty()) {
        /* SAY WHAT WAS PRESERVED. These are keys the other build - or a future
         * one - owns, and the whole point of the store is that this build did
         * not delete them. Printing the count makes that visible rather than
         * something to be taken on trust. */
        std::fprintf(stderr, "[settings] %s written; %zu key(s) from another "
                             "build preserved\n", target.c_str(), kept.size());
    }
}
