/* See hostsettings.h. */

#include "hostsettings.h"

#include "characterpick.h"

/* kLsw2ArenaJabbaLevel and kLsw2ArenaMosEisleyLevel. levels.h is generated
 * and header-only - no ROM, no frame, no guest runtime - so it does not cost
 * this file the standalone link that test_hostsettings.cpp depends on. */
#include "levels.h"

#include <cstdio>
#include <cstring>

namespace {

/* THE NAMES HERE ARE A FALLBACK, NOT THE TRUTH - see lsw2_extra_name.
 *
 * The Extras are strings 0x243-0x254 and flag = string id - 0x243, so
 * the cartridge already holds these eighteen names. The fork installs a decoder
 * and these are what a player sees only before the string bank is up, and what
 * the test sees always. They are still checked against the ROM by
 * `tools/sweep_rom_constants.py`, because a fallback that says something
 * different from the real name is a bug that only shows during loading.
 *
 * `offered` is NOT a fallback - it is a design decision of the host settings
 * menu and has no other source. It is marked per row rather than in a second
 * list so the rule and the names cannot drift apart, and the same sweep checks
 * the marked set against the eight the design hides. */
struct Extra { const char* fallback_name; bool offered; };
const Extra kExtras[kLsw2ExtrasCount] = {
    {"Slippery Floors", true},   /*  0 */
    {"Invincibility",   false},  /*  1  hidden by design */
    {"Disco",           true},   /*  2 */
    {"Whitebox",        true},   /*  3 */
    {"Letterbox",       true},   /*  4 */
    {"Rainy Day",       true},   /*  5 */
    {"Lights Out",      true},   /*  6 */
    {"Old Time",        true},   /*  7 */
    {"Old West",        true},   /*  8 */
    {"Medieval",        true},   /*  9 */
    {"Sunglasses",      false},  /* 10  hidden */
    {"Moustaches",      false},  /* 11  hidden */
    {"Minikit Detector",false},  /* 12  hidden */
    {"Tiny Enemies",    false},  /* 13  hidden */
    {"Ewok Invasion",   false},  /* 14  hidden */
    {"Pink",            false},  /* 15  hidden */
    {"Droid Invasion",  false},  /* 16  hidden */
    {"Walkie-Talkie",   true},   /* 17 */
};

Lsw2ExtraNameFn g_name_source = nullptr;
char g_decoded[kLsw2ExtrasCount][32] = {{0}};

Lsw2MenuRow toggle(const char* label, const char* off, const char* on,
                   int value) {
    Lsw2MenuRow r;
    r.label = label;
    r.values[0] = off;
    r.values[1] = on;
    r.value_count = 2;
    r.value = value;
    return r;
}

/* THE HOST'S "One" CHARACTER, SHOWN - AND PICKED ON THE FREE PLAY GRID.
 *
 * The Character row was a one-value "Greedo" stub and sync() never read it, so
 * the host could not change it (pressing X returned with it still Greedo).
 * It then became a value row cycling left and right over every name in the
 * game's pick table - one row of a big text-based list, which the cartridge's
 * own picker was to replace. It is now an ACTION row: its label
 * reads "Character: <name>", the wording the menu's design uses, and A hands the host
 * the cartridge's Free Play grid (freeplaypick.h), whose confirmed pick is
 * written straight into the settings. The name is resolved by
 * lsw2_character_name into a static buffer (the row holds a const char*, so
 * it must outlive the call). */
const char* host_char_label(int character_id) {
    static char name[32];
    static char label[48];
    std::snprintf(label, sizeof label, "Character: %s",
                  lsw2_character_name(character_id, name, sizeof name));
    return label;
}

}  // namespace

bool Lsw2HostSettings::any_extra() const {
    for (int i = 0; i < kLsw2ExtrasCount; ++i)
        if (extras[i]) return true;
    return false;
}

int Lsw2HostSettings::lobby_seconds() const {
    switch (lobby_time) {
    case Lsw2LobbyTime::Off:  return 0;
    case Lsw2LobbyTime::S30:  return 30;
    case Lsw2LobbyTime::S60:  return 60;
    case Lsw2LobbyTime::S300: return 300;
    }
    return 0;
}

