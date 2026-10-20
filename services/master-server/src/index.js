/* The hosted directory, speaking the protocol the game client actually uses.
 *
 * WHY THIS FILE WAS REWRITTEN. Measured by comparing this worker against
 * `port/net/lsw2_master.cpp` and against the self-host server, which has been
 * verified end to end with a live client. The worker was an EARLIER
 * protocol and the client had outgrown it on every authenticated call:
 *
 *   client GET  /v1/health              worker served /health          -> 404
 *   client PUT  /v1/servers/<id>        worker POST .../heartbeat      -> 404
 *   client GET  /v1/codes/<code>        worker had no such route       -> 404
 *   client X-Token: header              worker Authorization: Bearer   -> 401
 *   client sends {name,port,players}    validConfig DEMANDED map and
 *                                       game_mode                      -> 400
 *
 * That last one is the fatal one: `map` and `game_mode` are fields the client
 * has never sent, so REGISTRATION ITSELF was rejected. The default master host
 * in settings.json is this service, so the fork's out-of-the-box internet play
 * could not work at all. The self-host server was fine, which is exactly why it
 * went unnoticed - every test used it.
 *
 * The reference for every shape below is the self-host server, because that is
 * the one a real client has been tested against.
 */

import {announce, describeConfig, kindLabel, lasted, names, plain} from "./discord.js";
export {RelayRoom} from "./relay.js";

// Twenty seconds, as the self-host server: the game heartbeats about once a
// second, and a crashed host should leave the list quickly rather than linger
// as a zombie.
const TTL_SECONDS = 20;
const MAX_BODY = 4096;

/* Crockford base 32 without I, L, O and U - the characters people misread when
 * copying a code off a screen. Four of them is about a million codes, which is
 * plenty for concurrent lobbies and far too few to be unguessable. That is
 * deliberate: a room code is a convenience for friends, not access control.
 * Anything that must not be guessed uses the token. */
const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const CODE_LENGTH = 4;

/* Fold the substitutions people actually make when typing a code they read:
 * I and L are 1, O is 0, case does not matter. Must match the self-host
 * server's normalise_code exactly or a code issued by one will not resolve on
 * the other. */
function normaliseCode(raw) {
  return String(raw || "").toUpperCase().trim()
    .replace(/I/g, "1").replace(/L/g, "1").replace(/O/g, "0");
}

function normaliseKind(raw) {
  const k = String(raw || "bounty").toLowerCase().trim();
  return (k === "coop") ? "coop" : "bounty";
}

function reply(value, status = 200) {
  return new Response(JSON.stringify(value), {
    status,
    headers: {"content-type": "application/json; charset=utf-8"},
  });
}

async function readBody(request) {
  if (Number(request.headers.get("content-length") || 0) > MAX_BODY) {
    throw new Error("body too large");
  }
  const value = await request.json();
  if (JSON.stringify(value).length > MAX_BODY) throw new Error("body too large");
  return value;
}

async function sha256(value) {
  const bytes = new TextEncoder().encode(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, "0")).join("");
}

function base64(bytes) {
  return btoa(String.fromCharCode(...bytes));
}

async function hmacSha1(secret, value) {
  const encoder = new TextEncoder();
  const key = await crypto.subtle.importKey(
    "raw", encoder.encode(secret), {name: "HMAC", hash: "SHA-1"}, false, ["sign"]
  );
  return new Uint8Array(await crypto.subtle.sign("HMAC", key, encoder.encode(value)));
}

/* THE CLIENT AUTHENTICATES WITH X-Token, NOT Authorization: Bearer.
 * lsw2_master.cpp's request() adds `X-Token: <token>` to every call once it
 * holds one, and never sets an Authorization header at all. */
function clientToken(request) {
  return request.headers.get("x-token") || "";
}

async function authorised(request, env, id) {
  const token = clientToken(request);
  if (!token) return false;
  const row = await env.DB.prepare("SELECT token_hash FROM servers WHERE id=?")
    .bind(id).first();
  return !!row && row.token_hash === await sha256(token);
}

/* A roster is names and slots ONLY.
 *
 * THE PUBLIC LIST MUST NOT CARRY PLAYER ADDRESSES, and the self-host server
 * says why: /v1/servers is unauthenticated by design, because it is how every
 * client finds a game. A HOST's address belongs there - advertising it is the
 * point. The people who JOINED that host did not choose to advertise anything,
 * and publishing their addresses would turn the lobby browser into an IP
 * harvester for everyone playing. */
function sanitiseRoster(raw) {
  if (!Array.isArray(raw)) return [];
  return raw.slice(0, 4).map(entry => ({
    slot: Number.isInteger(entry && entry.slot) ? entry.slot : 0,
    name: String((entry && entry.name) || "").slice(0, 48),
  }));
}

