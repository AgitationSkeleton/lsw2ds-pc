/* The C++ client against the real Python service.
 *
 * NOT AGAINST A MOCK, deliberately. The two halves were written from the same
 * contract but by different hands and in different languages, and the failure
 * this catches is the one a mock cannot: the two disagreeing about the contract.
 * A mock would agree with whichever half wrote it.
 *
 * Start the service first:
 *     python ../../services/master-server-selfhost/master_server.py --port 28553
 *
 * then:
 *     g++ -std=c++20 -I. test_master_client.cpp lsw2_master.cpp \
 *         -o test_master_client -lws2_32
 *     ./test_master_client [port]
 */

#include "lsw2_master.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#endif

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const int port = argc > 1 ? std::atoi(argv[1]) : 28553;

    std::printf("the flat-JSON readers, with no server involved\n");
    const std::string sample =
        "{\"name\":\"Maclunkey\",\"port\":28600,\"players\":3,\"code\":\"K7M2\"}";
    check("reads a string", lsw2_json_string(sample, "name") == "Maclunkey");
    check("reads a number", lsw2_json_number(sample, "port", 0) == 28600);
    check("missing key falls back", lsw2_json_number(sample, "nope", -7) == -7);
    check("a key that is a SUBSTRING of another is not confused",
          lsw2_json_number("{\"maxplayers\":9,\"players\":2}", "players", 0) == 2);
    check("escaped quotes survive",
          lsw2_json_string("{\"name\":\"a\\\"b\"}", "name") == "a\"b");

    std::printf("splitting an array into objects\n");
    /* THE CASE THAT BROKE THE OLD WALKER. It found each "name" key and read the
     * fields after it, which was sound while a server object was flat. A
     * roster carries "name" too, so it manufactured a phantom server out of a
     * player and then read that phantom's port from the NEXT real server. The
     * earlier test missed it, since the lobby it registered was empty. */
    const std::string nested =
        "{\"servers\":["
        "{\"name\":\"Maclunkey\",\"port\":28600,\"players\":2,"
        "\"roster\":[{\"slot\":0,\"name\":\"Han\"},{\"slot\":1,\"name\":\"Greedo\"}]},"
        "{\"name\":\"Cantina\",\"port\":28601,\"players\":1,\"roster\":[]}"
        "]}";
    const auto rows = lsw2_json_objects(nested, "servers");
    check("two servers, not four", rows.size() == 2);
    check("the first is the whole object",
          rows.size() == 2 && lsw2_json_string(rows[0], "name") == "Maclunkey");
    check("and its port is its own, not the next one's",
          rows.size() == 2 && lsw2_json_number(rows[0], "port", 0) == 28600);
    check("the second is not a player",
          rows.size() == 2 && lsw2_json_string(rows[1], "name") == "Cantina");
    check("the nested roster splits on its own",
          lsw2_json_objects(rows.empty() ? "" : rows[0], "roster").size() == 2);
    check("a missing array is empty, not an error",
          lsw2_json_objects(nested, "nope").empty());
    check("a brace inside a string is not counted",
          lsw2_json_objects("{\"a\":[{\"name\":\"}{\",\"port\":7}]}", "a").size() == 1);

    /* The heartbeat reply's directive array, parsed the same way. The DELIVERY
     * is exercised on the service side - an operator queues a kick from the
     * console window, and there is deliberately no HTTP endpoint that would let
     * a stranger do the same - so what is checked here is the reading. */
    const std::string beat =
        "{\"ok\":true,\"expires_in\":90,\"directives\":["
        "{\"action\":\"kick\",\"slot\":2,\"address\":\"10.0.0.4\"}]}";
    const auto orders = lsw2_json_objects(beat, "directives");
    check("a directive is found", orders.size() == 1);
    check("its action reads", orders.size() == 1 &&
          lsw2_json_string(orders[0], "action") == "kick");
    check("its slot reads", orders.size() == 1 &&
          lsw2_json_number(orders[0], "slot", -1) == 2);
    check("an ordinary beat carries none",
          lsw2_json_objects("{\"ok\":true,\"expires_in\":90}", "directives").empty());

    Lsw2MasterClient c;
    c.configure("127.0.0.1", port);

    std::printf("against the service on 127.0.0.1:%d\n", port);
    if (!c.reachable()) {
        std::printf("  SKIP  the service is not running (%s)\n",
                    c.last_error().c_str());
        std::printf("\n  start it with:\n"
                    "    python services/master-server-selfhost/master_server.py"
                    " --port %d\n", port);
        return 2;
    }
    check("health answered", true);

    check("registered a lobby", c.register_lobby("Maclunkey", 28600, 1));
    check("got a room code", c.room_code().size() == 4);

    check("the listing fetched", c.refresh());
    bool found = false;
    for (const auto& e : c.servers())
        if (e.name == "Maclunkey" && e.port == 28600) found = true;
    check("our lobby is in it", found);
    check("the address came from the socket",
          !c.servers().empty() && c.servers()[0].address == "127.0.0.1");

    Lsw2ServerEntry got;
    check("the room code resolves", c.resolve(c.room_code(), &got));
    check("to the right port", got.port == 28600);
    check("an unknown code is refused", !c.resolve("ZZZZ", &got));

    check("heartbeat accepted", c.heartbeat(3));
    check("the player count updated", c.refresh() && [&] {
        for (const auto& e : c.servers())
            if (e.name == "Maclunkey") return e.players == 3;
        return false;
    }());

    /* THE SANITISER, END TO END. The service allow-lists and cuts to ten; this
     * proves the client does not defeat that by mangling the JSON on the way
     * out - an unescaped quote here would have produced a different name or a
     * parse error rather than a trimmed one. */
    Lsw2MasterClient hostile;
    hostile.configure("127.0.0.1", port);
    check("a hostile name is accepted and cleaned",
          hostile.register_lobby("<b>\"; DROP TABLE servers--", 28601, 1));
    check("and comes back sanitised", c.refresh() && [&] {
        for (const auto& e : c.servers()) {
            if (e.port != 28601) continue;
            return e.name.size() <= 10 &&
                   e.name.find('"') == std::string::npos &&
                   e.name.find('<') == std::string::npos &&
                   e.name.find(';') == std::string::npos;
        }
        return false;
    }());
    hostile.unregister();

    std::printf("kinds - arena and co-op share one directory\n");
    Lsw2MasterClient coop;
    coop.configure("127.0.0.1", port);
    coop.set_kind(LSW2_KIND_COOP);
    check("a co-op lobby registers", coop.register_lobby("Cantina", 28602, 1));

    check("a co-op client lists only co-op", coop.refresh() && [&] {
        if (coop.servers().empty()) return false;
        for (const auto& e : coop.servers()) if (e.kind != LSW2_KIND_COOP) return false;
        return true;
    }());

    Lsw2MasterClient arena;
    arena.configure("127.0.0.1", port);
    arena.set_kind(LSW2_KIND_BOUNTY);
    check("a bounty client does not see it", arena.refresh() && [&] {
        for (const auto& e : arena.servers()) if (e.port == 28602) return false;
        return true;
    }());
    check("but the unfiltered list does", c.refresh() && [&] {
        for (const auto& e : c.servers()) if (e.port == 28602) return true;
        return false;
    }());

    std::printf("rosters - who the host says is in the room\n");
    std::vector<Lsw2RosterEntry> seats;
    seats.push_back({0, "Maclunkey", "127.0.0.1"});
    seats.push_back({1, "Han", "10.0.0.3"});
    coop.set_roster(seats);
    check("a heartbeat carries the roster", coop.heartbeat(2));
    check("and the names come back", coop.refresh() && [&] {
        for (const auto& e : coop.servers()) {
            if (e.port != 28602) continue;
            return e.roster.size() == 2 && e.roster[0] == "Maclunkey" &&
                   e.roster[1] == "Han";
        }
        return false;
    }());
    check("the lobby's own port survived the nesting", coop.refresh() && [&] {
        for (const auto& e : coop.servers()) if (e.port == 28602) return true;
        return false;
    }());
    check("a player was not listed as a server", [&] {
        for (const auto& e : coop.servers())
            if (e.name == "Han" || e.name == "Maclunkey") return false;
        return true;
    }());
    check("no player addresses reach the client", [&] {
        /* The directory redacts them on the public list; the entry has no field
         * to put one in either, which is the belt to that braces. */
        for (const auto& e : coop.servers())
            for (const auto& who : e.roster)
                if (who.find('.') != std::string::npos) return false;
        return true;
    }());
    coop.unregister();

    check("unregistered", c.unregister());
    check("and it left the listing", c.refresh() && [&] {
        for (const auto& e : c.servers()) if (e.name == "Maclunkey") return false;
        return true;
    }());

    std::printf("\n");
    if (g_failures) { std::printf("%d FAILED\n", g_failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
