/* THE RELAY: online play that works with no port forwarding, through any NAT,
 * VPN or carrier-grade NAT, because every connection a player makes is OUTBOUND
 * to this server.
 *
 * WHY IT EXISTS. A host has to accept incoming connections, and many players
 * cannot: a router with UPnP off, an ISP's carrier-grade NAT, a VPN whose exit
 * address accepts nothing. The lobby was listed, the joiner dialled the
 * listed address, and nothing answered. Here both sides dial out instead, and
 * this room joins them.
 *
 * ONE ROOM PER LOBBY, a Durable Object named by the lobby's id, so every
 * WebSocket for that lobby lands in the same object.
 *
 *   host    GET /v1/relay/<id>?role=host  (X-Token: the lobby's token)
 *   joiner  GET /v1/relay/<id>?role=join
 *
 * THE WIRE, binary WebSocket messages:
 *   host -> room   [0][peer][bytes]  data for that joiner
 *                  [2][peer]         close that joiner
 *   room -> host   [0][peer][bytes]  data from that joiner
 *                  [1][peer]         a joiner connected
 *                  [2][peer]         a joiner went away
 *   joiner <-> room                  the bytes themselves, nothing added
 * Text messages: the room tells a joiner "ready" once it is paired with a live
 * host; either side may send "ping" (the room answers "pong") to keep an idle
 * connection open.
 *
 * WHAT CROSSES IT is the game's own TCP stream, byte for byte - the port's
 * transport runs over a loopback socket at each end (port/net/lsw2_wsrelay.*)
 * and does not know a relay is involved. The room never parses it.
 *
 * HIBERNATION: the sockets are accepted with acceptWebSocket and tagged, so a
 * room whose host is waiting in an empty lobby can sleep between pings, and
 * the peer numbering survives in the tags rather than in memory. */

const MAX_PEERS = 8;

export class RelayRoom {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
  }

  host() {
    return this.ctx.getWebSockets("host")[0] || null;
  }

  peer(n) {
    return this.ctx.getWebSockets("p" + n)[0] || null;
  }

  peerOf(ws) {
    for (const tag of this.ctx.getTags(ws)) {
      if (tag[0] === "p") return Number(tag.slice(1));
    }
    return -1;
  }

  async fetch(request) {
    const role = new URL(request.url).searchParams.get("role");
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);

    if (role === "host") {
      /* A reconnecting host replaces the old socket - and the joiners it was
       * carrying are gone with it, because their streams were mid-flight. */
      for (const old of this.ctx.getWebSockets()) {
        try { old.close(4000, "host reconnected"); } catch (e) {}
      }
      this.ctx.acceptWebSocket(server, ["host"]);
      return new Response(null, {status: 101, webSocket: client});
    }

    /* A joiner: only while a host is here to talk to. */
    const host = this.host();
    if (!host) {
      this.ctx.acceptWebSocket(server, ["refused"]);
      server.close(4004, "the host is not on the relay");
      return new Response(null, {status: 101, webSocket: client});
    }
    let n = -1;
    for (let i = 1; i <= MAX_PEERS; ++i) {
      if (!this.peer(i)) { n = i; break; }
    }
    if (n < 0) {
      this.ctx.acceptWebSocket(server, ["refused"]);
      server.close(4008, "the relay room is full");
      return new Response(null, {status: 101, webSocket: client});
    }
    this.ctx.acceptWebSocket(server, ["p" + n]);
    host.send(new Uint8Array([1, n]));
    server.send("ready");
    return new Response(null, {status: 101, webSocket: client});
  }

  async webSocketMessage(ws, message) {
    if (typeof message === "string") {
      if (message === "ping") ws.send("pong");
      return;
    }
    const bytes = new Uint8Array(message);
    const n = this.peerOf(ws);
    if (n > 0) {
      /* From a joiner: to the host, tagged. */
      const host = this.host();
      if (!host) { ws.close(4004, "the host left"); return; }
      const out = new Uint8Array(bytes.length + 2);
      out[0] = 0;
      out[1] = n;
      out.set(bytes, 2);
      host.send(out);
      return;
    }
    /* From the host: by the tag's peer. */
    if (bytes.length < 2) return;
    const to = this.peer(bytes[1]);
    if (!to) {
      if (bytes[0] === 0) ws.send(new Uint8Array([2, bytes[1]]));
      return;
    }
    if (bytes[0] === 0) to.send(bytes.subarray(2));
    else if (bytes[0] === 2) to.close(1000, "closed by the host");
  }

  async webSocketClose(ws, code, reason) {
    /* Answer the close (harmless where the runtime already has). */
    try { ws.close(code === 1005 ? 1000 : code, "bye"); } catch (e) {}
    this.gone(ws);
  }

  async webSocketError(ws, error) {
    this.gone(ws);
  }

  gone(ws) {
    const n = this.peerOf(ws);
    if (n > 0) {
      const host = this.host();
      if (host) {
        try { host.send(new Uint8Array([2, n])); } catch (e) {}
      }
      return;
    }
    if (this.ctx.getTags(ws).includes("host")) {
      /* The host left: every joiner's stream ended with it. */
      for (const other of this.ctx.getWebSockets()) {
        if (other !== ws) {
          try { other.close(4004, "the host left"); } catch (e) {}
        }
      }
    }
  }
}