bool lsw2_extra_offered(int flag) {
    if (flag < 0 || flag >= kLsw2ExtrasCount) return false;
    return kExtras[flag].offered;
}

const char* lsw2_extra_name(int flag) {
    if (flag < 0 || flag >= kLsw2ExtrasCount) return "";
    if (!g_name_source) return kExtras[flag].fallback_name;

    /* CACHED PER FLAG, because a menu row STORES this pointer rather than
     * copying it - a rotating scratch buffer would leave every row showing
     * whichever name was decoded last. */
    if (g_decoded[flag][0] == '\0') {
        char buf[sizeof g_decoded[0]] = {0};
        const char* got = g_name_source(flag, buf, sizeof buf);
        /* ONLY CACHE A SUCCESS. The bank is not always loaded; caching a
         * blank would pin the fallback for the rest of the run, and a menu
         * opened one frame too early would stay wrong forever. */
        if (got && got[0] != '\0') {
            std::strncpy(g_decoded[flag], got, sizeof g_decoded[0] - 1);
            g_decoded[flag][sizeof g_decoded[0] - 1] = '\0';
        }
    }
    return g_decoded[flag][0] ? g_decoded[flag] : kExtras[flag].fallback_name;
}

void lsw2_set_extra_name_source(Lsw2ExtraNameFn fn) {
    g_name_source = fn;
    for (int i = 0; i < kLsw2ExtrasCount; ++i) g_decoded[i][0] = '\0';
}

