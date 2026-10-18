/* Our shell — stage 1.
 *
 * THE SEAM. The framework's runner is one executable whose `main()` does all
 * the setup — CLI, config, ROM load, BIOS selection, bank registration, direct
 * boot — and then hands off to a single call:
 *
 *     nds_run_interactive_frontend(options)
 *
 * That function owns the SDL windows and the present loop, and there is no hook
 * into it. So it is exactly the right thing to replace: link every runner
 * object EXCEPT `frontend.cpp`, provide this file instead, and we own the loop
 * while keeping all of the setup we would otherwise have to reimplement.
 *
 * That is the same division Ship of Harkinian makes. libultraship is a shell
 * over the game, not a rewrite of it.
 *
 * WHAT THIS BUYS. Everything on the enhancement list needs to live inside the
 * present loop: imgui for configuration and input binding, resolution and
 * viewport policy, a floating touch panel inside the main window, debug and
 * cheat tabs. None of it can be bolted onto a loop we do not own.
 *
 * TWO PRESENT PATHS, and the shell works the same on both. The software path
 * reads the composited CPU framebuffer through an SDL_Renderer. The OpenGL path
 * (glpresent.cpp) presents the compute renderer's output directly, which is the
 * only way the higher internal resolution and filtered textures can be shown at
 * all - that detail exists solely in a GPU texture. Under OpenGL the top window
 * has no SDL_Renderer, so imgui switches backends and the floating touch panel
 * is fed a GL texture rather than an SDL one.
 *
 * STILL MISSING, listed here rather than discovered later:
 *   - the guest's culling frustum is not widened to match adaptive widescreen,
 *     so geometry may cull at the original 256-px edges (see widescreen.cpp)
 *   - supersampling and antialiasing (the framework has both; neither is wired
 *     up here yet)
 *   - MPH Prime controls, which belong to another game and are not wanted
 */

#include "diagnostics.h"
#include "asynclog.h"
#include "sampleprof.h"
#include "instancemute.h"
#include "vwh.h"
#include "statelog.h"
#include "ui.h"
#include "fork.h"
#include "whsession.h"
#include "inputlog.h"
#include "arenahud.h"
#include "scorescreen.h"
#include "lobbyhud.h"
#include "popuphud.h"
#include "topoverlay.h"
#include "windrag.h"
#include "freeplaypick.h"
#include "strings.h"

/* THE HLE COUNTERS, declared here because dispatch_overrides.cpp has no header
 * - it is reached through the override table rather than by name, so its few
 * exported accessors have nowhere else to be named. Used by the shutdown
 * summary at the end of nds_run_interactive_frontend. */
extern "C" uint64_t lsw2_hle_hits();
extern "C" uint64_t lsw2_hle_step_hits();
extern "C" uint64_t lsw2_hle_step_stalls();
extern "C" uint64_t lsw2_hle_string_cache_hits();
extern "C" uint64_t lsw2_hle_string_cache_misses();
#include "gamefont.h"
#include "gamemenu_render.h"
#include "presentshot.h"
#include "menuhost.h"
#include "forksession.h"
#include "../net/lsw2_relay_session.h"
#include "glpresent.h"
#include "widescreen.h"

#include "debug_server.h"
#include "frontend.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "io.h"
#include "runtime_arm.h"     /* g_runtime_cycles - the counts probe */
#include "scheduler.h"
#include "state.h"           /* bus_device_read32 - the counts probe */
#include "spu.h"
#include "wifi_net.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* LSW2_COUNTS_AT=<frame>[,<frame>...] - A DETERMINISM PROBE, off unless set.
 * At each listed host frame it prints what the emulation has computed so far:
 * the guest cycle clock, both CPUs' retired-instruction counts, and an FNV-1a
 * hash of the 4 MB of main RAM. Two runs of one build that print the same
 * lines are deterministic up to that frame; two BUILDS that print the same
 * lines computed the same thing - which is how a change to the runtime that
 * is meant to decide nothing differently (patch 0017) is graded, rather than
 * by reading it. Offline boots only: a session's frames wait on the link. */
void counts_probe(unsigned frame) {
    static std::vector<unsigned> at = [] {
        std::vector<unsigned> v;
        const char* s = std::getenv("LSW2_COUNTS_AT");
        while (s && *s) {
            char* end = nullptr;
            const unsigned long f = std::strtoul(s, &end, 10);
            if (end == s) break;
            v.push_back(unsigned(f));
            s = (*end == ',') ? end + 1 : end;
        }
        return v;
    }();
    if (at.empty() || std::find(at.begin(), at.end(), frame) == at.end()) return;
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t a = 0x02000000u; a < 0x02400000u; a += 4u) {
        const uint32_t w = bus_device_read32(0, a);
        for (int b = 0; b < 4; ++b) {
            hash ^= (w >> (8 * b)) & 0xFFu;
            hash *= 0x100000001B3ull;
        }
    }
    std::fprintf(stderr, "[counts] frame %u: cycles %llu insn9 %llu insn7 %llu "
                 "main RAM fnv %016llX\n", frame,
                 (unsigned long long)g_runtime_cycles,
                 (unsigned long long)g_insn_count[0],
                 (unsigned long long)g_insn_count[1], (unsigned long long)hash);
}

/* 2130 dot cycles x 263 scanlines. Taken from the framework's frontend rather
 * than recomputed, so the two agree by construction. */
constexpr uint64_t kSystemCyclesPerFrame = 2130ull * 263ull;

/* THE MIXER RUNS ONCE PER 1024 DS SYSTEM CYCLES, so this is its exact integer
 * host rate rather than a nominal 32768 Hz. Asking for the nominal rate leaves
 * a sub-sample remainder that accumulates into a roughly once-per-second
 * underrun - audible as a periodic tick. Taken from the framework's frontend
 * for the same reason its comment gives. */
constexpr int kAudioFrequency = 33513982 / 1024;
constexpr uint32_t kAudioFrameBytes = 2u * sizeof(int16_t);
constexpr uint32_t kAudioCapacityFrames = 65536;
/* How much audio may sit queued before the loop waits. This is the throttle
 * (see pump_audio()) and also the runway that decouples the single render
 * thread's present jitter from the audio consumer. At 32728 Hz, 2048 frames is
 * ~63 ms, about four DS frames.
 *
 * MEASURED 2026-09-13 (LSW2_PERF_LOG, per-second underrun delta, focused
 * lobby): steady-state runs at +0 underruns/s at this size - clean audio at
 * correct DS speed. The only underruns a run reports (~40-70 cumulative) all
 * occur during the boot warp, whose audio the fork already MUTES
 * (lsw2_fork_hiding_boot() zeroes the samples), so they are silence, not an
 * audible tick. A larger floor (tried 4096) removed nothing audible and only
 * added ~60 ms of output latency, so 2048 stands: lower latency is the better
 * audio fidelity when the buffer is not the bottleneck. */
constexpr uint32_t kAudioQueueFrames = 2048;

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;

bool g_exit_requested = false;

/* LSW2_PERF_LOG frame-phase breakdown, in performance-counter ticks, reset with
 * each perf line: where a second of wall time went - emulating (which includes
 * any wait on a wireless peer), the audio throttle, or presenting. */
uint64_t g_perf_emu = 0, g_perf_audio = 0, g_perf_present = 0;
uint64_t g_perf_input = 0, g_perf_debug = 0, g_perf_fork = 0, g_perf_ui = 0;

/* LSW2_MP_TRACE frame-phase marks, on the same steady-clock microsecond base as
 * the framework's [mp-trace] bridge events, so a frame's phases line up against
 * the wireless exchanges of this console and of the one it is linked to. */
void frame_trace(const char* phase) {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_MP_TRACE");
        return e && *e == '1';
    }();
    if (!on) return;
    const long long now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(stderr, "[mp-trace] %lld frame-%s 0 0 0\n", now, phase);
}

/* The live settings, shared so mouse mapping and drawing agree. */
UiSettings g_settings;

/* INPUT STATE LIVES AT FILE SCOPE so the debug surface can drive it.
 *
 * The framework's runner ships a TCP control surface (debug_server.cpp, enabled
 * with --serve) offering framebuffer capture, memory and register reads, and
 * input injection. It reaches input through nds_frontend_debug_*, which our
 * shell originally stubbed to return false - so the port was drivable through
 * the framework's frontend and not through ours.
 *
 * Implementing them properly means the game can be piloted and observed
 * remotely: screenshots of both screens, button presses, stylus taps. That is
 * worth far more than reading logs and guessing, and it is how a blank top
 * screen should have been caught in the first place.
 *
 * Injected input is OR-ed with real input rather than replacing it, so a human
 * at the keyboard and a script driving the same session do not fight. */
uint16_t g_keys_host = 0;      /* keyboard   */
uint16_t g_keys_pad = 0;       /* gamepad    */
uint16_t g_keys_injected = 0;  /* debug surface */
bool g_touch_injected = false;
uint16_t g_touch_x = 0, g_touch_y = 0;
uint64_t g_debug_key_events = 0;
uint64_t g_debug_touch_events = 0;

/* A bounded ring between the emulation thread (which produces a frame's worth
 * of samples at a time) and SDL's audio thread (which consumes in its own
 * callback). Bounded on purpose: the remainder mentioned above has to be
 * absorbed somewhere, and a growing buffer would trade a tick for latency. */
struct AudioQueue {
    std::vector<int16_t> samples;
    uint32_t read = 0, write = 0, count = 0;
    std::atomic<bool> started{false};
    std::atomic<uint32_t> underruns{0};
    AudioQueue() : samples(kAudioCapacityFrames * 2u, 0) {}
};

void SDLCALL audio_callback(void* userdata, Uint8* stream, int len) {
    auto* q = static_cast<AudioQueue*>(userdata);
    std::memset(stream, 0, static_cast<size_t>(len));
    if (!q || len <= 0) return;
    const uint32_t requested = static_cast<uint32_t>(len) / kAudioFrameBytes;
    const uint32_t take = std::min(requested, q->count);
    auto* out = reinterpret_cast<int16_t*>(stream);
    const uint32_t first = std::min(take, kAudioCapacityFrames - q->read);
    std::memcpy(out, q->samples.data() + q->read * 2u, first * kAudioFrameBytes);
    if (first < take)
        std::memcpy(out + first * 2u, q->samples.data(),
                    (take - first) * kAudioFrameBytes);
    q->read = (q->read + take) % kAudioCapacityFrames;
    q->count -= take;
    if (take < requested && q->started.load(std::memory_order_relaxed))
        q->underruns.fetch_add(1, std::memory_order_relaxed);
}

uint32_t queue_count(SDL_AudioDeviceID device, AudioQueue& q) {
    SDL_LockAudioDevice(device);
    const uint32_t n = q.count;
    SDL_UnlockAudioDevice(device);
    return n;
}

/* Drain the SPU into the ring once per frame, then throttle. If the ring is
 * full the samples are DROPPED rather than blocking the emulation thread -
 * stalling the guest to wait on audio would turn a full buffer into a
 * frame-rate problem, which is far worse than a discontinuity nobody can
 * hear. */
