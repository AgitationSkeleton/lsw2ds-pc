/* See lsw2_master.h. */

#include "lsw2_master.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define LSW2_INVALID INVALID_SOCKET
#  define lsw2_close closesocket
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#endif

namespace {

/* Find `"key"` at the top level and return where its value starts. Returns
 * npos when absent.
 *
 * NOT A JSON PARSER, and it does not pretend to be. The responses this talks to
 * are one flat object or one array of them; a real parser would be more code
 * than the thing it parses. What it must NOT do is find a key inside a nested
 * value and report it as top level, which is why the search includes the
 * quotes and the colon. */
size_t value_at(const std::string& json, const char* key, size_t from = 0) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t i = json.find(needle, from);
    if (i == std::string::npos) return std::string::npos;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size() || json[i] != ':') return std::string::npos;
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    return i;
}

std::string escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c >= 0x20) out.push_back(c);
        /* Control characters are dropped rather than escaped: nothing this
         * client sends should contain one, and a name that does is a name the
         * game could not draw anyway. */
    }
    return out;
}

}  // namespace

std::string lsw2_json_string(const std::string& json, const char* key) {
    size_t i = value_at(json, key);
    if (i == std::string::npos || i >= json.size() || json[i] != '"') return {};
    ++i;
    std::string out;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        out.push_back(json[i++]);
    }
    return out;
}

long long lsw2_json_number(const std::string& json, const char* key,
                           long long fallback) {
    size_t i = value_at(json, key);
    if (i == std::string::npos) return fallback;
    char* end = nullptr;
    const long long v = std::strtoll(json.c_str() + i, &end, 10);
    if (end == json.c_str() + i) return fallback;
    return v;
}

std::vector<std::string> lsw2_json_objects(const std::string& json,
                                           const char* array_key) {
    /* SPLIT BY BRACE DEPTH, NOT BY KEY NAME.
     *
     * The first version of the listing walker found each "name" key and read
     * the fields after it. That was sound while a server object was flat, and
     * it broke the day the directory started reporting a roster: a player entry
     * carries a "name" too, so the walker manufactured an extra "server" out of
     * a player, then read its port from the NEXT server along. Nothing caught
     * it, because the test registered a lobby with no players in it.
     *
     * Depth counting is barely more code and cannot be fooled by a nested
     * object that happens to share a key. Strings are tracked so a brace inside
     * a player name is not counted - names are allow-listed at the service, but
     * a client should not depend on the service's sanitiser for its own parser
     * to terminate.
     */
    std::vector<std::string> out;

    size_t at = 0;
    if (array_key && *array_key) {
        const std::string needle = std::string("\"") + array_key + "\"";
        at = json.find(needle);
        if (at == std::string::npos) return out;
        at = json.find('[', at);
        if (at == std::string::npos) return out;
        ++at;
    }

    int depth = 0;
    size_t start = std::string::npos;
    bool in_string = false;
    for (size_t i = at; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (c == '\\') ++i;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0 && --depth == 0 && start != std::string::npos) {
                out.push_back(json.substr(start, i - start + 1));
                start = std::string::npos;
            }
        } else if (c == ']' && depth == 0 && array_key && *array_key) {
            break;                 /* end of the named array */
        }
    }
    return out;
}

void Lsw2MasterClient::configure(const std::string& host, int port) {
    host_ = host;
    port_ = port;
}

