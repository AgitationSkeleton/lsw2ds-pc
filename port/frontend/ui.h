/* The shell's UI layer.
 *
 * Everything configurable about the port lives behind this: video and viewport
 * policy, input binding, the floating touch panel, and debug/cheat tabs. It is
 * the reason our own shell exists at all — none of it can be bolted onto a
 * present loop we do not own.
 *
 * THE LAYERING RULE STILL HOLDS (port/ARCHITECTURE.md). This layer may change
 * what the player experiences and must not change what the game computes.
 * Scaling changes how a frame is presented; it must never change the projection
 * the game produced. Anything that reaches into guest state is a title patch,
 * named, gated on the ROM SHA-1, and written down — not a checkbox here.
 */

#ifndef LSW2_UI_H
#define LSW2_UI_H

#include <SDL.h>
#include <cstdint>

/* MULTIPLAYER (internet play: master server, direct IP, room codes, LAN).
 * Fork-only: the baseline is deliberately
 * vanilla-esque and has no networking, so these are read and shown only when
 * ui_fork_build() is true.
 *
 * Fixed-size buffers rather than std::string because ui_save_settings writes
 * with fprintf and imgui's InputText wants a char* and a capacity anyway.
 * Sized generously and every write is bounded. */
struct UiMultiplayer {
    /* The specified default name. */
    char player_name[24] = "Maclunkey";

    /* LOCAL BY DEFAULT, for a while: a test build talking to a master server
     * running on the same machine could not see it while the default pointed
     * elsewhere, so the default became the local IP.
     *
     * === superseded, kept because the reasoning was reasonable and wrong =====
     * | The project's own server. Left as the real host rather than empty,   |
     * | because an empty field looks like something is missing when in fact  |
     * | this is where the master server will actually live; testing on this  |
     * | machine means typing 127.0.0.1 over it, which is one edit rather     |
     * | than a lookup.                                                       |
     * ========================================================================
     *
     * "One edit rather than a lookup" was the wrong trade. A TEST build talks
     * to a master server on the same machine, so the default that costs
     * nothing is the local one and the real host is what gets typed when there
     * is a real host to reach. Shipping the remote name as the default meant
     * the directory silently found nothing, which reads as "the feature is
     * broken" rather than "the address is not where you are".
     *
     * The real host has not been lost: the settings menu still offers it, so
     * this is a change of default rather than a change of capability.
     *
     * SUPERSEDED IN TURN: the hosted directory now runs on Cloudflare at
     * lsw2server.viosarcade.xyz (services/master-server), and that is the
     * default again; 127.0.0.1 stays a button away for local testing. */
    char master_host[128] = "lsw2server.viosarcade.xyz";

    /* THE PORT IS NOT ARBITRARY, though it had to be chosen: 553 is the ROM's
     * own release number ("0553 - LEGO Star Wars II"), lifted into the dynamic
     * range. That makes it traceable to this project rather than a number
     * somebody picked, and it keeps clear of the ranges Steam and the common
     * game servers occupy - 27015 in particular would collide on any machine
     * running a Source game. The player chooses it here; this is only the
     * starting value. The hosted directory answers on plain HTTP, port 80;
     * 28553 is the self-host server's. */
    int master_port = 80;
    /* The port THIS machine listens on when hosting. Distinct from the master
     * server port: one is where the directory lives, the other is where players
     * connect to us, and conflating them is a natural mistake to make once and
     * then be confused by. */
    int listen_port = 28550;

    /* THE DIRECT-CONNECT ADDRESS, which is NOT the master server's.
     *
     * Room codes are entered in the game and the direct IP here, on the
     * Multiplayer tab. They are different addresses doing different jobs - the directory tells you
     * where lobbies are, this is one specific host - and sharing a field would
     * mean joining a friend broke lobby browsing until it was typed back.
     *
     * LOCAL BY DEFAULT, for the same reason master_host was. A
     * bounty hunter match cannot be tested at all without two instances on one
     * machine talking to each other, because the cartridge's own receive pump
     * needs more than one player, and until this had a value the Join menu's
     * direct row read "not set" and could not be used. Empty was the safe
     * default for a field nobody could guess; 127.0.0.1 is the useful one for
     * the only setup that exists today, and it is one edit to change. */
    char direct_host[128] = "127.0.0.1";
    int direct_port = 28550;
};