void pump_audio(SDL_AudioDeviceID device, AudioQueue& q) {
    if (!device) {
        /* NO AUDIO DEVICE MEANS NO CLOCK. The throttle below is what keeps the
         * guest running at real speed, so without a device we must supply a
         * clock ourselves or the game runs many times too fast - which is
         * exactly the bug this whole block exists to fix, and it would come
         * back only for people whose audio failed to open. Pace on wall time
         * to the frame period instead. */
        int16_t scratch[2048];
        while (nds_spu_read_output(scratch, 1024) != 0u) { }

        static uint64_t next_deadline = 0;
        const uint64_t freq = SDL_GetPerformanceFrequency();
        const uint64_t period = freq / 60u;          /* ~16.7 ms */
        const uint64_t now = SDL_GetPerformanceCounter();
        if (next_deadline == 0 || now > next_deadline + period * 8u)
            next_deadline = now;                     /* first frame, or resync */
        next_deadline += period;
        while (SDL_GetPerformanceCounter() < next_deadline) SDL_Delay(1);
        return;
    }
    int16_t samples[2048];
    for (;;) {
        const uint32_t frames = nds_spu_read_output(samples, 1024);
        if (!frames) break;

        /* SILENCE THE BOOT THE FORK IS ALREADY HIDING.
         *
         * The game still briefly played the splash startup music, over a
         * black screen lasting a few seconds before loading in.
         *
         * The picture was already suppressed - the fork's start skips the
         * splash titles and hides them - but the audio was not, so the
         * one thing the player got from a screen they were not supposed to see
         * was its music. The black screen itself is the boot: the warp cannot
         * fire until the game has built a state machine, and the start is meant
         * to feel instant, with no imgui popup announcing the skip, so nothing
         * is drawn over it.
         *
         * DRAINED, NOT SKIPPED. `nds_spu_read_output` has already been called,
         * and the samples are zeroed rather than the read being avoided,
         * because that read is what paces the guest - dropping it would make
         * the boot run at whatever speed the CPU allows. Silence the output,
         * keep the clock. */
        /* AND A SECOND COPY'S, when the Debug tab says so (instancemute.h):
         * drained and silenced the same way. */
        if (lsw2_fork_hiding_boot() || lsw2_instance_mute_all_sound())
            std::memset(samples, 0, size_t(frames) * kAudioFrameBytes);

        SDL_LockAudioDevice(device);
        if (kAudioCapacityFrames - q.count >= frames) {
            const uint32_t first = std::min(frames, kAudioCapacityFrames - q.write);
            std::memcpy(q.samples.data() + q.write * 2u, samples,
                        first * kAudioFrameBytes);
            if (first < frames)
                std::memcpy(q.samples.data(), samples + first * 2u,
                            (frames - first) * kAudioFrameBytes);
            q.write = (q.write + frames) % kAudioCapacityFrames;
            q.count += frames;
        }
        SDL_UnlockAudioDevice(device);
    }

    /* AUDIO IS THE HOST'S REAL-TIME CLOCK, and this is what paces the whole
     * loop. SDL consumes samples at exactly the device rate, so waiting until
     * the queue falls below a floor throttles emulation to real time without
     * any timer of our own.
     *
     * WITHOUT THIS the loop free-runs: the first version of this shell reported
     * "closed after 2552530 frames" for a few minutes of play - guest time
     * advancing far faster than wall clock, i.e. the game playing at the wrong
     * speed. The frame counter was the tell.
     *
     * The wait only stops the queue RISING above the floor. It never forces the
     * queue down, so a slow stretch spends the buffered runway rather than
     * having it slept away. */
    uint32_t queued = queue_count(device, q);
    /* BOUNDED, so a stalled audio device cannot freeze the whole loop.
     *
     * This wait is keyed on an EXTERNAL thing draining the queue - SDL's audio
     * callback consuming samples. In normal play the callback drains every
     * frame, so the queue sits just above the floor and this spins only a few
     * ms. But if that device ever stops draining - a second instance on one
     * machine that never gets callbacks, a device that disconnects mid-session
     * - the queue never falls and an UNBOUNDED wait here hangs the frame loop
     * forever: the guest stops, the window stops, the debug socket stops
     * answering. That is the >10 s stall two-instance drives hit (a `pilot.py`
     * timeout with the game still "running").
     *
     * A half-second cap can only fire on a genuine stall - normal draining
     * never leaves the queue full for 500 ms - and running slightly fast for
     * the duration of a real stall is vastly better than a permanent freeze.
     * The audio clock still paces every normal frame; this only stops it being
     * a single point of failure. */
    /* A LINKED CLIENT IS PACED BY ITS HOST, NOT BY THIS THROTTLE.
     *
     * Over local wireless the two consoles run a lockstep: the host blocks for
     * each client's reply to its command frame, and a client's emulated time
     * cannot pass the host's next frame (melonDS's NextSync gate). A client that
     * also sleeps here on its own audio clock can be asleep exactly when the
     * host's command lands, and the host then waits out the sleep - on one PC
     * that phase-locked the pair into taking turns, ~24fps each with audio
     * underrunning on both. The host's frames already
     * deliver the client exactly real time, so the client skips the sleep and
     * waits on the host instead. The throttle stays as a high-water guard: if a
     * client's queue ever climbs past twice the floor, it paces as usual. */
    static const bool client_throttle = [] {
        const char* e = std::getenv("LSW2_CLIENT_THROTTLE");
        return e && *e == '1';
    }();
    if (!client_throttle && nds_wifi_is_mp_client() &&
        queued <= 2u * kAudioQueueFrames)
        return;
    /* THE SAME RULE FOR THE HOST-LEVEL WH, where the host is the
     * clock: a joiner holding more than a couple of the host's datasets it has
     * not consumed yet is running behind it - its blocks will start missing the
     * host's commits - so it skips the sleep until it has caught up. Bounded by
     * the same high-water guard. */
    if (lsw2_vwh_backlog() > 1 && queued <= 2u * kAudioQueueFrames)
        return;

    const uint32_t wait_start = SDL_GetTicks();
    while (queued > kAudioQueueFrames) {
        if (SDL_GetTicks() - wait_start > 500u) {
            static uint32_t last_warn = 0;
            const uint32_t now = SDL_GetTicks();
            if (now - last_warn > 1000u) {
                std::fprintf(stderr, "[shell] audio not draining (%u queued) - "
                             "pacing on without it to avoid a freeze\n", queued);
                last_warn = now;
            }
            break;
        }
        SDL_Delay(1);
        queued = queue_count(device, q);
    }
}

/* KEY AND PAD LOOKUP NOW GO THROUGH THE UI'S BINDING TABLE. The DS bit layout
 * is written down in exactly one place (ui.cpp), so a rebind in the settings
 * window takes effect immediately and the two can never disagree. */
uint16_t key_bit(SDL_Scancode key) {
    int n = 0;
    const UiBinding* b = ui_bindings(&n);
    for (int i = 0; i < n; ++i)
        if (b[i].scancode == int(key)) return b[i].ds_bit;
    return 0;
}

uint16_t controller_bit(SDL_GameControllerButton button) {
    int n = 0;
    const UiBinding* b = ui_bindings(&n);
    for (int i = 0; i < n; ++i)
        if (b[i].pad_button == int(button)) return b[i].ds_bit;
    return 0;
}

/* THE LEFT STICK, AS A D-PAD.
 *
 * Held separately from g_keys_pad because the two are updated by different
 * kinds of event - buttons arrive as discrete presses, the stick has to be
 * sampled - and mixing them meant a button release could clear a direction the
 * stick was still holding.
 *
 * The DS has no analog input. A stick can therefore only produce the same four
 * direction bits the d-pad produces; there is nothing finer to send. */
uint16_t g_keys_stick = 0;

uint16_t dpad_bit(const char* action) {
    int n = 0;
    const UiBinding* b = ui_bindings(&n);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(b[i].action, action) == 0) return b[i].ds_bit;
    return 0;
}

void sample_stick(SDL_GameController* pad, const UiSettings& s) {
    g_keys_stick = 0;
    if (!pad || !s.stick_as_dpad) return;

    /* SDL reports -32768..32767 per axis. The deadzone is a percentage so the
     * setting means the same thing whatever the hardware reports. */
    const int threshold = 32767 * std::clamp(s.stick_deadzone, 5, 90) / 100;
    const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);

    if (x < -threshold) g_keys_stick |= dpad_bit("D-pad Left");
    if (x > threshold)  g_keys_stick |= dpad_bit("D-pad Right");
    if (y < -threshold) g_keys_stick |= dpad_bit("D-pad Up");
    if (y > threshold)  g_keys_stick |= dpad_bit("D-pad Down");
}

/* THE ENHANCED CONTROLS' CAMERA INPUT - the right stick, and the Camera bindings.
 *
 * The right stick used to be read inside sample_stick, after its "stick as
 * d-pad" early-out - so turning that setting off silently took the camera
 * with it. It is its own sampler now, polled every frame for the same reason
 * the left stick is.
 *
 * ITS OWN DEADZONE, because it has its own setting and a camera
 * wants a wider one than movement: a stick resting a little off centre nudges
 * a character imperceptibly and drifts a camera all the way round.
 *
 * SCALED TO DS YAW UNITS: 32767 maps to 256 units a frame, about a quarter of a
 * turn a second at 60 fps - the most the game's own camera speed limit
 * (0x020A34BC, 2.0 units an update) lets the orbit move anyway. The vertical
 * axis uses the same scale; enhanced.cpp turns it into the orbit's height.
 *
 * BINDABLE FROM THE INPUT SCREEN: the four Camera rows of the binding table
 * (ui.cpp) drive the same motion at full deflection, from a key or a pad
 * button, so a keyboard player has a camera too - and so a script can turn it
 * through the debug surface (cam_left, cam_right, cam_up, cam_down). */
constexpr int kCameraFull = 256;
uint8_t g_camera_injected = 0;   /* debug surface: bit 0 L, 1 R, 2 U, 3 D */

bool camera_binding_held(const char* action, const Uint8* keys,
                         SDL_GameController* pad) {
    int n = 0;
    const UiBinding* b = ui_bindings(&n);
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(b[i].action, action) != 0) continue;
        if (keys && b[i].scancode > 0 && b[i].scancode < SDL_NUM_SCANCODES &&
            keys[b[i].scancode])
            return true;
        if (pad && b[i].pad_button >= 0 &&
            SDL_GameControllerGetButton(pad, SDL_GameControllerButton(b[i].pad_button)))
            return true;
    }
    return false;
}

