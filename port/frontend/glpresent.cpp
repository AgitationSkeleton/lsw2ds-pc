/* See glpresent.h. */

#include "glpresent.h"

#include "gpu2d.h"
#include "gpu3d.h"
#include "state.h"         /* bus_device_read32 */
#include "melonds_compute/ComputeHost.h"
#include "melonds_compute/TextureUpscale.h"

#include <SDL_opengl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool g_active = false;
SDL_GLContext g_context = nullptr;
GLuint g_touch_texture = 0;
bool g_touch_linear = false;

int clamp_scale(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

/* SET AN ENVIRONMENT VARIABLE THE C RUNTIME CAN SEE.
 *
 * On Windows there are TWO environments: the Win32 one (SetEnvironmentVariable
 * / GetEnvironmentVariable) and the C runtime's own copy (_putenv / getenv).
 * SDL_setenv writes the Win32 one. The framework reads its renderer policy with
 * std::getenv, which reads the CRT one - so SDL_setenv looked like it worked,
 * changed nothing getenv could see, and the compute renderer was skipped with
 * the policy still reading "soft". The symptom was a clean fallback to software
 * with no error anywhere, which is the hardest kind to spot.
 *
 * _putenv_s writes the CRT copy, which is the one that matters here. Both are
 * set so anything reading either agrees. */
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
    SDL_setenv(name, value, 1);
}

}  // namespace

Lsw2GlOptions lsw2_gl_requested(const Lsw2GlOptions& from_settings) {
    Lsw2GlOptions out = from_settings;

    /* Environment wins over the settings file, because this is the setting most
     * likely to stop the port starting at all: a driver that cannot do GL 4.3
     * needs a way back in that does not involve editing a file the port may be
     * rewriting on exit. */
    if (const char* value = std::getenv("LSW2_RENDERER")) {
        if (!SDL_strcasecmp(value, "gl") || !SDL_strcasecmp(value, "opengl"))
            out.enabled = true;
        else if (!SDL_strcasecmp(value, "software") ||
                 !SDL_strcasecmp(value, "soft"))
            out.enabled = false;
    }

    out.internal_resolution = clamp_scale(out.internal_resolution, 1, 4);
    /* 1, 2 or 4 - the framework rejects 3. Round down rather than refuse, so a
     * hand-edited settings file cannot stop the port booting. */
    out.texture_upscale = out.texture_upscale >= 4 ? 4
                        : out.texture_upscale >= 2 ? 2 : 1;
    return out;
}

uint32_t lsw2_gl_window_flags(const Lsw2GlOptions& options) {
    return options.enabled ? uint32_t(SDL_WINDOW_OPENGL) : 0u;
}

bool lsw2_gl_start(SDL_Window* top_window, const Lsw2GlOptions& options) {
    g_active = false;
    if (!options.enabled || !top_window) return false;

    /* THE 3D RENDERER MUST BE THE COMPUTE ONE. Our shell normally forces the
     * software rasteriser because the CPU present path cannot see GL output;
     * here the opposite is true, so say so before anything else reads the
     * policy. Setting it through the environment is how the framework's own
     * renderer selection is driven. */
    set_env("NDS_3D_RENDERER", "auto");

    /* THE SCALE IS A BAKED SHADER CONSTANT and sizes every framebuffer, so it
     * has to be known before the renderer compiles anything. The framework
     * refuses to change it under a live renderer, which is exactly why this
     * runs here and why the UI calls it restart-required. */
    if (!nds_gpu3d_set_internal_scale(uint8_t(options.internal_resolution))) {
        std::fprintf(stderr,
                     "[gl] internal resolution %dx is unavailable; using 1x\n",
                     options.internal_resolution);
        nds_gpu3d_set_internal_scale(1);
    }

    /* The compositor only pays for the extra per-pixel stores when something
     * can consume them, which is the GL presenter and nothing else. */
    nds_gpu2d_set_hd_emit(options.internal_resolution > 1);
    nds_texture_upscale_set_factor(options.texture_upscale);

    if (!nds_compute_host_start(top_window)) {
        std::fprintf(stderr, "[gl] compute host failed to start\n");
        nds_gpu2d_set_hd_emit(false);
        return false;
    }
    if (!nds_compute_host_active()) {
        /* Auto policy is allowed to fall back to software when GL 4.3 is
         * missing. That is a legitimate outcome, not an error - but it means
         * this window has no SDL_Renderer and no GL presenter, so the caller
         * has to rebuild it. Report the reason rather than leaving a black
         * window and no explanation.
         *
         * NOT "no OpenGL 4.3" - that is one cause of several, and this line
         * named it for years while the real one was a render width the
         * renderer refuses (only multiples of 64 start). The framework prints the actual reason
         * on the [gpu3d] line just before this one. */
        std::fprintf(stderr,
                     "[gl] the compute renderer did not start (the [gpu3d] "
                     "line above says why); falling back to the software "
                     "path\n");
        nds_gpu2d_set_hd_emit(false);
        nds_compute_host_stop();
        set_env("NDS_3D_RENDERER", "soft");
        return false;
    }

    g_context = SDL_GL_GetCurrentContext();
    nds_gpu2d_set_direct_present(nds_compute_host_has_visible_context());
    g_active = true;

    std::fprintf(stderr,
                 "[gl] OpenGL present active: internal %dx, textures %dx\n",
                 options.internal_resolution, options.texture_upscale);
    return true;
}

