/* The socket half of lsw2_upnp.h. The parsing lives in lsw2_upnp_parse.cpp.
 *
 * THREE REQUESTS, EACH ON A CLOCK. Discovery is a UDP broadcast and two HTTP
 * calls follow it, and every one of them runs on the frame thread the same way
 * the directory calls do - so every one of them is bounded. A router that does
 * not answer in a second is a router without UPnP, and the answer to that is to
 * host anyway and say so, not to stall the game while hoping.
 */

#include "lsw2_upnp.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define LSW2_INVALID INVALID_SOCKET
#  define lsw2_close closesocket
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#endif

namespace {

/* BUDGETS. Discovery is the long one because it waits for a reply that may not
 * come; the HTTP calls are to a device on the same wire and answer at once or
 * not at all. Together they are under two seconds, which is what a player pays
 * once when starting a lobby. */
/* HALVED, because a host froze on creating a lobby. This SSDP wait
 * runs synchronously on the FRAME thread from Lsw2ForkSession::start_lobby, so
 * every millisecond here is a millisecond the game is stopped. 600ms is enough
 * for a consumer router on the LAN to answer (they reply in well under that) and
 * halves the worst-case Start Lobby hitch. The real fix is to run the whole
 * lobby bring-up off the frame thread; this is the safe interim. */
constexpr int kDiscoverMs = 600;
constexpr int kHttpMs = 1500;

constexpr const char* kSsdpAddr = "239.255.255.250";
constexpr int kSsdpPort = 1900;

/* What is remembered between mapping and unmapping, so shutting a lobby down
 * does not have to discover the gateway all over again. */
std::string g_control_url;
std::string g_service;

void set_timeout(socket_t s, int ms) {
#ifdef _WIN32
    DWORD v = DWORD(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

/* One bounded HTTP request to a device on the LAN. `extra` carries any headers
 * beyond the basics; `body` empty means GET. Returns the response body, or an
 * empty string. `local_ip` receives this machine's address on the interface
 * that reached the gateway - which is the address the mapping must point at,
 * and is why it is taken from the socket rather than guessed by enumerating
 * interfaces and picking one. */
std::string http(const std::string& url, const std::string& body,
                 const std::string& extra, std::string* local_ip) {
    std::string host, path;
    int port = 0;
    if (!lsw2_upnp_split_url(url, &host, &port, &path)) return "";

    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == LSW2_INVALID) return "";
    set_timeout(s, kHttpMs);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(port));
    if (::inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) {
        lsw2_close(s);
        return "";
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) {
        lsw2_close(s);
        return "";
    }

    if (local_ip) {
        sockaddr_in me{};
#ifdef _WIN32
        int len = sizeof me;
#else
        socklen_t len = sizeof me;
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf);
            *local_ip = buf;
        }
    }

    std::string req = (body.empty() ? "GET " : "POST ") + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Connection: close\r\n";
    req += extra;
    if (!body.empty()) {
        req += "Content-Type: text/xml; charset=\"utf-8\"\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n" + body;

    if (::send(s, req.data(), int(req.size()), 0) < 0) {
        lsw2_close(s);
        return "";
    }

    std::string raw;
    char buf[4096];
    for (;;) {
        const int n = int(::recv(s, buf, sizeof buf, 0));
        if (n <= 0) break;
        raw.append(buf, size_t(n));
        /* A description is a few kilobytes and a SOAP reply less. The cap is
         * the same reasoning as the directory client's: a confused or hostile
         * device must not be able to make this allocate without bound. */
        if (raw.size() > 256 * 1024) break;
    }
    lsw2_close(s);

    const size_t split = raw.find("\r\n\r\n");
    return split == std::string::npos ? raw : raw.substr(split + 4);
}