void sample_camera(SDL_GameController* pad) {
    int dx = 0, dy = 0;
    if (pad) {
        const int rs_threshold =
            32767 * std::clamp(lsw2_ui_right_stick_deadzone(), 5, 90) / 100;
        /* RESCALED PAST THE DEADZONE, so the camera starts from a crawl just
         * outside it and reaches full speed at full deflection - rather than
         * jumping straight to the deadzone's own fraction of full speed. */
        auto shape = [rs_threshold](int v) {
            const int mag = v < 0 ? -v : v;
            if (mag <= rs_threshold) return 0;
            const int span = 32767 - rs_threshold;
            const int out = (std::min(mag, 32767) - rs_threshold) * kCameraFull / (span > 0 ? span : 1);
            return v < 0 ? -out : out;
        };
        dx += shape(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX));
        dy += shape(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (camera_binding_held("Camera Left", keys, pad) || (g_camera_injected & 1u))
        dx -= kCameraFull;
    if (camera_binding_held("Camera Right", keys, pad) || (g_camera_injected & 2u))
        dx += kCameraFull;
    if (camera_binding_held("Camera Up", keys, pad) || (g_camera_injected & 4u))
        dy -= kCameraFull;
    if (camera_binding_held("Camera Down", keys, pad) || (g_camera_injected & 8u))
        dy += kCameraFull;
    dx = std::clamp(dx, -kCameraFull, kCameraFull);
    dy = std::clamp(dy, -kCameraFull, kCameraFull);
    /* The player's speed settings (Input tab), percent of the base rate. */
    dx = dx * std::clamp(lsw2_ui_right_stick_speed_x(), 25, 600) / 100;
    dy = dy * std::clamp(lsw2_ui_right_stick_speed_y(), 25, 600) / 100;
    if (lsw2_ui_right_stick_invert_x()) dx = -dx;
    if (lsw2_ui_right_stick_invert_y()) dy = -dy;
    if (dx != 0) lsw2_ui_push_camera_delta(dx);
    if (dy != 0) lsw2_ui_push_camera_delta_y(dy);
}

struct Window {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int tex_width = kScreenWidth;   /* widens with the 3D raster */
};

bool open_window(Window& w, const char* title, int index,
                 uint32_t extra_flags = 0) {
    /* NEVER DO ARITHMETIC ON SDL_WINDOWPOS_*. They are magic constants
     * (0x2FFF0000 | display), not coordinates. The first version of this
     * function used
     *
     *     SDL_WINDOWPOS_CENTERED_DISPLAY(0) + (index ? 40 : -300)
     *
     * to nudge the two windows apart, which corrupts the magic value and put
     * the top window somewhere off-screen: it appeared in the taskbar, and
     * clicking it brought nothing to the foreground. Compute real coordinates
     * from the display bounds instead. */
    /* THE REMEMBERED SIZE, or the scale preset when there is none. Zero means
     * this install has never been resized, so the preset stands - which is why
     * the two are stored separately rather than the drag overwriting the
     * preset. */
    const int saved_w = (index == 0) ? g_settings.window_w : g_settings.touch_window_w;
    const int saved_h = (index == 0) ? g_settings.window_h : g_settings.touch_window_h;
    const int scale = std::clamp(g_settings.window_scale, 1, 6);
    const int win_w = saved_w > 0 ? saved_w : kScreenWidth * scale;
    const int win_h = saved_h > 0 ? saved_h : kScreenHeight * scale;

    /* WHICH DISPLAY, and optionally exactly where.
     *
     * The port centred the pair on display 0 - the primary - which on a
     * multi-monitor desk drops test windows on top of whatever else is on that
     * screen. Two opt-in overrides fix that without changing the default:
     *   LSW2_WINDOW_DISPLAY=N       - centre the pair on SDL display N instead;
     *   LSW2_WINDOW_RECT=X,Y,W,H    - use this exact region as the bounds,
     *                                 independent of how SDL orders displays
     *                                 (e.g. a secondary monitor's region).
     * With LSW2_NO_ACTIVATE=1 the windows then appear on that monitor without
     * taking the foreground, so a test run can use a spare screen without
     * interrupting other work. */
    int want_display = 0;
    if (const char* d = std::getenv("LSW2_WINDOW_DISPLAY")) want_display = std::atoi(d);
    const int ndisp = SDL_GetNumVideoDisplays();
    if (want_display < 0 || (ndisp > 0 && want_display >= ndisp)) want_display = 0;

    SDL_Rect bounds{};
    if (SDL_GetDisplayUsableBounds(want_display, &bounds) != 0 || bounds.w <= 0) {
        bounds = SDL_Rect{0, 0, 1280, 800};
    }
    if (const char* r = std::getenv("LSW2_WINDOW_RECT")) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (std::sscanf(r, "%d,%d,%d,%d", &rx, &ry, &rw, &rh) == 4 && rw > 0 && rh > 0)
            bounds = SDL_Rect{rx, ry, rw, rh};
    }
    if (index == 0)
        std::fprintf(stderr, "[shell] window bounds display %d: %d,%d %dx%d\n",
                     want_display, bounds.x, bounds.y, bounds.w, bounds.h);

    /* Side by side, centred as a pair, and clamped so neither can land outside
     * the usable area on a small display. */
    const int gap = 16;
    const int pair_w = win_w * 2 + gap;
    int x = bounds.x + (bounds.w - pair_w) / 2 + index * (win_w + gap);
    int y = bounds.y + (bounds.h - win_h) / 2;
    if (x < bounds.x) x = bounds.x;
    if (y < bounds.y) y = bounds.y;

    /* DO NOT STEAL FOCUS WHEN LSW2_NO_ACTIVATE IS SET.
     *
     * A test run can share a desktop with other work, and must not take the
     * foreground from it. Most verification now goes through the headless
     * debug surface, which never creates a window at all - but the fork's boot
     * behaviour lives in THIS loop, so testing it needs real windows.
     *
     * SDL's own hint does exactly the right thing on Windows: the window is
     * created and shown without being activated, so it appears without taking
     * the foreground. Named by string rather than by macro so this still
     * compiles against an SDL too old to define it; an unknown hint is ignored.
     *
     * Opt-in, because a player double-clicking the game DOES want it in front. */
    const char* no_activate = std::getenv("LSW2_NO_ACTIVATE");
    const bool quiet = no_activate && *no_activate == '1';
    if (quiet) SDL_SetHint("SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN", "1");

    w.window = SDL_CreateWindow(title, x, y, win_w, win_h,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                                extra_flags);
    if (!w.window) return false;
    /* Ask the window manager to actually show it. Cheap, and it removes a whole
     * class of "it is in the taskbar but I cannot see it" report.
     *
     * RaiseWindow is what actually takes the foreground, so it is the one that
     * has to be skipped - showing alone is harmless. */
    SDL_ShowWindow(w.window);
    if (!quiet) SDL_RaiseWindow(w.window);
    /* Moving or resizing it must not stop the frame loop (windrag.h). */
    lsw2_windrag_attach(w.window);

    /* AN OPENGL WINDOW GETS NO SDL_RENDERER. Creating one would have SDL make
     * its own GL context on this window and fight the compute renderer for it.
     * The texture below is skipped for the same reason: under OpenGL the frame
     * is presented by glpresent.cpp, and w.texture is never used. */
    if (extra_flags & SDL_WINDOW_OPENGL) return true;

    w.renderer = SDL_CreateRenderer(w.window, -1, SDL_RENDERER_ACCELERATED);
    if (!w.renderer) w.renderer = SDL_CreateRenderer(w.window, -1, 0);
    if (!w.renderer) return false;
    w.texture = SDL_CreateTexture(w.renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  kScreenWidth, kScreenHeight);
    return w.texture != nullptr;
}

void close_window(Window& w) {
    if (w.texture) SDL_DestroyTexture(w.texture);
    if (w.renderer) SDL_DestroyRenderer(w.renderer);
    if (w.window) SDL_DestroyWindow(w.window);
    w = Window{};
}

/* A SCREEN THAT IS BLACK FOREVER IS A BUG THIS SHELL HAS ALREADY SHIPPED ONCE,
 * and nothing in any log said so - the loop ran, the FPS was right, frames were
 * presented, and the top window was simply empty. It happened because the
 * framework's default 3D backend presents the top screen through OpenGL while
 * only the bottom goes through SDL textures, so our CPU-framebuffer path had
 * nothing to show.
 *
 * So: notice, and say so once. Fades and load transitions are legitimately
 * black, hence a threshold of several seconds rather than a frame or two. */
void watch_for_blank(int screen, const uint32_t* pixels) {
    static int blank_run[2] = {0, 0};
    static bool warned[2] = {false, false};
    if (screen < 0 || screen > 1) return;

    bool any = false;
    if (pixels) {
        for (int i = 0; i < kScreenWidth * kScreenHeight; i += 7) {
            if ((pixels[i] & 0x00FFFFFFu) != 0u) { any = true; break; }
        }
    }
    if (any) { blank_run[screen] = 0; return; }
    if (++blank_run[screen] == 300 && !warned[screen]) {
        warned[screen] = true;
        std::fprintf(stderr,
            "[shell] WARNING: the %s screen has been blank for 300 frames. "
            "If the 3D backend is the GL compute renderer this shell cannot "
            "present it - run with NDS_3D_RENDERER=soft.\n",
            screen == 0 ? "top" : "bottom");
    }
}

/* Present one screen, letterboxed so the picture keeps its aspect ratio however
 * the window is dragged. Scaling changes how a frame is shown; it must never
 * change what the game computed. */
/* Where a screen lands inside its window under the current policy. Shared by
 * drawing and by mouse-to-stylus mapping, so a click can never disagree with
 * what is on screen - if these were computed in two places they would drift the
 * first time an option was added. */
SDL_Rect viewport_for(int ww, int wh, const UiSettings& s,
                      int src_w = kScreenWidth) {
    SDL_Rect dst{0, 0, ww, wh};
    if (!s.preserve_aspect) return dst;

    /* Aspect is taken from the SOURCE, which widens with the 3D raster. Using
     * the native 4:3 here would letterbox a widescreen frame back down and
     * throw away exactly the extra world the widening produced. */
    const double want = static_cast<double>(src_w) / kScreenHeight;
    const double have = wh ? static_cast<double>(ww) / wh : want;

    /* WIDESCREEN FILLS THE WINDOW BY CROPPING, not by letterboxing.
     *
     * The 3D raster is fixed for the session at whatever the display's shape
     * suggested - usually the 448-px ceiling, which is 2.33:1. Letterboxing
     * that into an ordinary window left bars thick enough that adaptive
     * widescreen looked broken - it failed to fill the game window. It was
     * broken: a 4:3 window was showing a 2.33:1 picture squeezed into a third
     * of its height.
     *
     * The raster is a SUPERSET of the native picture, and the widening splits
     * its extra columns evenly either side with the native 256 staying centred
     * - widescreen.cpp guarantees that, and the compositor depends on it. So
     * scaling to fill the window's HEIGHT and letting the extra columns run off
     * the sides shows exactly as much world as the window's shape allows, at
     * the correct aspect, with no bars.
     *
     * NEVER CROPPING BELOW THE NATIVE 256 is what makes it safe: every HUD
     * element lives inside that centred region, so nothing the game considers
     * on-screen can be cut. A window narrower than 4:3 falls back to
     * letterboxing the native picture, which is the same thing the port has
     * always done.
     *
     * Only when the raster is actually wider than native - at 256 this is a
     * plain 4:3 picture and bars are the right answer. */
    if (src_w > kScreenWidth) {
        const double fill_height = wh / double(kScreenHeight);
        const double keep_native = ww / double(kScreenWidth);
        double scale = std::min(fill_height, keep_native);
        /* INTEGER SCALE APPLIES HERE TOO. This branch returned before the
         * integer_scale block below, so with adaptive widescreen on - the
         * OpenGL default sizes the raster to the display, so this branch is
         * the common one - the "Integer scale" checkbox changed nothing. The
         * crop policy is unchanged: the scale is still the one that fills the
         * window's height without cutting into the native 256 columns, only
         * rounded down to a whole multiple so every guest pixel is the same
         * size, which is all the checkbox ever promised. */
        if (s.integer_scale) scale = std::max(1.0, std::floor(scale));
        dst.w = static_cast<int>(src_w * scale + 0.5);
        dst.h = static_cast<int>(kScreenHeight * scale + 0.5);
        dst.x = (ww - dst.w) / 2;
        dst.y = (wh - dst.h) / 2;
        return dst;
    }

    if (have > want) {                    /* pillarbox */
        dst.h = wh;
        dst.w = static_cast<int>(wh * want + 0.5);
    } else {                              /* letterbox */
        dst.w = ww;
        dst.h = static_cast<int>(ww / want + 0.5);
    }
    if (s.integer_scale) {
        int k = std::min(dst.w / src_w, dst.h / kScreenHeight);
        if (k < 1) k = 1;
        dst.w = src_w * k;
        dst.h = kScreenHeight * k;
    }
    dst.x = (ww - dst.w) / 2;
    dst.y = (wh - dst.h) / 2;
    return dst;
}

/* Draw one screen. Presentation is the caller's job now, because the top window
 * has the UI drawn over it before the swap. */