/* Settings the UI owns. Read by the present loop each frame; persisted to
 * port/settings.ini so a build is configured rather than recompiled. */
struct UiSettings {
    /* Video */
    bool preserve_aspect = true;     /* letterbox rather than stretch        */
    bool integer_scale = false;      /* snap to whole multiples of 256x192   */
    /* THREE, NOT TWO. At 2x the window is 512x384 and the floating touch
     * panel - which is 256x192 of it - covers a quarter of the picture, which
     * is exactly why the default is bigger. */
    int  window_scale = 3;           /* 1..6, applied on request             */
    bool linear_filter = false;      /* nearest by default - it is pixel art */
    int  applied_scale = 0;          /* a preset request: the UI sets it, the
                                        loop resizes the windows and clears it */

    /* ADAPTIVE WIDESCREEN. When on, widening the gameplay window widens the 3D
     * viewport so more of the world is visible, rather than stretching or
     * letterboxing 256x192. The HUD stays at its own aspect.
     *
     * This is the one setting that reaches past presentation: showing more
     * world means the game must stop culling what is now on screen, which is a
     * guest-side change. See widescreen.cpp - it is a named, ROM-gated title
     * patch, not a checkbox that quietly rewrites the game. */
    bool adaptive_widescreen = false;
    int  max_extra_width = 192;      /* how far past 256 the raster may go;
                                        192 is the renderer ceiling (448 total) */

    /* RENDERER AND SAMPLE DENSITY. See glpresent.h.
     *
     * The DS renders at 256x192, so scaling to a modern window magnifies every
     * polygon edge and texel - the picture looks chunky and no amount of
     * filtering adds detail that was never rendered. The OpenGL path can render
     * the 3D layer at 2x-4x and filter textures on decode, which does.
     *
     * All three are RESTART-REQUIRED, and not as a convenience: the internal
     * scale is compiled into the compute shaders and sizes every framebuffer,
     * and SDL_WINDOW_OPENGL cannot be added to a window after it is created. */
    /* ON BY DEFAULT. It is a large, visible quality win and it degrades
     * safely: a machine without OpenGL 4.3 falls back to the software path at
     * startup with a line saying so, and everything the shell offers works on
     * both paths - including the floating touch panel, which is fed a GL
     * texture rather than an SDL one when this is live.
     *
     * The two below are pre-set to something worth seeing rather than to 1x,
     * so the setting is immediately visibly doing something. */
    bool opengl_renderer = true;
    int  internal_resolution = 2;    /* 1..4, the 3D raster multiplier       */
    int  texture_upscale = 2;        /* 1, 2 or 4                            */

    /* The floating touch panel: the bottom screen shown inside the main window
     * instead of (or as well as) its own. */
    bool floating_touch = false;
    float floating_touch_scale = 1.0f;
    bool floating_touch_only = false;  /* hide the separate touch window      */

    /* THE ANALOG STICK. The pad's BUTTONS were read from the start and its
     * sticks were not read at all, so a controller half-worked: face buttons
     * and the d-pad responded, the stick did nothing. The DS has no analog
     * input, so a stick can only ever drive the d-pad bits - which is what
     * every DS emulator does and what a player expects.
     *
     * The deadzone is a setting because sticks vary and a worn one rests off
     * centre; too small and the character walks on its own. */
    bool stick_as_dpad = true;
    int  stick_deadzone = 30;        /* percent of full deflection           */