void lsw2_host_menu_build(Lsw2Menu& menu, const Lsw2HostSettings& s) {
    menu.reset("Host Internet Game");

    menu.add(toggle("Visible to Master", "NO", "YES", s.visible_master ? 1 : 0));
    menu.add(toggle("Visible to LAN", "NO", "YES", s.visible_lan ? 1 : 0));

    Lsw2MenuRow roster;
    roster.label = "Roster";
    roster.values[0] = "Hunters";
    roster.values[1] = "ALL";
    roster.values[2] = "Random Hunter";
    roster.values[3] = "Random ALL";
    roster.values[4] = "One";
    roster.value_count = 5;
    roster.value = int(s.roster);
    menu.add(roster);

    /* Roster set to One reveals a row beneath it, "Character: <Greedo>", with
     * Greedo as the default. The value text is filled by sync() from the character id, so
     * there is one place that turns an id into a name. */
    Lsw2MenuRow character;
    character.label = host_char_label(s.character);
    character.hidden = (s.roster != Lsw2Roster::One);
    menu.add(character);

    menu.add(toggle("Extras", "OFF", "ON", s.any_extra() ? 1 : 0));
    menu.add(toggle("Controls", "TANK", "ENHANCED", int(s.controls)));

    Lsw2MenuRow custom;
    custom.label = "Customization";
    custom.values[0] = "OFF";
    custom.values[1] = "HATS";
    custom.values[2] = "ALL";
    custom.value_count = 3;
    custom.value = int(s.customization);
    menu.add(custom);

    /* THE ROW THE HOST MENU'S DESIGN ASKS FOR. IT IS APPLIED - SEE musicpick.h.
     *
     * CORRECTED. Everything below this line is the
     * research trail that got here, and it ENDED with the answer: the per-level
     * track set is `[[0x020FF674 + level*4] + 1]`, dumped by
     * tools/export_music_levels.py as 64 levels onto 22 slots, and
     * musicpick.cpp writes the pick into the arena maps' records, where the
     * game's own level constructor reads it on the arena load (it used to
     * guest-call the selector 0x020B90E4 there, which aborted mid-load -
     * see musicpick.cpp). The trail is kept because
     * four dead hypotheses are worth not re-testing; the STATUS it used to
     * assert is gone, and so is the runtime NOTE that told players the setting
     * did nothing. That note outlived its truth and would have read, to anyone
     * setting Music to RANDOM in play, as the feature being absent.
     *
     * The original header, for the record: "THE ROW THE DESIGN ASKS FOR,
     * CARRYING A SETTING NOTHING APPLIES YET."
     *
     * An earlier reading of the music path, taken as far as the ROM allows,
     * ended with a rule this menu was quietly breaking:
     *
     *   "Until that table exists, `Music: RANDOM` should not be offered - a
     *    setting that picks randomly from a set nobody has established is a
     *    setting that plays the wrong music with confidence."
     *
     * The row cannot simply go: the design names it in the host's list, and a
     * missing row is a different kind of wrong. So it stays and says so once,
     * because the failure it would otherwise have is the one this project keeps
     * naming - a control that looks live and is inert.
     *
     * ONE OF THE TWO BLOCKERS IS GONE, AND IT WAS NEVER REAL. This used to say
     * the application "needs 0x020B90E4(slot, param), a guest call, and this
     * port cannot make one except by tail-call out of an override it is already
     * inside". That was true when written and is not now:
     * `lsw2_guest_call` exists and is used from the ordinary tick in EIGHT
     * places across six files - gearapply, partapply, partyportrait,
     * pauseredraw, countboot. Calling the music selector is no harder than
     * calling party_refresh.
     *
     * WHAT ACTUALLY REMAINS is the TABLE: which slots are battle tracks. Four
     * hypotheses have now been tested against the ROM and all four are dead,
     * which is worth recording because each looks plausible from the outside:
     *
     *   THE SLOT TABLE DOES NOT ENCODE IT. 0x02100A00's 29 rows map slot to
     *   (u16 group, u8 mode, u8 param), and the groups are 0..28 each appearing
     *   EXACTLY ONCE - it is a permutation, a remap and nothing more.
     *
     *   THE MODE FIELD DOES NOT EITHER. Its three values split 1 / 24 / 4
     *   (slot 9 alone; twenty-four; slots 18, 20, 24, 25). A calm track plus a
     *   battle track for most levels would need a roughly even split across ~20 levels;
     *   this is not one.
     *
     *   THE FOUR-WAY CLUSTER AT 0x02086954-0x02086984 IS NOT COMBAT. It picks
     *   slots 21, 26, 27 and 28 from a jump table on its second argument, and
     *   the containing function (0x02086810) tests `block + 0x4518` bit n -
     *   the EXTRAS-ENABLED array. Those four are extra-triggered tracks.
     *
     *   AND THE FIELD WAS NEVER MISIDENTIFIED. An earlier analysis concluded
     *   that what was needed was re-identifying the field, not a play test, because
     *   0x0210DB14+9 reads 17 in every capture. The literal pool says that
     *   field is right: 0x020B9100 loads its base from 0x020B917C, which holds
     *   0x0210DB14, and reads +9. It reads 17 everywhere because nothing in a
     *   recompiled capture ever changes it - the boot sequence's 0 -> 10 -> 17
     *   is exactly the fixed-slot call sites below firing once.
     *
     * ALL TEN CALL SITES, since enumerating them is what killed three of those:
     *
     *   0x0206A654, 0x02073DF8, 0x020842EC   slot 0     fixed
     *   0x0206A1D0                           slot 10    fixed
     *   0x0207C7A0                           slot 18    fixed
     *   0x02086954/64/74/84                  21/26/27/28  by extras index
     *   0x020B3A3C                           DATA-DRIVEN
     *
     * THE LAST ONE IS THE LEAD. `r0 = [r4 + 0x12]` and `r1 = [r4 + [r4+1] +
     * 0x17]` - both operands come out of a record, so it is the only site whose
     * slot varies with content rather than with code path. If level music is
     * chosen anywhere, it is chosen there, and identifying r4 identifies the
     * per-level track set without a play test.
     *
     * AND THAT RECORD IS NOW READ. 0x020B3998 takes it as its first argument
     * and lays it out:
     *
     *     +0x01   a VARIATION index, from the function's second argument
     *     +0x12   the requested slot        (29 means stop - CMP r1,#0x1D)
     *     +0x16   the slot currently set
     *     +0x17   an inline ARRAY OF PARAMS, indexed by the +0x01 variation
     *
     * so the call is `select(slot = [rec+0x12], param = [rec + [rec+1] +
     * 0x17])`. One slot, several params, chosen by a variation index.
     *
     * WHICH IS PROBABLY WHAT "CALM AND BATTLE" ACTUALLY IS. The Music setting
     * rests on most levels having a calm and a battle track, and this is a structure that
     * gives one level one SLOT and several PARAMS. The selector agrees: at
     * 0x020B913C a param of 0xFF means "take the row's own default" and
     * anything else is an explicit variation, which is exactly the shape of a
     * default track plus alternates. Every one of the nine fixed call sites
     * passes 0xFF - they ask for the default - and only this data-driven one
     * passes a chosen param.
     *
     * NOT CLAIMED AS SETTLED, because what a given variation index MEANS is
     * still unread: the array's length is not established here and nothing yet
     * ties index 1 to combat rather than to a cutscene or a boss. That is the
     * next question, and it is a question about this record's writers rather
     * than about the sound engine.
     *
     * That was the status WHEN THIS WAS WRITTEN. It was superseded: the set is
     * a per-level table, not a variation index, and musicpick.h has the
     * derivation. The runtime NOTE that stood here - telling the player that
     * "no battle-track table exists yet and nothing applies it" - has been
     * removed rather than reworded, because the only honest version of it is
     * silence: the setting works. */
    menu.add(toggle("Music", "DEFAULT", "RANDOM", int(s.music)));

    /* The host chooses the lobby track: Hunter, Cantina or Random. */
    Lsw2MenuRow lobby_music;
    lobby_music.label = "Lobby Music";
    lobby_music.values[0] = "HUNTER";
    lobby_music.values[1] = "CANTINA";
    lobby_music.values[2] = "RANDOM";
    lobby_music.value_count = 3;
    lobby_music.value = int(s.lobby_music);
    menu.add(lobby_music);

    Lsw2MenuRow lobby;
    lobby.label = "LOBBY TIME";
    lobby.values[0] = "OFF";
    lobby.values[1] = "30s";
    lobby.values[2] = "60s";
    lobby.values[3] = "300s";
    lobby.value_count = 4;
    lobby.value = int(s.lobby_time);
    menu.add(lobby);

    /* NO MAP ROW. IT WAS AN INVENTION.
     *
     * Maps are not selectable in lobby setup, because starting the game is
     * what runs the normal Bounty Hunter pre-game phase of map, rule and
     * character select.
     *
     * This menu had a Map toggle, justified by the design's statement that
     * play begins on whichever of Jabba's Tower and Mos Eisley Streets the
     * host decides.
     * That statement is real, but it says WHERE the decision lands, not where
     * it is MADE - and the design also says that at round start the host
     * picks map, game mode and time the way the cartridge always has. That
     * means the cartridge's own pre-game phase, which already exists and
     * already asks all three.
     *
     * The design lists this menu's rows explicitly and Map is not among them:
     * Visible to Master, Visible to LAN, Roster, Character (conditional),
     * Extras, Controls, Customization, Music, LOBBY TIME, Start Lobby. Adding
     * an eleventh was a substitution, and the header's own phrase for this
     * screen - "the ten-row menu" - had been quietly wrong ever since. */

    menu.add([] { Lsw2MenuRow r; r.label = "Start Lobby"; return r; }());
}

