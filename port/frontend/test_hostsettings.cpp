/* The host settings menu, asserted against its design sentence by sentence.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_hostsettings.cpp \
 *       port/frontend/hostsettings.cpp port/frontend/gamemenu.cpp \
 *       -o test_hostsettings && ./test_hostsettings
 */

#include "hostsettings.h"

/* the two arena level ids the Map row and the round announcement use */
#include "levels.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

void cursor_to(Lsw2Menu& m, int target) {
    for (int i = 0; i < kLsw2MenuMaxRows * 2 && m.cursor() != target; ++i)
        m.step(kLsw2KeyDown);
}

/* The eight the design names as hidden, word for word. */
const char* kHiddenExtras[] = {
    "Invincibility", "Minikit Detector", "Tiny Enemies", "Ewok Invasion",
    "Pink", "Sunglasses", "Droid Invasion", "Moustaches",
};

}  // namespace

int main() {
    std::printf("host settings\n");

    Lsw2HostSettings s;
    Lsw2Menu m;
    lsw2_host_menu_build(m, s);

    /* "and no Map": this label said "plus the Map the round needs" after the
     * Map row was removed - the count it checks was right and the
     * words beside it were not, and a green line that describes a row which
     * no longer exists teaches the reader to distrust the green ones. */
    check(m.row_count() == kHostRowCount,
          "every row the design lists, and no Map");

    /* --- the defaults it states outright ---------------------------------- */
    check(std::strcmp(m.value_of(kHostControls), "ENHANCED") == 0,
          "Controls default enhanced");
    check(std::strcmp(m.value_of(kHostCustomization), "ALL") == 0,
          "Customization defaulting to all");
    check(std::strcmp(m.value_of(kHostLobbyTime), "30s") == 0,
          "LOBBY TIME default 30s");
    check(s.lobby_seconds() == 30, "and 30s means thirty seconds");
    check(std::strcmp(m.value_of(kHostRoster), "Hunters") == 0,
          "Roster starts on Hunters");
    check(m.value_of(kHostStart) == nullptr, "Start Lobby is an action");

    /* --- NO MAP ROW. Maps are not selectable in lobby setup, because
     *      starting the game is what runs the normal Bounty Hunter pre-game
     *      phase of map, rule and character select. The design lists this
     *      menu's rows and Map is not among them; the row was an invention.
     *      This check is what stops it coming back. ------------------------ */
    check(m.row_count() == kHostRowCount,
          "the host menu has exactly the rows the design lists");
    for (int i = 0; i < m.row_count(); ++i)
        check(std::strcmp(m.row(i).label, "Map") != 0,
              "and none of them is a Map picker");

    /* --- "If set to One, another setting below it unhides" ---------------- */
    check(m.row(kHostCharacter).hidden, "Character is hidden under Hunters");
    cursor_to(m, kHostRoster);
    for (int i = 0; i < 4; ++i) m.step(kLsw2KeyRight);
    lsw2_host_menu_sync(m, s);
    check(s.roster == Lsw2Roster::One, "Roster reaches One");
    check(!m.row(kHostCharacter).hidden, "and Character unhides");

    /* "Toggling Roster off of One re-hides and ignores this setting." */
    cursor_to(m, kHostRoster);
    m.step(kLsw2KeyRight);            /* One -> wraps to Hunters */
    lsw2_host_menu_sync(m, s);
    check(s.roster == Lsw2Roster::Hunters, "right from One wraps to Hunters");
    check(m.row(kHostCharacter).hidden, "and Character re-hides");

    /* --- "changes to Extras: <ON> if any are enabled" --------------------- */
    check(std::strcmp(m.value_of(kHostExtras), "OFF") == 0,
          "Extras reads OFF with none set");
    s.extras[7] = true;               /* Old Time */
    lsw2_host_menu_sync(m, s);
    check(std::strcmp(m.value_of(kHostExtras), "ON") == 0,
          "and ON once one is enabled");
    check(s.any_extra(), "any_extra agrees");

    /* --- the Extras submenu ----------------------------------------------- */
    Lsw2Menu ex;
    lsw2_extras_menu_build(ex, s);
    /* Ten offered plus Return. */
    check(ex.row_count() == 11, "the Extras submenu offers ten plus Return");

    for (const char* hidden : kHiddenExtras) {
        bool found = false;
        for (int i = 0; i < ex.row_count(); ++i)
            if (ex.row(i).label && std::strcmp(ex.row(i).label, hidden) == 0)
                found = true;
        char what[96];
        std::snprintf(what, sizeof what, "\"%s\" is not offered", hidden);
        check(!found, what);
    }

    /* The ones it must offer, by flag number (string id - 0x243). */
    check(lsw2_extra_offered(0) && lsw2_extra_offered(9) && lsw2_extra_offered(17),
          "Slippery Floors, Medieval and Walkie-Talkie are offered");
    check(!lsw2_extra_offered(1) && !lsw2_extra_offered(15),
          "Invincibility and Pink are not");

    /* "Extras can be stacked" - more than one at a time. */
    s.extras[2] = true;               /* Disco, alongside Old Time */
    lsw2_extras_menu_build(ex, s);
    lsw2_extras_menu_sync(ex, s);
    check(s.extras[7] && s.extras[2], "two Extras stay set at once");

    /* The submenu's rows must map back to the right flags after a round trip. */
    lsw2_extras_menu_build(ex, s);
    cursor_to(ex, 0);
    ex.step(kLsw2KeyRight);           /* toggle Slippery Floors on */
    lsw2_extras_menu_sync(ex, s);
    check(s.extras[0], "toggling the first row sets flag 0");
    check(s.extras[7], "and does not disturb flag 7");

    /* --- lobby time values ------------------------------------------------ */
    s.lobby_time = Lsw2LobbyTime::Off;
    check(s.lobby_seconds() == 0, "OFF is zero seconds");
    s.lobby_time = Lsw2LobbyTime::S300;
    check(s.lobby_seconds() == 300, "300s is three hundred");

    /* --- persistence: with Save Game hidden, "settings ... are to be persistent" */
    {
        Lsw2HostSettings a;
        a.visible_master = false;
        a.roster = Lsw2Roster::One;
        a.character = 68;                 /* an absurd pick, which is allowed */
        a.controls = Lsw2HostControls::Tank;
        a.customization = Lsw2HostCustomization::Hats;
        a.music = Lsw2HostMusic::Random;
        a.lobby_time = Lsw2LobbyTime::S300;
        for (int f = 0; f < kLsw2ExtrasCount; ++f) a.extras[f] = false;
        a.extras[7] = true;

        const uint32_t mask = lsw2_host_extras_mask(a);
        check((mask & (1u << 7)) != 0, "the mask carries a set Extra");
        check(mask == (1u << 7), "and carries only what was set");

        Lsw2HostSettings b;
        lsw2_host_extras_from_mask(b, mask);
        check(b.extras[7], "and it comes back");
        check(!b.extras[0], "with the rest still off");

        /* AN EXTRA THE HOST MAY NOT SEE MUST NOT COME BACK ON. A hand-edited
         * mask is exactly how one of the eight hidden ones would end up active
         * in a match while invisible on the menu. */
        int hidden = -1;
        for (int f = 0; f < kLsw2ExtrasCount; ++f)
            if (!lsw2_extra_offered(f)) { hidden = f; break; }
        check(hidden >= 0, "there is at least one hidden Extra to test with");
        Lsw2HostSettings c;
        lsw2_host_extras_from_mask(c, 0xFFFFFFFFu);
        check(hidden < 0 || !c.extras[hidden],
              "an all-ones mask does not enable a hidden Extra");
        check(c.extras[7], "but does enable an offered one");
    }

    /* --- the clamp, because settings.json is hand-editable ---------------- */
    {
        Lsw2HostSettings bad;
        bad.roster = Lsw2Roster(99);
        bad.controls = Lsw2HostControls(-3);
        bad.customization = Lsw2HostCustomization(7);
        bad.music = Lsw2HostMusic(4);
        bad.lobby_time = Lsw2LobbyTime(12);
        bad.character = 0x64;             /* the "not available" padding */
        lsw2_host_settings_clamp(bad);

        check(bad.roster == Lsw2Roster::Hunters, "a wild roster clamps to Hunters");
        check(bad.controls == Lsw2HostControls::Enhanced,
              "controls clamp to the design's default, ENHANCED");
        check(bad.customization == Lsw2HostCustomization::All,
              "customization clamps to ALL, the design's default");
        check(bad.music == Lsw2HostMusic::Default, "music clamps to DEFAULT");
        check(bad.lobby_time == Lsw2LobbyTime::S30,
              "lobby time clamps to 30s, the design's default");
        check(bad.character == 20, "the padding id clamps back to Greedo");

        check(!lsw2_host_character_valid(0x64), "0x64 is the padding, not a pick");
        check(!lsw2_host_character_valid(-1), "and neither is a negative id");
        check(lsw2_host_character_valid(68), "but Gonk Droid is fair game");
        check(lsw2_host_character_valid(20), "and so is Greedo");

        /* A struct that is already legal must survive untouched. */
        Lsw2HostSettings good;
        good.roster = Lsw2Roster::RandomAll;
        good.lobby_time = Lsw2LobbyTime::Off;
        lsw2_host_settings_clamp(good);
        check(good.roster == Lsw2Roster::RandomAll, "a legal roster is left alone");
        check(good.lobby_time == Lsw2LobbyTime::Off,
              "and OFF is legal, not a zero to be corrected");
    }

    /* --- the wire: Extras are "communicated to player clients when they join" */
    {
        Lsw2HostSettings host;
        host.visible_master = false;      /* host-local; must NOT travel */
        host.visible_lan = false;
        host.roster = Lsw2Roster::RandomHunter;
        host.character = 68;
        host.controls = Lsw2HostControls::Tank;
        host.customization = Lsw2HostCustomization::Off;
        host.music = Lsw2HostMusic::Random;
        host.lobby_time = Lsw2LobbyTime::S60;
        for (int f = 0; f < kLsw2ExtrasCount; ++f) host.extras[f] = false;
        host.extras[7] = true;
        host.map = kLsw2ArenaMosEisleyLevel;
        host.round_level = kLsw2ArenaMosEisleyLevel;

        uint8_t wire[kLsw2HostWireBytes] = {0};
        check(lsw2_host_settings_to_wire(host, wire, sizeof wire) ==
              kLsw2HostWireBytes, "the record is written");
        check(wire[0] == kLsw2HostWireVersion, "and carries its version first");

        Lsw2HostSettings client;
        client.visible_master = true;     /* the client's own choices */
        client.visible_lan = true;
        check(lsw2_host_settings_from_wire(client, wire, sizeof wire),
              "and reads back");

        check(client.roster == Lsw2Roster::RandomHunter, "roster travelled");
        check(client.character == 68, "the One character travelled");
        check(client.controls == Lsw2HostControls::Tank, "controls travelled");
        check(client.customization == Lsw2HostCustomization::Off,
              "the customization rule travelled - the host's authority");
        check(client.music == Lsw2HostMusic::Random, "music travelled");
        check(client.lobby_time == Lsw2LobbyTime::S60, "lobby time travelled");
        check(client.extras[7] && !client.extras[0], "the Extras travelled");

        /* THE ROUND CROSSES HERE OR NOWHERE. Measured 2026-09-02: the host
         * warped to level 4 and the client sat in the lobby, because
         * lsw2_netmsg_start_round writes only the LOCAL guest's receive buffer
         * despite its name. This payload is the only thing that reaches a
         * peer at round start, and roundfollow.cpp acts on what it finds
         * here. */
        check(client.map == kLsw2ArenaMosEisleyLevel, "the host's map travelled");
        check(client.round_level == kLsw2ArenaMosEisleyLevel,
              "and so does the round the client has to follow into");

        /* THE ADVERTISING FLAGS MUST NOT. A client that adopted them would
         * believe it was listing a lobby it does not host. */
        check(client.visible_master, "visible_master is left alone");
        check(client.visible_lan, "visible_lan is left alone");

        /* A record from another build is refused, not misread. */
        uint8_t stale[kLsw2HostWireBytes] = {0};
        lsw2_host_settings_to_wire(host, stale, sizeof stale);
        stale[0] = uint8_t(kLsw2HostWireVersion + 1);
        Lsw2HostSettings untouched;
        const Lsw2Roster before = untouched.roster;
        check(!lsw2_host_settings_from_wire(untouched, stale, sizeof stale),
              "a record from a different version is refused");
        check(untouched.roster == before, "and changes nothing");

        check(!lsw2_host_settings_from_wire(untouched, wire, 3),
              "a short record is refused");
        check(!lsw2_host_settings_from_wire(untouched, nullptr, 99),
              "and so is a null one");
        check(lsw2_host_settings_to_wire(host, wire, 3) == 0,
              "writing into too little space writes nothing");

        /* A HOSTILE RECORD IS CLAMPED, not trusted. Every byte here indexes an
         * enum the menu will read back. */
        uint8_t evil[kLsw2HostWireBytes] = {0};
        evil[0] = kLsw2HostWireVersion;
        for (int i = 1; i < kLsw2HostWireBytes; ++i) evil[i] = 0xFF;
        Lsw2HostSettings victim;
        check(lsw2_host_settings_from_wire(victim, evil, sizeof evil),
              "a wild record is accepted at the version check");
        check(victim.roster == Lsw2Roster::Hunters, "but its roster is clamped");
        check(victim.customization == Lsw2HostCustomization::All,
              "and its customization rule is");
        check(victim.character == 20, "and its character falls back to Greedo");
        /* THESE TWO ARE HANDED TO THE LEVEL LOADER, which is the one place a
         * wild value has already cost this project two days - the port passed
         * level 0, the game loaded Endor and fell over. 0xFF is not an arena. */
        check(victim.map == kLsw2ArenaJabbaLevel, "a wild map comes back to Jabba");
        check(victim.round_level == 0,
              "and a wild round level means no round rather than a bad warp");
        check([&] {
            for (int f = 0; f < kLsw2ExtrasCount; ++f)
                if (victim.extras[f] && !lsw2_extra_offered(f)) return false;
            return true;
        }(), "and no hidden Extra survives an all-ones mask");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (!g_failures) std::printf("all checks passed\n");
    return g_failures ? 1 : 0;
}
