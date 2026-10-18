/* See uiwidgets.h. */

#include "uiwidgets.h"
#include "uifont.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/* THE PALETTE: one accent, the greys around it, and the backdrop.
 *
 * The accent is a strong mid blue, used for everything the player can set -
 * checkbox squares, slider tracks, combo boxes, text fields - and for the
 * selected section and page. Plain buttons are a neutral grey, so an action
 * ("Apply", "Warp") never reads as a setting that is switched on. */
const ImVec4 kAccent(0.00f, 0.24f, 0.80f, 1.0f);
const ImVec4 kButton(0.45f, 0.45f, 0.45f, 1.0f);
const ImVec4 kPopup(0.15f, 0.15f, 0.15f, 1.0f);
const ImVec4 kBackdrop(0.0f, 0.0f, 0.0f, 0.85f);
const ImVec4 kRim(0.0f, 0.0f, 0.0f, 0.3f);
const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kOrigin(0.45f, 0.45f, 0.45f, 1.0f);
const ImVec4 kWarn(1.0f, 0.75f, 0.25f, 1.0f);
const ImVec4 kError(1.0f, 0.6f, 0.4f, 1.0f);
const ImVec4 kClear(0.0f, 0.0f, 0.0f, 0.0f);

ImVec4 with_alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

/* Hover and press are the accent at lower opacity rather than a second
 * colour, so every control reacts the same way. */
ImVec4 react(const ImVec4& base, bool hovered, bool held) {
    if (held && hovered) return with_alpha(base, base.w * 0.6f);
    if (hovered) return with_alpha(base, base.w * 0.8f);
    return base;
}

/* THE FACES, baked once at a ladder of sizes. Rebuilding the atlas whenever
 * the window is resized would mean recreating the backend's font texture
 * mid-session on both renderers; a ladder whose rungs are ~15% apart costs a
 * few megabytes of atlas and leaves no visible gap between a wanted size and
 * the nearest one. 66 px is the header at the largest scale (3x). */
constexpr float kFontSizes[] = {13, 15, 17, 19, 22, 25, 29, 33, 38, 44, 50, 58, 66};
constexpr int kFontCount = int(sizeof kFontSizes / sizeof *kFontSizes);
ImFont* g_fonts[kFontCount] = {};

/* Design sizes at scale 1: page text, the header and sidebar, and the notes
 * at the foot of the sidebar. */
constexpr float kBodyPx = 18.0f;
constexpr float kHeaderPx = 23.0f;
constexpr float kSmallPx = 15.0f;

ImFont* nearest_font(float px) {
    ImFont* best = nullptr;
    float best_d = FLT_MAX;
    for (int i = 0; i < kFontCount; ++i) {
        if (!g_fonts[i]) continue;
        const float d = std::fabs(kFontSizes[i] - px);
        if (d < best_d) { best_d = d; best = g_fonts[i]; }
    }
    return best ? best : ImGui::GetIO().Fonts->Fonts[0];
}

float g_k = 1.0f;
ImFont* g_body = nullptr;
ImFont* g_header = nullptr;
ImFont* g_small = nullptr;

/* This frame's geometry, worked out once in begin_menu so the header, sidebar
 * and page agree on where everything is. Screen coordinates. */
struct Layout {
    ImVec2 inner_min, inner_max;
    float header_h = 0, header_w = 0, search_w = 0, rule = 0, gap = 0;
    ImVec2 search_pos, close_pos;
    ImVec2 sidebar_pos, sidebar_size;
    ImVec2 content_pos, content_size;
} g_lay;

int g_vars = 0;     /* style vars begin_menu pushed */
int g_colors = 0;   /* style colours begin_menu pushed */
int g_sidebar_fonts = 0;

void var(ImGuiStyleVar v, float f) { ImGui::PushStyleVar(v, f); ++g_vars; }
void var(ImGuiStyleVar v, ImVec2 f) { ImGui::PushStyleVar(v, f); ++g_vars; }
void col(ImGuiCol c, const ImVec4& v) { ImGui::PushStyleColor(c, v); ++g_colors; }

/* SEARCH STATE. The raw text is what the box shows; the query is that text
 * folded the way labels are folded before they are compared. */