/* SSDP M-SEARCH. Returns the LOCATION of the first gateway that answers. */
std::string discover() {
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == LSW2_INVALID) return "";
    set_timeout(s, kDiscoverMs);

    const std::string msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "\r\n";

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(kSsdpPort));
    ::inet_pton(AF_INET, kSsdpAddr, &to.sin_addr);

    if (::sendto(s, msg.data(), int(msg.size()), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof to) <= 0) {
        lsw2_close(s);
        return "";
    }

    /* SEVERAL REPLIES ARE NORMAL and most are not gateways - printers, media
     * servers and anything else on the subnet answer a multicast. Read a few
     * and take the first whose ST actually says InternetGatewayDevice rather
     * than trusting that the first datagram back is the right one. */
    for (int attempt = 0; attempt < 6; ++attempt) {
        char buf[2048];
        const int n = int(::recv(s, buf, sizeof buf - 1, 0));
        if (n <= 0) break;
        buf[n] = '\0';
        const std::string reply(buf, size_t(n));
        const std::string st = lsw2_upnp_header(reply, "ST");
        if (st.find("InternetGatewayDevice") == std::string::npos) continue;
        const std::string loc = lsw2_upnp_header(reply, "LOCATION");
        if (!loc.empty()) { lsw2_close(s); return loc; }
    }

    lsw2_close(s);
    return "";
}

}  // namespace

bool lsw2_upnp_map_port(int port, std::string* external_ip, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };

    if (port <= 0 || port > 65535) return fail("invalid port");

    const std::string location = discover();
    if (location.empty()) return fail("no UPnP gateway answered");

    std::string local;
    const std::string desc = http(location, "", "", &local);
    if (desc.empty()) return fail("the gateway sent no description");
    if (local.empty()) return fail("could not determine this machine's address");

    std::string service;
    const std::string control = lsw2_upnp_control_url(desc, &service);
    if (control.empty()) return fail("the gateway offers no WAN connection service");

    const std::string url = lsw2_upnp_resolve_url(location, control);
    if (url.empty()) return fail("the gateway's control URL made no sense");

    const std::string body = lsw2_upnp_add_body(service, port, local);
    const std::string action =
        "SOAPAction: \"" + service + "#AddPortMapping\"\r\n";
    const std::string reply = http(url, body, action, nullptr);

    /* A SOAP FAULT IS A REPLY, so an empty check is not enough. The router
     * answers 500 with a fault body when it refuses - commonly because another
     * device already holds the mapping - and treating that as success would
     * advertise a lobby nobody can reach. */
    if (reply.empty()) return fail("the gateway did not answer");
    if (reply.find("AddPortMappingResponse") == std::string::npos)
        return fail("the gateway refused the mapping");

    g_control_url = url;
    g_service = service;

    /* The WAN address, if it will say. Not fatal: the mapping is what matters,
     * and a host can still be reached by anyone who learns the address another
     * way. */
    if (external_ip) {
        const std::string ext_body =
            "<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:GetExternalIPAddress xmlns:u=\"" + service + "\"/>"
            "</s:Body></s:Envelope>";
        const std::string ext_action =
            "SOAPAction: \"" + service + "#GetExternalIPAddress\"\r\n";
        const std::string ext = http(url, ext_body, ext_action, nullptr);
        const size_t a = ext.find("<NewExternalIPAddress>");
        if (a != std::string::npos) {
            const size_t b = ext.find("</NewExternalIPAddress>", a);
            if (b != std::string::npos) {
                const size_t s0 = a + std::strlen("<NewExternalIPAddress>");
                *external_ip = ext.substr(s0, b - s0);
            }
        }
    }
    return true;
}

void lsw2_upnp_unmap_port(int port) {
    /* BEST EFFORT, AND SILENT. A router that rebooted has forgotten the mapping
     * already; a router that never accepted one has nothing to remove. Neither
     * is worth telling a player about while they are closing a lobby. */
    if (g_control_url.empty() || g_service.empty()) return;

    char body[768];
    std::snprintf(body, sizeof body,
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:DeletePortMapping xmlns:u=\"%s\">"
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>%d</NewExternalPort>"
        "<NewProtocol>TCP</NewProtocol>"
        "</u:DeletePortMapping></s:Body></s:Envelope>",
        g_service.c_str(), port);

    const std::string action =
        "SOAPAction: \"" + g_service + "#DeletePortMapping\"\r\n";
    http(g_control_url, body, action, nullptr);

    g_control_url.clear();
    g_service.clear();
}
