/* The UPnP text handling, against the shapes real gateways produce.
 *
 *   g++ -std=c++20 port/net/test_upnp.cpp port/net/lsw2_upnp_parse.cpp \
 *       -o test_upnp
 *
 * Consumer routers are loose about all of this - headers in any
 * capitalisation, control URLs relative or absolute, namespace prefixes present
 * or not - and getting any of it wrong means port forwarding silently does not
 * happen while everything reports success.
 */

#include "lsw2_upnp.h"

#include <cstdio>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

void expect(const std::string& got, const char* want, const char* what) {
    const bool ok = got == want;
    if (!ok) std::printf("       got \"%s\" want \"%s\"\n", got.c_str(), want);
    check(ok, what);
}

}  // namespace

int main() {
    std::printf("UPnP parsing\n");

    /* --- SSDP headers, in the capitalisations routers actually send ------- */
    const std::string ssdp =
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=120\r\n"
        "location: http://192.168.1.1:5000/rootDesc.xml\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "\r\n";
    expect(lsw2_upnp_header(ssdp, "LOCATION"),
           "http://192.168.1.1:5000/rootDesc.xml",
           "LOCATION is found regardless of the case it was sent in");
    expect(lsw2_upnp_header(ssdp, "ST"),
           "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
           "and so is a header that IS uppercase");
    check(lsw2_upnp_header(ssdp, "SERVER").empty(),
          "a header that is not there is empty, not garbage");

    /* A header whose name is a prefix of another must not match it. */
    const std::string tricky = "LOCATIONX: no\r\nLOCATION: yes\r\n\r\n";
    expect(lsw2_upnp_header(tricky, "LOCATION"), "yes",
           "a longer header name is not mistaken for the one asked for");

    /* --- the device description ------------------------------------------ */
    const std::string desc =
        "<root><device><serviceList>"
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
        "<controlURL>/ctl/L3F</controlURL>"
        "</service>"
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>/ctl/IPConn</controlURL>"
        "</service>"
        "</serviceList></device></root>";

    std::string service;
    const std::string ctl = lsw2_upnp_control_url(desc, &service);

    /* THE POINT OF THE WHOLE FUNCTION. Layer3Forwarding is listed FIRST and
     * has a controlURL, and it does not do AddPortMapping. Taking the first
     * controlURL in the document is the obvious implementation and it forwards
     * nothing while reporting success. */
    expect(ctl, "/ctl/IPConn",
           "the control URL comes from the WAN service, not the first service");
    expect(service, "urn:schemas-upnp-org:service:WANIPConnection:1",
           "and the service type matches it");

    /* PPP is the fallback, for uplinks that still dial. */
    const std::string ppp =
        "<root><service>"
        "<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
        "<controlURL>/ctl/PPP</controlURL>"
        "</service></root>";
    std::string s2;
    expect(lsw2_upnp_control_url(ppp, &s2), "/ctl/PPP",
           "a PPP-only router still works");
    expect(s2, "urn:schemas-upnp-org:service:WANPPPConnection:1",
           "with its own service type");

    /* Namespace prefixes are legal and common. */
    const std::string prefixed =
        "<u:service>"
        "<u:serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</u:serviceType>"
        "<u:controlURL>/pfx</u:controlURL>"
        "</u:service>";
    std::string s3;
    expect(lsw2_upnp_control_url(prefixed, &s3), "/pfx",
           "namespace prefixes on the elements are ignored");

    check(lsw2_upnp_control_url("<root/>", nullptr).empty(),
          "a description with no WAN service yields nothing");

    /* --- resolving the control URL --------------------------------------- */
    const std::string loc = "http://192.168.1.1:5000/rootDesc.xml";
    expect(lsw2_upnp_resolve_url(loc, "/ctl/IPConn"),
           "http://192.168.1.1:5000/ctl/IPConn",
           "a rooted control URL joins onto the origin, not the path");
    expect(lsw2_upnp_resolve_url(loc, "ctl/IPConn"),
           "http://192.168.1.1:5000/ctl/IPConn",
           "and a bare relative one gets its slash");
    expect(lsw2_upnp_resolve_url(loc, "http://10.0.0.1/abs"),
           "http://10.0.0.1/abs",
           "an absolute control URL is left alone");

    /* --- splitting a URL -------------------------------------------------- */
    std::string host, path;
    int port = 0;
    check(lsw2_upnp_split_url("http://192.168.1.1:5000/ctl", &host, &port, &path),
          "an http URL splits");
    expect(host, "192.168.1.1", "into a host");
    check(port == 5000, "a port");
    expect(path, "/ctl", "and a path");

    check(lsw2_upnp_split_url("http://192.168.1.1/ctl", &host, &port, &path),
          "a URL with no port splits");
    check(port == 80, "and defaults to 80");

    check(lsw2_upnp_split_url("http://192.168.1.1", &host, &port, &path),
          "a URL with no path splits");
    expect(path, "/", "and the path defaults to root");

    check(!lsw2_upnp_split_url("ftp://x/y", &host, &port, &path),
          "a non-http URL is refused rather than half-parsed");

    /* --- the SOAP body ---------------------------------------------------- */
    const std::string body = lsw2_upnp_add_body(
        "urn:schemas-upnp-org:service:WANIPConnection:1", 28550, "192.168.1.20");
    check(body.find("<NewExternalPort>28550</NewExternalPort>") != std::string::npos,
          "the external port is in the envelope");
    check(body.find("<NewInternalPort>28550</NewInternalPort>") != std::string::npos,
          "and the internal one");
    check(body.find("<NewInternalClient>192.168.1.20</NewInternalClient>") !=
              std::string::npos,
          "pointed at this machine");
    check(body.find("<NewProtocol>TCP</NewProtocol>") != std::string::npos,
          "over TCP, which is what the transport uses");

    /* A FINITE LEASE WOULD EXPIRE MID-MATCH on a router that honours it, and
     * many consumer gateways reject a non-zero one outright. Zero means "until
     * removed", and the fork removes it when the lobby shuts down. */
    check(body.find("<NewLeaseDuration>0</NewLeaseDuration>") != std::string::npos,
          "with an indefinite lease rather than one that expires mid-match");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