char g_search_raw[96] = {0};
std::string g_query;
const char* g_origin_section = "";
const char* g_origin_page = "";
int g_hits = 0;
bool g_in_block = false;
bool g_combo_hit = false;   /* a combo's origin note waits for its EndCombo */

/* Lower case, no spaces, and nothing from a "##" on - so "Linear filtering"
 * is found by "linearfilter" and "linear filter" alike, and an imgui ID
 * suffix is never searchable text. */
std::string fold(const char* s) {
    std::string out;
    if (!s) return out;
    for (const char* p = s; *p; ++p) {
        if (p[0] == '#' && p[1] == '#') break;
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isspace(c)) continue;
        out.push_back(char(std::tolower(c)));
    }
    return out;
}

bool matches(const char* label, const char* extra) {
    if (g_query.empty()) return true;
    return fold(label).find(g_query) != std::string::npos ||
           fold(extra).find(g_query) != std::string::npos;
}

/* Whether a widget draws at all this frame. */
bool shown(const char* label, const char* tooltip) {
    if (g_query.empty() || g_in_block) return true;
    return matches(label, tooltip);
}

/* Whether plain text draws: never as a search result of its own. */
bool prose() { return g_query.empty() || g_in_block; }

/* After a widget that a search turned up, say where it lives. */
void found() {
    if (g_query.empty() || g_in_block) return;
    ++g_hits;
    ImGui::PushStyleColor(ImGuiCol_Text, kOrigin);
    ImGui::Text("  (%s -> %s)", g_origin_section, g_origin_page);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 6.0f * g_k));
}

void tooltip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

/* A label over the control it names, the way sliders, combos and text
 * fields are laid out in this style. */
void label_above(const char* label) {
    const char* end = ImGui::FindRenderedTextEnd(label);
    if (end != label) ImGui::TextUnformatted(label, end);
}

/* The rim the style draws around its controls: a thin dark edge that keeps
 * a blue square readable on the blue it sits beside. */
float rim() { return std::max(1.0f, 2.0f * g_k); }

/* A -/+ step button: square, accent-filled, as tall as the slider. */
bool step_button(const char* label, float size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(kAccent, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(kAccent, 0.6f));
    const bool pressed = ImGui::Button(label, ImVec2(size, size));
    ImGui::PopStyleColor(3);
    return pressed;
}

void push_slider_style() {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kAccent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kAccent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 1.0f, 1.0f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 1.0f, 1.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_k, 8.0f * g_k));
}

void pop_slider_style() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
}

/* Text fields and combo boxes: accent frame, white text. */
void push_field_style(float frame_alpha) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, with_alpha(kAccent, frame_alpha));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, with_alpha(kAccent, frame_alpha * 0.8f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, with_alpha(kAccent, frame_alpha * 0.75f));
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(kAccent, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(kAccent, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_k, 6.0f * g_k));
}

void pop_field_style() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(6);
}

/* COLUMN STATE. One level: a page's columns are never nested in another
 * page's columns. */
bool g_col_table = false;
int g_col_count = 0;
int g_col_index = 0;

void vtext(const ImVec4* color, bool disabled, const char* format, va_list args) {
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, format, args);
    ImGui::PushTextWrapPos(0.0f);
    if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    else if (disabled) ImGui::PushStyleColor(ImGuiCol_Text,
                                             ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(buf);
    if (color || disabled) ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

bool close_button(float size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID id = window->GetID("##close");
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + size, pos.y + size));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    ImDrawList* dl = window->DrawList;
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(react(kButton, hovered, held)),
                      3.0f * g_k);
    /* A white disc with an X cut out of it, drawn rather than taken from an
     * icon font this port does not carry. */
    const ImVec2 c = bb.GetCenter();
    const float r = size * 0.30f;
    dl->AddCircleFilled(c, r, ImGui::GetColorU32(with_alpha(kWhite, 0.9f)), 32);
    const float arm = r * 0.45f;
    const float thick = std::max(1.5f, 2.4f * g_k);
    const ImU32 cut = ImGui::GetColorU32(kButton);
    dl->AddLine(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), cut, thick);
    dl->AddLine(ImVec2(c.x - arm, c.y + arm), ImVec2(c.x + arm, c.y - arm), cut, thick);
    return pressed;
}

}  /* namespace */

