/* See ui.h. */

#include "ui.h"
#include "instancemute.h"   /* the Debug tab's second-instance mutes */
#include "widescreen.h"

#include "imgui.h"
#include "imgui_internal.h"   /* BringWindowToDisplayBack, for the panel */
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "backends/imgui_impl_opengl3.h"
#include "uiwidgets.h"

#include <SDL_opengl.h>      /* glReadPixels, for LSW2_UI_SHOT */

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
/* The top window, for LSW2_UI_SHOT's drawable size under OpenGL. */
SDL_Window* g_window = nullptr;
double g_fps = 0.0;
uint64_t g_frames = 0;
uint64_t g_underruns = 0;
void* g_touch_texture = nullptr;
SDL_Rect g_floating_rect{};
bool g_floating_shown = false;
/* Whether the floating panel was the window under the mouse last frame. See
 * ui_handle_event: a click over the panel is the game's only when nothing of
 * the menu's is on top of it. */
bool g_floating_hovered = false;

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

/* THE PAGES. Each function below is one page of the menu - one entry in a
 * section's sidebar - and holds every setting that page shows, with its
 * tooltip and its behaviour. How a checkbox or a slider LOOKS is uiwidgets.h's
 * business; what it does is decided here.
 *
 * Every page is drawn by draw_page(), by name and never through a table of
 * function pointers: the source sweeps follow calls by name to decide what is
 * reachable, and a page reached only through a pointer would make everything
 * it calls - the cheats' warp, the fork's probes - look dead. */

