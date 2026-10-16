/* The text-handling half of lsw2_upnp.h.
 *
 * SEPARATE FROM THE SOCKETS SO IT CAN BE TESTED. Everything here reads someone
 * else's text - a router's - and consumer gateways are famously loose about
 * theirs: headers in any capitalisation, control URLs relative or absolute,
 * namespace prefixes on elements or not. That is where this kind of code
 * actually goes wrong, and none of it needs a network to exercise.
 */

#include "lsw2_upnp.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

char lower(char c) {
    return char(std::tolower(static_cast<unsigned char>(c)));
}

bool iequal(const std::string& a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                     s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

/* Find an element's text, ignoring any namespace prefix.
 *
 * Routers write <controlURL>, <u:controlURL> and <ns0:controlURL> and all three
 * are legal. Matching on the closing angle bracket after the tag name is what
 * makes the prefix irrelevant without parsing namespaces. */
/* Find `<name>` or `</name>`, allowing any namespace prefix on either.
 *
 * BOTH ENDS CARRY THE PREFIX. A first version handled `<u:serviceType>` and
 * then looked for a literal `</serviceType>`, which never matches a document
 * that used a prefix - so prefixed descriptions parsed as empty and the router
 * looked like it had no WAN service. The test that caught it feeds exactly that
 * shape.
 *
 * Returns the offset of the '<', or npos. `after` receives the offset just past
 * the closing '>'. */
size_t find_tag(const std::string& xml, const char* name, size_t from,
                bool closing, size_t* after) {
    const std::string needle = std::string(name) + ">";
    size_t p = from;
    for (;;) {
        p = xml.find(needle, p);
        if (p == std::string::npos) return std::string::npos;

        /* Walk back over an optional `prefix:`. Only name characters may
         * appear there; anything else means this is a different tag that
         * happens to end with the same letters - matching "URL>" inside
         * "presentationURL>" is that mistake, and the `<` check below is what
         * rejects it. */
        size_t start = p;
        if (start > 0 && xml[start - 1] == ':') {
            size_t q = start - 1;
            while (q > 0 && (std::isalnum(static_cast<unsigned char>(xml[q - 1])) ||
                             xml[q - 1] == '-' || xml[q - 1] == '_'))
                --q;
            start = q;
        }

        const size_t want = closing ? 2u : 1u;   /* "</" or "<" */
        if (start >= want &&
            xml[start - 1] == (closing ? '/' : '<') &&
            (!closing || xml[start - 2] == '<')) {
            if (after) *after = p + needle.size();
            return start - want;
        }
        p += needle.size();
    }
}

std::string element_text(const std::string& xml, const char* tag, size_t from,
                         size_t* end_out) {
    size_t text_start = 0;
    if (find_tag(xml, tag, from, false, &text_start) == std::string::npos)
        return "";

    size_t after_close = 0;
    const size_t close_at = find_tag(xml, tag, text_start, true, &after_close);
    if (close_at == std::string::npos) return "";

    if (end_out) *end_out = after_close;
    return trim(xml.substr(text_start, close_at - text_start));
}

}  // namespace

std::string lsw2_upnp_header(const std::string& response, const char* name) {
    const size_t n = std::char_traits<char>::length(name);
    size_t line = 0;
    while (line < response.size()) {
        size_t eol = response.find('\n', line);
        if (eol == std::string::npos) eol = response.size();
        if (eol - line > n + 1 && iequal(response.substr(line, n), name, n) &&
            response[line + n] == ':') {
            return trim(response.substr(line + n + 1, eol - (line + n + 1)));
        }
        line = eol + 1;
    }
    return "";
}

std::string lsw2_upnp_control_url(const std::string& xml, std::string* service) {
    /* WANIPConnection FIRST, WANPPPConnection second. They are the same call on
     * two kinds of uplink - IP for anything modern, PPP for DSL that still
     * dials - and a router offering both wants the IP one used. */
    static const char* kServices[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };

    for (const char* want : kServices) {
        size_t p = 0;
        for (;;) {
            size_t after = 0;
            const std::string type = element_text(xml, "serviceType", p, &after);
            if (type.empty()) break;
            p = after;
            if (type != want) continue;

            /* THE CONTROL URL MUST COME FROM THIS SERVICE'S BLOCK. Searching
             * the whole document for the first controlURL would return some
             * other service's - a description lists several, and the first is
             * usually a layer-3 forwarding entry that does not do this call. */
            const std::string url = element_text(xml, "controlURL", p, nullptr);
            if (url.empty()) continue;
            if (service) *service = want;
            return url;
        }
    }
    return "";
}

std::string lsw2_upnp_resolve_url(const std::string& location,
                                  const std::string& control) {
    if (control.empty()) return "";
    /* Already absolute. */
    if (control.size() > 7 && iequal(control, "http://", 7)) return control;

    /* Otherwise it is relative to the LOCATION's origin - scheme, host and
     * port, without the description's own path. */
    const size_t scheme = location.find("://");
    if (scheme == std::string::npos) return "";
    const size_t slash = location.find('/', scheme + 3);
    const std::string origin =
        slash == std::string::npos ? location : location.substr(0, slash);

    if (control[0] == '/') return origin + control;
    return origin + "/" + control;
}

bool lsw2_upnp_split_url(const std::string& url, std::string* host, int* port,
                         std::string* path) {
    if (url.size() < 8 || !iequal(url, "http://", 7)) return false;

    const size_t start = 7;
    const size_t slash = url.find('/', start);
    std::string authority =
        slash == std::string::npos ? url.substr(start) : url.substr(start, slash - start);
    if (authority.empty()) return false;

    int p = 80;
    const size_t colon = authority.rfind(':');
    /* rfind, and only when what follows is all digits - an IPv6 literal has
     * colons of its own and none of them is a port. */
    if (colon != std::string::npos) {
        bool digits = colon + 1 < authority.size();
        for (size_t i = colon + 1; i < authority.size() && digits; ++i)
            if (!std::isdigit(static_cast<unsigned char>(authority[i])))
                digits = false;
        if (digits) {
            p = std::atoi(authority.c_str() + colon + 1);
            authority = authority.substr(0, colon);
        }
    }
    if (p <= 0 || p > 65535) return false;

    if (host) *host = authority;
    if (port) *port = p;
    if (path) *path = slash == std::string::npos ? "/" : url.substr(slash);
    return true;
}

std::string lsw2_upnp_add_body(const std::string& service, int port,
                               const std::string& internal_ip) {
    char buf[1024];
    /* NewLeaseDuration is ZERO deliberately - "until something removes it".
     * A finite lease would expire mid-match on a router that honours it, and
     * the fork already removes its mapping when the lobby shuts down. Many
     * consumer gateways reject non-zero leases outright. */
    std::snprintf(buf, sizeof buf,
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:AddPortMapping xmlns:u=\"%s\">"
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>%d</NewExternalPort>"
        "<NewProtocol>TCP</NewProtocol>"
        "<NewInternalPort>%d</NewInternalPort>"
        "<NewInternalClient>%s</NewInternalClient>"
        "<NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>LEGO Star Wars II Arena</NewPortMappingDescription>"
        "<NewLeaseDuration>0</NewLeaseDuration>"
        "</u:AddPortMapping></s:Body></s:Envelope>",
        service.c_str(), port, port, internal_ip.c_str());
    return buf;
}