namespace uiw {

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    for (int i = 0; i < kFontCount; ++i) {
        ImFontConfig cfg;
        /* One horizontal sample per pixel: these are baked at the size they
         * are drawn, so oversampling buys nothing but a larger atlas. */
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        std::snprintf(cfg.Name, sizeof cfg.Name, "Roboto Medium %.0fpx",
                      double(kFontSizes[i]));
        g_fonts[i] = io.Fonts->AddFontFromMemoryCompressedTTF(
            lsw2_ui_font_data(), lsw2_ui_font_size(), kFontSizes[i], &cfg);
    }
}

float scale() { return g_k; }

void begin_menu(bool just_opened) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float W = vp->WorkSize.x, H = vp->WorkSize.y;

    /* THE SCALE: the window against a 1280x800 design, by whichever axis is
     * tighter, so the menu fits both ways. The floor keeps text readable in a
     * small window - below it the page scrolls instead of shrinking - and the
     * ceiling is the largest face baked. */
    g_k = std::clamp(std::min(W / 1280.0f, H / 800.0f), 0.72f, 3.0f);
    g_body = nearest_font(kBodyPx * g_k);
    g_header = nearest_font(kHeaderPx * g_k);
    g_small = nearest_font(kSmallPx * g_k);
    g_hits = 0;
    g_in_block = false;

    const float k = g_k;
    g_vars = 0;
    g_colors = 0;
    var(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    var(ImGuiStyleVar_WindowRounding, 0.0f);
    var(ImGuiStyleVar_WindowBorderSize, 0.0f);
    var(ImGuiStyleVar_ChildRounding, 0.0f);
    var(ImGuiStyleVar_ChildBorderSize, 0.0f);
    var(ImGuiStyleVar_PopupRounding, 3.0f * k);
    var(ImGuiStyleVar_PopupBorderSize, 0.0f);
    var(ImGuiStyleVar_FramePadding, ImVec2(10.0f * k, 6.0f * k));
    var(ImGuiStyleVar_FrameRounding, 3.0f * k);
    var(ImGuiStyleVar_FrameBorderSize, 0.0f);
    var(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * k, 6.0f * k));
    var(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f * k, 4.0f * k));
    var(ImGuiStyleVar_CellPadding, ImVec2(6.0f * k, 4.0f * k));
    var(ImGuiStyleVar_IndentSpacing, 20.0f * k);
    var(ImGuiStyleVar_ScrollbarSize, 14.0f * k);
    var(ImGuiStyleVar_ScrollbarRounding, 3.0f * k);
    var(ImGuiStyleVar_GrabMinSize, 12.0f * k);
    var(ImGuiStyleVar_GrabRounding, 3.0f * k);
    var(ImGuiStyleVar_TabRounding, 3.0f * k);
    var(ImGuiStyleVar_SeparatorTextBorderSize, std::max(1.0f, 2.0f * k));
    var(ImGuiStyleVar_SeparatorTextPadding, ImVec2(16.0f * k, 4.0f * k));

    col(ImGuiCol_PopupBg, kPopup);
    col(ImGuiCol_Button, kButton);
    col(ImGuiCol_ButtonHovered, with_alpha(kButton, 0.8f));
    col(ImGuiCol_ButtonActive, with_alpha(kButton, 0.6f));
    col(ImGuiCol_FrameBg, with_alpha(kAccent, 0.8f));
    col(ImGuiCol_FrameBgHovered, with_alpha(kAccent, 0.6f));
    col(ImGuiCol_FrameBgActive, with_alpha(kAccent, 0.6f));
    col(ImGuiCol_Header, with_alpha(kAccent, 0.5f));
    col(ImGuiCol_HeaderHovered, with_alpha(kAccent, 0.6f));
    col(ImGuiCol_HeaderActive, with_alpha(kAccent, 0.6f));
    col(ImGuiCol_CheckMark, with_alpha(kWhite, 0.7f));
    col(ImGuiCol_Border, kRim);
    col(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
    col(ImGuiCol_TableHeaderBg, with_alpha(kAccent, 0.45f));
    col(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.04f));
    col(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.2f));

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackdrop);
    /* NOT BROUGHT TO THE FRONT BY A CLICK, only by appearing. A click on the
     * menu would otherwise bury imgui's metrics window, which the Debug page
     * opens over it and which must stay reachable. */
    ImGui::Begin("##lsw2menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    if (just_opened) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::PushFont(g_body);

    /* THE BLOCK: the whole window when it is small, and 90% of it, centred,
     * once it is larger than the design size - never wider than 16:9, so an
     * ultrawide display does not stretch a page into one long line. */
    float bw = W, bh = H;
    if (W > 1280.0f) bw = std::min(W * 0.9f, H * 1.77f);
    if (H > 800.0f) bh = H * 0.9f;
    const float pad = 10.0f * k;
    Layout& L = g_lay;
    L.inner_min = ImVec2(vp->WorkPos.x + (W - bw) * 0.5f + pad,
                         vp->WorkPos.y + (H - bh) * 0.5f + pad);
    L.inner_max = ImVec2(L.inner_min.x + bw - 2.0f * pad,
                         L.inner_min.y + bh - 2.0f * pad);
    const float inner_w = L.inner_max.x - L.inner_min.x;
    L.gap = 8.0f * k;
    L.rule = std::max(2.0f, std::round(4.0f * k));
    L.header_h = g_header->FontSize + 16.0f * k;
    L.search_w = std::min(220.0f * k, inner_w * 0.3f);
    L.header_w = inner_w - L.search_w - L.header_h - 2.0f * L.gap;
    L.search_pos = ImVec2(L.inner_min.x + L.header_w + L.gap, L.inner_min.y);
    L.close_pos = ImVec2(L.inner_max.x - L.header_h, L.inner_min.y);

    const float rule_y = L.inner_min.y + L.header_h + L.gap;
    const float body_top = rule_y + L.rule + 2.0f * L.gap;
    const float sidebar_w = std::max(190.0f * k, inner_w * 0.14f);
    L.sidebar_pos = ImVec2(L.inner_min.x, body_top);
    L.sidebar_size = ImVec2(sidebar_w, L.inner_max.y - body_top);
    const float vrule_x = L.inner_min.x + sidebar_w + L.gap;
    L.content_pos = ImVec2(vrule_x + L.rule + 2.0f * L.gap, body_top);
    L.content_size = ImVec2(L.inner_max.x - L.content_pos.x, L.inner_max.y - body_top);

    /* The two white rules: under the header, and between the sidebar and
     * the page. */
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 white = ImGui::GetColorU32(kWhite);
    dl->AddRectFilled(ImVec2(L.inner_min.x, rule_y),
                      ImVec2(L.inner_max.x, rule_y + L.rule), white, L.rule * 0.5f);
    dl->AddRectFilled(ImVec2(vrule_x, body_top),
                      ImVec2(vrule_x + L.rule, L.inner_max.y), white, L.rule * 0.5f);
}