/* Settings > Display: how the frame is shown, and the window's size. */
void display_page(UiSettings& s) {
    uiw::columns(2);
    uiw::section("Presentation");
    uiw::checkbox("Preserve aspect ratio", &s.preserve_aspect,
        "Letterbox rather than stretch.\n\n"
        "This changes how the frame is SHOWN. It never changes the\n"
        "projection the game produced - the guest still renders 256x192\n"
        "and still believes it is on a DS.");

    ImGui::BeginDisabled(!s.preserve_aspect);
    uiw::checkbox("Integer scale", &s.integer_scale,
                  "Snap to whole multiples of the guest pixel so every\n"
                  "one is the same size. Leaves a wider border.");
    ImGui::EndDisabled();

    uiw::next_column();
    uiw::section("Gameplay window size");
    uiw::note("Applies to the top screen only; the touch window keeps its own size.");
    uiw::slider_int("Scale", &s.window_scale, 1, 6, "%dx");
    if (uiw::button("Apply", "Resize the gameplay window to this scale now."))
        s.applied_scale = s.window_scale;
    uiw::note("%d x %d per screen", kScreenWidth * s.window_scale,
              kScreenHeight * s.window_scale);

    uiw::section("Presets");
    struct { const char* name; int scale; } presets[] = {
        {"1x  256x192",  1}, {"2x  512x384",  2}, {"3x  768x576",  3},
        {"4x 1024x768",  4}, {"5x 1280x960",  5}, {"6x 1536x1152", 6},
    };
    /* Three to a row, sized from the column rather than a fixed pixel width,
     * so the row fits whatever width the column has at this window size. */
    const float preset_w = (ImGui::GetContentRegionAvail().x -
                            2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f / uiw::scale();
    for (int i = 0; i < 6; ++i) {
        if (i % 3) uiw::same_line();
        if (uiw::button(presets[i].name, "Snap the gameplay window to this size.",
                        preset_w)) {
            s.window_scale = presets[i].scale;
            s.applied_scale = presets[i].scale;
        }
    }
    uiw::text(
        "The window stays freely resizable; a preset just snaps it to an exact "
        "multiple so every guest pixel is the same size.");
    uiw::end_columns();
}

/* Settings > Graphics: the renderer, its sample density, and widescreen. */
void graphics_page(UiSettings& s) {
    uiw::columns(2);
    uiw::section("Rendering quality");

    /* WHY THE UI SAYS "RESTART" AND MEANS IT. The internal scale is compiled
     * into the compute shaders and sizes every framebuffer, so the framework
     * refuses to change it under a live renderer; and SDL_WINDOW_OPENGL cannot
     * be added to a window that already exists. A slider that appeared to work
     * and quietly did nothing would be worse than one that says so. */
    const bool gl_live = lsw2_gl_active();
    uiw::checkbox("OpenGL renderer", &s.opengl_renderer,
        "Render the 3D layer on the GPU and present it directly.\n\n"
        "This is what makes the two settings below possible: the extra\n"
        "detail they produce exists only in a GPU texture and never\n"
        "reaches the CPU framebuffer the software path presents from.\n\n"
        "Needs OpenGL 4.3. If it is unavailable the port falls back to\n"
        "the software path and says so on startup.");

    ImGui::BeginDisabled(!s.opengl_renderer);
    uiw::slider_int("3D resolution", &s.internal_resolution, 1, 4, "%dx",
        "Render the 3D layer at this multiple of 256x192.\n\n"
        "This is the setting that fixes chunkiness at large window\n"
        "sizes. The DS renders 256x192, so scaling up magnifies every\n"
        "polygon edge; rendering at 3x means there is real detail to\n"
        "show instead of bigger pixels.\n\n"
        "The guest is unaffected - it still reads the native surface.");
    /* NAMED FOR WHAT IT DRIVES. This combo feeds nds_texture_upscale_set_factor
     * - an xBR resample of each decoded texture at 2x or 4x - and it was
     * labelled "Texture filtering", which is the name of the checkbox below
     * that does something else. A player reading "filtering" here and
     * "Linear filtering" there had two controls with one name. */
    const char* upscales[] = {"Off", "2x", "4x"};
    int upscale_index = s.texture_upscale >= 4 ? 2 : s.texture_upscale >= 2 ? 1 : 0;
    if (uiw::combo("Texture upscale", &upscale_index, upscales, 3,
            "Resample each decoded DS texture once (xBR), when it is\n"
            "first used. Restart to apply.\n\n"
            "Worth turning on alongside a higher 3D resolution: without it\n"
            "the geometry sharpens but the textures on it stay blocky."))
        s.texture_upscale = upscale_index == 2 ? 4 : upscale_index == 1 ? 2 : 1;
    ImGui::EndDisabled();

    /* A KNOWN DEFECT, SAID OUT LOUD. Either of the two settings above above 1x
     * turns on the compute renderer's HD compositing path, and that path
     * renders the Arena Lobby's hologram as a solid white shape instead of a
     * translucent one. Bisected: 1x/1x is correct, 2x/1x and 1x/2x both break
     * it, and the software renderer is correct at any setting - so it is the
     * HD path, not the upscaling ratio.
     *
     * Shown here rather than buried in a document because the alternative is a
     * player turning on a quality setting and quietly getting a broken picture.
     * It sits under both controls now rather than between the slider and its
     * tooltip, where it once took the slider's tooltip for itself. */
    if (s.opengl_renderer &&
        (s.internal_resolution > 1 || s.texture_upscale > 1)) {
        uiw::warn("Known: above 1x, the Cantina hologram renders as a solid "
                  "white shape. Set both to 1x to avoid it.");
    }

    /* Report what is ACTUALLY running next to what is selected, so a failed
     * OpenGL start is visible here and not only in a startup log nobody reads. */
    if (s.opengl_renderer != gl_live) {
        uiw::warn("%s", gl_live
            ? "Running on OpenGL - restart to go back to software."
            : "Running on software - restart to apply, or OpenGL 4.3 is "
              "unavailable here (the startup log says which).");
    } else {
        uiw::note("now: %s%s", gl_live ? "OpenGL" : "software",
                  gl_live ? "" : " (3D resolution unavailable)");
    }

    uiw::spacing();
    /* WHAT IT REACHES, said where the player can read it. The SDL present
     * path honours this on both screens (SDL_SetTextureScaleMode) and the
     * floating panel's GL texture does too (lsw2_gl_touch_texture). The
     * OpenGL TOP screen does not: nds_compute_host_present_top samples
     * integer textures with texelFetch inside its own shader and offers no
     * filter switch, so under the default renderer this checkbox changes the
     * touch screen only. Saying so here is the difference between a setting
     * that is honest about its reach and one that looks broken. */
    uiw::checkbox("Linear filtering", &s.linear_filter,
                  "Off is nearest-neighbour, which is usually what you\n"
                  "want for a DS: this is pixel art.\n\n"
                  "Under OpenGL the top screen is always nearest - the\n"
                  "GPU presenter samples exact texels - so this applies\n"
                  "to the touch screen and the floating panel.");
    if (gl_live) uiw::note("(touch screen only under OpenGL)");

    uiw::next_column();
    uiw::section("Widescreen");
    uiw::checkbox("Adaptive widescreen", &s.adaptive_widescreen,
        "Widen the 3D viewport to match the window instead of\n"
        "letterboxing, so more of the world is visible.\n\n"
        "Under OpenGL the width is fixed when the renderer starts,\n"
        "so this takes effect on the next launch.");
    /* SAY "RESTART" BESIDE THE CHECKBOX, not under the slider. Under OpenGL
     * the raster width is settled before the compute renderer exists and
     * cannot move afterwards (widescreen.cpp locks it), so a click here is a
     * request for the next launch. The note used to sit at the bottom of the
     * slider block, where a player who only ever touched the checkbox would
     * not read it and would report the checkbox as doing nothing. */
    if (lsw2_widescreen_locked()) {
        uiw::same_line();
        uiw::note("(applies on restart)");
    }
    ImGui::BeginDisabled(!s.adaptive_widescreen);
    /* The ceiling comes from the renderer, not from a number picked here. A
     * slider that offers widths the rasteriser refuses looks like a setting
     * that does nothing. */
    const int max_extra = lsw2_widescreen_max_extra();
    if (s.max_extra_width > max_extra) s.max_extra_width = max_extra;
    uiw::slider_int("Max extra width", &s.max_extra_width, 0, max_extra, "%d px",
        "How far past the native 256 px the 3D raster may widen.\n\n"
        "The width actually used is whatever makes the picture fill\n"
        "the window exactly; this is only the ceiling. Lower it if a\n"
        "very wide view shows scenery the level was never built for.\n\n"
        "The rasteriser cannot go past 448 px total, which is 2.33:1 -\n"
        "wider than 21:9. On a window wider than that the picture stops\n"
        "growing and the remainder stays letterboxed.", 8);
    uiw::note("raster now: %d x 192", lsw2_widescreen_width());
    if (lsw2_widescreen_locked()) {
        /* The width cannot follow the window on this path, so say what it is
         * following instead. Without this the bars on a differently-shaped
         * window look like a bug rather than a consequence. */
        uiw::note("%s", "fixed for this session: sized to the display");
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
        uiw::warn("%s", "OBJECTS to the left and right of centre pop in and out "
                        "until they are near the middle. Level geometry is fine.");
    }
    uiw::end_columns();
}

/* Settings > Controls: the stick, and the binding table. */
void controls_page(UiSettings& s) {
    uiw::columns(2);
    uiw::section("Analog stick");
    uiw::checkbox("Stick as D-pad", &s.stick_as_dpad,
        "The DS has no analog input, so a stick can only drive the same\n"
        "four direction bits the D-pad does - there is nothing finer to\n"
        "send. This is what every DS emulator does.\n\n"
        "The pad's buttons were read from the start and its sticks were\n"
        "not read at all, which is why a controller only half-worked.");
    ImGui::BeginDisabled(!s.stick_as_dpad);
    uiw::slider_int("Deadzone", &s.stick_deadzone, 5, 90, "%d%%",
        "How far the stick must move before it counts as a direction.\n"
        "Raise it if the character wanders on its own - a worn stick\n"
        "rests off centre.");
    ImGui::EndDisabled();

    uiw::spacing();
    /* ONE LINE, AND A TRUE ONE. The second line here said right-stick camera
     * control was "planned for the Bounty Hunter fork", forty lines below the
     * controls that drive it. A stale promise under a working control reads
     * as the control not working. */
    uiw::note("The stylus is the mouse, on the touch screen or the floating panel.");
    /* The camera controls the right stick drives are an enhancement and live
     * on that section's Arena Controls page; the Camera rows of the binding
     * table beside this are the keyboard's way to the same camera. */
    uiw::note("The arena camera's settings are under Enhancements > Arena Controls.");

    uiw::next_column();
    uiw::section("Buttons");
    if (uiw::begin_block("Buttons",
            "bindings rebind keyboard key gamepad controller pad button "
            "A B X Y L R Start Select D-pad camera reset defaults")) {
        uiw::text("Click a binding to change it, then press the key or button "
                  "you want. Escape cancels.");
        uiw::spacing();

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

                /* Centred on the row's buttons, which are padded more than
                 * an ordinary frame. */
                ImGui::TableNextColumn();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(10.0f * uiw::scale(), 8.0f * uiw::scale()));
                ImGui::AlignTextToFramePadding();
                ImGui::PopStyleVar();
                ImGui::TextUnformatted(b.action);

                ImGui::TableNextColumn();
                const bool waiting_key = (g_capture_index == i && !g_capture_pad);
                const char* key_label = waiting_key
                    ? "press a key..."
                    : (b.scancode >= 0 ? SDL_GetScancodeName(SDL_Scancode(b.scancode))
                                       : "unbound");
                /* Each cell in its own ID scope: a row whose key and pad are
                 * both "unbound" would otherwise be two buttons with one ID. */
                ImGui::PushID("key");
                if (uiw::button(*key_label ? key_label : "unbound")) {
                    g_capture_index = i;
                    g_capture_pad = false;
                }
                ImGui::PopID();

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
                ImGui::PushID("pad");
                if (uiw::button(pad_label)) {
                    g_capture_index = i;
                    g_capture_pad = true;
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        uiw::spacing();
        if (uiw::button("Reset to defaults", nullptr, -1.0f)) {
            static const int keys[] = {
                SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_S, SDL_SCANCODE_A,
                SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_RETURN,
                SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
                SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_I, SDL_SCANCODE_K};
            /* One default per row: a short initialiser zero-fills the rest,
             * and the camera rows came back unbound. */
            static_assert(sizeof keys / sizeof *keys == kBindingCount, "");
            for (int i = 0; i < kBindingCount; ++i) g_bindings[i].scancode = keys[i];
            g_capture_index = -1;
        }
        uiw::end_block();
    }
    uiw::end_columns();
}

/* Settings > Touch Screen: where the DS's lower screen is shown. */
void touch_page(UiSettings& s) {
    uiw::columns(2);
    uiw::section("Floating touch panel");
    uiw::text(
        "The DS touch screen normally gets its own window. It can instead "
        "float inside this one as a movable panel.");
    uiw::spacing();

    uiw::checkbox("Floating touch panel", &s.floating_touch,
                  "Show the touch screen as a panel inside the gameplay\n"
                  "window, which can be moved by its title bar and resized\n"
                  "by its corner.");
    ImGui::BeginDisabled(!s.floating_touch);
    /* A MOVED SLIDER IS A RESIZE REQUEST. The panel remembers the size it was
     * dragged to and that memory outranked this slider on every frame after
     * the first, so once the panel had been shown the slider changed nothing
     * (see the note over SetNextWindowSize in ui_draw). Moving it now asks
     * for the preset size, the same way the window Scale slider's Apply does
     * for the top window. The -/+ buttons are moves too. */
    if (uiw::slider_float("Panel scale", &s.floating_touch_scale, 0.5f, 3.0f,
                          "%.2fx", 0.25f,
                          "Resizes the panel to this multiple of 256x192.\n"
                          "Dragging its corner afterwards is remembered too."))
        g_panel_resize = true;
    uiw::checkbox("Hide touch window", &s.floating_touch_only,
                  "Stop showing the separate touch window while the panel\n"
                  "is doing its job, so the same screen is not shown twice.");
    ImGui::EndDisabled();

    uiw::next_column();
    uiw::section("Using the panel");
    uiw::note(
        "The panel is clickable: clicks map back to stylus coordinates through "
        "the same transform used to draw it.");
    /* THE MENU COVERS THE PANEL. The menu is drawn over the whole game window
     * now, the panel included, so the panel is touched with the menu closed;
     * it stays visible, dimmed, behind the menu like the rest of the game. */
    uiw::note("While this menu is open it covers the panel with the rest of the "
              "game; close the menu to touch it.");
    if (s.arena_hud_overlay)
        uiw::note("The floating touch panel is hidden during matches while the "
                  "arena HUD overlay is on (Enhancements > Arena HUD).");
    uiw::end_columns();
}

/* Enhancements > Arena Controls: the Enhanced control set. */
void arena_controls_page(UiSettings& s) {
    uiw::columns(2);
    uiw::section("Enhanced arena controls");
    uiw::checkbox("Enhanced controls", &s.enhanced_controls,
        "Free camera on the right stick and ordinary movement on the\n"
        "left, in arena gameplay only - the lobby keeps its usual\n"
        "controls.\n\n"
        "Turning this OFF always wins: you get tank controls even in a\n"
        "match whose host has Enhanced enabled. The reverse is not true -\n"
        "a host who disallows it still disallows it.");
    /* SAY WHAT IS LIVE, and say it precisely - this is the copy the player
     * reads. It said the checkbox gives movement "the camera's heading instead
     * of the ship's" by writing the actor's view index, and that the stick
     * writes the view record's yaw. Neither ever moved anything in an arena:
     * the index is never the sentinel there, and the record is overwritten by
     * the next exchange (enhanced.cpp has the measurements). The checkbox now
     * switches the arena's per-player camera mode through the game's own
     * message, and the stick steers that camera. */
    uiw::note("In arena matches: the left stick or d-pad moves you relative to "
              "the camera, and the right stick (or the Camera bindings under "
              "Settings > Controls) turns it around you and raises or lowers "
              "it. The lobby keeps the usual controls.");

    uiw::next_column();
    uiw::section("Camera");
    ImGui::BeginDisabled(!s.enhanced_controls);
    uiw::checkbox("Invert camera X", &s.right_stick_invert_x,
                  "Reverse which way the right stick turns the camera.");
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
    uiw::checkbox("Invert camera Y", &s.right_stick_invert_y,
                  "Reverse which way the right stick raises and lowers\n"
                  "the camera.");
    uiw::slider_int("Camera deadzone", &s.right_stick_deadzone, 5, 90, "%d%%",
                    "How far the right stick must move before the camera\n"
                    "turns.");
    uiw::slider_int("Camera turn speed", &s.right_stick_speed_x, 25, 600, "%d%%",
                    "How fast the camera turns around you, as a percentage of\n"
                    "the original rate.", 25);
    uiw::slider_int("Camera height speed", &s.right_stick_speed_y, 25, 600, "%d%%",
                    "How fast the camera rises and falls, as a percentage of\n"
                    "the original rate.", 25);
    ImGui::EndDisabled();
    uiw::end_columns();
}

/* Enhancements > Arena HUD: the match HUD on the top screen. */
void arena_hud_page(UiSettings& s) {
    /* The arena match HUD on the top screen. It REPLACES the touch screen
     * during a match - the floating panel is not drawn at all then - which is
     * why the Touch Screen page mentions it; it lives here because it is an
     * enhancement, offered as a setting and off by default. */
    uiw::section("Match HUD on the top screen");
    uiw::checkbox("Arena HUD overlay", &s.arena_hud_overlay,
        "During an arena match, mirrors the other players' column to the\n"
        "lower left, the radar to the top right and the signal meter to\n"
        "the bottom right, and hides the touch screen.\n\n"
        "Only during a match - the lobby keeps its touch screen, which\n"
        "is where the character select lives.\n\n"
        "Copied from the real touch screen, in player order rather than\n"
        "score order - the game draws them, this only moves them.");

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
     * so the toggle above is the whole of what this page has to offer for the
     * arena HUD. The
     * old settings keys are left untouched in the file by the store. */
    ImGui::BeginDisabled(!s.arena_hud_overlay);
    uiw::slider_float("HUD size", &s.arena_hud_scale, 0.5f, 1.5f, "%.2fx", 0.05f,
                      "The mirrored HUD's size on the top screen: 1.00x is the\n"
                      "DS's own size, 0.50x is half.");
    ImGui::EndDisabled();
    if (s.arena_hud_overlay) {
        uiw::note("The floating touch panel is hidden during matches.");
    }
}

/* Enhancements > Lobby: what the lobby's lower screen offers. */
void lobby_page(UiSettings& s) {
    /* THE LOBBY CHARACTER SELECT LIVES HERE, not on the Multiplayer page.
     *
     * This group was once on Multiplayer and it is not a networking setting -
     * it decides what the LOWER SCREEN SHOWS in the lobby. The Multiplayer
     * page holds only settings that have no sensible in-game home - the
     * player name and the master server address - so anything else sitting on
     * it was miscategorised by that rule as well as by feel. It replaces the
     * cartridge's own Party Members tab, which makes it an enhancement. */
    uiw::section("Lobby character select");
    /* A flag for whether the select shows all characters or only the bounty
     * hunters, defaulting to only bounty hunters. It is not specified as a game
     * menu row - so by the rule above it lives here. It is not a match setting;
     * the host's own Roster row is, and that one is in the game's host menu. */
    uiw::checkbox("Free Play select",
                  &s.bh_freeplay_select,
        "The lobby's touch screen shows the Free Play character select,\n"
        "so a hunter can be picked and customized before joining.\n\n"
        "Turning it off restores the stock Party Members tab - which is\n"
        "the way back if the roster screen misbehaves in the lobby.");
    /* GREYED WHILE A SESSION IS UP, because the host owns this then, and a
     * control that still moves while being ignored is worse than one that
     * refuses. See lsw2_hle_fork_hunters_only() for the precedence. */
    const bool host_owns_roster = lsw2_fork_session().in_session();
    ImGui::BeginDisabled(!s.bh_freeplay_select || host_owns_roster);
    uiw::checkbox("Bounty Hunters only", &s.bh_roster_only,
        host_owns_roster
          ? "The host's Roster setting decides this during a session.\n\n"
            "A client cannot widen its own roster - that would mean picking\n"
            "from twelve pages while everyone else picks from two."
          : "Restricts the select to the two Bounty Hunters pages, cycling\n"
            "between them in both directions.\n\n"
            "Off shows all twelve character pages.");
    ImGui::EndDisabled();
    if (host_owns_roster) {
        uiw::same_line();
        uiw::note("(host decides)");
    }
}

/* Enhancements > Portraits: the top-screen portrait fixes. */
void portraits_page(UiSettings& s) {
    uiw::section("Top-screen portrait");
    /* The optional enhancements, and they belong in BOTH builds - they are
     * specified for both, and the baseline's customize panel applies hats
     * through the same function. */
    uiw::checkbox("Player-colour ring", &s.enhanced_slot_colour,
        "Recolour the top-screen portrait ring to this player's\n"
        "colour, in the hunter lobby and the co-op netgame.");
    uiw::checkbox("Portrait follows character", &s.enhanced_portrait,
        "Changing character on the Free Play screen leaves the top-screen\n"
        "portrait showing the old one until you tag a partner and back.\n\n"
        "The touch path swaps the party without asking the game to re-read\n"
        "it; the d-pad path does. On, the same refresh runs either way.");
}

/* Debug > General: the live numbers, the metrics window, the debug surface. */
void debug_general_page(UiSettings& s) {
    uiw::columns(2);
    uiw::section("Performance");
    if (uiw::begin_block("Performance",
                         "fps frames presented audio underruns performance stats")) {
        uiw::text("%.1f FPS", g_fps);
        uiw::text("%llu frames presented", (unsigned long long)g_frames);
        if (g_underruns)
            uiw::warn("%llu audio underruns", (unsigned long long)g_underruns);
        else
            uiw::note("no audio underruns");
        uiw::end_block();
    }

    uiw::spacing();
    uiw::checkbox("ImGui metrics", &s.show_metrics,
                  "Open imgui's own metrics window over the menu.");

    uiw::next_column();
    uiw::section("Remote control");
    if (uiw::begin_block("Remote control",
                         "debug surface pilot remote capture screenshot port 19842")) {
        uiw::text(
            "This build serves a debug surface on 127.0.0.1:19842 - framebuffer "
            "capture, memory and register reads, and input injection.");
        uiw::note("tools/pilot.py shot out.png");
        uiw::note("tools/pilot.py press start");
        uiw::end_block();
    }
    uiw::end_columns();
}

/* Debug > Instances: several clients on one machine, heard once. */
void debug_instances_page(UiSettings& s) {
    uiw::section("When another copy of the game is already running");
    uiw::checkbox("Mute music", &s.mute_music_second_instance,
                  "A second copy of the game plays no music, so several\n"
                  "clients on one machine are not heard several times over.");
    uiw::same_line();
    uiw::checkbox("Mute all sound", &s.mute_sound_second_instance,
                  "A second copy of the game plays no sound at all.");
    uiw::note("%s", lsw2_instance_is_secondary()
                        ? "This window is a second copy - these apply to it."
                        : "This window is the first copy - these do not apply to it.");
}

/* Debug > Probes: the fork's guest-call probes.
 *
 * Each is one call into the cartridge's own code, armed by the button and
 * fired from the fork tick, and each reports with a log line rather than
 * changing anything a player sees. That is a debugging instrument, so it
 * lives in the section that says so. Fork only, because every one of them
 * refuses in the baseline and a button that only prints "fork build only"
 * is noise. Labels are the noun; the tooltip carries the explanation. */
void debug_probes_page() {
    uiw::section("Probes");
    uiw::note("Each probe makes one call into the game and reports in the log.");
    if (uiw::button("Probe camera yaw",
            "During a match, writes a quarter turn into the camera's yaw\n"
            "and reports next frame whether it stuck. Outside a match it\n"
            "refuses and says so.", -1.0f))
        lsw2_enhanced_probe_camera();
    uiw::same_line();
    if (uiw::button("Probe gear applier",
            "One call to the game's own part attacher, and a\n"
            "log line saying whether it returned.", -1.0f))
        lsw2_partapply_arm();
    uiw::same_line();
    if (uiw::button("Seat players",
            "Runs the cartridge's own player-count bootstrap\n"
            "with the session's real count.", -1.0f))
        lsw2_countboot_arm();
}

/* The name and the two addresses, which both builds' Multiplayer pages share.
 *
 * ONE FIELD WIDTH FOR THE WHOLE FORM, by construction now. imgui draws a
 * label to the RIGHT of its widget, so with no explicit width every field took
 * its own and the labels - "Player name", "Host", "Port", "Host or IP",
 * "Listen port" - started at five different x positions down a single column;
 * both builds once pinned one width to line them up. The menu's fields carry
 * their labels ABOVE them and fill the column, so every field and every label
 * starts at the same x without a width to keep in step. */
void master_server_fields(UiSettings& s) {
    uiw::section("Player");
    uiw::input_text("Player name", s.mp.player_name, sizeof s.mp.player_name,
                    "Shown to other players and on the master server list.");
    uiw::note("Shown to other players and on the master server list.");

    uiw::section("Master server");
    uiw::input_text("Host", s.mp.master_host, sizeof s.mp.master_host,
                    "The lobby directory's address.");
    uiw::input_int("Port", &s.mp.master_port, "The lobby directory's port.");
    /* A port outside the legal range is not a preference, it is a typo, and
     * clamping here means nothing downstream has to check it. Port 0 is legal
     * to bind but meaningless to connect to, so the floor is 1. */
    s.mp.master_port = std::clamp(s.mp.master_port, 1, 65535);

    uiw::spacing();
    if (uiw::button("Use 127.0.0.1", "A master server running on this machine.")) {
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host, "127.0.0.1");
        s.mp.master_port = 28553;
    }
    if (uiw::button("Use lsw2server.viosarcade.xyz", "The hosted lobby directory.")) {
        std::snprintf(s.mp.master_host, sizeof s.mp.master_host,
                      "lsw2server.viosarcade.xyz");
        s.mp.master_port = 80;
    }
}