async function freeCode(env) {
  for (let attempt = 0; attempt < 64; ++attempt) {
    let code = "";
    const rnd = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
    for (let i = 0; i < CODE_LENGTH; ++i) code += CROCKFORD[rnd[i] % CROCKFORD.length];
    const clash = await env.DB.prepare("SELECT 1 FROM servers WHERE code=?")
      .bind(code).first();
    if (!clash) return code;
  }
  /* Every attempt collided, so the space is saturated. Say so rather than loop
   * forever or hand out a duplicate. */
  return null;
}

function validRegistration(value) {
  return value &&
    Number.isInteger(value.port) && value.port >= 1024 && value.port <= 65535 &&
    Number.isInteger(value.players) && value.players >= 0 && value.players <= 4;
}

async function register(request, env, ctx) {
  const value = await readBody(request);
  if (!validRegistration(value)) {
    return reply({error: "invalid server registration"}, 400);
  }
  const address = String(value.address || request.headers.get("cf-connecting-ip") || "").slice(0, 64);
  if (!address) return reply({error: "public address unavailable"}, 400);

  const code = await freeCode(env);
  if (!code) return reply({error: "no free room code"}, 503);

  const id = crypto.randomUUID();
  const token = crypto.randomUUID() + crypto.randomUUID();
  const now = Math.floor(Date.now() / 1000);

  const kind = normaliseKind(value.kind);
  const name = String(value.name || "Bounty Arena").slice(0, 48);
  const config = sanitiseConfig(value.config);
  const state = normaliseState(value.state);
  const lan = sanitiseLan(value.lan);
  await env.DB.prepare(
    "INSERT INTO servers (id,token_hash,name,address,port,code,kind,players,max_players,config_json,roster_json,state,lan,created_at,updated_at,expires_at)" +
    " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
  ).bind(
    id, await sha256(token), name,
    address, value.port, code, kind,
    value.players, 4,
    JSON.stringify(config),
    JSON.stringify(sanitiseRoster(value.roster)),
    state, lan, now, now, now + TTL_SECONDS
  ).run();

  const rules = kind === "bounty" ? describeConfig(config, false) : "";
  announce(env, ctx, kind,
    `🟢 **${plain(name)}** is hosting a ${kindLabel(kind)} lobby - room code \`${code}\`` +
    (rules ? `\n${rules}` : ""));

  return reply({id, token, code, expires_in: TTL_SECONDS}, 201);
}

/* PUT, not POST-to-a-subpath. lsw2_master.cpp: request("PUT", "/v1/servers/" + id_, ...) */
async function heartbeat(request, env, ctx, id) {
  if (!await authorised(request, env, id)) return reply({error: "unauthorized"}, 401);
  const value = await readBody(request);
  const players = Math.max(0, Math.min(4, Number(value.players || 0)));
  const now = Math.floor(Date.now() / 1000);
  const before = await env.DB.prepare(
    "SELECT name,kind,roster_json,state,config_json FROM servers WHERE id=?"
  ).bind(id).first();
  const roster = sanitiseRoster(value.roster);
  /* A client that does not report its state or rules keeps what it had. */
  const state = value.state === undefined ? (before && before.state) || "lobby"
                                          : normaliseState(value.state);
  const config = value.config === undefined
    ? JSON.parse((before && before.config_json) || "{}") : sanitiseConfig(value.config);
  await env.DB.prepare(
    "UPDATE servers SET players=?,roster_json=?,state=?,config_json=?,updated_at=?,expires_at=? WHERE id=?"
  ).bind(players, JSON.stringify(roster), state, JSON.stringify(config), now,
         now + TTL_SECONDS, id).run();
  if (before) reportChanges(env, ctx, before, roster, state, config);

  /* THE HEARTBEAT REPLY IS THE ONLY CHANNEL FROM THE DIRECTORY TO A HOST - the
   * host is behind whatever NAT it punched one game port through, so nothing
   * can dial in. The client drains "directives" from this reply. This worker
   * has no operator console to raise them, so the array is always empty; the
   * field is present so the client's parse finds what it expects. */
  return reply({expires_in: TTL_SECONDS, directives: []});
}

async function remove(request, env, ctx, id) {
  if (!await authorised(request, env, id)) return reply({error: "unauthorized"}, 401);
  const row = await env.DB.prepare("SELECT name,kind,created_at FROM servers WHERE id=?")
    .bind(id).first();
  await env.DB.prepare("DELETE FROM servers WHERE id=?").bind(id).run();
  if (row) {
    const now = Math.floor(Date.now() / 1000);
    announce(env, ctx, row.kind,
      `🔴 **${plain(row.name)}** disbanded its ${kindLabel(row.kind)} lobby (open ${lasted(now - row.created_at)})`);
  }
  return reply({ok: true});
}