void lsw2_host_menu_sync(Lsw2Menu& menu, Lsw2HostSettings& s) {
    if (menu.row_count() < kHostRowCount) return;

    s.visible_master = menu.row(kHostVisibleMaster).value != 0;
    s.visible_lan = menu.row(kHostVisibleLan).value != 0;
    s.roster = Lsw2Roster(menu.row(kHostRoster).value);
    s.controls = Lsw2HostControls(menu.row(kHostControls).value);
    s.customization = Lsw2HostCustomization(menu.row(kHostCustomization).value);
    s.music = Lsw2HostMusic(menu.row(kHostMusic).value);
    s.lobby_music = Lsw2LobbyMusic(menu.row(kHostLobbyMusic).value);
    s.lobby_time = Lsw2LobbyTime(menu.row(kHostLobbyTime).value);

    /* THE CHARACTER ROW FOLLOWS THE ROSTER, in both directions. */
    menu.set_hidden(kHostCharacter, s.roster != Lsw2Roster::One);

    /* AND ITS LABEL FOLLOWS THE SETTING. The row carries no value any more -
     * the Free Play grid writes s.character (Lsw2ForkSession::
     * finish_character_pick) - so there is nothing to read back, only the
     * name to refresh. */
    if (s.roster == Lsw2Roster::One)
        menu.row(kHostCharacter).label = host_char_label(s.character);

    /* The row reads "Extras: <ON>" whenever at least one Extra is on - so the
     * row REPORTS the submenu's state rather than being edited directly. The
     * host toggles individual Extras behind A; left and right on this row would
     * otherwise claim to turn all of them on at once. */
    menu.row(kHostExtras).value = s.any_extra() ? 1 : 0;
}