/* Direct connect and this machine's listen port, also shared. */
void direct_connect_fields(UiSettings& s, const char* offered) {
    uiw::section("Direct connect");
    /* THE OTHER HALF OF THE 2026-08-27 DECISION. The room code is entered in
     * the game, on a d-pad, because four characters is what a d-pad can do
     * without an invented keyboard. A full address is not, so it lives here and
     * the in-game screen offers it as a second way in. */
    uiw::input_text("Host or IP", s.mp.direct_host, sizeof s.mp.direct_host,
                    "One specific host to join, by address.");
    uiw::input_int("Host port", &s.mp.direct_port, "That host's listen port.");
    s.mp.direct_port = std::clamp(s.mp.direct_port, 1, 65535);
    uiw::note("%s", offered);

    uiw::section("This machine");
    uiw::input_int("Listen port", &s.mp.listen_port,
                   "The port other players connect to when you host.");
    s.mp.listen_port = std::clamp(s.mp.listen_port, 1, 65535);
    uiw::note("The port other players connect to when you host.");
}

/* The fork's networking configuration, and for now ONLY the
 * configuration - nothing here connects to anything yet. It exists first
 * because the settings have to be persisted and shown before the transport is
 * written, and because the master server host is already known.
 *
 * FORK-ONLY, deliberately. The base recomp is meant to stay close to the
 * original game, so the baseline gets its own co-op page below rather than
 * this one. */