namespace {

/* HOW LONG ANY ONE DIRECTORY CALL MAY COST A FRAME.
 *
 * Two seconds to connect, two to talk. A directory that cannot answer in that
 * is a directory that is down, and the answer to "is it down" is worth two
 * dropped frames once - not twenty seconds of frozen game, and certainly not
 * twenty seconds every second while a heartbeat retries.
 *
 * These are deliberately short rather than generous. Every caller already
 * treats failure as non-fatal: hosting continues without the directory and says
 * so, and the browser reports that it cannot be reached. Waiting longer would
 * buy a slightly better chance of reaching a very slow server at the cost of
 * the game being unresponsive while it tried. */
/* CAPPED, because a host froze on creating a lobby. These run
 * synchronously on the FRAME thread from start_lobby (register), View Master
 * Server (refresh) and Join (resolve), so an UNREACHABLE master stalls the game
 * for the full budget. A reachable host - localhost or a live directory -
 * completes its TCP handshake and reply in far less than these, so the smaller
 * budgets do not affect real connections; they only shorten the failure of an
 * unreachable one. The real fix is to run these off the frame thread. */
constexpr int kConnectMs = 900;
constexpr int kIoMs = 1000;

void set_timeouts(socket_t s) {
#ifdef _WIN32
    DWORD ms = kIoMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
               sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms),
               sizeof ms);
#else
    timeval tv{};
    tv.tv_sec = kIoMs / 1000;
    tv.tv_usec = (kIoMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

/* A BOUNDED CONNECT, which SO_SNDTIMEO does not give you - the send timeout
 * applies to sends, and a blocking connect() ignores it entirely. The only
 * portable way to put a clock on it is to make the socket non-blocking, start
 * the connect, and wait on select. */
bool connect_within(socket_t s, const sockaddr* addr, int len) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

    bool ok = false;
    if (connect(s, addr, len) == 0) {
        ok = true;                       /* connected immediately - loopback */
    } else {
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv{};
        tv.tv_sec = kConnectMs / 1000;
        tv.tv_usec = (kConnectMs % 1000) * 1000;
        if (select(int(s) + 1, nullptr, &wr, nullptr, &tv) > 0) {
            /* SELECT SAYING "WRITABLE" IS NOT THE SAME AS "CONNECTED". A
             * refused connection also makes the socket writable, and the only
             * way to tell them apart is to ask for the pending error. */
            int err = 0;
#ifdef _WIN32
            int elen = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err),
                       &elen);
#else
            socklen_t elen = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
#endif
            ok = (err == 0);
        }
    }

    /* BACK TO BLOCKING for the send and recv below, which rely on the
     * SO_*TIMEO bounds rather than on another select. */
#ifdef _WIN32
    u_long off = 0;
    ioctlsocket(s, FIONBIO, &off);
#else
    fcntl(s, F_SETFL, flags);
#endif
    return ok;
}

}  // namespace