bool lsw2_gl_active() { return g_active; }
SDL_GLContext lsw2_gl_context() { return g_context; }

bool lsw2_gl_present_top(const uint32_t* pixels, int width,
                         const SDL_Rect& viewport) {
    if (!g_active) return false;

    /* THE DIRECT FRAME IS OPTIONAL. When the compositor produced a GPU-side
     * description of this frame the presenter uses it; when it did not - an
     * unsupported scene, a capture, engine B on top - the CPU composite is the
     * source instead. Asking for it every frame and passing null when there is
     * none is how the framework's own presenter does it, and it means an
     * unsupported scene degrades to the same picture the software path would
     * have shown rather than to nothing. */
    NdsGpu2dDirectFrame direct{};
    const bool have_direct = nds_gpu2d_direct_frame(&direct);

    /* WHY THE WIDENED PICTURE DROPPED TO A NATIVE ONE BETWEEN BARS, logged on
     * every change of reason. The adaptive compositor (gpu2d.cpp) widens a scene
     * only when the display is in normal mode with 3D on BG0, every other
     * enabled BG is a text layer, the screen is not force-blanked and no
     * display capture is running; anything else is drawn as the native
     * 256-pixel image centred between bars, with 1x 3D. The same conditions,
     * read from the registers each frame, so a log says which one a scene
     * failed. (Windows were a reason until patch 0022.) */
    {
        const uint32_t dispcnt = bus_device_read32(0, 0x04000000u);
        const uint32_t capcnt = bus_device_read32(0, 0x04000064u);
        const uint32_t bg_mode = dispcnt & 7u;
        const uint32_t display = (dispcnt >> 16) & 3u;
        const uint32_t bgs = (dispcnt >> 8) & 0xFu;
        /* Which enabled BG1-3 are not text layers in this BG mode (gpu2d.cpp's
         * bg_kind): BG2 in modes 2, 4, 5; BG3 in modes 1-5; BG0/1 in mode 6. */
        uint32_t nontext = 0;
        if (bg_mode == 6u) nontext |= bgs & 2u;
        if (bg_mode == 2u || bg_mode >= 4u) nontext |= bgs & 4u;
        if (bg_mode >= 1u) nontext |= bgs & 8u;
        char why[160] = "";
        if (dispcnt & 0x80u) std::snprintf(why + std::strlen(why), 40, " forced-blank");
        if (display != 1u) std::snprintf(why + std::strlen(why), 40, " display-mode-%u", unsigned(display));
        if (!((dispcnt & 8u) && (dispcnt & 0x100u))) std::snprintf(why + std::strlen(why), 40, " no-3D-BG0");
        if (nontext) std::snprintf(why + std::strlen(why), 40, " non-text-BG%X", unsigned(nontext));
        if (capcnt & 0x80000000u) std::snprintf(why + std::strlen(why), 40, " capture");
        static char last[160] = "?";
        static int said = 0;
        if (std::strcmp(why, last) != 0 && said < 400) {
            ++said;
            std::fprintf(stderr, "[gl] widened scene:%s (DISPCNT %08X)\n",
                         why[0] ? why : " supported", unsigned(dispcnt));
            std::snprintf(last, sizeof last, "%s", why);
        }
    }

    /* SDL's y grows downward; GL's grows upward. Flipping here rather than
     * expecting callers to know that keeps the rect in the same coordinates
     * everything else in the shell uses. */
    int drawable_w = 0, drawable_h = 0;
    SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &drawable_w, &drawable_h);
    const int rect[4] = {
        viewport.x,
        drawable_h - (viewport.y + viewport.h),
        viewport.w,
        viewport.h,
    };

    NdsComputePresentTicks ticks{};
    return nds_compute_host_present_top(
        pixels, uint16_t(width), have_direct ? &direct : nullptr, &ticks,
        rect, /*defer_swap=*/true);
}

void lsw2_gl_clear() {
    if (!g_active) return;
    /* SCISSOR OFF FIRST. The presenter sets a scissor to letterbox the picture,
     * and a clear obeys it - so without this only the previous frame's viewport
     * would go black and the bars would keep whatever was there. */
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void* lsw2_gl_touch_texture(const uint32_t* pixels, bool linear) {
    if (!g_active || !pixels) return nullptr;

    if (!g_touch_texture) {
        glGenTextures(1, &g_touch_texture);
        if (!g_touch_texture) return nullptr;
        glBindTexture(GL_TEXTURE_2D, g_touch_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 192, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        g_touch_linear = !linear;   /* force the filter to be set below */
    }

    glBindTexture(GL_TEXTURE_2D, g_touch_texture);
    if (linear != g_touch_linear) {
        g_touch_linear = linear;
        const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }
    /* ARGB8888 in a little-endian uint32 is B,G,R,A in memory, which is exactly
     * GL_BGRA + GL_UNSIGNED_BYTE. Naming it that way rather than swizzling by
     * hand keeps the upload a straight memcpy on the driver side. */
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192,
                    GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    return (void*)(intptr_t)g_touch_texture;
}

void lsw2_gl_swap() {
    if (!g_active) return;
    NdsComputePresentTicks ticks{};
    nds_compute_host_swap(&ticks);
}

void lsw2_gl_stop() {
    if (!g_active) return;
    lsw2_gl_overlay_stop();
    if (g_touch_texture) {
        glDeleteTextures(1, &g_touch_texture);
        g_touch_texture = 0;
    }
    g_active = false;
    g_context = nullptr;
    nds_compute_host_stop();
}