void multiplayer_tab(UiSettings& s) {
    /* WHAT BELONGS ON THIS PAGE, and nothing else.
     *
     * The imgui Multiplayer page is limited to settings with no sensible
     * in-game home: the player/DS name, the master server address, and
     * whatever else the design does not place in the game's own UI.
     *
     * So: the name, the directory address, the listen port. Every match
     * setting - Roster, Extras, Controls, Customization, Music, LOBBY TIME -
     * is an IN-GAME menu row and is deliberately absent here. */
    Lsw2ForkSession& session = lsw2_fork_session();

    uiw::note("Name and server address only. Match settings are in the game, "
              "on the host menu.");
    uiw::columns(2);
    master_server_fields(s);
    uiw::note("Local for testing on this machine; the real host otherwise.");

    uiw::next_column();
    direct_connect_fields(s, "Offered in the game's Join screen as \"Join <address>\".");
    uiw::spacing();
    uiw::text("Will connect to:  %s:%d", s.mp.master_host, s.mp.master_port);

    /* PUSH THE SETTINGS DOWN EVERY FRAME rather than on a button.
     *
     * A page that needs an Apply press is a page someone forgets to press, and
     * the values are three copies rather than a computation - pushing them is
     * cheaper than explaining when they take effect. */
    session.configure(s.mp.player_name, s.mp.master_host, s.mp.master_port,
                      s.mp.listen_port);
    session.configure_direct(s.mp.direct_host, s.mp.direct_port);

    uiw::section("Session");
    if (uiw::begin_block("Session",
                         "session status hosting joined connecting offline "
                         "match players room code error")) {
        switch (session.state()) {
        case Lsw2SessionState::Offline:
            uiw::text("Offline.");
            uiw::note("Host or join from the arena menu inside the game.");
            break;
        case Lsw2SessionState::Hosting:
            uiw::text("Hosting on port %d - %d/4 players", s.mp.listen_port,
                      session.player_count());
            if (session.console().room_code[0])
                uiw::text("Room code: %s", session.console().room_code);
            else
                uiw::note("No room code - the directory has not issued one.");
            break;
        case Lsw2SessionState::Joining:
            uiw::text("Connecting...");
            break;
        case Lsw2SessionState::Joined:
            uiw::text("Joined - %d/4 players", session.player_count());
            break;
        case Lsw2SessionState::InMatch:
            uiw::text("In a match.");
            break;
        }
        if (session.last_error() && *session.last_error())
            uiw::error("%s", session.last_error());
        uiw::end_block();
    }
    uiw::end_columns();
}