    /* The Enhanced control set (right-stick camera, non-tank movement).
     *
     * DEFAULTS ON, by specification, along with the
     * precedence rule that is easy to get backwards: a client turning this OFF
     * wins over a host that has Enhanced enabled, but never the reverse. A
     * player who wants tank controls gets them; a player who wants Enhanced
     * still needs the host to allow it.
     *
     * ARENA GAMEPLAY ONLY, not the lobby - the specification is explicit, and
     * the lobby's walk-around controls are the ones the game already has.
     *
     * The right stick and its inversions are configuration, and configuration
     * is what this file is for.
     *
     * THIS USED TO END "these settings are read by nothing yet", and that has
     * not been true for some time. frontend.cpp applies the deadzone and the X
     * inversion to the raw axes and calls lsw2_ui_push_camera_delta; enhanced.
     * cpp takes that delta in lsw2_enhanced_camera_step and writes the view
     * record's yaw. The chain is complete and gated to arena maps.
     *
     * AND THE OBJECTION IT RAISED WAS ANSWERED BY A DIFFERENT ROUTE. It said
     * the conversion was the hard part - "the guest's model is turn-and-advance,
     * so mapping a stick vector onto it is a real change to how movement is
     * fed". The port never had to do that mapping: lsw2_enhanced_view_write
     * changes the ACTOR'S VIEW INDEX, so the game's own movement code does
     * camera-relative movement for us. One byte, not a physics port. See
     * enhanced.h.
     *
     * What genuinely remains is the Y inversion, and for a measured reason
     * rather than an unbuilt one - see the note on lsw2_ui_right_stick_invert_x
     * below. */
    /* THE FORK'S FREE PLAY SELECT: hunters only by default, with the whole
     * roster available.
     *
     * TWO SETTINGS, NOT ONE, because these are two things. The Free
     * Play character select REPLACING Party Members in the lobby is the
     * feature; restricting it to the two Bounty Hunter pages is a separate flag
     * choosing all characters or only the bounty hunters. Tying both to
     * one bool meant turning the restriction off also took the screen away. */
    bool bh_freeplay_select = true;   /* show the Free Play select at all   */
    bool bh_roster_only = true;       /* ...restricted to pages 6 and 7     */

    /* The one presentation difference the baseline's co-op networking has:
     * the current room code, hidden by default behind a checkbox, is shown on
     * the Multiplayer imgui tab instead of in-game as in the hunter fork.
     *
     * DEFAULT OFF, because hidden by default is the specified behaviour. It is
     * a screenshot guard rather than a secret - a code on screen is a code
     * anyone watching can join with. */
    bool coop_show_room_code = false;

    bool enhanced_controls = true;
    /* DEBUG: a SECOND copy of the
     * game - another instance already running when it started - plays no
     * music, or no sound at all, so testing several clients on one machine is
     * not heard several times over. Default off. See instancemute.h. */
    bool mute_music_second_instance = false;
    bool mute_sound_second_instance = false;
    /* A FREE PLAY CHARACTER CHANGE UPDATES THE TOP-SCREEN PORTRAIT. Both
     * builds, default on for the same reason the head fix is: it is a
     * cartridge bug, and the fix applies in both. See partyportrait.h. */
    bool enhanced_portrait = true;
    /* THE PORTRAIT RING IS DRAWN IN THE PLAYER'S SLOT COLOUR. Both builds - the
     * hunter lobby and the baseline's co-op
     * netgame share this HUD. See slotcolour.h. */
    bool enhanced_slot_colour = true;
    bool right_stick_invert_x = false;
    bool right_stick_invert_y = false;
    int  right_stick_deadzone = 30;  /* percent, same units as the left stick */
    /* How fast the camera turns and rises, percent of the original rate
     * (a full turn in about 4 s at full deflection). The original was found
     * too slow in play, so the defaults are faster. */
    int  right_stick_speed_x = 250;
    int  right_stick_speed_y = 150;