void end_menu() {
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(g_colors);
    ImGui::PopStyleVar(g_vars);
    g_colors = 0;
    g_vars = 0;
}

/* THE HEADER IS AN IMGUI TAB BAR WEARING THE BUTTON LOOK. A tab bar is what a
 * row of mutually exclusive sections is, and it brings keyboard handling and
 * programmatic selection with it; only its drawing is replaced. Every tab
 * colour is cleared, and header_tab() paints a rounded fill behind each tab on
 * a channel UNDER the one the tab bar draws its labels on. */
void begin_header() {
    const Layout& L = g_lay;
    ImGui::SetCursorScreenPos(L.inner_min);
    ImGui::PushFont(g_header);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_k, 8.0f * g_k));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(8.0f * g_k, 4.0f * g_k));
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 0.0f);
    const ImGuiCol clear[] = {
        ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected,
        ImGuiCol_TabSelectedOverline, ImGuiCol_TabDimmed,
        ImGuiCol_TabDimmedSelected, ImGuiCol_TabDimmedSelectedOverline};
    for (ImGuiCol c : clear) ImGui::PushStyleColor(c, kClear);
    ImGui::BeginChild("##header", ImVec2(L.header_w, L.header_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
}

void header_tab(bool selected) {
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec4 base = selected ? kAccent : with_alpha(kAccent, 0.0f);
    ImVec4 fill = base;
    if (held && hovered) fill = with_alpha(kAccent, 0.6f);
    else if (hovered) fill = with_alpha(kAccent, 0.8f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    if (fill.w > 0.0f)
        dl->AddRectFilled(a, b, ImGui::GetColorU32(fill), 3.0f * g_k);
    dl->ChannelsSetCurrent(1);
}

bool end_header() {
    const Layout& L = g_lay;
    ImGui::GetWindowDrawList()->ChannelsMerge();
    ImGui::EndChild();
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(5);

    /* The search box, the same height as the tabs beside it. */
    ImGui::SetCursorScreenPos(L.search_pos);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, with_alpha(kAccent, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, with_alpha(kAccent, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, with_alpha(kAccent, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_k, 8.0f * g_k));
    ImGui::SetNextItemWidth(L.search_w);
    ImGui::InputTextWithHint("##search", "Search...", g_search_raw, sizeof g_search_raw);
    g_query = fold(g_search_raw);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImGui::SetCursorScreenPos(L.close_pos);
    const bool closed = close_button(L.header_h);
    tooltip("Close (Esc or F1). The game keeps running behind the menu.");
    ImGui::PopFont();
    return closed;
}

void begin_sidebar() {
    const Layout& L = g_lay;
    ImGui::SetCursorScreenPos(L.sidebar_pos);
    ImGui::BeginChild("##sidebar", L.sidebar_size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(g_header);
    g_sidebar_fonts = 1;
}

/* A page in the sidebar: its name centred in the column, on a rounded fill
 * when it is the page showing. */
bool sidebar_entry(const char* label, bool selected) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiID id = window->GetID(label);
    const ImVec2 pad(10.0f * g_k, 8.0f * g_k);
    const ImVec2 size = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 pos = window->DC.CursorPos;
    pos.x = window->WorkRect.GetCenter().x - size.x * 0.5f;
    pos.y += pad.y;
    const ImRect bb(ImVec2(pos.x - pad.x, pos.y - pad.y),
                    ImVec2(pos.x + size.x + pad.x, pos.y + size.y + pad.y));
    ImGui::ItemSize(bb, pad.y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const ImVec4 base = selected ? kAccent : with_alpha(kAccent, 0.0f);
    ImVec4 fill = base;
    if (held && hovered) fill = with_alpha(kAccent, 0.6f);
    else if (hovered) fill = with_alpha(kAccent, 0.8f);
    if (fill.w > 0.0f)
        window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(fill), 3.0f * g_k);
    ImGui::RenderText(pos, label);
    ImGui::Dummy(ImVec2(0.0f, 2.0f * g_k));
    return pressed;
}

void sidebar_footer() {
    if (g_sidebar_fonts) {
        ImGui::PopFont();
        g_sidebar_fonts = 0;
    }
    ImGui::PushFont(g_small);
    g_sidebar_fonts = 1;
    ImGui::Dummy(ImVec2(0.0f, 14.0f * g_k));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * g_k));
}

void end_sidebar() {
    if (g_sidebar_fonts) ImGui::PopFont();
    g_sidebar_fonts = 0;
    ImGui::EndChild();
}

void begin_content() {
    const Layout& L = g_lay;
    ImGui::SetCursorScreenPos(L.content_pos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * g_k, 2.0f * g_k));
    ImGui::BeginChild("##content", L.content_size, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    if (!g_query.empty()) {
        /* Results run down one column of readable width, the way a page's
         * columns would, with a way back out above them. */
        ImGui::PushFont(g_header);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Search Results");
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::Button("Clear Search")) clear_search();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * g_k));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f * g_k));
        const float w = std::min(ImGui::GetContentRegionAvail().x, 640.0f * g_k);
        ImGui::BeginChild("##results", ImVec2(w, 0.0f), ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoBackground);
    }
}