/* THE BASELINE'S CO-OP PAGE - the baseline's online co-op configuration.
 *
 * The surface is divided explicitly, and the division is the reverse of the
 * fork's in one place. This page holds the master server address (by default
 * the fork's), connecting directly by IP, and the room code - and the room
 * code of the current lobby is shown on this imgui page, concealed until a
 * checkbox is ticked, where the Bounty Hunter fork shows it in the game. In
 * the baseline, a co-op lobby is hosted or joined through the pause menu or
 * through a console entity in the Cantina's central hub.
 *
 * So the page is CONFIGURATION ONLY, and deliberately so: hosting and joining
 * happen on the cartridge's own screens, which the pause menu and the
 * Cantina's console entity lead to. Adding an imgui Host button here would
 * give a control with no requirement behind it, in the one build that exists to leave the game's UI presentation as
 * it is.
 *
 * WHAT IS SHARED WITH THE FORK, and why that is not laziness: the master
 * address defaults to the same one, the transport is the same, and the
 * settings file is already shared between the two launchers. Two copies of
 * these four fields would be two things to keep in step for no gain - which is
 * why both pages draw them from the same two functions. */
void coop_tab(UiSettings& s) {
    Lsw2ForkSession& session = lsw2_fork_session();

    uiw::note("Online co-op. Games are started and joined in the game - from "
              "the Cantina's console, or the pause menu - the way the cartridge "
              "does it. This page is the address book.");
    uiw::columns(2);
    master_server_fields(s);
    uiw::note("The same directory the Bounty Hunter fork uses. Co-op lobbies "
              "are listed separately from arena ones.");

    uiw::next_column();
    direct_connect_fields(s, "Offered in the game's own join list as a direct entry.");

    session.configure(s.mp.player_name, s.mp.master_host, s.mp.master_port,
                      s.mp.listen_port);
    session.configure_direct(s.mp.direct_host, s.mp.direct_port);

    uiw::section("Room code");
    if (uiw::begin_block("Room code", "show room code lobby join share hidden")) {
        /* THE ONE PLACE THE BASELINE DIFFERS FROM THE FORK BY DESIGN. The fork
         * puts its code on an in-game console screen; the baseline's goes
         * here, behind a checkbox that starts unticked. */
        uiw::checkbox("Show room code", &s.coop_show_room_code,
            "Hidden by default: a code on screen is a code anyone watching\n"
            "a stream or a screenshot can join with.");
        if (!s.coop_show_room_code) {
            uiw::note("Hidden.");
        } else if (session.state() == Lsw2SessionState::Offline) {
            uiw::note("No lobby - host one from the game first.");
        } else if (session.console().room_code[0]) {
            uiw::text("Room code: %s", session.console().room_code);
            /* SAY WHAT THE PAGE IS SHOWING, once per code. LSW2_PRESENT_SHOT
             * sees the DS frames and not this overlay, so the requirement that
             * the code be shown on the imgui Multiplayer page behind a checkbox
             * had no instrument that could see it - a drive could prove the
             * checkbox was wired and the code was issued, and still not that
             * this text was ever drawn. This line is drawn from the same branch
             * as the text, so it is only printed when the text is, which is the
             * traced value that was missing. (LSW2_UI_SHOT now captures the
             * menu itself as well.) */
            static char said[8] = {0};
            if (std::strncmp(said, session.console().room_code, sizeof said - 1) != 0) {
                std::strncpy(said, session.console().room_code, sizeof said - 1);
                said[sizeof said - 1] = '\0';
                std::fprintf(stderr, "[ui] Multiplayer tab is drawing \"Room code: "
                                     "%s\" (checkbox on)\n", said);
            }
        } else {
            uiw::note("The directory has not issued one.");
        }
        uiw::end_block();
    }

    uiw::section("Session");
    if (uiw::begin_block("Session",
                         "session status advertising hosting joined connecting "
                         "offline game players error")) {
        uiw::text("Advertising as:  %s", session.kind());
        switch (session.state()) {
        case Lsw2SessionState::Offline:
            uiw::text("Offline.");
            break;
        case Lsw2SessionState::Hosting:
            uiw::text("Hosting on port %d - %d/4 players", s.mp.listen_port,
                      session.player_count());
            break;
        case Lsw2SessionState::Joining:
            uiw::text("Connecting...");
            break;
        case Lsw2SessionState::Joined:
            uiw::text("Joined - %d/4 players", session.player_count());
            break;
        case Lsw2SessionState::InMatch:
            uiw::text("In a game.");
            break;
        }
        if (session.last_error() && *session.last_error())
            uiw::error("%s", session.last_error());

        /* SAY WHAT IS AND IS NOT KNOWN, WHICH IS NOT WHAT THIS USED TO SAY.
         *
         * It read: "No way in yet - the Cantina console and pause-menu entries
         * are not built", on the reasoning that the two co-op entry points (the
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
         * end, because no capture reaches the co-op screen - and that is the
         * same end-to-end run the rest of the baseline's online co-op is
         * waiting on. Saying "not built" claimed something worse and more
         * specific than the truth, in a place the PLAYER reads. */
        uiw::spacing();
        uiw::warn("Wired but unproven: the lobby list and connect are in the "
                  "baseline, through the game's own screens. No session tested yet.");
        uiw::end_block();
    }
    uiw::end_columns();
}

/* Set from the executable name at startup - see ui_set_fork_build. */
bool g_fork_build = false;