void present_game(Window& w, int screen, const UiSettings& s) {
    /* THE TOP SCREEN MAY BE WIDER THAN 256. nds_gpu2d_adaptive_framebuffer
     * returns the widened surface when the 3D raster has been widened, and the
     * native one otherwise - so this is safe to call unconditionally, and it
     * reports the width it actually produced rather than the width we asked
     * for. Trusting the requested width would tear the picture the first time
     * the renderer declined. */
    uint16_t src_w = kScreenWidth;
    const uint32_t* pixels = (screen == 0)
        ? nds_gpu2d_adaptive_framebuffer(screen, &src_w)
        : nds_gpu2d_framebuffer(screen);
    if (screen != 0) src_w = kScreenWidth;

    /* THE FORK'S MENUS GO INTO THE FRAME, not over it. Doing it here means they
     * are scaled, filtered and letterboxed by the same path as the game's own
     * picture, on both present backends - see gamemenu_render.h for why that
     * matters and why imgui was the wrong tool. Returns `pixels` unchanged when
     * no menu is open. */
    /* THE ARENA MATCH HUD goes in FIRST, then the menus over it. The HUD is part
     * of the game's picture; a menu is something on top of the picture, and a
     * menu that a portrait could cover would be unreadable exactly when it
     * mattered. */
    /* ALL THREE DRAW INTO ONE LAYER, not into copies of the frame
     * (topoverlay.h): the OpenGL presenter shows its own GPU picture whenever
     * it has one and never looks at `pixels`, so a copy was invisible there.
     * `frame` keeps the game's picture without the layer, for that presenter's
     * fallback - the layer goes over it separately, below, and must not be in
     * it twice. */
    const uint32_t* frame = pixels;
    const uint32_t* layer = nullptr;
    int layer_w = 0;
    const uint32_t* hud_layer = nullptr;
    int hud_w = 0, hud_h = 0;
    if (screen == 0) {
        uint32_t* draw = lsw2_top_overlay_begin(int(src_w));
        /* THE WINDOW'S COLUMNS OF THE FRAME, before anything anchors to them
         * (topoverlay.h): a widened frame runs off the sides of a narrower
         * window, and the HUD's corners have to be the window's. The same
         * rectangle the present below computes. */
        {
            int ww = 0, wh = 0;
            if (lsw2_gl_active()) SDL_GL_GetDrawableSize(w.window, &ww, &wh);
            else if (w.renderer) SDL_GetRendererOutputSize(w.renderer, &ww, &wh);
            int x0 = 0, x1 = int(src_w);
            const SDL_Rect v = viewport_for(ww, wh, s, int(src_w));
            if (ww > 0 && v.w > 0) {
                x0 = std::max(0, int(std::ceil(double(-v.x) * src_w / v.w)));
                x1 = std::min(int(src_w),
                              int(std::floor(double(ww - v.x) * src_w / v.w)));
            }
            lsw2_top_overlay_set_visible(x0, x1);
        }
        /* The arena match HUD, in its own layer at twice the frame's resolution
         * (topoverlay.h), under everything below. */
        lsw2_arena_hud_composite(lsw2_top_overlay_hud_begin(int(src_w)),
                                 int(src_w) * kLsw2HudLayerScale);
        /* The online lobby's countdown, over the HUD: both are part of the picture,
         * and the countdown is the one that must never be covered. */
        lsw2_lobby_countdown_composite(draw, int(src_w));
        /* The online lobby's join and disconnect popups, over the picture like the game's own
         * message box - see popuphud.h for why the fork draws it. */
        lsw2_popup_composite(draw, int(src_w));
        layer = lsw2_top_overlay_layer(&layer_w);
        hud_layer = lsw2_top_overlay_hud_layer(&hud_w, &hud_h);
        pixels = lsw2_top_overlay_merge(pixels, int(src_w));
    }

    /* THE FORK'S MENUS GO ON THE TOUCH SCREEN, not the top screen.
     *
     * They were composited onto screen 0 with the HUD, and that was wrong about
     * the requirement rather than about the code. LSW2 DS puts its menus on the
     * bottom screen: the arena lobby's Host/Join rows, the pause menu, the
     * Party Members and Free Play tabs are all down there, and the top screen
     * is the world. A settings menu drawn over the top screen is not "the
     * game's own presentation" - it is an overlay wearing the game's font,
     * which is the thing the fork's menus are meant not to be.
     *
     * THE HUD STAYS ON SCREEN 0 and that is not an inconsistency: the match
     * HUD's design says so in as many words - the mirrored portraits and radar go on the top screen
     * precisely BECAUSE the touch screen is hidden during gameplay. The two
     * requirements point in opposite directions on purpose, so the composites
     * are split rather than moved together. */
    if (screen == 1) {
        pixels = lsw2_menu_composite(pixels, int(src_w),
                                     lsw2_menuhost_current());
    }
    /* EVERYTHING THE PORT DRAWS IS IN `pixels` BY NOW, and nothing writes it
     * back into the framework's framebuffer - so this is the only place a
     * capture of what the player actually sees can be taken. Silent unless
     * LSW2_PRESENT_SHOT is set. See presentshot.h. */
    lsw2_presentshot_frame(screen, pixels, int(src_w));

    watch_for_blank(screen, pixels);

    /* THE OPENGL PATH PRESENTS THE TOP SCREEN ITSELF and there is no
     * SDL_Renderer on that window, so everything below - texture upload,
     * scale mode, RenderCopy - has nothing to act on. The letterbox rectangle
     * is computed the same way for both paths, so the picture is placed
     * identically whichever one is running. */
    if (screen == 0 && lsw2_gl_active()) {
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(w.window, &dw, &dh);
        const SDL_Rect dst = viewport_for(dw, dh, s, int(src_w));
        /* No swap here: the UI is drawn over this frame first. */
        lsw2_gl_present_top(frame, int(src_w), dst);
        if (hud_layer) lsw2_gl_present_overlay(hud_layer, hud_w, hud_h, dst, 1);
        if (layer) lsw2_gl_present_overlay(layer, layer_w, kScreenHeight, dst, 0);
        lsw2_gl_presentshot(dst);
        return;
    }

    /* The texture is sized to the native screen; a widened frame needs a
     * bigger one. Recreate only when the width actually changes. */
    if (src_w != w.tex_width) {
        if (w.texture) SDL_DestroyTexture(w.texture);
        w.texture = SDL_CreateTexture(w.renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      src_w, kScreenHeight);
        w.tex_width = w.texture ? src_w : kScreenWidth;
    }
    if (pixels && w.texture) {
        SDL_UpdateTexture(w.texture, nullptr, pixels,
                          int(src_w) * static_cast<int>(sizeof(uint32_t)));
    }
    SDL_SetTextureScaleMode(w.texture,
                            s.linear_filter ? SDL_ScaleModeLinear
                                            : SDL_ScaleModeNearest);
    int ww = 0, wh = 0;
    SDL_GetRendererOutputSize(w.renderer, &ww, &wh);
    const SDL_Rect dst = viewport_for(ww, wh, s, int(src_w));
    SDL_SetRenderDrawColor(w.renderer, 0, 0, 0, 255);
    SDL_RenderClear(w.renderer);
    SDL_RenderCopy(w.renderer, w.texture, nullptr, &dst);
}

/* THE FORK'S BOOT COVER. Presents a black top screen instead of whatever the boot
 * sequence is drawing, on whichever path is live.
 *
 * IT FEEDS BLACK PIXELS THROUGH THE NORMAL PRESENT PATH rather than clearing
 * the window directly. Clearing would need one implementation for SDL and
 * another for OpenGL, and the two would drift - the port has already been bitten
 * once by a present path that worked on only one renderer. A black source
 * buffer is the same code either way, and it letterboxes identically, so the
 * transition into the real frame is seamless. */
void present_fork_boot(Window& w, const UiSettings& s) {
    static const std::vector<uint32_t> black(size_t(kScreenWidth) * kScreenHeight,
                                             0xFF000000u);
    if (lsw2_gl_active()) {
        /* A REAL CLEAR, not black pixels through the presenter.
         *
         * The first version passed a black CPU buffer to lsw2_gl_present_top
         * and reported that it hid the boot. It did not: that function prefers
         * the compositor's GPU-side frame description whenever one exists, and
         * for a 3D scene like the splash one always does - so the black buffer
         * was ignored and the splash rendered underneath an overlay that said
         * "Skipping boot...". Both were plainly visible at once in play.
         *
         * The comment that version carried made it worse: it claimed feeding
         * pixels through the normal path avoided divergence between the two
         * renderers. The divergence was the whole problem. */
        lsw2_gl_clear();
        return;
    }
    SDL_SetRenderDrawColor(w.renderer, 0, 0, 0, 255);
    SDL_RenderClear(w.renderer);
}

/* THE EFFECTIVE STYLUS, SHADOWED.
 *
 * `nds_set_touch` is called from several event handlers, so by the time the
 * frame publishes there is no one variable holding what the guest will see.
 * inputlog.h needs exactly that value - a recording of "where the mouse was"
 * would replay differently in a different window, and the whole point is to
 * record what the DS saw. So every set in this file goes through here.
 *
 * The shadow is also what a replay writes back into, which is why it is a
 * plain struct rather than a call: the replayed frame must overwrite whatever
 * the live mouse did this frame, and it can only do that if there is one
 * place holding it. */
Lsw2InputFrame g_touch_now;

/* Was the stylus down last time, so a press can be told from a drag. */
bool g_touch_was_down = false;

void set_touch(uint16_t x, uint16_t y, bool down) {
    /* A FORK MENU OWNS THE TOUCH SCREEN WHILE IT IS UP.
     *
     * The touch screen did not drive the menu; it drove the underlying
     * widgets instead. The menu had moved to the touch screen and was drawn
     * there, but every tap went straight past it into the game - so tapping a
     * row paged the Free Play select behind it.
     *
     * THIS IS THE ONE FUNNEL. `nds_set_touch` is called from several event
     * handlers and they all come through here, which is why the shadow exists;
     * that makes it the only place the swallow has to be written once.
     *
     * THE GUEST SEES A STYLUS THAT IS NOT DOWN, not a stylus somewhere else.
     * Passing the coordinates through and ignoring the result would still let
     * the widget underneath highlight and scroll; the fork's menus are modal
     * the way the cartridge's are, and modal means the screen below does not
     * get the input at all. This mirrors lsw2_menuhost_guest_keys, which
     * already returns 0 for the buttons - touch was simply never given the same
     * treatment.
     *
     * ON THE PRESS, NOT THE RELEASE OR THE DRAG. A held stylus would otherwise
     * re-activate the row under it sixty times a second. */
    if (lsw2_menuhost_open_now()) {
        if (down && !g_touch_was_down) {
            const int row = lsw2_menu_row_at(int(y));
            if (row >= 0) lsw2_menuhost_touch_row(row);
        }
        g_touch_was_down = down;
        g_touch_now.touch_x = 0;
        g_touch_now.touch_y = 0;
        g_touch_now.touch_down = false;
        nds_set_touch(0, 0, false);
        return;
    }
    g_touch_was_down = down;

    g_touch_now.touch_x = down ? x : 0;
    g_touch_now.touch_y = down ? y : 0;
    g_touch_now.touch_down = down;
    nds_set_touch(g_touch_now.touch_x, g_touch_now.touch_y, down);
}

/* Map a click in the bottom window to stylus coordinates, undoing the same
 * letterboxing applied on the way out. */
void touch_from_mouse(Window& w, int mx, int my, bool down) {
    if (!down) { set_touch(0, 0, false); return; }
    int ww = 0, wh = 0;
    SDL_GetRendererOutputSize(w.renderer, &ww, &wh);
    const SDL_Rect v = viewport_for(ww, wh, g_settings);
    if (mx < v.x || my < v.y || mx >= v.x + v.w || my >= v.y + v.h) return;
    const int tx = (mx - v.x) * kScreenWidth / (v.w ? v.w : 1);
    const int ty = (my - v.y) * kScreenHeight / (v.h ? v.h : 1);
    set_touch(static_cast<uint16_t>(tx), static_cast<uint16_t>(ty), true);
}

/* A click inside the floating panel is a stylus touch too. The panel reports
 * where it drew itself, so this uses the same rectangle rather than a second
 * guess at the layout. */
bool touch_from_floating_panel(int mx, int my, bool down) {
    SDL_Rect r{};
    if (!ui_floating_touch_rect(&r)) return false;
    if (!down) { set_touch(0, 0, false); return true; }
    if (mx < r.x || my < r.y || mx >= r.x + r.w || my >= r.y + r.h) return false;
    const int tx = (mx - r.x) * kScreenWidth / (r.w ? r.w : 1);
    const int ty = (my - r.y) * kScreenHeight / (r.h ? r.h : 1);
    set_touch(static_cast<uint16_t>(tx), static_cast<uint16_t>(ty), true);
    return true;
}

/* Diagnose a stuck per-frame step: the system timestamp stopped advancing for a
 * whole watchdog window of scheduler rounds. Logs the geometry-engine state that
 * distinguishes the known cause (a GXFIFO stall the engine's Run() cannot drain
 * because it early-returns on FlushRequest/!GeometryEnabled, so CmdFIFORead - the
 * sole caller of GXFIFOUnstall - never runs) from other non-advance stalls.
 * Rate-limited: a persistent wedge re-enters the loop every frame. */