bool Lsw2MasterClient::request(const char* method, const std::string& path,
                               const std::string& body, std::string* response) {
    error_.clear();
    if (host_.empty() || port_ <= 0 || port_ > 65535) {
        error_ = "no master server configured";
        return false;
    }

    /* getaddrinfo rather than inet_pton: the configured value is a HOSTNAME
     * (play.viosarcade.xyz), and only the testing value happens to be an
     * address. Resolving handles both. */
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portbuf[16];
    std::snprintf(portbuf, sizeof portbuf, "%d", port_);
    if (getaddrinfo(host_.c_str(), portbuf, &hints, &res) != 0 || !res) {
        error_ = "cannot resolve " + host_;
        return false;
    }

    socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == LSW2_INVALID) { freeaddrinfo(res); error_ = "no socket"; return false; }

    /* EVERY CALL HERE RUNS ON THE FRAME THREAD, so every one of them needs a
     * bound. Without these the OS default applies - about twenty seconds on
     * Windows for an address that does not answer - and that is not a slow
     * request, it is the game hanging.
     *
     * The heartbeat is what makes it serious rather than annoying. It fires
     * every sixty frames while hosting, so a directory that goes down MID-MATCH
     * would freeze the game for twenty seconds, resume for one, and freeze
     * again, for as long as the lobby was up. A browser that takes a while to
     * open is a wait; that is unplayable. */
    set_timeouts(s);

    if (!connect_within(s, res->ai_addr, int(res->ai_addrlen))) {
        freeaddrinfo(res);
        lsw2_close(s);
        error_ = "cannot reach " + host_ + ":" + portbuf;
        return false;
    }
    freeaddrinfo(res);

    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host_ + "\r\n";
    req += "Connection: close\r\n";
    if (!token_.empty()) req += "X-Token: " + token_ + "\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n" + body;

    if (::send(s, req.data(), int(req.size()), 0) < 0) {
        lsw2_close(s);
        error_ = "send failed";
        return false;
    }

    std::string raw;
    char buf[4096];
    for (;;) {
        const int n = int(recv(s, buf, sizeof buf, 0));
        if (n <= 0) break;
        raw.append(buf, size_t(n));
        /* A DIRECTORY RESPONSE IS SMALL. Four lobbies of scalars is well under
         * a kilobyte, so a cap costs nothing and means a confused or hostile
         * server cannot make this allocate without bound. */
        if (raw.size() > 256 * 1024) break;
    }
    lsw2_close(s);

    const size_t head = raw.find("\r\n\r\n");
    if (head == std::string::npos) { error_ = "malformed response"; return false; }
    /* THE STATUS CODE, PARSED - not a fixed prefix match.
     *
     * The first version compared against "HTTP/1.1 200" and reported the
     * service as unreachable when it answered "HTTP/1.0 200 OK", which is what
     * Python's BaseHTTPRequestHandler sends by default. The version is the
     * server's business; the code is what this cares about.
     *
     * Checked at all because a 403 with a JSON body would otherwise parse
     * cleanly and read as success. */
    int code = 0;
    {
        const size_t sp = raw.find(' ');
        if (sp != std::string::npos) code = std::atoi(raw.c_str() + sp + 1);
    }
    const bool ok = (code == 200 || code == 201);
    if (response) *response = raw.substr(head + 4);
    if (!ok) {
        error_ = raw.substr(0, raw.find("\r\n"));
        return false;
    }
    return true;
}

bool Lsw2MasterClient::reachable() {
    std::string body;
    return request("GET", "/v1/health", "", &body) &&
           body.find("\"ok\"") != std::string::npos;
}

/* The roster fragment both register and heartbeat send.
 *
 * WHY THE HOST REPORTS ITS PLAYERS AT ALL. The directory never sees a join -
 * players connect to the host - so an operator looking at the console window
 * would otherwise have a player COUNT and nothing to act on. The server GUI
 * shows a lobby that expands to a list of who is in it, with Kick and Ban IP on any
 * of them, and this is the only place that information exists.
 */
static std::string roster_json(const std::vector<Lsw2RosterEntry>& roster) {
    if (roster.empty()) return {};
    std::string out = ",\"roster\":[";
    for (size_t i = 0; i < roster.size(); ++i) {
        if (i) out += ",";
        out += "{\"slot\":" + std::to_string(roster[i].slot) +
               ",\"name\":\"" + escape(roster[i].name) + "\"" +
               ",\"address\":\"" + escape(roster[i].address) + "\"}";
    }
    out += "]";
    return out;
}

void Lsw2MasterClient::set_roster(const std::vector<Lsw2RosterEntry>& roster) {
    roster_ = roster;
}

/* ,"state":"...","config":{...} - or nothing for a client that never said. The
 * config is the caller's JSON object, passed through as it is. */
std::string Lsw2MasterClient::status_json() const {
    std::string out;
    if (!state_.empty()) out += ",\"state\":\"" + escape(state_) + "\"";
    if (!config_.empty()) out += ",\"config\":" + config_;
    return out;
}

bool Lsw2MasterClient::register_lobby(const std::string& name, int listen_port,
                                      int players) {
    std::string body =
        "{\"name\":\"" + escape(name) + "\",\"port\":" +
        std::to_string(listen_port) + ",\"players\":" +
        std::to_string(players);
    if (!kind_.empty()) body += ",\"kind\":\"" + kind_ + "\"";
    body += roster_json(roster_);
    body += status_json();
    body += "}";

    std::string reply;
    token_.clear();   /* a registration must not send a stale token */
    if (!request("POST", "/v1/servers", body, &reply)) return false;
    id_ = lsw2_json_string(reply, "id");
    token_ = lsw2_json_string(reply, "token");
    code_ = lsw2_json_string(reply, "code");
    return !id_.empty() && !token_.empty();
}