void end_content() {
    if (!g_query.empty()) {
        if (g_hits == 0) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f * g_k));
            ImGui::TextDisabled("No results found");
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

bool searching() { return !g_query.empty(); }

void set_search(const char* text) {
    std::snprintf(g_search_raw, sizeof g_search_raw, "%s", text ? text : "");
    g_query = fold(g_search_raw);
}

void clear_search() {
    g_search_raw[0] = '\0';
    g_query.clear();
}

void search_origin(const char* section, const char* page) {
    g_origin_section = section ? section : "";
    g_origin_page = page ? page : "";
}

int search_hits() { return g_hits; }

bool begin_block(const char* title, const char* keywords) {
    if (!g_query.empty()) {
        if (!matches(title, keywords)) return false;
        ++g_hits;
        ImGui::PushStyleColor(ImGuiCol_Text, kOrigin);
        ImGui::Text("%s  (%s -> %s)", title, g_origin_section, g_origin_page);
        ImGui::PopStyleColor();
    }
    g_in_block = true;
    return true;
}

void end_block() {
    g_in_block = false;
    if (!g_query.empty()) ImGui::Dummy(ImVec2(0.0f, 10.0f * g_k));
}

void columns(int count) {
    g_col_table = false;
    g_col_count = 1;
    g_col_index = 0;
    if (!g_query.empty() || count <= 1) return;
    /* A column narrower than this wraps every tooltip-length line twice, so a
     * narrow window gets fewer columns rather than thinner ones. */
    const float avail = ImGui::GetContentRegionAvail().x;
    const int fit = std::max(1, int(avail / (330.0f * g_k)));
    const int n = std::min(count, fit);
    if (n <= 1) return;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(12.0f * g_k, 0.0f));
    g_col_table = ImGui::BeginTable("##columns", n,
                                    ImGuiTableFlags_SizingStretchSame |
                                    ImGuiTableFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (g_col_table) {
        g_col_count = n;
        ImGui::TableNextColumn();
    }
}

