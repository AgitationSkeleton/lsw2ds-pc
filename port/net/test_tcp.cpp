/* A host and three joiners over real loopback sockets, in one process.
 *
 * The in-memory test proves the ALGORITHM; this proves the WIRE. Different
 * things break in each: framing, partial reads, seat assignment, and a peer
 * closing are all invisible to a bus that hands whole messages around.
 *
 *   g++ -std=c++20 -I. test_tcp.cpp lsw2_tcp.cpp lsw2_lockstep.cpp \
 *       -o test_tcp -lws2_32
 */

#include "lsw2_tcp.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

void fill(uint8_t* b, int station, int frame) {
    std::memset(b, 0, kLsw2ShareBytes);
    b[0] = uint8_t(station);
    b[1] = uint8_t(frame & 0xFF);
    b[kLsw2ShareBytes - 1] = 0x5A;
}

void settle(Lsw2TcpTransport* t[], int n, int rounds = 40) {
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < n; ++i) t[i]->service();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace

int main() {
    std::printf("wire format\n");
    check("132-byte records: 8 header + 124 payload",
          kLsw2RecordBytes == 132 && kLsw2ShareBytes == 124);

    Lsw2TcpTransport host;
    const uint16_t port = host.host(0);
    std::printf("host listening on port %u\n", port);
    check("the host bound a port", port != 0);
    if (!port) return 1;

    Lsw2TcpTransport c1, c2, c3;
    check("client 1 joined", c1.join("127.0.0.1", port));
    check("client 2 joined", c2.join("127.0.0.1", port));
    check("client 3 joined", c3.join("127.0.0.1", port));

    Lsw2TcpTransport* all[4] = {&host, &c1, &c2, &c3};
    settle(all, 4);

    std::printf("seating\n");
    check("client 1 was told its seat", c1.assigned_station() > 0);
    check("client 2 got a different seat",
          c2.assigned_station() > 0 &&
          c2.assigned_station() != c1.assigned_station());
    check("client 3 got a third seat",
          c3.assigned_station() > 0 &&
          c3.assigned_station() != c1.assigned_station() &&
          c3.assigned_station() != c2.assigned_station());
    check("the host is station 0", host.assigned_station() == 0);

    int joined = 0;
    while (host.take_joined() >= 0) ++joined;
    check("the host saw three joins", joined == 3);

    std::printf("a fifth station is refused at the door\n");
    Lsw2TcpTransport c4;
    c4.join("127.0.0.1", port);
    settle(all, 4, 20);
    c4.service();
    check("no fourth joiner was seated", host.take_joined() < 0);

    std::printf("blocks reach every station, with the right frame and sender\n");
    Lsw2LockstepSession sh(Lsw2NetRole::Host, 0, &host);
    Lsw2LockstepSession s1(Lsw2NetRole::Client, c1.assigned_station(), &c1);
    Lsw2LockstepSession s2(Lsw2NetRole::Client, c2.assigned_station(), &c2);
    Lsw2LockstepSession s3(Lsw2NetRole::Client, c3.assigned_station(), &c3);
    Lsw2LockstepSession* ses[4] = {&sh, &s1, &s2, &s3};
    const int seats[4] = {0, c1.assigned_station(),
                          c2.assigned_station(), c3.assigned_station()};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            ses[i]->seat(seats[j], "PLAYER", seats[j] == seats[i]);

    uint8_t out[kLsw2MaxStations][kLsw2ShareBytes];
    uint16_t bitmap = 0;
    bool complete = false, content_ok = true;
    for (int f = 0; f < 60; ++f) {
        for (int i = 0; i < 4; ++i) all[i]->service();
        for (int i = 0; i < 4; ++i) {
            uint8_t blk[kLsw2ShareBytes];
            fill(blk, seats[i], ses[i]->frame());
            if (ses[i]->step(blk, out, &bitmap) && bitmap == 0xF) {
                complete = true;
                for (int j = 0; j < kLsw2MaxStations; ++j) {
                    if (out[j][0] != j) content_ok = false;
                    if (out[j][kLsw2ShareBytes - 1] != 0x5A) content_ok = false;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    check("every station's block arrived at every station", complete);
    check("payloads survived the wire intact", content_ok);

    std::printf("the shared random seed\n");
    {
        /* Without this every machine draws from an MT19937 seeded off its own
         * real-time clock, so a match diverges on the first random number. */
        uint32_t got = 0;
        check("nobody has a seed yet", !c1.take_seed(&got));

        host.send_seed(0xC0FFEE01u);
        settle(all, 4, 30);
        check("a client receives it", c1.take_seed(&got) && got == 0xC0FFEE01u);
        check("every client does",
              c2.take_seed(&got) && got == 0xC0FFEE01u);
        check("and taking it twice yields nothing", !c1.take_seed(&got));

        /* A CLIENT MAY NOT CHOOSE WHAT EVERYONE ROLLS. Same authority as the
         * settings record, and worth its own check because the consequence is
         * subtler: a seed accepted from a client would let one player decide
         * every other player's match. */
        c1.send_seed(0xDEADBEEFu);
        settle(all, 4, 30);
        check("a client's seed reaches the host not at all",
              !host.take_seed(&got));
        check("nor the other clients", !c2.take_seed(&got));

        /* And it is not a block. */
        host.send_seed(0x12345678u);
        settle(all, 4, 30);
        int frame = -1, from = -1;
        uint8_t block[kLsw2ShareBytes];
        bool leaked = false;
        while (c1.poll(&frame, &from, block))
            if (block[0] == 0x78 && block[1] == 0x56) leaked = true;
        check("the seed never arrives as a block", !leaked);
        c1.take_seed(&got); c2.take_seed(&got); c3.take_seed(&got);
    }

    std::printf("names travel both ways, and a client may only name itself\n");
    {
        /* The "<Playername> has joined the game!" popup and the Kick list both draw
         * a name; until this record existed they drew "Player 1". */
        host.send_name("Maclunkey");
        c1.send_name("Han");
        c2.send_name("Greedo");
        settle(all, 4, 40);

        int station = -1;
        char got[24] = {0};

        bool host_learned_han = false, host_learned_greedo = false;
        while (host.take_name(&station, got, sizeof got)) {
            if (station == seats[1] && std::strcmp(got, "Han") == 0)
                host_learned_han = true;
            if (station == seats[2] && std::strcmp(got, "Greedo") == 0)
                host_learned_greedo = true;
        }
        check("the host learns a client's name", host_learned_han);
        check("and every client's", host_learned_greedo);

        bool c1_learned_host = false, c1_learned_c2 = false;
        while (c1.take_name(&station, got, sizeof got)) {
            if (station == seats[0] && std::strcmp(got, "Maclunkey") == 0)
                c1_learned_host = true;
            if (station == seats[2] && std::strcmp(got, "Greedo") == 0)
                c1_learned_c2 = true;
        }
        check("a client learns the host's name", c1_learned_host);
        check("and another client's, through the relay", c1_learned_c2);

        check("an empty queue says so", !c1.take_name(&station, got, sizeof got));
        check("and a null destination is refused",
              !c1.take_name(&station, nullptr, 24));

        /* A CLIENT MAY NAME ITSELF AND NOBODY ELSE. The host stamps the peer's
         * own station over whatever arrived, so this cannot rename a rival. */
        c1.send_name("Impostor");
        settle(all, 4, 30);
        bool renamed_someone_else = false;
        while (host.take_name(&station, got, sizeof got))
            if (station != seats[1]) renamed_someone_else = true;
        check("a client cannot rename another station", !renamed_someone_else);

        /* Drain what the relay left in the other clients. */
        while (c1.take_name(&station, got, sizeof got)) {}
        while (c2.take_name(&station, got, sizeof got)) {}
        while (c3.take_name(&station, got, sizeof got)) {}

        /* THE HOST CAN REPEAT A NAME FOR SOMEBODY ELSE, which is what a late
         * joiner needs: names are announced once, so the third player to arrive
         * never saw the second player's record. */
        host.send_name_as(seats[1], "Han");
        settle(all, 4, 30);
        bool c3_learned_c1 = false;
        while (c3.take_name(&station, got, sizeof got))
            if (station == seats[1] && std::strcmp(got, "Han") == 0)
                c3_learned_c1 = true;
        check("the host can name a station for a late joiner", c3_learned_c1);

        /* AND A CLIENT CANNOT. Same authority, stated twice because this is a
         * second door into it. */
        c2.send_name_as(seats[1], "Nobody");
        settle(all, 4, 30);
        bool leaked = false;
        while (host.take_name(&station, got, sizeof got)) leaked = true;
        while (c3.take_name(&station, got, sizeof got)) leaked = true;
        check("a client naming another station sends nothing at all", !leaked);

        while (c1.take_name(&station, got, sizeof got)) {}
        while (c2.take_name(&station, got, sizeof got)) {}
    }

    std::printf("every block says who its sender is playing as\n");
    {
        /* The host's roster rules need every machine to agree on who each station is,
         * and remote avatars need to know which model to draw. The record's
         * header had two spare bytes; one of them now carries it. */
        check("nobody has stated a character yet",
              host.peer_character(1) == -1 && c1.peer_character(0) == -1);

        host.set_local_character(20);         /* Greedo */
        c1.set_local_character(29);           /* Jango Fett */
        c2.set_local_character(0);            /* 4-LOM, whose id is ZERO */

        for (int f = 0; f < 4; ++f) {
            uint8_t blk[kLsw2ShareBytes] = {0};
            host.send(200 + f, seats[0], blk);
            c1.send(200 + f, seats[1], blk);
            c2.send(200 + f, seats[2], blk);
            settle(all, 4, 20);
        }

        check("a client learns the host's character",
              c1.peer_character(seats[0]) == 20);
        check("the host learns a client's", host.peer_character(seats[1]) == 29);
        check("and a client learns another client's, through the relay",
              c1.peer_character(seats[2]) == 0);
        check("character id ZERO is a character, not 'unstated'",
              host.peer_character(seats[2]) == 0);
        check("a station reads its own back",
              host.peer_character(seats[0]) == 20 &&
              c1.peer_character(seats[1]) == 29);
        check("a station nobody has spoken for is still unstated",
              host.peer_character(3) == -1 || seats[3] != 3);
        check("an out-of-range station is refused",
              host.peer_character(-1) == -1 && host.peer_character(9) == -1);

        /* 0xFF is the "not stated" value, so it must not be settable as a
         * character - otherwise a peer could claim to be nobody. */
        c1.set_local_character(0xFF);
        settle(all, 4, 10);
        uint8_t blk[kLsw2ShareBytes] = {0};
        c1.send(300, seats[1], blk);
        settle(all, 4, 20);
        check("0xFF reads back as unstated, not as character 255",
              host.peer_character(seats[1]) == -1);
        c1.set_local_character(29);
    }

    std::printf("host settings reach the clients, and only from the host\n");
    {
        /* The host's Extras are communicated to player clients when they
         * join. The record rides flags bit 1, carries no block, and must not
         * appear as one. */
        uint8_t payload[16] = {0};
        for (int i = 0; i < 16; ++i) payload[i] = uint8_t(0xA0 + i);
        host.broadcast_settings(payload, sizeof payload);
        settle(all, 4, 30);

        uint8_t got[16] = {0};
        check("a client receives the record", c1.take_settings(got, sizeof got));
        check("with its payload intact",
              std::memcmp(got, payload, sizeof payload) == 0);
        check("every client receives it", c2.take_settings(got, sizeof got) &&
              std::memcmp(got, payload, sizeof payload) == 0);
        check("taking it twice yields nothing - it is not a queue",
              !c1.take_settings(got, sizeof got));

        /* A CLIENT MAY NOT DICTATE THE RULES. broadcast_settings is host-only,
         * so this must do nothing at all rather than reach the host or the
         * other clients. */
        uint8_t hostile[16] = {0};
        for (int i = 0; i < 16; ++i) hostile[i] = 0x5B;
        c1.broadcast_settings(hostile, sizeof hostile);
        settle(all, 4, 30);
        check("a client's broadcast reaches the host not at all",
              !host.take_settings(got, sizeof got));
        check("nor the other clients", !c2.take_settings(got, sizeof got));

        /* THE RECORD IS NOT A BLOCK. If it leaked into the lockstep's inbox the
         * next round would step on a frame nobody sent. */
        host.broadcast_settings(payload, sizeof payload);
        settle(all, 4, 30);
        int frame = -1, from = -1;
        uint8_t block[kLsw2ShareBytes];
        bool leaked = false;
        while (c1.poll(&frame, &from, block))
            if (std::memcmp(block, payload, sizeof payload) == 0) leaked = true;
        check("and never arrives as one", !leaked);
        c1.take_settings(got, sizeof got);
    }

    std::printf("the host knows where each peer connected from\n");
    /* THE ROSTER'S BAN IP HAS NOTHING TO ACT ON WITHOUT THIS. The directory
     * never sees a joiner - joiners connect here - so the host is the only
     * party that can report a player's address, and it must come from accept()
     * rather than from anything the peer claims about itself. */
    check("a seated peer has an address",
          host.peer_address(seats[1]) == "127.0.0.1");
    check("every seat does",
          host.peer_address(seats[2]) == "127.0.0.1" &&
          host.peer_address(seats[3]) == "127.0.0.1");
    check("an unknown station has none", host.peer_address(99).empty());
    check("and so does the host's own seat", host.peer_address(0).empty());
    check("a CLIENT knows nobody's - it only dialled the host",
          c1.peer_address(seats[2]).empty());

    std::printf("a departed seat forgets what it was playing as\n");
    {
        c3.set_local_character(8);            /* Bossk */
        uint8_t blk[kLsw2ShareBytes] = {0};
        c3.send(400, seats[3], blk);
        settle(all, 4, 20);
        check("the host learned it", host.peer_character(seats[3]) == 8);

        c3.close();
        settle(all, 3, 40);
        /* Seats are reused, so a stale character would put the departed
         * player's model on whoever takes the seat next - for one frame, which
         * reads as a graphics glitch rather than as uncleared state. */
        check("and forgets it when the peer goes",
              host.peer_character(seats[3]) == -1);
        /* The loss itself is LEFT IN THE QUEUE for the next section, which
         * asserts on it. Draining it here would make that section fail for a
         * reason that has nothing to do with what it tests. */
    }

    std::printf("a peer that goes away is noticed\n");
    settle(all, 3, 60);
    int lost = host.take_lost();
    check("the host saw the loss", lost >= 0);
    check("and it was the station that left", lost == seats[3]);

    std::printf("\n");
    if (g_failures) { std::printf("%d FAILED\n", g_failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