bool Lsw2MasterClient::heartbeat(int players) {
    if (id_.empty()) { error_ = "not registered"; return false; }
    std::string body = "{\"token\":\"" + escape(token_) +
                       "\",\"players\":" + std::to_string(players);
    body += roster_json(roster_);
    body += status_json();
    body += "}";

    std::string reply;
    if (!request("PUT", "/v1/servers/" + id_, body, &reply)) return false;

    /* THE HEARTBEAT REPLY IS THE ONLY CHANNEL FROM THE DIRECTORY TO A HOST.
     * The host is behind whatever NAT the UPnP mapping just punched one game
     * port through, so nothing there can dial in. An operator's Kick therefore
     * waits at the directory and rides the next beat back. Delivered once -
     * they are consumed on the service side - so they are appended for the
     * caller to drain, not overwritten. */
    for (const std::string& chunk : lsw2_json_objects(reply, "directives")) {
        Lsw2Directive d;
        d.action = lsw2_json_string(chunk, "action");
        d.address = lsw2_json_string(chunk, "address");
        d.slot = int(lsw2_json_number(chunk, "slot", -1));
        if (!d.action.empty()) directives_.push_back(d);
    }
    return true;
}

std::vector<Lsw2Directive> Lsw2MasterClient::take_directives() {
    std::vector<Lsw2Directive> out;
    out.swap(directives_);
    return out;
}

bool Lsw2MasterClient::unregister() {
    if (id_.empty()) return true;    /* nothing to withdraw is success */
    const bool ok = request("DELETE", "/v1/servers/" + id_, "", nullptr);
    id_.clear(); token_.clear(); code_.clear();
    return ok;
}

bool Lsw2MasterClient::refresh() {
    std::string path = "/v1/servers";
    /* ASK FOR ONE KIND WHEN ONLY ONE IS WANTED. The fork's browser lists arena
     * lobbies and the baseline's lists co-op ones; neither should download the
     * other's and sift them, and the filter is one query parameter. */
    if (!kind_.empty()) path += "?kind=" + kind_;

    std::string reply;
    if (!request("GET", path, "", &reply)) return false;
    servers_.clear();

    for (const std::string& chunk : lsw2_json_objects(reply, "servers")) {
        Lsw2ServerEntry e;
        e.name = lsw2_json_string(chunk, "name");
        e.address = lsw2_json_string(chunk, "address");
        e.code = lsw2_json_string(chunk, "code");
        e.kind = lsw2_json_string(chunk, "kind");
        e.port = int(lsw2_json_number(chunk, "port", 0));
        e.players = int(lsw2_json_number(chunk, "players", 0));
        e.max_players = int(lsw2_json_number(chunk, "max_players", 4));

        /* The roster's NAMES only. The service does not publish player
         * addresses on this endpoint, deliberately, and the client has no use
         * for one - it connects to the host. */
        for (const std::string& who : lsw2_json_objects(chunk, "roster")) {
            const std::string name = lsw2_json_string(who, "name");
            if (!name.empty()) e.roster.push_back(name);
        }

        if (!e.name.empty() && e.port > 0) servers_.push_back(e);
    }
    return true;
}

bool Lsw2MasterClient::resolve(const std::string& code, Lsw2ServerEntry* out) {
    std::string reply;
    if (!request("GET", "/v1/codes/" + code, "", &reply)) return false;
    if (!out) return true;
    out->name = lsw2_json_string(reply, "name");
    out->address = lsw2_json_string(reply, "address");
    out->port = int(lsw2_json_number(reply, "port", 0));
    out->players = int(lsw2_json_number(reply, "players", 0));
    out->max_players = int(lsw2_json_number(reply, "max_players", 4));
    return out->port > 0;
}