void lsw2_extras_menu_build(Lsw2Menu& menu, const Lsw2HostSettings& s) {
    menu.reset("Extras");
    for (int flag = 0; flag < kLsw2ExtrasCount; ++flag) {
        if (!lsw2_extra_offered(flag)) continue;
        menu.add(toggle(lsw2_extra_name(flag), "OFF", "ON", s.extras[flag] ? 1 : 0));
    }
    menu.add([] { Lsw2MenuRow r; r.label = "Return"; return r; }());
}

void lsw2_extras_menu_sync(Lsw2Menu& menu, Lsw2HostSettings& s) {
    /* Walk the same filter the build used, so row order and flag order stay in
     * step without storing a second mapping that could fall out of date. */
    int row = 0;
    for (int flag = 0; flag < kLsw2ExtrasCount; ++flag) {
        if (!lsw2_extra_offered(flag)) continue;
        if (row >= menu.row_count()) break;
        s.extras[flag] = menu.row(row).value != 0;
        ++row;
    }
}

/* ---------------------------------------------------------------- persistence
 * See hostsettings.h for why these exist: the fork's pause menu hides Save Game, so what the host
 * configures has to survive on its own. */

/* THE MASK IS 32 BITS AND THE WIRE CARRIES FOUR BYTES OF IT, so an
 * ExtrasCount past 32 would shift out of range here and be truncated in
 * lsw2_host_settings_to_wire - silently, on the HOST side, so the host would
 * see an Extra enabled on their own menu and no client would ever get it.
 *
 * The host settings menu lets several Extras be on together and sends them to
 * each client as it joins, and that is the requirement this bound protects:
 * stacking is what makes the count matter, since one flag at a time would never
 * reach the top of the word. Eighteen today, and the whole point of the Extras
 * list is that it can grow.
 *
 * Caught by auditing the clause rather than by anything failing - the codec is
 * correct for eighteen and stays correct until the day it is not. */
static_assert(kLsw2ExtrasCount <= 32,
              "the extras mask is a uint32_t and the wire carries four bytes "
              "of it; widen both together or extras past bit 31 vanish "
              "between the host and its clients");

uint32_t lsw2_host_extras_mask(const Lsw2HostSettings& s) {
    uint32_t mask = 0;
    for (int flag = 0; flag < kLsw2ExtrasCount; ++flag)
        if (s.extras[flag]) mask |= 1u << flag;
    return mask;
}

void lsw2_host_extras_from_mask(Lsw2HostSettings& s, uint32_t mask) {
    for (int flag = 0; flag < kLsw2ExtrasCount; ++flag) {
        /* AN EXTRA THE HOST MAY NOT SEE MAY NOT BE SET EITHER. The design
         * hides eight of the eighteen, and a hand-edited mask is exactly the
         * way one of them would come back on - invisible on the menu and
         * active in the match, which is the worst of both. */
        s.extras[flag] = lsw2_extra_offered(flag) && (mask & (1u << flag)) != 0;
    }
}

bool lsw2_host_character_valid(int character_id) {
    /* 0x64 is the select table's "not available" padding,
     * and every real id in that table is below it. */
    return character_id >= 0 && character_id < 0x64;
}

