/* The fork's host settings menu, and the Extras submenu it opens.
 *
 * The menu's design spells it out entry by entry, so this header does not
 * paraphrase it - the rows, their values and their defaults are transcribed
 * from it, and `test_hostsettings.cpp` asserts each one against the sentence it
 * came from.
 *
 * IT IS AN IN-GAME MENU. The design is explicit: nearly all
 * of what the fork adds is drawn inside the game and styled like the game's
 * own screens, and imgui is left with only what has no sensible home there -
 * the player name and the master server address. So this builds an `Lsw2Menu`,
 * which `gamemenu_render.cpp` composites into the DS frame itself.
 *
 * THE SETTINGS OUTLIVE THE MENU. A host opens the menu, changes things, closes
 * it, and the lobby keeps running with the new values - and the console entity's
 * "Change Settings" reopens the same state. So the struct is the truth and the
 * menu is a view of it, rather than the other way round.
 */

#ifndef LSW2_HOSTSETTINGS_H
#define LSW2_HOSTSETTINGS_H

#include <cstddef>

#include "gamemenu.h"

#include <cstdint>

/* "Roster: <Hunters/ALL/Random Hunter/Random ALL/One>" */
enum class Lsw2Roster { Hunters = 0, All, RandomHunter, RandomAll, One };

/* "Controls: <TANK/ENHANCED>", ENHANCED by default */
enum class Lsw2HostControls { Tank = 0, Enhanced };

/* "Customization: <OFF/HATS/ALL>", ALL by default */
enum class Lsw2HostCustomization { Off = 0, Hats, All };

/* "Music: <DEFAULT/RANDOM>" */
enum class Lsw2HostMusic { Default = 0, Random };

/* Lobby Music - Hunter, Cantina or Random: the arena
 * lobby's own track, the Mos Eisley Cantina's, or a random one of the levels'
 * tracks. Applied by musicpick.cpp (lsw2_lobby_music_tick). */
enum class Lsw2LobbyMusic { Hunter = 0, Cantina, Random };

/* "LOBBY TIME <OFF/30s/60s/300s>" - "default 30s" */
enum class Lsw2LobbyTime { Off = 0, S30, S60, S300 };

/* The eighteen Extras, of which the design hides eight from the host.
 * They are numbered by string id: flag = string id - 0x243. */
constexpr int kLsw2ExtrasCount = 18;

struct Lsw2HostSettings {
    bool visible_master = true;
    bool visible_lan = true;
    Lsw2Roster roster = Lsw2Roster::Hunters;
    /* "Character: <Greedo>" as default - character id 20. Only
     * meaningful while roster is One. */
    int character = 20;
    /* Which Extras the host has switched on. Indexed by the flag number
     * (string id - 0x243), so the eight the design hides are simply never offered. */
    bool extras[kLsw2ExtrasCount] = {false};
    Lsw2HostControls controls = Lsw2HostControls::Enhanced;
    Lsw2HostCustomization customization = Lsw2HostCustomization::All;
    Lsw2HostMusic music = Lsw2HostMusic::Default;
    Lsw2LobbyMusic lobby_music = Lsw2LobbyMusic::Hunter;
    Lsw2LobbyTime lobby_time = Lsw2LobbyTime::S30;

    /* A SUICIDE SCORES NOTHING (killcredit.h). The cartridge credits a death
     * to whoever the killer resolves to, and its own explosives (Bossk's,
     * Leia in Boushh disguise's, 4-LOM's) resolve to the thrower - so blowing
     * yourself up counted as a kill. On, a death the victim caused is scored
     * as the game scores a fall: a death and nothing else. A match rule,
     * so it travels with the others and the host's choice holds everywhere. */
    bool suicide_no_kill = true;