    /* REMEMBERED WINDOW AND PANEL GEOMETRY.
     *
     * window_scale is a PRESET the player applies; these are what the windows
     * actually ended up as after being dragged. Zero means "not remembered
     * yet", so a fresh install still uses the preset. Kept separate for that
     * reason: overwriting the preset with the dragged size would make the
     * preset buttons behave differently after the first resize.
     *
     * The floating touch panel is an imgui window, and imgui's own .ini is
     * disabled here (we persist our own settings, not imgui's), so nothing was
     * remembering where the player put it. */
    int  window_w = 0, window_h = 0;             /* top window, pixels    */
    int  touch_window_w = 0, touch_window_h = 0; /* bottom window, pixels */
    float touch_panel_x = 0, touch_panel_y = 0;  /* floating panel, screen */
    float touch_panel_w = 0, touch_panel_h = 0;

    /* THE TOP-SCREEN ARENA HUD. Put the match HUD on the top screen and stop
     * showing the touch screen during gameplay.
     *
     * IT COPIES PIXELS RATHER THAN READING GAME STATE, and that is the whole
     * reason it is cheap. The touch screen was measured from 10,733 live
     * captures: the player column at (5, 5 + 48k) with a 48-px pitch, the radar
     * centred on (169, 71). Those regions already contain correctly sorted
     * scores and mode-dependent icons, drawn by the game. Blitting them is
     * ROM-exact and sidesteps the per-player scores, which remained unlocated
     * after three separate attempts.
     *
     * Off by default: it is an enhancement, offered as a setting. */
    bool arena_hud_overlay = false;

    /* THERE ARE NO SOURCE-RECTANGLE SETTINGS FOR THE HUD ANY MORE - retired
     * 2026-09-12, and this is the retraction of the two blocks that stood here.
     *
     * Eight fields lived here: the radar's centre and size, and the signal
     * meter's on/off plus its box. Each had a slider on the Touch tab, and the
     * argument for them was "the fastest way to settle a which-rectangle
     * question is to look at the screen while changing it". Both questions
     * ARE settled (the radar panel is x 96..247, y 8..139 on the game's own
     * frame; the meter is OAM sprite tile 0, palette 0, 16x16), and once
     * settled the sliders did the opposite of their job: settings.json still
     * held the OLD values on every install that had ever saved - radar
     * (169,71,96), meter off, 24x24 - so the "new defaults" reached nobody and
     * the port drew a cropped radar and no meter while the code claimed both
     * were fixed. A persisted debug knob is a setting that silently outvotes
     * every later correction.
     *
     * The top-screen HUD's rule: the switch lives in imgui, the pixels come
     * from the game. Where
     * the cartridge draws its radar and its meter is the game's business, so
     * arenahud.cpp now reads the meter's box out of the touch screen's OAM
     * each frame and keeps the radar's measured panel as a constant. The one
     * setting left is the toggle above, and the scale below. */
    /* THE HUD'S SIZE, in top-screen pixels per touch-screen pixel: 1.0 is the
     * DS's own size, 0.5 is half, drawn a touch pixel to a pixel of the
     * HUD's double-resolution layer so it stays sharp (topoverlay.h). Stored as
     * "arena_hud_size" since 2026-09-26: the old "arena_hud_scale" key
     * held 1.0 on every install that had saved, a size that proved too big
     * in play, and a new default under the old key would never have reached
     * those installs. */
    float arena_hud_scale = 0.5f;    /* 0.5 .. 1.5 */

    /* Debug */
    bool show_metrics = false;

    /* Multiplayer, fork-only. See UiMultiplayer above. */
    UiMultiplayer mp;
};

/* One rebindable action. The DS bit is what the guest sees; everything else is
 * host policy. */
struct UiBinding {
    const char* action;   /* "A", "Start", "D-pad Up" ...        */
    uint16_t ds_bit;      /* KEYINPUT bit this drives            */
    int scancode;         /* SDL_Scancode, editable              */
    int pad_button;       /* SDL_GameControllerButton, or -1     */
};