void report_scheduler_wedge(uint64_t rounds, uint64_t ts) {
    static uint64_t reported = 0;
    ++reported;
    if (reported > 8 && (reported % 600) != 0) return;
    NdsGxStateSnapshot gx{};
    nds_gpu3d_state(&gx);
    std::fprintf(stderr,
        "[shell] scheduler wedge: system timestamp frozen at %llu after %llu "
        "rounds this frame (gxfifo_stalled=%d geom=%u gxstat=0x%08X "
        "cyclecount=%d fifo=%u pipe=%u flush=%u | arm9 cyc=%llu halt=%d, "
        "arm7 cyc=%llu halt=%d) - presenting and retrying [x%llu]\n",
        static_cast<unsigned long long>(ts),
        static_cast<unsigned long long>(rounds),
        nds_gxfifo_stalled() ? 1 : 0, gx.geometry_enabled, gx.gxstat,
        gx.cycle_count, gx.fifo_level, gx.pipe_level, gx.flush_request,
        static_cast<unsigned long long>(scheduler_cpu_cycles(0)),
        scheduler_cpu_terminal_halted(0) ? 1 : 0,
        static_cast<unsigned long long>(scheduler_cpu_cycles(1)),
        scheduler_cpu_terminal_halted(1) ? 1 : 0,
        static_cast<unsigned long long>(reported));
}

}  /* namespace */

/* See vwh.h: which VBlank period the guest is in, from the frame length this
 * file owns. The raster is physical time - line = (t / 2130) mod 263, VBlank
 * starts at line 192 (io.cpp nds_tick_display) - so shifting t by the 71 lines
 * from VBlank to the end of the frame makes the index step exactly as each
 * VBlank begins, not at line 0. */
uint64_t lsw2_vblank_ordinal() {
    constexpr uint64_t kVblankToFrameEnd = 2130ull * (263ull - 192ull);
    return (scheduler_system_timestamp() + kVblankToFrameEnd) /
           kSystemCyclesPerFrame;
}

