/* The relay's WebSocket layer (lsw2_wsrelay.h): the framing both ways, and a
 * client talking to a minimal server over a real loopback socket.
 *
 *   g++ -std=c++20 port/net/test_wsrelay.cpp port/net/lsw2_wsrelay.cpp -lws2_32
 *
 * The relay room itself (services/master-server/src/relay.js) is exercised end
 * to end against the deployed Worker, not here - this file needs no network. */

#include "lsw2_wsrelay.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using sock = SOCKET;
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using sock = int;
#  define CLOSESOCK ::close
#endif

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/* A server frame: unmasked, as a server sends. */
std::string server_frame(uint8_t op, const std::string& data, bool fin = true) {
    std::string f;
    f.push_back(char((fin ? 0x80 : 0) | op));
    if (data.size() < 126) {
        f.push_back(char(data.size()));
    } else {
        f.push_back(char(126));
        f.push_back(char(data.size() >> 8));
        f.push_back(char(data.size() & 0xFF));
    }
    return f + data;
}

/* A minimal echo server: answer the upgrade, then send back every client
 * message (unmasked) until a close. */
void echo_server(sock l) {
    sock c = ::accept(l, nullptr, nullptr);
    std::string head;
    char tmp[4096];
    while (head.find("\r\n\r\n") == std::string::npos) {
        const int k = ::recv(c, tmp, sizeof tmp, 0);
        if (k <= 0) { CLOSESOCK(c); return; }
        head.append(tmp, size_t(k));
    }
    const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\n\r\n";
    std::string first = ok + server_frame(1, "ready");   /* rides with the header */
    ::send(c, first.data(), int(first.size()), 0);
    std::string buf = head.substr(head.find("\r\n\r\n") + 4), partial;
    uint8_t pop = 0;
    for (;;) {
        std::vector<Lsw2WsMessage> got;
        lsw2_ws_parse(buf, partial, pop, &got);
        for (const Lsw2WsMessage& m : got) {
            if (m.opcode == 8) { CLOSESOCK(c); return; }
            const std::string f = server_frame(m.opcode, m.data);
            ::send(c, f.data(), int(f.size()), 0);
        }
        const int k = ::recv(c, tmp, sizeof tmp, 0);
        if (k <= 0) break;
        buf.append(tmp, size_t(k));
    }
    CLOSESOCK(c);
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    std::printf("relay websocket framing\n");

    /* A client frame, parsed as the server would: masked, and the mask undone. */
    {
        const std::string payload = "lockstep record";
        std::string f = lsw2_ws_frame(2, (const uint8_t*)payload.data(), payload.size(), 0xA1B2C3D4u);
        check((uint8_t(f[0]) == 0x82) && (uint8_t(f[1]) & 0x80), "a client frame is FIN, binary and masked");
        std::string partial;
        uint8_t pop = 0;
        std::vector<Lsw2WsMessage> got;
        check(lsw2_ws_parse(f, partial, pop, &got) && got.size() == 1 &&
              got[0].opcode == 2 && got[0].data == payload, "a masked frame parses back to its payload");
        check(f.empty(), "a whole frame is consumed");
    }

    /* Lengths at each encoding: 125, 126 (16-bit) and 70000 (64-bit). */
    for (size_t n : {size_t(125), size_t(126), size_t(70000)}) {
        std::string payload(n, 'q');
        for (size_t i = 0; i < n; ++i) payload[i] = char(i * 31);
        std::string f = lsw2_ws_frame(2, (const uint8_t*)payload.data(), n, 0x01020304u);
        std::string partial;
        uint8_t pop = 0;
        std::vector<Lsw2WsMessage> got;
        lsw2_ws_parse(f, partial, pop, &got);
        char what[64];
        std::snprintf(what, sizeof what, "a %zu-byte payload round-trips", n);
        check(got.size() == 1 && got[0].data == payload, what);
    }

    /* A stream split anywhere waits for the rest; continuation frames join. */
    {
        std::string stream = server_frame(2, "abc", false) + server_frame(0, "def", true) +
                             server_frame(1, "ping");
        std::string buf, partial;
        uint8_t pop = 0;
        std::vector<Lsw2WsMessage> got;
        for (char ch : stream) {           /* one byte at a time */
            buf.push_back(ch);
            lsw2_ws_parse(buf, partial, pop, &got);
        }
        check(got.size() == 2 && got[0].opcode == 2 && got[0].data == "abcdef",
              "a fragmented binary message is joined");
        check(got.size() == 2 && got[1].opcode == 1 && got[1].data == "ping",
              "a text message after it, fed byte by byte, arrives whole");
    }

    /* The client against a real socket: upgrade, the first message riding
     * with the response, and an echo. */
    {
        sock l = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t alen = sizeof a;
        ::bind(l, (sockaddr*)&a, sizeof a);
        ::listen(l, 1);
        ::getsockname(l, (sockaddr*)&a, &alen);
        std::thread server(echo_server, l);

        Lsw2WsClient ws;
        std::string why;
        const bool opened = ws.open("127.0.0.1", ntohs(a.sin_port), "/v1/relay/x?role=join", "",
                                    3000, &why);
        check(opened, "the client completes the upgrade");
        std::vector<Lsw2WsMessage> early;
        ws.take_early(&early);
        check(early.size() == 1 && early[0].data == "ready",
              "a message in the same read as the response is not lost");

        const uint8_t rec[132] = {1, 2, 3};
        ws.send_binary(rec, sizeof rec);
        std::vector<Lsw2WsMessage> back;
        for (int i = 0; i < 50 && back.empty(); ++i) ws.receive(&back);
        check(back.size() == 1 && back[0].data.size() == sizeof rec &&
              std::memcmp(back[0].data.data(), rec, sizeof rec) == 0,
              "a binary record echoes back intact");
        ws.close();
        server.join();
        CLOSESOCK(l);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
