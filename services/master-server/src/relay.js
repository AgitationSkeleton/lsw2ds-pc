/* THE RELAY: online play that works with no port forwarding, through any NAT,
 * VPN or carrier-grade NAT, because every connection a player makes is OUTBOUND
 * to this server.
 *
 * WHY IT EXISTS. A host has to accept incoming connections, and many players
 * cannot: a router with UPnP off, an ISP's carrier-grade NAT, a VPN whose exit
 * address accepts nothing. Here both sides dial out instead, and this room
 * joins them.
 *
 * ONE ROOM PER LOBBY, a Durable Object named by the lobby's id, so every
 * WebSocket for that lobby lands in the same object. Two protocols:
 *
 * VERSION 1 (0.3.3): one WebSocket each.
 *   host    GET /v1/relay/<id>?role=host  (X-Token: the lobby's token)
 *   joiner  GET /v1/relay/<id>?role=join
 *   host -> room   [0][peer][bytes] data, [2][peer] close that joiner
 *   room -> host   [0][peer][bytes] data, [1][peer] joined, [2][peer] gone
 *   joiner <-> room  the bytes themselves
 *
 * VERSION 2 (0.3.4): LANES. Measured in play: one TCP stream across the
 * internet stalls for a retransmit timeout (250 ms and more) whenever a single
 * packet is lost, and a lockstep game freezes with it - dozens of times a
 * match. So each side holds several WebSockets ("lanes") and sends every chunk
 * on all of them; the game (port/net/lsw2_wsrelay.cpp) numbers the chunks and
 * keeps whichever copy arrives first. A stall on one lane is covered by the
 * others. The room routes lane k to lane k where it can, else to any lane.
 *   host    ?role=host&v=2&lane=<k>
 *   joiner  ?role=join&v=2&lane=<k>&link=<nonce>   (one nonce per joiner;
 *           its lanes share one peer number)
 *   The messages are version 1's; [1][peer] goes to the host once per joiner,
 *   [2][peer] once its last lane is gone.
 *
 * Text messages: the room tells a joiner lane "ready" once a host is here;
 * "ping" is answered "pong" (keepalive).
 *
 * WHAT CROSSES IT is the game's own TCP stream; the room never parses it.
 *
 * HIBERNATION: sockets are accepted with acceptWebSocket and tagged, so a room
 * whose host is waiting in an empty lobby can sleep between pings, and the
 * routing lives in the tags rather than in memory. */

const MAX_PEERS = 8;
const MAX_LANES = 4;