int nds_run_interactive_frontend(const NdsFrontendOptions& options) {
    (void)options;   /* stage 1 honours the defaults; policy arrives in stage 2 */

    /* Before anything else logs: from here on a stderr line costs the game
     * thread a copy into memory, not a write to a console or a file. See
     * asynclog.h for what that write was measured to cost. */
    lsw2_asynclog_start();
    lsw2_sampleprof_start();   /* LSW2_SAMPLE_PROF only; see sampleprof.h */
    /* WHICH COPY OF THE GAME THIS IS, decided before anything else can start a
     * second one (the Debug tab's mutes, instancemute.h). */
    lsw2_instance_claim();

    /* SDL_INIT_TIMER is not optional on Windows: it raises the OS timer
     * resolution to 1 ms. Without it SDL_Delay(1) sleeps a full ~15.6 ms
     * scheduler quantum, the audio throttle overshoots every frame, and the
     * loop pins at ~57 FPS with audible crackle. Learned from the framework's
     * frontend, which carries the same note. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO |
                 SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[shell] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    /* The emulation thread raised, as the framework's frontend does. Linked
     * consoles hand frames to each other several times a frame and each hand-off
     * wakes a waiting thread; at normal priority that wake-up queues behind
     * whatever else the machine is running (a browser, another instance). */
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) != 0)
        std::fprintf(stderr, "[shell] thread priority unchanged: %s\n",
                     SDL_GetError());

    UiSettings& settings = g_settings;
    /* ONE SETTINGS FILE, WHEREVER THE PORT IS STARTED FROM.
     *
     * This used to be the bare name "settings.ini", resolved against the
     * working directory - and the two launchers have different ones.
     * PlayLSW2.bat runs from build/, port/launch.sh runs from port/. So the
     * port kept two separate configurations and whichever one you had not used
     * last appeared to have forgotten your settings. The brief is explicitly
     * "shared settings and assets" between the baseline and the fork, and this
     * broke it between launch methods, never mind between modes.
     *
     * The launchers now name the file, and the fallback keeps a bare
     * double-click of the executable working. */
    /* WHICH BUILD THIS IS. The runner publishes it into the CRT environment
     * from its own argv[0]; see the LSW2_FORK patch in runner/src/main.cpp for
     * why it is _putenv_s rather than SDL_setenv, and std::getenv here rather
     * than SDL_getenv. Reader and writer must agree on which environment they
     * are talking about, and on Windows there are two.
     *
     * Absent means baseline. A shell started some other way is the baseline,
     * which is the safe answer: the fork's behaviour grants unlocks and
     * restricts the roster, and doing either by accident is worse than not
     * doing it at all. */
    {
        const char* fork_env = std::getenv("LSW2_FORK");
        ui_set_fork_build(fork_env && *fork_env == '1');
    }

    /* WHICH ENVIRONMENT AN INHERITED VARIABLE LANDS IN, said once, because two
     * test instruments went silent for an afternoon and the log could not tell
     * "the variable did not arrive" from "the code never ran".
     *
     * LSW2_FORK works and is set in-process with _putenv_s. Everything the port
     * is TOLD from outside - LSW2_TEST_CONSOLE, LSW2_PRESENT_SHOT - is read
     * with std::getenv, and if the CRT copy is not the store a parent process
     * writes into then none of them can ever arrive. This prints both readers
     * for one variable and settles it. */
    {
        const char* crt = std::getenv("LSW2_PRESENT_SHOT");
        const char* win = SDL_getenv("LSW2_PRESENT_SHOT");
        std::fprintf(stderr, "[env] LSW2_PRESENT_SHOT: getenv=%s SDL_getenv=%s\n",
                     crt ? crt : "(null)", win ? win : "(null)");
    }

    const char* settings_path = SDL_getenv("LSW2_SETTINGS");
    if (!settings_path || !*settings_path) settings_path = "settings.ini";
    ui_load_settings(settings, settings_path);

    /* THE GAME'S OWN FONT, without which the fork's in-game menus draw nothing.
     *
     * WHY THIS LINE EXISTS. `port/build.sh` exports font_18 and installs it to
     * `assets/fonts/` beside the executable, with a comment saying the port
     * needs it there "or its menus fall back to nothing" - and until now
     * nothing in the shell ever opened it. `lsw2_font_ready()` was therefore
     * false for the whole run, and both text paths in gamemenu_render.cpp
     * (`draw_text` at :58 and the row builder at :159) return early on it, as
     * does lobbyhud.cpp:40. Every fork menu was drawn as its background and
     * highlight with no labels on it. Same shape as the menu-event bug: the
     * loader was written, tested and delivered, and unreached.
     *
     * BESIDE THE EXECUTABLE, NOT THE WORKING DIRECTORY. build.sh copies the
     * font next to the exe and PlayLSW2.bat is double-clicked from anywhere,
     * so a relative path would resolve against whatever cwd Explorer chose.
     * SDL_GetBasePath is the exe's own directory. The cwd-relative path is
     * still tried second, because port/launch.sh runs from port/ with the
     * binary elsewhere.
     *
     * LSW2_FONT OVERRIDES BOTH, matching the LSW2_SETTINGS convention above -
     * one env var, read the same way, so a build under test can point at an
     * export that has not been installed yet.
     *
     * A MISSING FONT IS NOT FATAL, which is gamefont.h's own stated contract:
     * it says so and keeps running. But it now says so HERE as well, because
     * "the menus have no text" is otherwise indistinguishable from a menu bug,
     * and that ambiguity is what let this survive.
     *
     * font_13 AND NOT font_18 - at font_18 the fork's menu rows were too big
     * to fit on-screen. font_18 was chosen by matching six PAUSE MENU row widths
     * to the pixel, and that matching is sound - for the pause menu, whose
     * labels are "Resume", "Settings", "Extras". The fork's own menus are not
     * that menu: "Visible to Master: <YES>" and "Controls: <ENHANCED>" are two
     * and three times as long, and at font_18's advance they run to the edge of
     * a 256-pixel screen and past it.
     *
     * The right face is the one the game uses for ITS long rows - the arena
     * lobby's own "Host Multi-Card Game" fits on the touch screen, so the
     * cartridge is not using font_18 there either. font_13 is the same
     * archive's face, so this is still the game's presentation and not a
     * substitution; it is picking the size the game picks for this kind of row.
     *
     * That width measurement is not invalidated - it answered "which face
     * is the pause menu", and that was read as "which face is the game". */
    {
        const char* font_env = SDL_getenv("LSW2_FONT");
        bool loaded = false;
        if (font_env && *font_env) {
            loaded = lsw2_font_load(font_env);
        } else {
            if (char* base = SDL_GetBasePath()) {
                std::string p = std::string(base) +
                                "assets/fonts/font_13.lsw2font";
                SDL_free(base);
                loaded = lsw2_font_load(p.c_str());
            }
            if (!loaded)
                loaded = lsw2_font_load("assets/fonts/font_13.lsw2font");
        }
        if (!loaded) {
            std::fprintf(stderr,
                "[shell] the game font did not load - in-game menus will draw "
                "their frames with no text on them. Run port/build.sh to "
                "export it, or set LSW2_FONT to a .lsw2font file.\n");
        }
    }

    /* BEFORE ANY WINDOW, so the checklist is the first thing in the log rather
     * than buried under SDL and renderer chatter. Silent unless LSW2_DIAG is
     * set. */
    lsw2_diag_announce();

    /* THE RENDERER IS DECIDED BEFORE ANY WINDOW EXISTS, because the answer
     * changes how the window is created: SDL_WINDOW_OPENGL cannot be added
     * afterwards. */
    Lsw2GlOptions gl = lsw2_gl_requested({settings.opengl_renderer,
                                          settings.internal_resolution,
                                          settings.texture_upscale});

    Window top{}, bottom{};
    if (!open_window(top, "LEGO Star Wars II - Top", 0,
                     lsw2_gl_window_flags(gl)) ||
        !open_window(bottom, "LEGO Star Wars II - Touch", 1)) {
        std::fprintf(stderr, "[shell] window creation failed: %s\n", SDL_GetError());
        close_window(top); close_window(bottom); SDL_Quit();
        return 1;
    }

    /* FALLING BACK TO SOFTWARE MEANS REBUILDING THE WINDOW, not just setting a
     * flag. The OpenGL window has no SDL_Renderer and the flag cannot be
     * removed in place, so a failed start leaves a window nothing can draw
     * into. Destroy it and open a plain one - the player gets the software
     * picture and a line saying why, rather than a black rectangle. */
    /* THE RASTER WIDTH HAS TO BE SETTLED BEFORE THE COMPUTE RENDERER EXISTS.
     * It is baked into every shader and framebuffer, and the framework refuses
     * to change it under an accelerated renderer - so a widescreen request made
     * after startup would be silently declined on every frame. Establish it
     * here, while the software rasteriser is still the one answering, then lock
     * it so nothing retries. Under OpenGL the widescreen width therefore
     * follows the window as it was at launch, not as it is resized. */
    if (gl.enabled) {
        /* USE THE FULL ALLOWED WIDTH, not the width this particular window
         * shape wants. The window is 4:3 when it opens, so asking it what it
         * wants would answer "256" and lock widescreen off for the session -
         * the setting would appear to do nothing until the player restarted at
         * exactly the right size, which is not a thing anyone would work out.
         *
         * Sizing to the ceiling instead means the raster is as wide as the
         * player allowed, and viewport_for letterboxes it into whatever shape
         * the window happens to be. A wide window fills; a 4:3 window gets
         * bars. Both are predictable, neither is silent. */
        /* SIZE IT TO THE DISPLAY, not to the window and not to the ceiling.
         *
         * The window is 4:3 when it opens, so asking what IT wants answers 256
         * and locks widescreen off for the session. Using the ceiling instead
         * gives 448 - 2.33:1, wider than almost any monitor - so a 16:9 window
         * gets bars for the rest of the session.
         *
         * The display's own shape is the best available guess at what the
         * player will end up looking at, because maximising or going fullscreen
         * lands exactly there. It is a guess, which is why the width is
         * reported in the UI rather than left to be discovered. */
        SDL_DisplayMode mode{};
        int aspect_w = kScreenWidth + settings.max_extra_width;
        int aspect_h = kScreenHeight;
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(top.window),
                                      &mode) == 0 && mode.w > 0 && mode.h > 0) {
            aspect_w = mode.w;
            aspect_h = mode.h;
        }
        /* IN WHOLE 64-PX COLUMNS, which is all the compute renderer takes -
         * the display's own 16:9 asks for 340 and stopped it from starting
         * at all (widescreen.cpp, kComputeWidthStep). */
        lsw2_widescreen_set_accelerated(true);
        lsw2_widescreen_apply(settings.adaptive_widescreen,
                              aspect_w, aspect_h, settings.max_extra_width);
        lsw2_widescreen_lock(true);
    }

    if (gl.enabled && !lsw2_gl_start(top.window, gl)) {
        lsw2_widescreen_lock(false);
        lsw2_widescreen_set_accelerated(false);
        gl.enabled = false;
        /* CRT env, not just Win32 - see set_env in glpresent.cpp. */
        _putenv_s("NDS_3D_RENDERER", "soft");
        close_window(top);
        if (!open_window(top, "LEGO Star Wars II - Top", 0)) {
            std::fprintf(stderr,
                         "[shell] could not reopen the top window on the "
                         "software path: %s\n", SDL_GetError());
            close_window(bottom); SDL_Quit();
            return 1;
        }
    }

    /* THE FLOATING PANEL NEEDS ITS OWN TEXTURE. An SDL_Texture belongs to the
     * renderer that created it and cannot be drawn by another, so handing
     * imgui (which draws on the TOP renderer) the bottom window's texture
     * produced a panel that was correctly placed, correctly sized, and
     * entirely black. Mirror the bottom framebuffer into a texture the top
     * renderer owns. */
    SDL_Texture* touch_mirror = nullptr;
    if (top.renderer) {
        touch_mirror = SDL_CreateTexture(
            top.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            kScreenWidth, kScreenHeight);
        if (!touch_mirror) {
            std::fprintf(stderr,
                         "[shell] could not create the touch mirror: %s\n",
                         SDL_GetError());
        }
    }
    /* No mirror under OpenGL, and none needed: there is no SDL_Renderer
     * on the top window, so the floating panel is fed a GL texture
     * instead. See lsw2_gl_touch_texture. */

    if (!ui_init(top.window, top.renderer)) {
        std::fprintf(stderr, "[shell] imgui init failed - continuing without "
                             "the settings UI\n");
    }

    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    }
    std::fprintf(stderr,
        "[shell] our frontend. ESC or F1 = settings | arrows=D-pad Z=A X=B "
        "A=Y S=X Q=L W=R Enter=Start Backspace=Select | click the touch "
        "window | close the window to quit%s\n",
        pad ? " | gamepad connected" : "");

    /* Audio. A mismatched format is refused rather than accepted and resampled
     * badly - silence with a clear reason beats wrong pitch with none. */
    static AudioQueue audio_queue;
    SDL_AudioSpec want{}, got{};
    want.freq = kAudioFrequency;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = &audio_queue;
    /* LSW2_NO_AUDIO: don't open a device at all, so pump_audio takes its
     * wall-clock pacing path (the !device branch). Two instances on one machine
     * fight for the default device - the second can get one that never drains,
     * which used to hang the loop (now capped) and, free-running, races the WH
     * handshake into a datasharing error. A secondary/test instance wants no
     * audio anyway (two copies would echo). Opt-in; a normal single launch
     * still opens audio and is paced by it. */
    const char* no_audio = std::getenv("LSW2_NO_AUDIO");
    SDL_AudioDeviceID audio = (no_audio && *no_audio == '1')
        ? 0 : SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (no_audio && *no_audio == '1')
        std::fprintf(stderr, "[shell] LSW2_NO_AUDIO: no device; pacing on wall clock\n");
    if (audio && (got.freq != want.freq || got.format != want.format ||
                  got.channels != want.channels)) {
        std::fprintf(stderr,
            "[shell] refusing mismatched audio format: "
            "want %d/%u/%u got %d/%u/%u\n",
            want.freq, want.format, want.channels,
            got.freq, got.format, got.channels);
        SDL_CloseAudioDevice(audio);
        audio = 0;
    }
    if (audio) {
        SDL_PauseAudioDevice(audio, 0);
        audio_queue.started.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[shell] audio %d Hz stereo\n", got.freq);
    } else {
        std::fprintf(stderr, "[shell] no audio device - running silent\n");
    }

    bool mouse_down = false;
    bool running = true;
    uint64_t frames = 0;

    /* Live FPS in the title bar, the way the framework's frontend does it. It
     * is also the cheapest possible check that the throttle works: an untimed
     * loop shows thousands here, and the first version of this shell was doing
     * exactly that without any visible sign. */
    uint64_t fps_frames = 0;
    uint32_t fps_last_ms = SDL_GetTicks();
    double last_fps = 0.0;
    char title_top[96], title_bottom[96];

    /* THE FORK'S OWN WIRELESS RELAY. Off unless LSW2_RELAY is set, so
     * an ordinary launch binds nothing; when set it stands up the TCP link to
     * the peer and carries the cartridge's loopback radio across it, replacing
     * the external python bridge that first proved the approach. Serviced once a
     * frame beside debug_pump(). */
    lsw2_relay_session_start_from_env();

    while (running && !g_exit_requested) {
        const uint64_t perf_loop_start = SDL_GetPerformanceCounter();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            /* Escape and F1 toggle the menu, checked before imgui so they
             * always work - including to close a menu that currently has
             * keyboard focus. */
            if (e.type == SDL_KEYDOWN &&
                (e.key.keysym.scancode == SDL_SCANCODE_F1 ||
                 e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) &&
                !ui_capturing_binding()) {
                ui_toggle();
                continue;
            }
            /* imgui gets first refusal. When it wants the event the game must
             * not also see it, or typing in a field would drive the guest. */
            if (ui_handle_event(e)) continue;
            switch (e.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN:
                    /* A keypress meant for the rebind prompt must not also be
                     * gameplay input, or binding "A" would press A on the way
                     * in. */
                    if (ui_capturing_binding()) {
                        ui_offer_captured_key(e.key.keysym.scancode);
                        break;
                    }
                    /* ESCAPE OPENS THE MENU, it does not quit. Quitting on the
                     * key people press to look at options is a hostile default:
                     * the first thing anyone does with a new build is hit Escape
                     * expecting a menu, and losing the session for it is a poor
                     * introduction. Close the window to quit. */
                    if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                        e.key.keysym.scancode == SDL_SCANCODE_F1) {
                        ui_toggle();
                        break;
                    }
                    g_keys_host |= key_bit(e.key.keysym.scancode);
                    break;
                case SDL_KEYUP:
                    g_keys_host &= static_cast<uint16_t>(~key_bit(e.key.keysym.scancode));
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                    if (ui_capturing_binding()) {
                        ui_offer_captured_pad(e.cbutton.button);
                        break;
                    }
                    g_keys_pad |= controller_bit(
                        static_cast<SDL_GameControllerButton>(e.cbutton.button));
                    break;
                case SDL_CONTROLLERBUTTONUP:
                    g_keys_pad &= static_cast<uint16_t>(~controller_bit(
                        static_cast<SDL_GameControllerButton>(e.cbutton.button)));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.button != SDL_BUTTON_LEFT) break;
                    if (e.button.windowID == SDL_GetWindowID(top.window)) {
                        if (touch_from_floating_panel(e.button.x, e.button.y, true))
                            mouse_down = true;
                    } else if (e.button.windowID == SDL_GetWindowID(bottom.window)) {
                        mouse_down = true;
                        touch_from_mouse(bottom, e.button.x, e.button.y, true);
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        mouse_down = false;
                        set_touch(0, 0, false);
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (!mouse_down) break;
                    if (e.motion.windowID == SDL_GetWindowID(top.window))
                        touch_from_floating_panel(e.motion.x, e.motion.y, true);
                    else if (e.motion.windowID == SDL_GetWindowID(bottom.window))
                        touch_from_mouse(bottom, e.motion.x, e.motion.y, true);
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
                    /* REMEMBER THE SIZE THE PLAYER CHOSE. SIZE_CHANGED rather
                     * than RESIZED: the latter fires only for user drags, and
                     * misses a maximise or a size set by the window manager,
                     * which are exactly the cases worth remembering. */
                    else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        const Uint32 id = e.window.windowID;
                        if (top.window && id == SDL_GetWindowID(top.window)) {
                            settings.window_w = e.window.data1;
                            settings.window_h = e.window.data2;
                        } else if (bottom.window &&
                                   id == SDL_GetWindowID(bottom.window)) {
                            settings.touch_window_w = e.window.data1;
                            settings.touch_window_h = e.window.data2;
                        }
                    }
                    break;
                default: break;
            }
        }

        /* The stick is POLLED, not evented. SDL does send axis-motion events,
         * but a stick held still produces none - so a purely event-driven
         * reading would drop the direction the moment the player stopped
         * pushing further. Sample it once per frame instead. */
        sample_stick(pad, settings);
        sample_camera(pad);

        /* Active low: a set bit means NOT pressed. Injected input is OR-ed in
         * so a script and a human can share one session. */
        /* The fork's auto-press joins the other sources rather than replacing
         * them, so it cannot lock the player out if it is still running when
         * they start playing - and it returns 0 the moment the fork arrives. */
        /* THE MENU GETS FIRST REFUSAL ON INPUT. It needs newly-pressed keys and
         * the guest needs to see none of them while it is open, so both come
         * from one place rather than two callers keeping two copies of the
         * previous mask. */
        const uint16_t held = static_cast<uint16_t>(
            g_keys_host | g_keys_pad | g_keys_stick | g_keys_injected |
            lsw2_score_auto_keys() |
            lsw2_fork_boot_keys() |
            lsw2_wh_join_answer_keys() |
            lsw2_wh_browse_pick_keys());
        lsw2_menuhost_feed_keys(held);

        /* AND DELIVER WHAT IT PRODUCED. Without this the menu host stepped the
         * cursor and threw the result away - see lsw2_menuhost_take_event for
         * what that cost. Cancelled is delivered too: the host closes on it and
         * the session's own handler backs out one screen and reopens, which is
         * what makes B go up a level rather than quit everything. */
        if (const Lsw2MenuEvent ev = lsw2_menuhost_take_event();
            ev != Lsw2MenuEvent::None)
            lsw2_fork_session().on_menu_event(ev);

        /* RECORD OR REPLAY, at the one point where the frame's input is
         * finally decided and before the guest is given it.
         *
         * The keys recorded are what the MENU HOST lets through, not what the
         * keyboard did - a replay has to reproduce what the game received, and
         * the two differ whenever a fork menu is open. The touch comes from
         * the shadow above for the same reason: it is what the DS saw, in DS
         * pixels, so a session recorded in a 4x window replays in a 1x one.
         *
         * `frames` IS the guest frame index here, not merely a host counter -
         * the loop below advances exactly one frame's worth of system time per
         * iteration, deliberately, so the two cannot drift. See inputlog.h. */
        Lsw2InputFrame input;
        input.keys = lsw2_menuhost_guest_keys(held);
        input.touch_x = g_touch_now.touch_x;
        input.touch_y = g_touch_now.touch_y;
        input.touch_down = g_touch_now.touch_down;

        lsw2_inputlog_frame(input, frames);

        /* SAY WHEN A REPLAY RUNS OUT, once. The run deliberately KEEPS GOING
         * with the last recorded state held - a replay that ended by closing
         * the window would take the screen it was made to reach away with it,
         * which is the opposite of the point. */
        if (lsw2_inputlog_replay_finished()) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr, "[input] replay finished at frame %llu - "
                             "holding the last state; input is yours again\n",
                             static_cast<unsigned long long>(frames));
            }
        }

        if (lsw2_inputlog_mode() == Lsw2InputMode::Replay) {
            /* A replay OVERWRITES the live devices - otherwise a stray mouse
             * move during playback would fight the recording, and the run
             * would not be the run that was recorded. */
            set_touch(input.touch_x, input.touch_y, input.touch_down);
        }

        /* THE FREE PLAY PICKERS (freeplaypick.h) see the keys AFTER the log,
         * so a recording keeps the player's own A and B and a replay drives
         * the picker the same way the player did. */
        nds_set_key_mask(static_cast<uint16_t>(
            0x0FFFu & ~lsw2_freeplaypick_filter_keys(input.keys)));
        if (lsw2_inputlog_mode() != Lsw2InputMode::Replay &&
            g_touch_injected)
            set_touch(g_touch_x, g_touch_y, true);
        /* AND THE TAP THE PRE-GAME PICK MAKES FOR A: the highlighted portrait
         * tapped a second time, the widget's own confirm. */
        {
            uint16_t tx = 0, ty = 0;
            const int tap = lsw2_freeplaypick_touch(&tx, &ty);
            if (tap == 1) set_touch(tx, ty, true);
            else if (tap == 2) set_touch(0, 0, false);
        }

        /* Advance exactly one frame's worth of system time. Copied in shape
         * from the framework's loop so the guest tick is preserved rather than
         * free-running - which is what makes 60fps mean the same thing here as
         * it does on hardware. */
        const uint64_t now = scheduler_system_timestamp();
        const uint64_t next = (now / kSystemCyclesPerFrame + 1u) * kSystemCyclesPerFrame;
        /* NON-ADVANCE WATCHDOG. The framework's own scheduler_run() bails after a
         * long run of rounds in which the ARM9 timestamp never moves (see
         * scheduler.cpp, "ARM9 stalled"). This present loop REPLACED
         * scheduler_run() and, until now, dropped that guard - so any condition
         * that freezes scheduler_system_timestamp() spun this loop forever: a hard
         * freeze with no window, no debug surface, no recovery. The known trigger
         * is a GXFIFO stall the geometry engine cannot drain (its Run()
         * early-returns on FlushRequest/!GeometryEnabled while CmdStallQueue still
         * holds entries, and CmdFIFORead - the only caller of GXFIFOUnstall -
         * never runs), which pins the ARM9 GXFIFO-stalled at a fixed timestamp.
         * Bounding the spin turns the freeze into a recoverable hitch: present the
         * last frame, service the debug surface and fork tick, retry next frame.
         * kMaxStallRounds is ~7x a full healthy frame (kSystemCyclesPerFrame /
         * kIterCap ~= 8.7k rounds) so no real frame reaches it; the counter only
         * climbs on CONSECUTIVE non-advancing rounds, so ordinary brief GXFIFO
         * stalls (a handful of rounds) never trip it. */
        constexpr uint64_t kMaxStallRounds = 65536u;
        uint64_t rounds = 0;
        uint64_t last_ts = now;
        uint64_t stall = 0;
        const uint64_t perf_emu_start = SDL_GetPerformanceCounter();
        g_perf_input += perf_emu_start - perf_loop_start;
        frame_trace("emu");
        while (running && scheduler_system_timestamp() < next &&
               !(scheduler_cpu_terminal_halted(0) &&
                 scheduler_cpu_terminal_halted(1))) {
            scheduler_run_round();
            ++rounds;
            const uint64_t ts = scheduler_system_timestamp();
            if (ts == last_ts) {
                if (++stall >= kMaxStallRounds) {
                    report_scheduler_wedge(rounds, ts);
                    break;
                }
            } else {
                stall = 0;
                last_ts = ts;
            }
        }
        g_perf_emu += SDL_GetPerformanceCounter() - perf_emu_start;
        frame_trace("emudone");

        if (nds_powered_off()) {
            std::fprintf(stderr, "[shell] guest powered off; closing\n");
            break;
        }

        /* SERVICE THE DEBUG SURFACE HERE, between frames. Most of its commands
         * - framebuffer capture, register and memory reads, input injection -
         * are answered on this thread at this safe point, so a shell that never
         * calls debug_pump() answers `ping` from the network thread and hangs on
         * everything else. That is exactly what ours did: ping returned, and
         * framebuffer timed out with zero bytes. */
        const uint64_t perf_between_start = SDL_GetPerformanceCounter();
        debug_pump();

        /* Carry the DS radio across the network if a relay session is up
         * (LSW2_RELAY); a no-op otherwise. Kept next to debug_pump so it is
         * serviced every frame at the same safe point. */
        lsw2_relay_session_service();

        /* THE FORK'S START. Runs after the guest frame and before anything is shown,
         * so the fork's warp request is made at the same safe point the cheats
         * tab makes one - between frames, with the guest settled. Does nothing
         * at all in the baseline. */
        const uint64_t perf_fork_start = SDL_GetPerformanceCounter();
        lsw2_fork_tick();
        lsw2_instance_mute_tick();
        const uint64_t perf_ui_start = SDL_GetPerformanceCounter();
        g_perf_debug += perf_fork_start - perf_between_start;
        g_perf_fork += perf_ui_start - perf_fork_start;

        ui_begin_frame();
        ui_set_stats(last_fps, frames, audio ?
                     audio_queue.underruns.load(std::memory_order_relaxed) : 0);
        /* THE FLOATING PANEL'S SOURCE, whichever present path is running. The
         * pixels are the same either way - only the object holding them
         * differs, because imgui has to be handed something its live backend
         * can sample. */
        /* HIDE THE BOTTOM SCREEN TOO WHILE THE FORK IS GETTING THERE.
         *
         * The first version covered only the top, and the result was visible:
         * the touch screen still displayed the splash copyright text. The
         * boot draws on both screens, so hiding one hides half a splash. */
        const uint32_t* touch_px = lsw2_fork_hiding_boot()
                                 ? nullptr : nds_gpu2d_framebuffer(1);

        /* AND THE FORK'S MENUS GO INTO IT TOO. In the deployed executable, the
         * touch screen was blank aside from the starfield while the player
         * interacted with the arena console.
         *
         * This line used to hand imgui the RAW guest framebuffer, while
         * lsw2_menu_composite() - which draws every menu this fork adds - ran
         * only in present_game() for screen 1, the SEPARATE BOTTOM WINDOW. So
         * the menus existed in one layout and could not appear in the other,
         * and the floating panel is the one the deployed build uses.
         *
         * THE SCREENSHOTS SAID EXACTLY THIS and all of them agree: the panel
         * showed the Free Play select and the Customize screen, because those
         * are drawn by the GUEST and are in this framebuffer - and showed bare
         * stars for the arena menu, because menubackdrop's suppression is
         * guest-side and lands while the menu rows are host-side and did not.
         *
         * WHY IT SURVIVED SO LONG, worth writing down: testing used
         * --screen-layout separate, and LSW2_PRESENT_SHOT hooks present_game -
         * the same path that layout uses. The instrument could only ever agree
         * with the test setup, so it "confirmed" a menu the player could not see.
         * A capture wired into one of two present paths says nothing about the
         * other, and the earlier sighting of an invisible menu was right.
         *
         * SAME CALL, SAME ARGUMENTS as the bottom window's, so the two layouts
         * cannot drift apart again. Cheap: lsw2_menu_composite returns its
         * input untouched when no menu is open. */
        touch_px = lsw2_menu_composite(touch_px, kScreenWidth,
                                       lsw2_menuhost_current());

        /* AND THE CAPTURE FOLLOWS THIS PATH TOO, for the same reason the bug
         * existed. With floating_touch_only there is no bottom window, so
         * present_game() never runs for screen 1 and LSW2_PRESENT_SHOT would
         * have nothing to say about the very layout that was broken. Writing
         * the panel's own source here means the instrument covers both present
         * paths instead of quietly covering one. */
        lsw2_presentshot_frame(1, touch_px, kScreenWidth);
        if (lsw2_gl_active()) {
            ui_set_touch_texture(
                lsw2_gl_touch_texture(touch_px, settings.linear_filter));
        } else if (touch_mirror) {
            if (touch_px) {
                SDL_UpdateTexture(touch_mirror, nullptr, touch_px,
                                  kScreenWidth * int(sizeof(uint32_t)));
            }
            SDL_SetTextureScaleMode(touch_mirror,
                                    settings.linear_filter ? SDL_ScaleModeLinear
                                                           : SDL_ScaleModeNearest);
            ui_set_touch_texture(touch_mirror);
        } else {
            ui_set_touch_texture(nullptr);
        }
        ui_draw(settings);

        const uint64_t perf_audio_start = SDL_GetPerformanceCounter();
        g_perf_ui += perf_audio_start - perf_ui_start;
        frame_trace("audio");
        pump_audio(audio, audio_queue);
        g_perf_audio += SDL_GetPerformanceCounter() - perf_audio_start;
        frame_trace("present");

        /* A preset is a request, applied here where the windows are owned.
         * Cleared immediately so it fires once rather than fighting the player
         * every frame if they then drag the window. */
        if (settings.applied_scale > 0) {
            /* THE GAMEPLAY WINDOW ONLY. Resizing the touch window to match was
             * wrong: they are separate windows the player positions
             * independently, and a scale preset chosen to make the game bigger
             * should not also move and resize the panel they had set up how
             * they wanted it. */
            SDL_SetWindowSize(top.window,
                              kScreenWidth * settings.applied_scale,
                              kScreenHeight * settings.applied_scale);
            settings.applied_scale = 0;
        }

        {
            int tw = 0, th = 0;
            SDL_GetRendererOutputSize(top.renderer, &tw, &th);
            /* Order matters: the frame just rendered is what tells us whether
             * this is a world view, and apply() acts on that answer. */
            lsw2_widescreen_note_frame();
            lsw2_widescreen_apply(settings.adaptive_widescreen, tw, th,
                                  settings.max_extra_width);
        }
        /* SAME THREE STEPS ON BOTH PATHS - draw the frame, draw the UI over
         * it, then put it on screen. Only the last one differs, because under
         * OpenGL the swap belongs to the GL context rather than to a renderer
         * that does not exist. */
        /* THE FORK DOES NOT SHOW THE BOOT SEQUENCE.
         *
         * The boot still RUNS - it has to, because the warp copies arguments
         * from the level state it builds - and this suppresses only the showing
         * of it. That keeps the change on the right side of the line
         * port/ARCHITECTURE.md draws: the host layer may change what the player
         * experiences and must not change what the game computes.
         *
         * The overlay is drawn instead of the frame rather than over it,
         * because the splash is what the player must not see. */
        const uint64_t perf_present_start = SDL_GetPerformanceCounter();
        if (lsw2_fork_hiding_boot() || lsw2_fork_hiding_join())
            present_fork_boot(top, settings);
        else present_game(top, 0, settings);
        ui_render(top.renderer);
        if (lsw2_gl_active()) lsw2_gl_swap();
        else SDL_RenderPresent(top.renderer);

        /* The separate touch window can be hidden when the floating panel is
         * doing its job, so the same screen is not shown twice.
         *
         * AND WHEN THE ARENA HUD IS UP: the match HUD puts the radar on the
         * top-right and HIDES THE TOUCH SCREEN during gameplay. ui.cpp already suppresses the floating panel for this
         * reason and this window was missed, so with the HUD on and a separate
         * touch window open the player saw the portraits and the radar twice -
         * mirrored onto the top screen, and still sitting on the bottom one.
         * The overlay exists precisely to replace that view. */
        const bool want_bottom_window = !(settings.floating_touch &&
                                          settings.floating_touch_only) &&
                                        !lsw2_arena_hud_active();
        if (want_bottom_window) {
            /* PARENTHESES, and they are not cosmetic. This was
             *
             *     if (!SDL_GetWindowFlags(bottom.window) & SDL_WINDOW_SHOWN)
             *
             * where `!` binds tighter than `&`, so it read as
             * `(!flags) & SDL_WINDOW_SHOWN`. A real window has non-zero flags,
             * so `!flags` is 0 and the whole test was ALWAYS FALSE:
             * SDL_ShowWindow was unreachable and a hidden touch window could
             * never come back. Harmless only while nothing hid it - which the
             * line above now does. */
            if ((SDL_GetWindowFlags(bottom.window) & SDL_WINDOW_SHOWN) == 0)
                SDL_ShowWindow(bottom.window);
            if (lsw2_fork_hiding_boot()) {
                SDL_SetRenderDrawColor(bottom.renderer, 0, 0, 0, 255);
                SDL_RenderClear(bottom.renderer);
            } else {
                present_game(bottom, 1, settings);
            }
            SDL_RenderPresent(bottom.renderer);
        } else {
            SDL_HideWindow(bottom.window);
        }
        g_perf_present += SDL_GetPerformanceCounter() - perf_present_start;
        ++frames;
        ++fps_frames;
        counts_probe(unsigned(frames));

        const uint32_t now_ms = SDL_GetTicks();
        if (now_ms - fps_last_ms >= 1000u) {
            const double fps = fps_frames * 1000.0 / (now_ms - fps_last_ms);
            last_fps = fps;
            /* MEASUREMENT-ONLY, off unless LSW2_PERF_LOG is set. Prints the
             * per-second underrun DELTA so a one-time boot/focus burst is told
             * apart from a steady-state tick rate - the cumulative shutdown
             * total conflates the two. Harmless in a normal run. */
            static const bool perf_log = std::getenv("LSW2_PERF_LOG") != nullptr;
            if (perf_log) {
                static uint64_t last_under = 0;
                const uint64_t u = audio ?
                    audio_queue.underruns.load(std::memory_order_relaxed) : 0;
                const double ms_per_tick =
                    1000.0 / double(SDL_GetPerformanceFrequency());
                std::fprintf(stderr, "[perf] t=%ums fps=%.1f underruns=%llu (+%llu/s)"
                             " emu=%.0fms audio=%.0fms present=%.0fms input=%.0fms"
                             " debug=%.0fms fork=%.0fms ui=%.0fms\n",
                             now_ms, fps, (unsigned long long)u,
                             (unsigned long long)(u - last_under),
                             double(g_perf_emu) * ms_per_tick,
                             double(g_perf_audio) * ms_per_tick,
                             double(g_perf_present) * ms_per_tick,
                             double(g_perf_input) * ms_per_tick,
                             double(g_perf_debug) * ms_per_tick,
                             double(g_perf_fork) * ms_per_tick,
                             double(g_perf_ui) * ms_per_tick);
                last_under = u;
            }
            g_perf_emu = g_perf_audio = g_perf_present = 0;
            g_perf_input = g_perf_debug = g_perf_fork = g_perf_ui = 0;
            lsw2_sampleprof_note(lsw2_state_current(), fps);
            std::snprintf(title_top, sizeof title_top,
                          "LEGO Star Wars II - Top - %.1f FPS", fps);
            std::snprintf(title_bottom, sizeof title_bottom,
                          "LEGO Star Wars II - Touch - %.1f FPS", fps);
            SDL_SetWindowTitle(top.window, title_top);
            SDL_SetWindowTitle(bottom.window, title_bottom);
            fps_last_ms = now_ms;
            fps_frames = 0;
        }
    }

    /* WITHDRAW A HOSTED LOBBY FROM THE DIRECTORY before the sockets close, so
     * closing the game by the window (not the graceful "Shut Down Lobby") does
     * not leave a ghost row in the master for the TTL window - which a re-host
     * would then trip over. A no-op unless this process is a visible-to-master
     * host. */
    lsw2_fork_session().withdraw_from_master();

    /* Close the relay link (if one was up) before the sockets go with the
     * process, and print what it carried so a drive can read it. */
    {
        const Lsw2RelaySessionStatus rs = lsw2_relay_session_status();
        if (std::strcmp(rs.phase, "off") != 0)
            std::fprintf(stderr, "[relay] closing: phase %s forwarded %llu injected %llu\n",
                         rs.phase, rs.forwarded, rs.injected);
        lsw2_relay_session_stop();
    }

    std::fprintf(stderr, "[shell] closed after %llu frames\n",
                 static_cast<unsigned long long>(frames));

    /* WHAT THE RUN ACTUALLY DID, in one line, because a live test run is scarce.
     *
     * Every counter here already existed and every one of them was read by
     * nothing - lsw2_hle_hits and the two step counters by not even a test,
     * despite their own comment in dispatch_overrides.cpp saying they are
     * "reported rather than assumed, because 'registered' and 'reached' are
     * different claims and this project has already been burned by three
     * commands that were accepted and inert". They were not reported. An
     * override that registers and never fires looked exactly like one that
     * works, which is the failure that comment was written about.
     *
     * At shutdown rather than per frame: these are run-level facts, and the
     * line sits beside "closed after N frames" where the other one already is.
     *
     * declared here rather than in a header because dispatch_overrides.cpp has
     * none - it is reached through the override table, not by name. */
    {
        std::fprintf(stderr,
            "[shell] overrides fired %llu | data-share %llu steps, %llu stalls"
            " | strings %llu cached, %llu decoded\n",
            static_cast<unsigned long long>(lsw2_hle_hits()),
            static_cast<unsigned long long>(lsw2_hle_step_hits()),
            static_cast<unsigned long long>(lsw2_hle_step_stalls()),
            static_cast<unsigned long long>(lsw2_hle_string_cache_hits()),
            static_cast<unsigned long long>(lsw2_hle_string_cache_misses()));
    }

    /* AND THE ONE THAT MEANS SOMETHING IS WRONG. strings.h says a nonzero
     * count means "a string has appeared whose terminator rule differs from
     * ours, and the decode of it should not be trusted" - a warning that had
     * no way to reach anyone, since only the tests ever asked. Printed like
     * the audio underruns below: only when it is not zero, because that is the
     * only time it is news. */
    if (const uint64_t lead = lsw2_string_lead_bytes_seen(); lead != 0) {
        std::fprintf(stderr,
            "[shell] WARNING: %llu string lead bytes >= 0xF0 seen - a decode "
            "this run used a terminator rule the port does not model, and that "
            "string should not be trusted (see strings.h)\n",
            static_cast<unsigned long long>(lead));
    }
    if (touch_mirror) SDL_DestroyTexture(touch_mirror);
    ui_save_settings(settings, settings_path);
    ui_shutdown();
    if (audio) {
        const uint32_t under = audio_queue.underruns.load(std::memory_order_relaxed);
        if (under) std::fprintf(stderr, "[shell] audio underruns: %u\n", under);
        SDL_CloseAudioDevice(audio);
    }
    if (pad) SDL_GameControllerClose(pad);
    close_window(top);
    close_window(bottom);
    SDL_Quit();
    return 0;
}

