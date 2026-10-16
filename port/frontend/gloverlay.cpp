/* The OpenGL half of the top-screen layer (topoverlay.h): after the presenter
 * has drawn the frame, draw the port's layer over it in the same rectangle.
 *
 * ITS OWN FILE because it needs GL beyond 1.1 - shaders and a vertex array -
 * and that means the loader the compute renderer already brought up (glad,
 * loaded by ComputeHost.cpp before the renderer starts). glpresent.cpp includes
 * SDL_opengl.h, and glad refuses to share a translation unit with another GL
 * header.
 *
 * A DS PIXEL TO A LAYER PIXEL. The fragment shader maps the letterbox rectangle
 * onto the layer the way the presenter maps it onto the frame, by proportion
 * and rounding down, and fetches without filtering: the port's pieces are DS
 * pixels, like the game's own 2D layers under the HD path, and must land on the
 * same grid.
 *
 * STATE IS PUT BACK. The presenter binds what it needs each frame, and imgui's
 * backend saves and restores its own, but nothing is gained by leaving this
 * pass's program, texture or blend behind for whichever of them runs next. */

#include "../vendor/melonds/glad/glad.h"

#include "glpresent.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

GLuint g_program = 0;
GLuint g_vao = 0;
GLuint g_texture[2] = {0, 0};
int g_texture_width[2] = {0, 0};
int g_texture_height[2] = {0, 0};
bool g_failed = false;

const char* const kVertex = R"(#version 430 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

/* viewport: the letterbox in GL window coordinates (x, y from the bottom, w, h);
 * size: the layer, width x height. Row 0 of the layer is the TOP of the picture. */
const char* const kFragment = R"(#version 430 core
layout(location = 0) uniform ivec4 viewport;
layout(location = 1) uniform ivec2 size;
layout(binding = 0) uniform sampler2D layer;
out vec4 colour;
void main() {
    vec2 rel = (gl_FragCoord.xy - vec2(viewport.xy)) / vec2(viewport.zw);
    ivec2 t = ivec2(floor(vec2(rel.x, 1.0 - rel.y) * vec2(size)));
    t = clamp(t, ivec2(0), size - 1);
    vec4 c = texelFetch(layer, t, 0);
    if (c.a == 0.0) discard;
    colour = c;
}
)";

GLuint compile(GLenum type, const char* source) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[gl] overlay shader did not compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool ready() {
    if (g_program) return true;
    if (g_failed) return false;
    g_failed = true;   /* until proven otherwise: one report, not one a frame */
    if (!glCreateShader) {
        std::fprintf(stderr, "[gl] overlay pass: the GL loader is not up\n");
        return false;
    }
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "[gl] overlay program did not link: %s\n", log);
        glDeleteProgram(p);
        return false;
    }
    glGenVertexArrays(1, &g_vao);
    glGenTextures(2, g_texture);
    g_program = p;
    g_failed = false;
    std::fprintf(stderr, "[gl] overlay pass ready (the port's top-screen layer "
                         "is drawn over the presented frame)\n");
    return true;
}

}  // namespace

bool lsw2_gl_present_overlay(const uint32_t* layer, int width, int height,
                             const SDL_Rect& viewport, int slot) {
    if (!layer || width <= 0 || height <= 0 || !lsw2_gl_active() || !ready())
        return false;
    slot = slot ? 1 : 0;

    GLint last_program = 0, last_vao = 0, last_texture = 0, last_active = 0;
    GLint last_row_length = 0, last_alignment = 0, last_unpack = 0;
    /* A BOUND UNPACK BUFFER would turn `layer` into an offset into it. */
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &last_unpack);
    if (last_unpack) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &last_row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &last_alignment);
    const GLboolean last_blend = glIsEnabled(GL_BLEND);
    const GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);

    glBindTexture(GL_TEXTURE_2D, g_texture[slot]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (g_texture_width[slot] != width || g_texture_height[slot] != height) {
        /* NEAREST, because the default minification filter wants mipmaps and a
         * texture without them is incomplete - it would read as black. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, nullptr);
        g_texture_width[slot] = width;
        g_texture_height[slot] = height;
    }
    /* ARGB8888 in a little-endian word is B, G, R, A in memory: GL_BGRA, as
     * lsw2_gl_touch_texture uploads the touch screen. */
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA,
                    GL_UNSIGNED_BYTE, layer);

    int drawable_w = 0, drawable_h = 0;
    SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &drawable_w, &drawable_h);
    const int gl_y = drawable_h - (viewport.y + viewport.h);

    glDisable(GL_SCISSOR_TEST);
    glViewport(viewport.x, gl_y, viewport.w, viewport.h);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                        GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_program);
    glUniform4i(0, viewport.x, gl_y, viewport.w, viewport.h);
    glUniform2i(1, width, height);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(GLuint(last_vao));
    glUseProgram(GLuint(last_program));
    glBindTexture(GL_TEXTURE_2D, GLuint(last_texture));
    glActiveTexture(GLenum(last_active));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, last_row_length);
    glPixelStorei(GL_UNPACK_ALIGNMENT, last_alignment);
    if (last_unpack) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(last_unpack));
    if (!last_blend) glDisable(GL_BLEND);
    if (last_scissor) glEnable(GL_SCISSOR_TEST);

    static bool said[2] = {false, false};
    if (!said[slot]) {
        said[slot] = true;
        std::fprintf(stderr, "[gl] overlay drawn: %dx%d %s layer over the %dx%d "
                             "letterbox at (%d,%d)\n",
                     width, height, slot ? "HUD" : "text", viewport.w,
                     viewport.h, viewport.x, viewport.y);
    }
    return true;
}