/* The live binding table. The shell reads it every frame, the UI edits it, and
 * ui_save_settings persists it. Exposed rather than hidden because the shell's
 * input path and the UI must agree on exactly one table - two copies would
 * drift the first time a binding was added. */
UiBinding* ui_bindings(int* count);

/* Editing state: when the UI is waiting for a key, the shell must not treat
 * that keypress as gameplay input. */
bool ui_capturing_binding();
void ui_offer_captured_key(int scancode);
void ui_offer_captured_pad(int button);

/* Lifecycle. `window`/`renderer` are the main (top) window's.
 *
 * A NULL renderer means the OpenGL present path is running: the top window has
 * no SDL_Renderer at all, and imgui switches to its SDL2+OpenGL3 backends. That
 * is the one signal - there is no separate mode flag to get out of step with
 * the window that was actually created. */
bool ui_init(SDL_Window* window, SDL_Renderer* renderer);
void ui_shutdown();

/* Returns true if imgui consumed the event and the game should not see it. */
bool ui_handle_event(const SDL_Event& event);

/* Call once per frame, after the guest frame and before presenting. */
void ui_begin_frame();
void ui_draw(UiSettings& settings);
void ui_render(SDL_Renderer* renderer);

/* F1 toggles the menu; the game keeps running behind it. */
void ui_toggle();
bool ui_visible();

/* Live numbers the debug tab reports. Fed by the loop so the UI never has to
 * reach into emulator state itself. */
void ui_set_stats(double fps, uint64_t frames, uint64_t audio_underruns);

/* The bottom screen, for the floating panel. Uploaded once per frame from the
 * same framebuffer the separate window uses.
 *
 * OPAQUE ON PURPOSE. Under SDL this is an SDL_Texture*; under OpenGL it is a GL
 * texture name. imgui takes either as an ImTextureID and the panel does not
 * care which, so typing it as SDL_Texture* would force a lie at one of the two
 * call sites. Pass whatever the live present path produced, or nullptr. */
void ui_set_touch_texture(void* texture_handle);

/* Where the floating panel ended up on screen, so the loop can map clicks in
 * it back to stylus coordinates. Returns false when it is not being shown. */
bool ui_floating_touch_rect(SDL_Rect* out);


/* THE FORK FLAG.
 *
 * lsw2.exe and lsw2_bountyhunter.exe are ONE binary under two names. Until now
 * the only thing that differed was which save the runner opened, decided from
 * argv[0] and never visible above that. The Bounty Hunter fork needs the
 * distinction in the UI and in the game layer - a hidden cheats tab, a
 * restricted roster, a different pause menu - so it is promoted to a flag the
 * whole shell can read.
 *
 * Set once at startup from the executable's own name. Deliberately not a
 * setting: a build is what it is, and a user toggling "am I the fork" mid-run
 * would put the two saves and the two rule sets in reach of each other. */
void ui_set_fork_build(bool fork);
bool ui_fork_build();

/* Settings persistence. */
void ui_load_settings(UiSettings& settings, const char* path);
void ui_save_settings(const UiSettings& settings, const char* path);

#endif /* LSW2_UI_H */

/* THE FORK'S FREE PLAY SELECT. Whether the fork's character select shows only
 * the two Bounty Hunter pages. This is a flag defaulting to hunters only, so a
 * host or a player who wants the whole roster can have it.
 *
 * Read from the dispatch override, which runs before the frontend exists, so it
 * cannot reach UiSettings directly - hence a function rather than a field. */
bool lsw2_hle_fork_wants_roster();
/* The hunters-only flag: restrict that select to the two Bounty Hunter pages. */
bool lsw2_hle_fork_hunters_only();

