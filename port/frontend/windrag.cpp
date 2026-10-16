/* See windrag.h. */

#include "windrag.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

/* Two game windows at most; their original procedures, to chain to. */
struct Hooked {
    HWND hwnd;
    WNDPROC prev;
};
Hooked g_hooked[4] = {};
int g_hooked_count = 0;

/* The drag in progress: which window, which part of it was pressed (HTCAPTION
 * or one of the eight sizing edges), and where cursor and window started. */
struct Drag {
    HWND hwnd = nullptr;
    WPARAM hit = 0;
    POINT cursor{};
    RECT rect{};
};
Drag g_drag;

/* A window this small is still a window; a resize past this is refused. */
constexpr LONG kMinW = 160, kMinH = 120;

WNDPROC prev_of(HWND h) {
    for (int i = 0; i < g_hooked_count; ++i)
        if (g_hooked[i].hwnd == h) return g_hooked[i].prev;
    return nullptr;
}

bool is_edge(WPARAM hit) { return hit >= HTLEFT && hit <= HTBOTTOMRIGHT; }

void follow(HWND h) {
    POINT p;
    if (!GetCursorPos(&p)) return;
    const LONG dx = p.x - g_drag.cursor.x, dy = p.y - g_drag.cursor.y;
    RECT r = g_drag.rect;
    if (g_drag.hit == HTCAPTION) {
        SetWindowPos(h, nullptr, r.left + dx, r.top + dy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    const WPARAM e = g_drag.hit;
    if (e == HTLEFT || e == HTTOPLEFT || e == HTBOTTOMLEFT)
        r.left = std::min<LONG>(r.left + dx, r.right - kMinW);
    if (e == HTRIGHT || e == HTTOPRIGHT || e == HTBOTTOMRIGHT)
        r.right = std::max<LONG>(r.right + dx, r.left + kMinW);
    if (e == HTTOP || e == HTTOPLEFT || e == HTTOPRIGHT)
        r.top = std::min<LONG>(r.top + dy, r.bottom - kMinH);
    if (e == HTBOTTOM || e == HTBOTTOMLEFT || e == HTBOTTOMRIGHT)
        r.bottom = std::max<LONG>(r.bottom + dy, r.top + kMinH);
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK drag_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const WNDPROC prev = prev_of(h);
    switch (msg) {
    case WM_NCLBUTTONDOWN:
        /* THE PRESS THAT WOULD START THE SYSTEM'S LOOP. Activation has already
         * happened (WM_MOUSEACTIVATE comes first), so nothing is lost by not
         * passing this on. A maximised window's caption drag restores it; that
         * is left to the system. */
        if ((wp == HTCAPTION && !IsZoomed(h)) || (is_edge(wp) && !IsZoomed(h))) {
            g_drag.hwnd = h;
            g_drag.hit = wp;
            GetCursorPos(&g_drag.cursor);
            GetWindowRect(h, &g_drag.rect);
            SetCapture(h);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (g_drag.hwnd == h) {
            /* A RELEASE THIS WINDOW NEVER SAW ends the drag, as it ends the
             * system's own loop: the button is asked directly. */
            const int button =
                GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
            if (!(GetAsyncKeyState(button) & 0x8000)) {
                g_drag.hwnd = nullptr;
                ReleaseCapture();
                return 0;
            }
            follow(h);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_drag.hwnd == h) {
            follow(h);
            g_drag.hwnd = nullptr;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_drag.hwnd == h && reinterpret_cast<HWND>(lp) != h)
            g_drag.hwnd = nullptr;
        break;
    default:
        break;
    }
    return prev ? CallWindowProcW(prev, h, msg, wp, lp)
                : DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void lsw2_windrag_attach(SDL_Window* window) {
    if (!window || g_hooked_count >= 4) return;
    if (const char* off = std::getenv("LSW2_WINDRAG_OFF"); off && *off == '1')
        return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
        return;
    HWND h = info.info.win.window;
    if (!h || prev_of(h)) return;
    const WNDPROC prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(drag_proc)));
    if (!prev) return;
    g_hooked[g_hooked_count++] = {h, prev};
    std::fprintf(stderr, "[shell] window drags run without the system's move "
                         "loop, so the game keeps running while a window is "
                         "moved or resized\n");
}

#else

void lsw2_windrag_attach(SDL_Window*) {}

#endif