    /* WHICH ARENA MAP THE ROUND USES - AND IT IS NOT A ROW, AND NOT WHAT PICKS
     * THE LEVEL. levels.h names both, from rows the level table marks
     * `confirmed`: 4 Jabba's Tower, 5 Mos Eisley Streets.
     *
     * CORRECTED. This comment used to say the design
     * "gives the host the choice ... so it is a host setting like every other
     * row". That reading was withdrawn: maps are not selectable in lobby
     * setup, because starting the game is what runs the normal Bounty Hunter
     * pre-game phase of map, rule and character select. The Map ROW was
     * removed then; this comment was
     * not, and left standing it invites the next reader to put the row back.
     *
     * WHAT ACTUALLY HAPPENS, measured: the cartridge's own pre-game phase picks
     * the level. A full two-console run reaches state 42 on level 5 while this
     * field still holds its default 4, and the fork's own round-start warp -
     * which is the only code that reads this field for that purpose - never
     * logs a line in such a run. The field survives because it still travels to
     * clients with the rest of the settings, not because it decides anything. */
    int map = 4;

    /* THE LEVEL A ROUND IS RUNNING ON, or 0 for "everyone is in the lobby".
     *
     * NOT A SETTING THE HOST EDITS - it is session STATE, and it is here
     * because this is the structure that already reaches every client. Two
     * requirements need it:
     *
     *   the round start itself: the host warps to the arena and the clients
     *   have to follow, and nothing else in the port crosses the wire at that
     *   moment - `lsw2_netmsg_start_round` only writes the LOCAL guest's
     *   receive buffer despite its name;
     *
     *   joining a match already under way - a joiner needs to know a
     *   round is running and where, which is exactly this field.
     */
    int round_level = 0;

    /* True when at least one Extra is on - the row then reads "Extras: <ON>". */
    bool any_extra() const;
    int lobby_seconds() const;
};

/* Row identity, so callers can act on an activation without matching strings. */
enum Lsw2HostRow {
    kHostVisibleMaster = 0, kHostVisibleLan, kHostRoster, kHostCharacter,
    kHostExtras, kHostControls, kHostCustomization, kHostMusic,
    kHostLobbyMusic, kHostLobbyTime, kHostStart, kHostRowCount
};

/* The Map row's helpers went with the row itself - see hostsettings.cpp for
 * why there is no Map row. `Lsw2HostSettings::map` survives only as the level
 * the legacy round-start warp falls back to, and that warp is itself on the way
 * out: the map choice belongs to the cartridge's own pre-game phase. */

/* Build the menu as a view of `s`. */
void lsw2_host_menu_build(Lsw2Menu& menu, const Lsw2HostSettings& s);

/* Copy the menu's values back into `s`, and keep the Character row's
 * visibility in step with Roster: Roster at One reveals the Character row
 * beneath it, and moving Roster off One hides that row again and disregards
 * its value. */
void lsw2_host_menu_sync(Lsw2Menu& menu, Lsw2HostSettings& s);

/* The Extras submenu: every Extra can be switched by the host, apart from
 * eight that are never shown - Pink, Moustaches, Sunglasses, Invincibility,
 * Ewok Invasion, Droid Invasion, Minikit Detector and Tiny Enemies. */
void lsw2_extras_menu_build(Lsw2Menu& menu, const Lsw2HostSettings& s);
void lsw2_extras_menu_sync(Lsw2Menu& menu, Lsw2HostSettings& s);

/* Whether the host is allowed to see a given Extra. Exposed for the test, and
 * because the same rule decides what is sent to clients. */
bool lsw2_extra_offered(int flag);

/* WHERE AN EXTRA'S NAME COMES FROM, and why it is a hook rather than a table.
 *
 * The eighteen names in hostsettings.cpp used to BE the names. That made this
 * file answerable to the cartridge's string bank - the Extras are strings
 * 0x243-0x254, flag = id - 0x243 - with nothing in the C to say so, which is
 * the shape `fork.h` already rejected for character names: "transcribing them
 * into a table here would be sixty-five chances" to get one wrong.
 *
 * So the fork installs a decoder and the literals become a FALLBACK, used only
 * before the bank is loaded and in tests. The name a player reads now comes
 * from the cartridge.
 *
 * IT IS A HOOK AND NOT A DIRECT CALL for the same reason
 * `lsw2_set_character_name_source` is: hostsettings.cpp includes nothing but
 * <cstdio> and <cstring> on purpose, so `test_hostsettings.cpp` can assert every
 * row of this menu against the sentence of the design it came from without a
 * ROM, a frame, or the guest runtime on its link line. */