bool nds_frontend_request_exit() { g_exit_requested = true; return true; }

/* THE REMOTE-CONTROL SURFACE. debug_server.cpp (enabled with --serve) reaches
 * input through these, so implementing them makes the game drivable and
 * observable from a script: `framebuffer` for screenshots of either screen,
 * `frontend_input` for buttons and stylus, plus read_mem / regs / run_to_pc.
 *
 * Key names match the keyboard mapping, so a caller says "z" for A and "start"
 * for Start rather than having to know DS bit positions. */
bool nds_frontend_debug_key(const char* name, bool down) {
    if (!name || !*name) return false;

    /* Shell controls, not guest buttons. Exposing F1 here means the settings UI
     * can be opened and screenshotted from a script - otherwise the only way to
     * see it is to ask a human to press a key and describe the result, which is
     * how three bugs in this shell survived longer than they should have. */
    if (SDL_strcasecmp(name, "f1") == 0 ||
        SDL_strcasecmp(name, "escape") == 0 ||
        SDL_strcasecmp(name, "esc") == 0) {
        if (down) ui_toggle();
        return true;
    }
    struct { const char* name; uint16_t bit; } table[] = {
        {"a", 1u << 0}, {"z", 1u << 0},          /* DS A      */
        {"b", 1u << 1}, {"x", 1u << 1},          /* DS B      */
        {"select", 1u << 2}, {"backspace", 1u << 2},
        {"start", 1u << 3}, {"return", 1u << 3},
        {"right", 1u << 4}, {"left", 1u << 5},
        {"up", 1u << 6},    {"down", 1u << 7},
        {"r", 1u << 8}, {"w", 1u << 8},
        {"l", 1u << 9}, {"q", 1u << 9},
        {"ds_x", 1u << 10}, {"s", 1u << 10},
        {"ds_y", 1u << 11},
    };
    for (const auto& e : table) {
        if (SDL_strcasecmp(name, e.name) != 0) continue;
        if (down) g_keys_injected |= e.bit;
        else      g_keys_injected &= static_cast<uint16_t>(~e.bit);
        ++g_debug_key_events;
        return true;
    }
    /* The enhanced controls' camera, held like a button (sample_camera). */
    const char* cams[] = {"cam_left", "cam_right", "cam_up", "cam_down"};
    for (int i = 0; i < 4; ++i) {
        if (SDL_strcasecmp(name, cams[i]) != 0) continue;
        if (down) g_camera_injected = uint8_t(g_camera_injected | (1u << i));
        else      g_camera_injected = uint8_t(g_camera_injected & ~(1u << i));
        ++g_debug_key_events;
        return true;
    }
    return false;   /* unknown key: say so rather than silently doing nothing */
}