void next_column() {
    if (g_col_table && g_col_index + 1 < g_col_count) {
        ++g_col_index;
        ImGui::TableNextColumn();
    } else if (prose()) {
        /* Stacked: the groups that would have sat side by side follow one
         * another with a gap between. */
        ImGui::Dummy(ImVec2(0.0f, 12.0f * g_k));
    }
}

void end_columns() {
    if (g_col_table) ImGui::EndTable();
    g_col_table = false;
    g_col_count = 0;
    g_col_index = 0;
}

/* THE CHECKBOX, drawn rather than taken from imgui so that the label is part
 * of the clickable area and the square is the style's: as tall as a frame,
 * accent-filled whether ticked or not, with a pale tick. */
bool checkbox(const char* label, bool* value, const char* tip) {
    if (!shown(label, tip)) return false;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    const float pad_y = 6.0f * g_k;
    const float square = ImGui::GetFontSize() + 2.0f * pad_y;
    const float label_x = square + style.ItemInnerSpacing.x * 2.0f;
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + label_x + text.x, pos.y + square));
    ImGui::ItemSize(bb, pad_y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }
    ImDrawList* dl = window->DrawList;
    const ImVec2 sq_max(pos.x + square, pos.y + square);
    const float round = 3.0f * g_k;
    dl->AddRectFilled(pos, sq_max, ImGui::GetColorU32(react(kAccent, hovered, held)), round);
    dl->AddRect(pos, sq_max, ImGui::GetColorU32(kRim), round, 0, rim());
    if (*value) {
        const float inset = std::max(1.0f, std::floor(square / 5.0f));
        ImGui::RenderCheckMark(dl, ImVec2(pos.x + inset, pos.y + inset),
                               ImGui::GetColorU32(ImGuiCol_CheckMark), square - inset * 2.0f);
    }
    ImGui::RenderText(ImVec2(pos.x + label_x, pos.y + pad_y), label);
    tooltip(tip);
    found();
    return pressed;
}

bool slider_int(const char* label, int* value, int lo, int hi, const char* format,
                const char* tip, int step) {
    if (!shown(label, tip)) return false;
    bool changed = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    label_above(label);
    push_slider_style();
    const float h = ImGui::GetFrameHeight();
    const float gap = 3.0f * g_k;
    if (step_button("-", h)) {
        *value = std::max(lo, *value - step);
        changed = true;
    }
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(std::max(h, ImGui::GetContentRegionAvail().x - h - gap));
    if (ImGui::SliderInt("##value", value, lo, hi, format)) changed = true;
    ImGui::SameLine(0.0f, gap);
    if (step_button("+", h)) {
        *value = std::min(hi, *value + step);
        changed = true;
    }
    pop_slider_style();
    ImGui::EndGroup();
    ImGui::PopID();
    tooltip(tip);
    found();
    return changed;
}