/* LSW2_PRESENT_SHOT'S OPENGL HALF (presentshot.h). That instrument writes the
 * CPU frame the port hands to the present path - and under OpenGL the presenter
 * may never have shown that frame, which is exactly how the arena HUD was
 * invisible while its captures showed it. So under
 * OpenGL the window's own picture is read back too: the letterbox rectangle of
 * the back buffer, after the frame and the layer are drawn and before the UI,
 * written as present-top-gl.ppm beside present-top.ppm every 30 frames. Off
 * unless LSW2_PRESENT_SHOT names a directory. */
void lsw2_gl_presentshot(const SDL_Rect& viewport) {
    static const char* dir = std::getenv("LSW2_PRESENT_SHOT");
    static int counter = 0;
    if (!dir || !*dir || !lsw2_gl_active() || !glReadPixels) return;
    if (++counter < 30) return;
    counter = 0;
    if (viewport.w <= 0 || viewport.h <= 0) return;

    int drawable_w = 0, drawable_h = 0;
    SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &drawable_w, &drawable_h);
    const int gl_y = drawable_h - (viewport.y + viewport.h);

    GLint last_pack = 0, last_alignment = 0, last_row_length = 0;
    /* The compute renderer leaves its readback buffer bound to the PACK point
     * (GPU3D_Compute.cpp); with it bound, the pointer below would be an offset
     * into that buffer. */
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &last_pack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &last_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &last_row_length);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    std::vector<unsigned char> rgba(size_t(viewport.w) * size_t(viewport.h) * 4u);
    glReadPixels(viewport.x, gl_y, viewport.w, viewport.h, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(last_pack));
    glPixelStorei(GL_PACK_ALIGNMENT, last_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, last_row_length);

    const std::string path = std::string(dir) + "/present-top-gl.ppm";
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", viewport.w, viewport.h);
    std::vector<unsigned char> row(size_t(viewport.w) * 3u);
    for (int y = viewport.h - 1; y >= 0; --y) {   /* GL rows run bottom-up */
        const unsigned char* s = &rgba[size_t(y) * size_t(viewport.w) * 4u];
        for (int x = 0; x < viewport.w; ++x) {
            row[size_t(x) * 3u + 0] = s[size_t(x) * 4u + 0];
            row[size_t(x) * 3u + 1] = s[size_t(x) * 4u + 1];
            row[size_t(x) * 3u + 2] = s[size_t(x) * 4u + 2];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[presentshot] OpenGL window picture -> %s "
                             "(%dx%d letterbox)\n",
                     path.c_str(), viewport.w, viewport.h);
    }
}

void lsw2_gl_overlay_stop() {
    g_failed = false;
    if (!g_program) return;   /* never built: nothing to release, maybe no loader */
    if (g_texture[0]) glDeleteTextures(2, g_texture);
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_program) glDeleteProgram(g_program);
    g_texture[0] = g_texture[1] = 0;
    g_vao = 0;
    g_program = 0;
    g_texture_width[0] = g_texture_width[1] = 0;
    g_texture_height[0] = g_texture_height[1] = 0;
    g_failed = false;
}