export class RelayRoom {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
  }

  tags(ws) {
    try { return this.ctx.getTags(ws); } catch (e) { return []; }
  }

  /* ------------------------------------------------------------ version 1 */

  host() {
    return this.ctx.getWebSockets("host")[0] || null;
  }

  peer(n) {
    return this.ctx.getWebSockets("p" + n)[0] || null;
  }

  peerOf(ws) {
    for (const tag of this.tags(ws)) {
      if (tag[0] === "p") return Number(tag.slice(1));
    }
    return -1;
  }

  /* ------------------------------------------------------------ version 2 */

  hostLanes() {
    return this.ctx.getWebSockets("h2");
  }

  /* Host lane k, or any host lane. */
  hostLane(k) {
    return this.ctx.getWebSockets("h2l" + k)[0] || this.hostLanes()[0] || null;
  }

  joinLanes(n) {
    return this.ctx.getWebSockets("q" + n);
  }

  /* Joiner n's lane k, or any of its lanes. */
  joinLane(n, k) {
    return this.ctx.getWebSockets("q" + n + "l" + k)[0] || this.joinLanes(n)[0] || null;
  }

  laneOf(ws) {
    for (const tag of this.tags(ws)) {
      const m = /^(?:h2l|q\d+l)(\d+)$/.exec(tag);
      if (m) return Number(m[1]);
    }
    return 0;
  }

  joinerOf(ws) {
    for (const tag of this.tags(ws)) {
      const m = /^q(\d+)$/.exec(tag);
      if (m) return Number(m[1]);
    }
    return -1;
  }

  refuse(server, code, why) {
    this.ctx.acceptWebSocket(server, ["refused"]);
    server.close(code, why);
  }

  async fetch(request) {
    const params = new URL(request.url).searchParams;
    const role = params.get("role");
    const v2 = params.get("v") === "2";
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    const ok = new Response(null, {status: 101, webSocket: client});

    if (v2) {
      const lane = Math.max(0, Math.min(MAX_LANES - 1, Number(params.get("lane") || 0) | 0));
      if (role === "host") {
        /* A reconnecting lane replaces its own old socket; the joiners keep
         * going on the other lanes meanwhile. */
        for (const old of this.ctx.getWebSockets("h2l" + lane)) {
          try { old.close(4000, "host lane reconnected"); } catch (e) {}
        }
        this.ctx.acceptWebSocket(server, ["h2", "h2l" + lane]);
        return ok;
      }
      if (this.hostLanes().length === 0) {
        this.refuse(server, 4004, "the host is not on the relay");
        return ok;
      }
      const link = String(params.get("link") || "").replace(/[^0-9A-Za-z]/g, "").slice(0, 32);
      if (!link) {
        this.refuse(server, 4000, "a v2 joiner needs a link");
        return ok;
      }
      /* The joiner's peer number: its link's, if another of its lanes is here. */
      let n = -1;
      const mine = this.ctx.getWebSockets("k" + link);
      if (mine.length) n = this.joinerOf(mine[0]);
      const fresh = n < 0;
      if (fresh) {
        for (let i = 1; i <= MAX_PEERS; ++i) {
          if (!this.joinLanes(i).length && !this.peer(i)) { n = i; break; }
        }
      }
      if (n < 0) {
        this.refuse(server, 4008, "the relay room is full");
        return ok;
      }
      this.ctx.acceptWebSocket(server, ["q" + n, "q" + n + "l" + lane, "k" + link]);
      if (fresh) {
        for (const h of this.hostLanes()) {
          try { h.send(new Uint8Array([1, n])); } catch (e) {}
        }
      }
      server.send("ready");
      return ok;
    }

    if (role === "host") {
      /* A reconnecting host replaces the old socket - and the joiners it was
       * carrying are gone with it, because their streams were mid-flight. */
      for (const old of this.ctx.getWebSockets()) {
        try { old.close(4000, "host reconnected"); } catch (e) {}
      }
      this.ctx.acceptWebSocket(server, ["host"]);
      return ok;
    }

    /* A version-1 joiner: only while a version-1 host is here. */
    const host = this.host();
    if (!host) {
      this.refuse(server, 4004, "the host is not on the relay");
      return ok;
    }
    let n = -1;
    for (let i = 1; i <= MAX_PEERS; ++i) {
      if (!this.peer(i) && !this.joinLanes(i).length) { n = i; break; }
    }
    if (n < 0) {
      this.refuse(server, 4008, "the relay room is full");
      return ok;
    }
    this.ctx.acceptWebSocket(server, ["p" + n]);
    host.send(new Uint8Array([1, n]));
    server.send("ready");
    return ok;
  }

  async webSocketMessage(ws, message) {
    if (typeof message === "string") {
      if (message === "ping") ws.send("pong");
      return;
    }
    const bytes = new Uint8Array(message);
    const tags = this.tags(ws);

    /* Version 2. */
    const q = this.joinerOf(ws);
    if (q > 0) {
      const h = this.hostLane(this.laneOf(ws));
      if (!h) { ws.close(4004, "the host left"); return; }
      const out = new Uint8Array(bytes.length + 2);
      out[0] = 0;
      out[1] = q;
      out.set(bytes, 2);
      h.send(out);
      return;
    }
    if (tags.includes("h2")) {
      if (bytes.length < 2) return;
      const n = bytes[1];
      if (bytes[0] === 0) {
        const to = this.joinLane(n, this.laneOf(ws));
        if (to) to.send(bytes.subarray(2));
        else ws.send(new Uint8Array([2, n]));
      } else if (bytes[0] === 2) {
        for (const j of this.joinLanes(n)) {
          try { j.close(1000, "closed by the host"); } catch (e) {}
        }
      }
      return;
    }

    /* Version 1. */
    const n = this.peerOf(ws);
    if (n > 0) {
      const host = this.host();
      if (!host) { ws.close(4004, "the host left"); return; }
      const out = new Uint8Array(bytes.length + 2);
      out[0] = 0;
      out[1] = n;
      out.set(bytes, 2);
      host.send(out);
      return;
    }
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
    const tags = this.tags(ws);

    const q = this.joinerOf(ws);
    if (q > 0) {
      /* A joiner is gone when its last lane is. */
      const left = this.joinLanes(q).filter(o => o !== ws);
      if (!left.length) {
        for (const h of this.hostLanes()) {
          try { h.send(new Uint8Array([2, q])); } catch (e) {}
        }
      }
      return;
    }
    if (tags.includes("h2")) {
      /* The host is gone when its last lane is: every joiner's stream with it. */
      const left = this.hostLanes().filter(o => o !== ws);
      if (!left.length) {
        for (const other of this.ctx.getWebSockets()) {
          if (other !== ws && this.joinerOf(other) > 0) {
            try { other.close(4004, "the host left"); } catch (e) {}
          }
        }
      }
      return;
    }

    const n = this.peerOf(ws);
    if (n > 0) {
      const host = this.host();
      if (host) {
        try { host.send(new Uint8Array([2, n])); } catch (e) {}
      }
      return;
    }
    if (tags.includes("host")) {
      for (const other of this.ctx.getWebSockets()) {
        if (other !== ws) {
          try { other.close(4004, "the host left"); } catch (e) {}
        }
      }
    }
  }
}