bool slider_float(const char* label, float* value, float lo, float hi, const char* format,
                  float step, const char* tip) {
    if (!shown(label, tip)) return false;
    bool changed = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    label_above(label);
    push_slider_style();
    const float h = ImGui::GetFrameHeight();
    const float gap = 3.0f * g_k;
    /* Stepped values are snapped to the step, so pressing "+" from 0.5 lands
     * on 0.55 and not on 0.5499999. */
    auto snap = [&](float v) {
        v = std::round(v / step) * step;
        return std::clamp(v, lo, hi);
    };
    if (step_button("-", h)) {
        *value = snap(*value - step);
        changed = true;
    }
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(std::max(h, ImGui::GetContentRegionAvail().x - h - gap));
    if (ImGui::SliderFloat("##value", value, lo, hi, format)) changed = true;
    ImGui::SameLine(0.0f, gap);
    if (step_button("+", h)) {
        *value = snap(*value + step);
        changed = true;
    }
    pop_slider_style();
    ImGui::EndGroup();
    ImGui::PopID();
    tooltip(tip);
    found();
    return changed;
}

bool begin_combo(const char* label, const char* preview, const char* tip, float width) {
    if (!shown(label, tip)) return false;
    label_above(label);
    push_field_style(0.8f);
    ImGui::SetNextItemWidth(width > 0.0f ? width * g_k : -FLT_MIN);
    const std::string id = std::string("##") + label;
    const bool open = ImGui::BeginCombo(id.c_str(), preview);
    pop_field_style();
    if (open) {
        g_combo_hit = true;
    } else {
        tooltip(tip);
        found();
    }
    return open;
}

void end_combo() {
    ImGui::EndCombo();
    if (g_combo_hit) {
        g_combo_hit = false;
        found();
    }
}

bool combo(const char* label, int* index, const char* const items[], int count,
           const char* tip) {
    const int current = std::clamp(*index, 0, count - 1);
    if (!begin_combo(label, items[current], tip)) return false;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const bool chosen = (i == current);
        if (ImGui::Selectable(items[i], chosen) && i != *index) {
            *index = i;
            changed = true;
        }
        if (chosen) ImGui::SetItemDefaultFocus();
    }
    end_combo();
    return changed;
}

bool button(const char* label, const char* tip, float width) {
    if (!shown(label, tip)) return false;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_k, 8.0f * g_k));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, rim());
    const ImVec2 size = width == 0.0f ? ImVec2(-FLT_MIN, 0.0f)
                      : width < 0.0f  ? ImVec2(0.0f, 0.0f)
                                      : ImVec2(width * g_k, 0.0f);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    tooltip(tip);
    found();
    return pressed;
}

bool input_text(const char* label, char* buffer, size_t size, const char* tip) {
    if (!shown(label, tip)) return false;
    label_above(label);
    push_field_style(1.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string id = std::string("##") + label;
    const bool changed = ImGui::InputText(id.c_str(), buffer, size);
    pop_field_style();
    tooltip(tip);
    found();
    return changed;
}

bool input_int(const char* label, int* value, const char* tip) {
    if (!shown(label, tip)) return false;
    label_above(label);
    push_field_style(1.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string id = std::string("##") + label;
    const bool changed = ImGui::InputInt(id.c_str(), value);
    pop_field_style();
    tooltip(tip);
    found();
    return changed;
}

void section(const char* title) {
    if (!prose()) return;
    /* Air above every section but the first in its column, so a page reads
     * as groups rather than one list. */
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().ItemSpacing.y * 2.0f)
        ImGui::Dummy(ImVec2(0.0f, 8.0f * g_k));
    ImGui::SeparatorText(title);
}

void text(const char* format, ...) {
    if (!prose()) return;
    va_list args;
    va_start(args, format);
    vtext(nullptr, false, format, args);
    va_end(args);
}

void note(const char* format, ...) {
    if (!prose()) return;
    va_list args;
    va_start(args, format);
    vtext(nullptr, true, format, args);
    va_end(args);
}

void warn(const char* format, ...) {
    if (!prose()) return;
    va_list args;
    va_start(args, format);
    vtext(&kWarn, false, format, args);
    va_end(args);
}

void error(const char* format, ...) {
    if (!prose()) return;
    va_list args;
    va_start(args, format);
    vtext(&kError, false, format, args);
    va_end(args);
}

void same_line() {
    if (prose()) ImGui::SameLine();
}

void spacing() {
    if (prose()) ImGui::Dummy(ImVec2(0.0f, 6.0f * g_k));
}

}  /* namespace uiw */