/* WHAT CHANGED SINCE THE LAST BEAT, for the log: who arrived, who left, and a
 * game starting or ending. Names are compared as a multiset, so two players
 * with one name still count as two. */
function reportChanges(env, ctx, before, roster, state, config) {
  const was = names(JSON.parse(before.roster_json || "[]"));
  const now = names(roster);
  const left = [...was];
  const joined = [];
  for (const n of now) {
    const i = left.indexOf(n);
    if (i >= 0) left.splice(i, 1); else joined.push(n);
  }
  const lobby = plain(before.name);
  for (const n of joined) {
    announce(env, ctx, before.kind, `➡️ **${plain(n)}** joined **${lobby}** (${now.length}/4)`);
  }
  for (const n of left) {
    announce(env, ctx, before.kind, `⬅️ **${plain(n)}** left **${lobby}** (${now.length}/4)`);
  }
  if (before.state !== "match" && state === "match") {
    const rules = before.kind === "bounty" ? describeConfig(config, true) : "";
    announce(env, ctx, before.kind,
      `▶️ A game started in **${lobby}** with ${now.map(plain).join(", ") || "nobody listed"}` +
      (rules ? `\n${rules}` : ""));
  } else if (before.state === "match" && state !== "match") {
    announce(env, ctx, before.kind, `⏹️ The game in **${lobby}** is over - back to the lobby`);
  }
}

/* EXPIRED LOBBIES: their host stopped answering (a crash, a closed window, a
 * lost connection). Reported and removed by the once-a-minute cron, so the log
 * says when a lobby vanished rather than only when its host said goodbye. */
async function purgeExpired(env, ctx) {
  const now = Math.floor(Date.now() / 1000);
  const {results} = await env.DB.prepare(
    "SELECT id,name,kind,created_at FROM servers WHERE expires_at < ?"
  ).bind(now).all();
  for (const row of results || []) {
    await env.DB.prepare("DELETE FROM servers WHERE id=?").bind(row.id).run();
    announce(env, ctx, row.kind,
      `⚫ **${plain(row.name)}** closed - its host stopped answering (open ${lasted(now - row.created_at)})`);
  }
}

function normaliseState(raw) {
  return String(raw || "lobby").toLowerCase() === "match" ? "match" : "lobby";
}

/* The rules as the client reports them: short strings and one short list,
 * nothing else kept, so the public list cannot be used to store anything. */
function sanitiseConfig(raw) {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return {};
  const out = {};
  for (const key of ["roster", "character", "customization", "controls", "music",
                     "lobby_music", "lobby_time", "map", "mode"]) {
    if (typeof raw[key] === "string") out[key] = raw[key].slice(0, 48);
  }
  if (Array.isArray(raw.extras)) {
    out.extras = raw.extras.filter(x => typeof x === "string").slice(0, 18)
      .map(x => x.slice(0, 48));
  }
  return out;
}

async function list(request, env) {
  const now = Math.floor(Date.now() / 1000);
  /* Expired rows are filtered here and removed (and reported) by the cron. */
  const kind = new URL(request.url).searchParams.get("kind");
  let sql = "SELECT id,name,address,port,players,max_players,code,kind,config_json,roster_json,lan,created_at" +
            " FROM servers WHERE expires_at>=?";
  const args = [now];
  if (kind) { sql += " AND kind=?"; args.push(normaliseKind(kind)); }
  sql += " ORDER BY updated_at DESC LIMIT 100";

  const {results} = await env.DB.prepare(sql).bind(...args).all();
  const requester = request.headers.get("cf-connecting-ip") || "";
  const servers = (results || []).map(row => ({
    name: row.name,
    ...reachableAddress(row, requester),
    port: row.port,
    players: row.players,
    max_players: row.max_players,
    code: row.code,
    config: JSON.parse(row.config_json || "{}"),
    kind: row.kind,
    id: row.id,
    roster: JSON.parse(row.roster_json || "[]"),
    created_at: row.created_at,
  }));
  return reply({servers});
}

async function resolveCode(request, env, raw) {
  const now = Math.floor(Date.now() / 1000);
  const row = await env.DB.prepare(
    "SELECT id,name,address,port,players,max_players,lan FROM servers WHERE code=? AND expires_at>=?"
  ).bind(normaliseCode(raw), now).first();
  if (!row) return reply({error: "no such room code"}, 404);
  const requester = request.headers.get("cf-connecting-ip") || "";
  /* The id is what a joiner needs to reach the lobby through the relay when
   * its address does not answer. */
  return reply({
    id: row.id, name: row.name, ...reachableAddress(row, requester), port: row.port,
    players: row.players, max_players: row.max_players,
  });
}

/* THE RELAY's front door (relay.js has the room). A host must prove it owns the
 * lobby with its token; a joiner only needs the lobby to be live. Then the
 * WebSocket goes to the lobby's own room. */