bool nds_frontend_debug_touch(uint16_t x, uint16_t y, bool down) {
    if (down && (x >= kScreenWidth || y >= kScreenHeight)) return false;
    g_touch_injected = down;
    g_touch_x = x;
    g_touch_y = y;
    if (!down) set_touch(0, 0, false);
    ++g_debug_touch_events;
    return true;
}

/* Mouse-driven control is a convenience over the same touch path; there is no
 * separate pointer device in the guest. */
bool nds_frontend_debug_mouse_button(uint8_t button, bool down) {
    if (button != 1u) return false;             /* left button only */
    return nds_frontend_debug_touch(g_touch_x, g_touch_y, down);
}
bool nds_frontend_debug_mouse_motion(int dx, int dy) {
    const int nx = static_cast<int>(g_touch_x) + dx;
    const int ny = static_cast<int>(g_touch_y) + dy;
    g_touch_x = static_cast<uint16_t>(std::min(std::max(nx, 0), kScreenWidth - 1));
    g_touch_y = static_cast<uint16_t>(std::min(std::max(ny, 0), kScreenHeight - 1));
    if (g_touch_injected) set_touch(g_touch_x, g_touch_y, true);
    return true;
}

/* Relative-mouse capture is a windowing convenience this shell does not need:
 * injected input already bypasses the window entirely. Reported as unsupported
 * rather than accepted and ignored. */
bool nds_frontend_debug_capture_mouse() { return false; }
bool nds_frontend_debug_release_mouse() { return false; }

void nds_frontend_input_debug_state(NdsFrontendInputDebugState* out) {
    if (!out) return;
    *out = NdsFrontendInputDebugState{};
    out->active = 1;
    /* Report what the guest is actually being told, including injected input,
     * so a script can confirm its own presses landed rather than assume. */
    out->keyboard_pressed = g_keys_host;
    out->mouse_pressed = g_touch_injected ? 1u : 0u;
    out->stick_pressed = g_keys_pad;
    out->published_key_mask =
        static_cast<uint16_t>(0x0FFFu & ~(g_keys_host | g_keys_pad |
                                      g_keys_stick | g_keys_injected));

    /* THE COUNTERS THIS FUNCTION EXISTS FOR. The comment above says a script
     * should be able to confirm its own presses landed rather than assume, and
     * the two counters that answer that were incremented on every accepted
     * injection and then reported nowhere - so the surface said what the guest
     * is being told THIS INSTANT and never how many injections it had accepted.
     * The difference matters for a press that is accepted and then immediately
     * cleared: the mask is back to zero by the time anything reads it, and the
     * count is the only evidence it happened.
     *
     * The framework declares these fields; only filling them was missing. */
    out->debug_key_events = g_debug_key_events;
    out->debug_touch_events = g_debug_touch_events;

    /* WHERE THE INJECTED STYLUS IS, which is what makes a touch verifiable to
     * the pixel - the reason the record/replay format stores x and y rather
     * than a region. Held rather than momentary, so it reads correctly after
     * the press has been released. */
    out->virtual_stylus_x = int(g_touch_x);
    out->virtual_stylus_y = int(g_touch_y);
}
void nds_frontend_live_stats(NdsFrontendLiveStats* out) {
    if (out) *out = NdsFrontendLiveStats{};
}
void nds_frontend_black_band_scan(bool, bool) { }
void nds_frontend_black_band_capture(NdsFrontendBlackBandCapture* out) {
    if (out) *out = NdsFrontendBlackBandCapture{};
}