/* Cheats > Levels: where the game is, and the warp. */
void cheats_levels_page() {
    uiw::text(
        "The baseline runs the game as shipped. Nothing here is applied unless "
        "you press it, and nothing is applied at startup.");

    uiw::section("Levels");
    if (!lsw2_cheats_available()) {
        uiw::note("Waiting for the game to build its state - this is normal "
                  "during boot and on the title screen.");
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

    if (live >= 0) uiw::text("Currently: %s  (id %d)", live_name, live);
    else uiw::note("Currently: unreadable");
    if (live != check) {
        uiw::note("The installed level state says %d - it differs after a warp, "
                  "because a warp sets the request and lets the game build from it.",
                  check);
    }

    uiw::spacing();
    static int selected = 0;
    /* Names carry their confidence, because most of them are DERIVED from the
     * level's own asset names rather than confirmed. Presenting a guess in the
     * same typeface as a fact is how a guess becomes a fact by accident. */
    if (uiw::begin_combo("Level", kLsw2Levels[selected].name,
                         "The level to warp to. Each is marked with how its\n"
                         "name is known.")) {
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
        uiw::end_combo();
    }

    const bool can_warp = lsw2_cheats_can_warp();
    ImGui::BeginDisabled(!can_warp);
    if (uiw::button("Warp", "Travel to the level selected above.")) {
        lsw2_cheats_warp(kLsw2Levels[selected].id);
    }
    ImGui::EndDisabled();

    uiw::spacing();
    if (!can_warp) {
        /* Disabled rather than failing quietly, and with the reason, because
         * "the button did nothing" is the least useful thing a UI can say. */
        uiw::text(
            "Warp is unavailable right now: the game is not in a level state. "
            "It works from inside the game, not from the menus.");
    } else {
        uiw::text(
            "Warp uses the game's own state-request queue - it fills in a "
            "request and the game performs the transition itself, on its own "
            "thread, rather than anything being called from outside.");
    }
}

/* Cheats > Unlocks: the flag arrays, and unlocking everything. */
void cheats_unlocks_page() {
    uiw::section("Unlock flags");
    if (!lsw2_cheats_available()) {
        uiw::note("Waiting for the game to build its state - this is normal "
                  "during boot and on the title screen.");
        return;
    }

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
        uiw::note("Unavailable - the game has not built its context.");
        return;
    }

    uiw::columns(2);
    if (uiw::begin_block("Roster flag arrays",
                         "flags arrays roster copy hex A B nine")) {
        uiw::text("A  %08X %08X %08X %08X",
                  flags_a[0], flags_a[1], flags_a[2], flags_a[3]);
        uiw::text("B  %08X %08X %08X %08X",
                  flags_b[0], flags_b[1], flags_b[2], flags_b[3]);
        uiw::spacing();
        if (uiw::button("Copy", "Copy both lines to the clipboard.", -1.0f)) {
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "A %08X %08X %08X %08X\nB %08X %08X %08X %08X\n",
                          flags_a[0], flags_a[1], flags_a[2], flags_a[3],
                          flags_b[0], flags_b[1], flags_b[2], flags_b[3]);
            ImGui::SetClipboardText(buf);
        }
        uiw::spacing();
        uiw::text(
            "Those two are 0x02104D90 + 0x44CC and + 0x44DC, and a capture "
            "showed them reading identically on a fully-unlocked save and an "
            "empty one. That refuted them - but enumerating the "
            "accessors then found NINE flag arrays, so the capture had ruled out "
            "two candidates out of nine, not all of them.");
        uiw::spacing();
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
                if (uiw::button("Copy all nine", nullptr, -1.0f)) {
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
        uiw::end_block();
    }

    uiw::next_column();
    uiw::section("Unlock everything");
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
    if (uiw::button("Unlock everything",
            "Sets OWNED and SEEN for every character and vehicle id the\n"
            "game ships, and marks all 22 terminal Extras as bought.\n\n"
            "Both roster arrays, because the Characters tab tests SEEN\n"
            "first and draws \"????\" when it is clear - so OWNED alone\n"
            "would give a roster of question marks.\n\n"
            "Does NOT switch Extras on: that is a different array and\n"
            "belongs to the host's match settings.")) {
        if (lsw2_cheats_unlock_everything())
            g_unlock_note = "written - open the terminal's Characters tab "
                            "to check for names rather than ????";
        else
            g_unlock_note = "not available yet - the game has not built "
                            "its context";
    }
    if (g_unlock_note) uiw::text("%s", g_unlock_note);
    uiw::spacing();
    uiw::warn("%s", "The array identification is strong but not settled.");
    uiw::text(
        "Four lines of evidence say +0x44CC/+0x44DC are the roster state, "
        "against one capture that says they do not differ "
        "between saves. If the button above does nothing visible, that "
        "capture was right and this needs a save-to-save comparison.");
    uiw::text(
        "One array IS the Extras, corroborated four ways: +0x4518, "
        "indices 0..19. The unlocks are not identified yet. Expand the "
        "list beside this on a fresh save and on a fully-unlocked one and copy "
        "both: the array that differs is the answer, and the buttons can "
        "be written the same day.");
    uiw::end_columns();
}

/* THE MENU'S SHAPE: sections across the header, pages down the sidebar.
 *
 * It was a single floating window with a row of tabs - Video, Input, Touch,
 * Debug, Cheats, Multiplayer - each one long scrolling list. The tabs became
 * pages and the pages were grouped the way a player looks for them:
 *
 *   Settings      Display, Graphics, Controls, Touch Screen  - the port's own
 *                 presentation and input, what any emulator would offer;
 *   Enhancements  Arena Controls, Arena HUD, Lobby, Portraits - the changes
 *                 to the game itself, all optional;
 *   Multiplayer   the fork's Online page or the baseline's Co-op page;
 *   Cheats        Levels, Unlocks - the baseline only;
 *   Debug         General, Instances, and the fork's Probes.
 *
 * Nothing that was reachable before is unreachable now: every control on the
 * old tabs is on exactly one page, and the search box finds any of them by
 * name or by a word in its tooltip. */
enum MenuSection {
    kSecSettings, kSecEnhancements, kSecMultiplayer, kSecCheats, kSecDebug, kSecCount
};
const char* const kSectionNames[kSecCount] = {
    "Settings", "Enhancements", "Multiplayer", "Cheats", "Debug"};

const char* const kSettingsPages[] = {"Display", "Graphics", "Controls", "Touch Screen"};
const char* const kEnhancementPages[] = {"Arena Controls", "Arena HUD", "Lobby", "Portraits"};
const char* const kForkNetPages[] = {"Online"};
const char* const kCoopNetPages[] = {"Co-op"};
const char* const kCheatPages[] = {"Levels", "Unlocks"};
const char* const kForkDebugPages[] = {"General", "Instances", "Probes"};
const char* const kBaseDebugPages[] = {"General", "Instances"};

template <size_t N>
int page_count(const char* const (&)[N]) { return int(N); }

/* Whether this build has a section at all. The fork's Cheats are ABSENT, not
 * greyed - see the header row in draw_menu. */
bool section_present(int section) {
    return section != kSecCheats || !g_fork_build;
}

/* The pages of a section in sidebar order, as this build has them. */
int section_pages(int section, const char* const** names) {
    switch (section) {
    case kSecSettings:
        *names = kSettingsPages;
        return page_count(kSettingsPages);
    case kSecEnhancements:
        *names = kEnhancementPages;
        return page_count(kEnhancementPages);
    case kSecMultiplayer:
        *names = g_fork_build ? kForkNetPages : kCoopNetPages;
        return 1;
    case kSecCheats:
        *names = kCheatPages;
        return page_count(kCheatPages);
    default:
        *names = g_fork_build ? kForkDebugPages : kBaseDebugPages;
        return g_fork_build ? page_count(kForkDebugPages) : page_count(kBaseDebugPages);
    }
}