async function relay(request, env, id) {
  if ((request.headers.get("upgrade") || "").toLowerCase() !== "websocket") {
    return reply({error: "expected a WebSocket upgrade"}, 426);
  }
  if (!env.RELAY) return reply({error: "relay not configured"}, 503);
  const role = new URL(request.url).searchParams.get("role");
  if (role === "host") {
    if (!await authorised(request, env, id)) return reply({error: "unauthorized"}, 401);
  } else if (role === "join") {
    const now = Math.floor(Date.now() / 1000);
    const row = await env.DB.prepare("SELECT 1 FROM servers WHERE id=? AND expires_at>=?")
      .bind(id, now).first();
    if (!row) return reply({error: "no such lobby"}, 404);
  } else {
    return reply({error: "role must be host or join"}, 400);
  }
  return env.RELAY.get(env.RELAY.idFromName(id)).fetch(request);
}

/* THE ADDRESS THAT WILL ACTUALLY ANSWER. A player behind the same router as
 * the host shares its public address, and dialling your own public address
 * from inside the network only works where the router supports hairpin NAT -
 * many don't. Such a player is given the host's LAN address instead, which the
 * host reported when it registered. It is never shown to anyone else. The
 * client then sees the same address as the host's LAN beacon, so the lobby
 * lists once rather than twice. */
function reachableAddress(row, requester) {
  if (row.lan && requester && row.address === requester) {
    return {address: row.lan, same_network: true};
  }
  return {address: row.address};
}

/* A host's LAN address: a private-range dotted IPv4 (10/8, 172.16/12,
 * 192.168/16, 100.64/10), or nothing. */
function sanitiseLan(raw) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(String(raw || ""));
  if (!m) return "";
  const o = m.slice(1).map(Number);
  if (o.some(n => n > 255)) return "";
  const priv = o[0] === 10 ||
               (o[0] === 172 && o[1] >= 16 && o[1] <= 31) ||
               (o[0] === 192 && o[1] === 168) ||
               (o[0] === 100 && o[1] >= 64 && o[1] <= 127);
  return priv ? o.join(".") : "";
}

async function turnCredentials(request, env) {
  if (!env.TURN_SHARED_SECRET || !env.TURN_URL) {
    return reply({error: "relay not configured"}, 503);
  }
  const now = Math.floor(Date.now() / 1000);
  const expires = now + 300;
  const client = String(request.headers.get("cf-ray") || crypto.randomUUID()).slice(0, 48);
  const username = `${expires}:${client}`;
  const credential = base64(await hmacSha1(env.TURN_SHARED_SECRET, username));
  const iceServers = [];
  if (env.STUN_URL) iceServers.push({urls: [env.STUN_URL]});
  iceServers.push({urls: [env.TURN_URL], username, credential});
  return reply({iceServers, expires_at: expires});
}

export default {
  async fetch(request, env, ctx) {
    try {
      const url = new URL(request.url);
      const parts = url.pathname.split("/").filter(Boolean);
      const method = request.method;

      /* The client probes /v1/health and looks for "ok" in the body. /health is
       * kept as an alias so anything pointing at the old path still answers. */
      if (method === "GET" && (url.pathname === "/v1/health" || url.pathname === "/health" || url.pathname === "/")) {
        return reply({ok: true, service: "lsw2-master", ttl: TTL_SECONDS});
      }
      if (method === "GET" && url.pathname === "/v1/servers") return list(request, env);
      if (method === "POST" && url.pathname === "/v1/servers") return register(request, env, ctx);
      if (method === "GET" && url.pathname === "/v1/turn-credentials") return turnCredentials(request, env);

      if (parts.length === 3 && parts[0] === "v1" && parts[1] === "relay" && method === "GET") {
        return relay(request, env, parts[2]);
      }
      if (parts.length === 3 && parts[0] === "v1" && parts[1] === "codes" && method === "GET") {
        return resolveCode(request, env, parts[2]);
      }
      if (parts.length === 3 && parts[0] === "v1" && parts[1] === "servers") {
        if (method === "PUT") return heartbeat(request, env, ctx, parts[2]);
        if (method === "DELETE") return remove(request, env, ctx, parts[2]);
      }
      /* The old heartbeat shape, so a client mid-upgrade is not stranded. */
      if (parts.length === 4 && parts[0] === "v1" && parts[1] === "servers" &&
          parts[3] === "heartbeat" && method === "POST") {
        return heartbeat(request, env, ctx, parts[2]);
      }
      return reply({error: "not found"}, 404);
    } catch (error) {
      return reply({error: String(error && error.message || error)}, 400);
    }
  },

  async scheduled(event, env, ctx) {
    ctx.waitUntil(purgeExpired(env, ctx));
  },
};