using Lsw2ExtraNameFn = const char* (*)(int flag, char* buf, size_t n);
void lsw2_set_extra_name_source(Lsw2ExtraNameFn fn);

/* The decoded name when a source is installed and the bank is up; the built-in
 * fallback otherwise. The returned pointer is stable for the life of the
 * program - menu rows store it rather than copying. */
const char* lsw2_extra_name(int flag);

/* ---------------------------------------------------------------- persistence
 *
 * The fork's pause menu hides Save Game (and Host Wireless), and the reason its
 * design gives is an aside that is easy to read past: settings and
 * customization persist on their own, so no explicit save is needed.
 *
 * Customization got that treatment when it was built. These did not: a host who
 * spent a minute on the ten-row menu lost every choice on exit, because the
 * struct was default-constructed each run. Hiding the save AND not persisting is
 * the combination that aside exists to forbid.
 */

/* The Extras as one bitmask, for a settings file that should not carry eighteen
 * near-identical keys. Homogeneous flags, so unlike the customization slots
 * there is nothing to be gained by letting them fail independently - a bad mask
 * gives wrong Extras, not a corrupted anything. */
uint32_t lsw2_host_extras_mask(const Lsw2HostSettings& s);
void lsw2_host_extras_from_mask(Lsw2HostSettings& s, uint32_t mask);

/* Bring every field back into range.
 *
 * CALLED AFTER LOADING, ALWAYS. settings.json is hand-editable and these values
 * are cast straight to enums that index the menu's own value arrays - a
 * `Lsw2Roster(7)` would read past the end of a five-entry table the next time
 * the menu drew. Clamping at the door is cheaper than trusting every reader. */
void lsw2_host_settings_clamp(Lsw2HostSettings& s);

/* Whether a character id is one the host may pick for "Roster: One".
 *
 * Every playable character is allowed, unlikely ones such as R5-D4 and Gonk
 * Droid included - so this is deliberately permissive. What it rejects is the game's own
 * "not available" padding (0x64) and anything outside the id space the select
 * table uses, which is what a hand-edited file would produce. */
bool lsw2_host_character_valid(int character_id);

/* ------------------------------------------------------------------- the wire
 *
 * The host settings menu lets several Extras be on together, and they must
 * **reach each client as it joins**. And the character-customization design
 * gives the host authority over customization: the host chooses whether it
 * is permitted in matches, and when it is not, no customization applies at
 * all.
 *
 * Neither can hold until the host's choices reach the client. Until now they did
 * not: `effective_policy()` read the LOCAL session's settings, so a client
 * applied its own rules and the host's authority stopped at its own machine.
 *
 * WHAT TRAVELS IS THE MATCH RULES, NOT THE HOST'S ADVERTISING CHOICES.
 * `visible_master` and `visible_lan` are decisions about where this host lists
 * its own lobby; they mean nothing to a client and a client that adopted them
 * would believe it was advertising. They are deliberately absent from the
 * record.
 *
 * VERSIONED, because a client built from a different revision must refuse the
 * record rather than misread it - a settings block off by one byte would apply
 * the wrong Extras to a match and look like a game bug. */
constexpr uint8_t kLsw2HostWireVersion = 3;
/* THIRTEEN SINCE THE ROUND START CROSSED THE WIRE: the map the host picked and
 * the level a round is running on, one byte each. The version byte at [0]
 * guards the change - an older peer's payload is rejected outright rather than
 * read short, which is what lsw2_host_settings_from_wire's first check is
 * for. FOURTEEN, version 2, since Lobby Music joined the rules. FIFTEEN,
 * version 3, since the suicide rule did. */
constexpr int kLsw2HostWireBytes = 15;

/* Write the match rules. Returns bytes written, or 0 if `n` is too small. */
int lsw2_host_settings_to_wire(const Lsw2HostSettings& s, uint8_t* out, int n);

/* Read them back over `s`, leaving the advertising flags alone. False when the
 * record is short or its version is not one this build knows. Values are
 * clamped on the way in - the record arrives from the network. */
bool lsw2_host_settings_from_wire(Lsw2HostSettings& s, const uint8_t* in, int n);

#endif /* LSW2_HOSTSETTINGS_H */