void draw_page(UiSettings& s, int section, int page) {
    switch (section) {
    case kSecSettings:
        switch (page) {
        case 0: display_page(s); break;
        case 1: graphics_page(s); break;
        case 2: controls_page(s); break;
        default: touch_page(s); break;
        }
        break;
    case kSecEnhancements:
        switch (page) {
        case 0: arena_controls_page(s); break;
        case 1: arena_hud_page(s); break;
        case 2: lobby_page(s); break;
        default: portraits_page(s); break;
        }
        break;
    case kSecMultiplayer:
        /* BOTH BUILDS HAVE A MULTIPLAYER SECTION, and they are different
         * pages. The baseline has online co-op; the fork's page points at the
         * in-game menus, the baseline's carries the room code, which the
         * baseline shows OUT of the game. Same name, because to a player it
         * is the same question. */
        if (g_fork_build) multiplayer_tab(s);
        else              coop_tab(s);
        break;
    case kSecCheats:
        if (page == 0) cheats_levels_page();
        else           cheats_unlocks_page();
        break;
    default:
        if (page == 0)          debug_general_page(s);
        else if (page == 1)     debug_instances_page(s);
        else if (g_fork_build)  debug_probes_page();
        break;
    }
}

/* Which section and page are showing. The section is the one the header's
 * tab bar last reported; each section remembers its own page, so moving
 * between sections and back returns to the page that was open. */
int g_section = kSecSettings;
int g_pages[kSecCount] = {};
/* A section to select programmatically on the next header row, or -1. */
int g_want_section = -1;
/* Whether the menu was drawn last frame, so the frame it opens is known. */
bool g_was_visible = false;

void select_page(int section, int page) {
    if (!section_present(section)) return;
    g_want_section = section;
    g_pages[section] = page;
}

/* LSW2_UI_TAB, resolved once the build is known.
 *
 * It names a section ("Settings"), a page ("Graphics"), or both
 * ("Settings/Graphics"). The names the menu had when it was a row of tabs are
 * still accepted and land on the page that now holds that tab's settings, so
 * a script written against them keeps working. */
void apply_tab_request(const char* want) {
    if (!want || !*want) return;
    static const struct { const char* name; int section; int page; } legacy[] = {
        {"Video", kSecSettings, 0},   {"Input", kSecSettings, 2},
        {"Touch", kSecSettings, 3},   {"Debug", kSecDebug, 0},
        {"Cheats", kSecCheats, 0},    {"Multiplayer", kSecMultiplayer, 0},
    };
    for (const auto& l : legacy) {
        if (SDL_strcasecmp(want, l.name) == 0) {
            select_page(l.section, l.page);
            return;
        }
    }
    const std::string text = want;
    const size_t slash = text.find('/');
    const std::string section_name = slash == std::string::npos ? text : text.substr(0, slash);
    const std::string page_name = slash == std::string::npos ? text : text.substr(slash + 1);
    for (int sec = 0; sec < kSecCount; ++sec) {
        if (!section_present(sec)) continue;
        const bool sec_match = SDL_strcasecmp(section_name.c_str(), kSectionNames[sec]) == 0;
        const char* const* names = nullptr;
        const int n = section_pages(sec, &names);
        for (int p = 0; p < n; ++p) {
            const bool page_match = SDL_strcasecmp(page_name.c_str(), names[p]) == 0;
            if (page_match && (sec_match || slash == std::string::npos)) {
                select_page(sec, p);
                return;
            }
        }
        if (sec_match && slash == std::string::npos) {
            select_page(sec, 0);
            return;
        }
    }
    std::fprintf(stderr, "[ui] LSW2_UI_TAB=%s names no section or page\n", want);
}

ImGuiTabItemFlags section_tab_flags(int section) {
    return g_want_section == section ? ImGuiTabItemFlags_SetSelected
                                     : ImGuiTabItemFlags_None;
}

void draw_menu(UiSettings& s, bool just_opened) {
    static bool tab_requested = false;
    if (!tab_requested) {
        /* LSW2_UI_TAB preselects a page once, so the UI can be screenshotted
         * from a script. imgui has no remote clicking, and "it compiles" is
         * not evidence that a panel renders - this UI has already had a
         * panel that was correctly placed, correctly sized and entirely
         * black. */
        tab_requested = true;
        apply_tab_request(SDL_getenv("LSW2_UI_TAB"));
    }

    uiw::begin_menu(just_opened);

    uiw::begin_header();
    int shown = g_section;
    if (ImGui::BeginTabBar("##sections", ImGuiTabBarFlags_NoTooltip |
                                          ImGuiTabBarFlags_FittingPolicyScroll |
                                          ImGuiTabBarFlags_NoCloseWithMiddleMouseButton)) {
        auto finish = [&](int section, bool open) {
            uiw::header_tab(open);
            if (open) {
                shown = section;
                ImGui::EndTabItem();
            }
        };
        finish(kSecSettings,
               ImGui::BeginTabItem("Settings", nullptr, section_tab_flags(kSecSettings)));
        finish(kSecEnhancements,
               ImGui::BeginTabItem("Enhancements", nullptr,
                                   section_tab_flags(kSecEnhancements)));
        finish(kSecMultiplayer,
               ImGui::BeginTabItem("Multiplayer", nullptr,
                                   section_tab_flags(kSecMultiplayer)));
        /* The fork has no cheats tab. Not greyed out - absent.
         * A disabled control still advertises the feature, and the point
         * of the fork is that unlocks are not the player's to grant: they
         * are granted wholesale at startup (everything unlocked from the
         * start) and the match rules come from the host's settings menu.
         * The second test keeps header_tab from painting a fill behind the
         * tab before it when there is no Cheats tab to paint behind. */
        const bool cheats = !g_fork_build && ImGui::BeginTabItem("Cheats", nullptr,
                                                    section_tab_flags(kSecCheats));
        if (!g_fork_build) finish(kSecCheats, cheats);
        finish(kSecDebug,
               ImGui::BeginTabItem("Debug", nullptr, section_tab_flags(kSecDebug)));
        ImGui::EndTabBar();
    }
    g_want_section = -1;
    const bool close = uiw::end_header();
    if (shown != g_section) {
        /* A new section is a new place to look, so a search left in the box
         * would hide it; the box is cleared the way a page change clears it. */
        g_section = shown;
        uiw::clear_search();
    }

    const char* const* names = nullptr;
    const int count = section_pages(g_section, &names);
    int& page = g_pages[g_section];
    if (page < 0 || page >= count) page = 0;

    uiw::begin_sidebar();
    for (int p = 0; p < count; ++p) {
        if (uiw::sidebar_entry(names[p], p == page)) {
            page = p;
            uiw::clear_search();
        }
    }
    uiw::sidebar_footer();
    /* SAY WHICH BUILD THIS IS. The two executables are identical to look
     * at and share one settings file, so without this the only way to tell
     * them apart is to notice which sections are missing. */
    ImGui::PushTextWrapPos(0.0f);
    if (g_fork_build) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Bounty Hunter fork");
        ImGui::TextDisabled("(saves to bountyhunter.sav)");
        ImGui::Spacing();
    }
    ImGui::TextDisabled("Esc or F1 closes this. The game keeps running behind it.");
    ImGui::PopTextWrapPos();
    uiw::end_sidebar();

    uiw::begin_content();
    if (uiw::searching()) {
        /* A search looks through EVERY page of every section this build has,
         * and each page draws only what matches. */
        for (int sec = 0; sec < kSecCount; ++sec) {
            if (!section_present(sec)) continue;
            const char* const* pages = nullptr;
            const int n = section_pages(sec, &pages);
            for (int p = 0; p < n; ++p) {
                uiw::search_origin(kSectionNames[sec], pages[p]);
                ImGui::PushID(sec * 16 + p);
                draw_page(s, sec, p);
                ImGui::PopID();
            }
        }
    } else {
        ImGui::PushID(g_section * 16 + page);
        draw_page(s, g_section, page);
        ImGui::PopID();
    }
    uiw::end_content();
    uiw::end_menu();

    if (close) g_visible = false;
}

