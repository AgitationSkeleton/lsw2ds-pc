/* Automatic port forwarding, so hosting works without touching a router.
 *
 * miniupnpc is the obvious library for automatic port forwarding, but the
 * requirement is the OUTCOME rather than the library: the port must be easily
 * distributable, and a fresh install must host from behind a consumer router
 * with nothing to configure, no manual port forwarding and nothing extra to
 * ship.
 *
 * WHY NOT THE LIBRARY. Vendoring miniupnpc means a submodule or a copied tree,
 * a second build system to drive, and a licence file to carry, and it makes the
 * repository's build depend on something outside it - which cuts directly
 * against "easily distributable". What it buys is a protocol this port needs
 * exactly three requests of:
 *
 *   1. SSDP M-SEARCH over UDP to find a gateway
 *   2. GET its description XML to find the control endpoint
 *   3. POST one SOAP AddPortMapping
 *
 * That is a few hundred lines against a dependency, and the parsing - which is
 * where this kind of code actually goes wrong - is pure and testable either way.
 * If this turns out to miss real-world gateways the library remains available;
 * the interface here would not change.
 *
 * IT IS ALWAYS OPTIONAL. Every failure is soft. A player on a network with no
 * gateway, with UPnP switched off, or behind carrier-grade NAT simply hosts
 * without a mapping - which still works over LAN and for anyone who can reach
 * them directly - and is told so rather than left wondering.
 */

#ifndef LSW2_UPNP_H
#define LSW2_UPNP_H

#include <string>

/* Ask the local gateway to forward `port` (TCP) to this machine.
 *
 * Returns true when a mapping was accepted. `external_ip` receives the router's
 * WAN address when it could be learned - which is what a host would give a
 * friend who is not on the directory. `error` receives a short reason on
 * failure, suitable for a log line.
 *
 * BOUNDED. Like every other network call the fork makes from the frame thread,
 * this cannot run long: discovery and each request have their own short budget,
 * because a router that does not answer is a router that is not going to. */
bool lsw2_upnp_map_port(int port, std::string* external_ip, std::string* error);

/* Remove a mapping made earlier. Best-effort: a router that has forgotten it,
 * or rebooted, is not an error worth reporting to a player. */
void lsw2_upnp_unmap_port(int port);

/* ------------------------------------------------------------------------- */
/* The pure parts, exposed because they are where this goes wrong.
 *
 * Every one of these reads someone else's text - a router's, and routers are
 * famously loose about it. They are separated so a test can feed them the
 * malformed shapes real gateways produce without needing a gateway. */

/* One header out of an HTTP-style response, matched case-insensitively because
 * SSDP replies use every capitalisation of "Location" that exists. Returns an
 * empty string when absent. */
std::string lsw2_upnp_header(const std::string& response, const char* name);

/* The control URL for the WAN connection service, out of a device description.
 * Prefers WANIPConnection and falls back to WANPPPConnection - the two differ
 * by how the router reaches the internet and offer the same call. `service`
 * receives the matching service type, which the SOAP action needs. */
std::string lsw2_upnp_control_url(const std::string& xml, std::string* service);

/* Join a control URL against the LOCATION it came from. Descriptions give the
 * control URL relative far more often than absolute. */
std::string lsw2_upnp_resolve_url(const std::string& location,
                                  const std::string& control);

/* Split an http URL into host, port and path. Returns false if it is not one. */
bool lsw2_upnp_split_url(const std::string& url, std::string* host, int* port,
                         std::string* path);

/* The AddPortMapping envelope. */
std::string lsw2_upnp_add_body(const std::string& service, int port,
                               const std::string& internal_ip);

#endif /* LSW2_UPNP_H */