/* WHAT PAGE RANGE THE FORK WANTS ON THE FREE PLAY SELECT, IF ANY.
 *
 * Returns false when the fork should leave the widget entirely alone, and
 * otherwise fills a half-open page range in *low and *high.
 *
 * ONE POLICY, TWO WRITERS. dispatch_overrides.cpp sets the range as the Free
 * Play constructor returns, so the first frame is already right; fork.cpp
 * maintains it every frame after, because the host opening the host settings
 * menu's Character row
 * has to widen a widget that was built long before. Both ask this.
 *
 * THE LEVEL GATE IS PART OF THE POLICY, not of either call site. The
 * restriction holds on the Arena lobby map and nowhere else, and the per-frame
 * writer had no such condition when it was first written - so in any other
 * level the fork would have cut a perfectly ordinary Free Play select down to
 * two pages. The constructor path had the gate and the new writer did not,
 * which is exactly the disagreement one shared answer prevents. */
bool lsw2_hle_fork_select_range(unsigned* low, unsigned* high);

/* The top-screen arena HUD's toggle, mirrored out of the settings struct the
 * way the Free Play select flags are. The HUD itself lives in arenahud.cpp and draws into the DS frame, so it
 * needs the switch without needing the whole UiSettings. */
bool lsw2_ui_arena_hud_enabled();

/* The top-screen arena HUD's "HUD size", 0.5..1.5. Read by arenahud.cpp, which resamples
 * its rectangles by it - the slider used to be over nothing. */
float lsw2_ui_arena_hud_scale();

/* lsw2_ui_hud_radar() and lsw2_ui_hud_ping() are gone with the settings they
 * mirrored - see UiSettings::arena_hud_scale for the retraction. The radar's
 * panel is a constant in arenahud.cpp and the meter is located from the game's
 * own OAM there; neither is the UI's to hand over. */

/* The Enhanced controls' client-side switch, mirrored the same way and for the same
 * reason: enhanced.cpp writes an actor byte and has no business knowing what a
 * UiSettings is. This is the CLIENT's wish - the host's allowance is a host
 * setting, and lsw2_enhanced_effective() is where the two meet. */
bool lsw2_ui_enhanced_controls();


/* The Debug tab's second-instance mutes, read by instancemute.cpp. */
bool lsw2_ui_mute_music_second_instance();
bool lsw2_ui_mute_sound_second_instance();

/* The portrait-follows-character switch, read by partyportrait.cpp. */
bool lsw2_ui_enhanced_portrait();

/* The slot-colour portrait ring's switch, read by slotcolour.cpp. */
bool lsw2_ui_enhanced_slot_colour();

/* The Enhanced controls' right stick, read by frontend.cpp for the raw axes and by
 * enhanced.cpp for the yaw write. Deadzone is a percentage, 5..90. */
bool lsw2_ui_right_stick_invert_x();
int  lsw2_ui_right_stick_deadzone();
/* Camera turn and height speed, percent (25..600). */
int  lsw2_ui_right_stick_speed_x();
int  lsw2_ui_right_stick_speed_y();

/* "inverted up/down and left/right" - the up/down half, live since 2026-09-23.
 *
 * RETRACTED: this said there was no vertical control to invert, because the
 * VIEW RECORD this port wrote into carries one angle (the yaw at +0x6A). True
 * of the record, and the record was the wrong object: it is what the game
 * sends, not the camera. The camera the enhanced controls now steer is the
 * game's own mode 4 on the local camera object (enhanced.cpp), which sits at an
 * offset from the player - and that offset has a height. The stick's vertical
 * axis raises and lowers it; this inverts it. */
bool lsw2_ui_right_stick_invert_y();

/* The camera deltas the right stick (or the Camera bindings) asked for this
 * frame, cleared by the reader: x in DS yaw units, y in the same scale for the
 * orbit height. Set by frontend.cpp; consumed by enhanced.cpp, which is the
 * only file that knows where a camera lives. */
void lsw2_ui_push_camera_delta(int dx);
int  lsw2_ui_take_camera_delta();
void lsw2_ui_push_camera_delta_y(int dy);
int  lsw2_ui_take_camera_delta_y();