void lsw2_host_settings_clamp(Lsw2HostSettings& s) {
    auto in_range = [](int v, int count) { return v >= 0 && v < count; };

    if (!in_range(int(s.roster), 5)) s.roster = Lsw2Roster::Hunters;
    if (!in_range(int(s.controls), 2)) s.controls = Lsw2HostControls::Enhanced;
    if (!in_range(int(s.customization), 3))
        s.customization = Lsw2HostCustomization::All;
    if (!in_range(int(s.music), 2)) s.music = Lsw2HostMusic::Default;
    if (!in_range(int(s.lobby_music), 3)) s.lobby_music = Lsw2LobbyMusic::Hunter;
    if (!in_range(int(s.lobby_time), 4)) s.lobby_time = Lsw2LobbyTime::S30;
    if (!lsw2_host_character_valid(s.character)) s.character = 20;

    /* THE MAP, and the round level a client may have adopted from the wire.
     * Both are handed to the level loader, and a level index the game has not
     * set up is what cost two days: the port passed 0, the game loaded Endor
     * and fell over. Anything that is not one of the fork's two arenas comes
     * back to Jabba's Tower; anything that is not a real level id, or is one
     * this build would not have announced, means no round. */
    if (s.map != kLsw2ArenaJabbaLevel && s.map != kLsw2ArenaMosEisleyLevel)
        s.map = kLsw2ArenaJabbaLevel;
    if (s.round_level != 0 && s.round_level != kLsw2ArenaJabbaLevel &&
        s.round_level != kLsw2ArenaMosEisleyLevel)
        s.round_level = 0;

    /* The same filter the mask loader applies, so a settings struct that
     * reached here another way is cleaned too. */
    for (int flag = 0; flag < kLsw2ExtrasCount; ++flag)
        if (!lsw2_extra_offered(flag)) s.extras[flag] = false;
}

/* ------------------------------------------------------------------- the wire
 * See hostsettings.h for what travels and what deliberately does not. */

int lsw2_host_settings_to_wire(const Lsw2HostSettings& s, uint8_t* out, int n) {
    if (!out || n < kLsw2HostWireBytes) return 0;

    const uint32_t extras = lsw2_host_extras_mask(s);
    out[0] = kLsw2HostWireVersion;
    out[1] = uint8_t(int(s.roster));
    out[2] = uint8_t(s.character);
    out[3] = uint8_t(int(s.controls));
    out[4] = uint8_t(int(s.customization));
    out[5] = uint8_t(int(s.music));
    out[6] = uint8_t(int(s.lobby_time));
    out[7] = uint8_t(extras & 0xFFu);
    out[8] = uint8_t((extras >> 8) & 0xFFu);
    out[9] = uint8_t((extras >> 16) & 0xFFu);
    out[10] = uint8_t((extras >> 24) & 0xFFu);
    /* The map the host chose, and whether a round is running on one. The
     * second is session state rather than a setting - see the field's note in
     * hostsettings.h - and it rides here because this is the payload that
     * already reaches every client. */
    out[11] = uint8_t(s.map);
    out[12] = uint8_t(s.round_level);
    out[13] = uint8_t(int(s.lobby_music));
    out[14] = s.suicide_no_kill ? 1u : 0u;
    return kLsw2HostWireBytes;
}

bool lsw2_host_settings_from_wire(Lsw2HostSettings& s, const uint8_t* in, int n) {
    if (!in || n < kLsw2HostWireBytes) return false;
    if (in[0] != kLsw2HostWireVersion) return false;

    s.roster = Lsw2Roster(int(in[1]));
    s.character = int(in[2]);
    s.controls = Lsw2HostControls(int(in[3]));
    s.customization = Lsw2HostCustomization(int(in[4]));
    s.music = Lsw2HostMusic(int(in[5]));
    s.lobby_time = Lsw2LobbyTime(int(in[6]));

    const uint32_t extras = uint32_t(in[7]) | (uint32_t(in[8]) << 8) |
                            (uint32_t(in[9]) << 16) | (uint32_t(in[10]) << 24);
    lsw2_host_extras_from_mask(s, extras);
    s.map = int(in[11]);
    s.round_level = int(in[12]);
    s.lobby_music = Lsw2LobbyMusic(int(in[13]));
    s.suicide_no_kill = in[14] != 0;

    /* CLAMPED, because this arrived from the network. Every value above is cast
     * straight to an enum that indexes the menu's own value arrays, and a
     * hostile or simply older peer could send anything. The same clamp the
     * settings file gets, for the same reason and a worse threat model. */
    lsw2_host_settings_clamp(s);
    return true;
}