/* LSW2_UI_SHOT: ONE PICTURE OF THE TOP WINDOW AS PRESENTED, MENU INCLUDED.
 *
 * LSW2_PRESENT_SHOT and the debug surface's capture both read the DS frames,
 * which is exactly the layer this menu is NOT drawn on - so neither can show
 * whether the menu looks right, and "it compiles" is not that evidence. This
 * reads the finished back buffer after the menu is drawn and before it is
 * presented, once, LSW2_UI_SHOT_FRAME frames in (default 300, five seconds at
 * 60 fps, so the game has something on screen behind the menu), and writes it
 * as a binary PPM. Off unless LSW2_UI_SHOT names a file. Pair it with
 * LSW2_UI_OPEN=1, and LSW2_UI_TAB or LSW2_UI_SEARCH, to choose what the menu
 * shows. */
void take_ui_shot(SDL_Renderer* renderer) {
    /* COPIED, NOT KEPT. On Windows SDL_getenv answers into one shared buffer,
     * so a pointer kept from the first lookup reads whatever the second one
     * found - the shot was once written to a file named after its frame. */
    static const std::string path = [] {
        const char* p = SDL_getenv("LSW2_UI_SHOT");
        return std::string(p ? p : "");
    }();
    if (path.empty()) return;
    static int frames = 0;
    static const int when = [] {
        const char* f = SDL_getenv("LSW2_UI_SHOT_FRAME");
        const int n = f ? std::atoi(f) : 0;
        return n > 0 ? n : 300;
    }();
    if (++frames != when) return;

    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
    if (g_gl_backend) {
        SDL_GL_GetDrawableSize(g_window, &w, &h);
        if (w <= 0 || h <= 0) return;
        rgb.resize(size_t(w) * size_t(h) * 3);
        /* READ WHAT IS ABOUT TO BE PRESENTED: whatever framebuffer the menu
         * was just drawn into, which the read binding need not match. */
        GLint draw_fb = 0, read_fb = 0, read_buffer = 0, pack_buffer = 0, align = 4;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &align);
        const auto bind = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(
            SDL_GL_GetProcAddress("glBindFramebuffer"));
        const auto bind_buffer = reinterpret_cast<PFNGLBINDBUFFERPROC>(
            SDL_GL_GetProcAddress("glBindBuffer"));
        if (bind) bind(GL_READ_FRAMEBUFFER, GLuint(draw_fb));
        /* THE COMPUTE RENDERER READS BACK THROUGH A PACK BUFFER, and while one
         * is bound glReadPixels takes its pointer argument as an offset into
         * that buffer - the read failed and the picture came out black. */
        if (bind_buffer && pack_buffer) bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        if (draw_fb == 0) glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        glPixelStorei(GL_PACK_ALIGNMENT, align);
        if (draw_fb == 0) glReadBuffer(GLenum(read_buffer));
        if (bind_buffer && pack_buffer) bind_buffer(GL_PIXEL_PACK_BUFFER, GLuint(pack_buffer));
        if (bind) bind(GL_READ_FRAMEBUFFER, GLuint(read_fb));
        /* Said rather than swallowed: a read that failed leaves a black
         * picture, which would otherwise look like a menu that drew nothing. */
        if (const GLenum err = glGetError(); err != GL_NO_ERROR)
            std::fprintf(stderr, "[ui] shot: GL error 0x%04X reading framebuffer %d\n",
                         unsigned(err), int(draw_fb));
        /* GL's rows run bottom-up; a PPM's run top-down. */
        const size_t row = size_t(w) * 3;
        std::vector<unsigned char> tmp(row);
        for (int y = 0; y < h / 2; ++y) {
            unsigned char* a = rgb.data() + size_t(y) * row;
            unsigned char* b = rgb.data() + size_t(h - 1 - y) * row;
            std::memcpy(tmp.data(), a, row);
            std::memcpy(a, b, row);
            std::memcpy(b, tmp.data(), row);
        }
    } else {
        if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0 || w <= 0 || h <= 0) return;
        rgb.resize(size_t(w) * size_t(h) * 3);
        if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGB24, rgb.data(),
                                 w * 3) != 0) {
            std::fprintf(stderr, "[ui] shot: read failed: %s\n", SDL_GetError());
            return;
        }
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[ui] shot: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "[ui] shot: %s (%dx%d, menu %s)\n", path.c_str(), w, h,
                 g_visible ? "open" : "closed");
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
    /* The menu's typeface, baked before either backend builds the atlas
     * texture. imgui's own font stays the default for everything else. */
    uiw::load_fonts();

    /* LSW2_UI_TAB is for screenshotting the UI from a script, and on its own it
     * could not do that: it preselects a tab in a window that starts hidden, so
     * nothing was ever drawn to capture. Setting it also opens the menu, which
     * is what "screenshot this tab" has to mean. LSW2_UI_OPEN=1 opens the menu
     * on whatever page it would open on anyway, and LSW2_UI_SEARCH opens it
     * with that text already in the search box. All three are off unless set;
     * see take_ui_shot for the capture they are paired with. */
    if (SDL_getenv("LSW2_UI_TAB")) g_visible = true;
    if (const char* open = SDL_getenv("LSW2_UI_OPEN"); open && *open == '1')
        g_visible = true;
    if (const char* search = SDL_getenv("LSW2_UI_SEARCH"); search && *search) {
        uiw::set_search(search);
        g_visible = true;
    }

    g_window = window;
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
             * because the title bar is outside this rectangle.
             *
             * ONLY WHILE THE PANEL IS ON TOP. The menu now covers the whole
             * window, the panel included, and a click on a menu control that
             * happens to sit over the panel's picture is the menu's - handing
             * it back as well would press the control AND touch the game. The
             * panel is kept behind the menu while it is open (ui_draw), so in
             * practice this is the rule for any imgui window that is not. */
            if (g_floating_shown && g_floating_hovered && event.type != SDL_MOUSEWHEEL) {
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
        g_floating_hovered = false;
        if (ImGui::Begin("Touch screen", nullptr,
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse)) {
            /* BEHIND THE MENU WHILE IT IS OPEN. The panel is part of the game
             * view, and the menu dims the game view; a panel that appeared
             * while the menu was open - ticking "Floating touch panel" is
             * exactly that - would otherwise be created in front of it and
             * cover the page that created it. */
            if (g_visible) ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
            g_floating_hovered = ImGui::IsWindowHovered();
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

    if (!g_visible) {
        g_was_visible = false;
        return;
    }
    const bool just_opened = !g_was_visible;
    g_was_visible = true;
    draw_menu(s, just_opened);

    /* The metrics window opens OVER the menu, and stays over it: the menu is
     * only brought forward when it appears, never by a click on it. */
    if (s.show_metrics) ImGui::ShowMetricsWindow(&s.show_metrics);
}

void ui_render(SDL_Renderer* renderer) {
    ImGui::Render();
    if (g_gl_backend) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    else ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    take_ui_shot(renderer);
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
