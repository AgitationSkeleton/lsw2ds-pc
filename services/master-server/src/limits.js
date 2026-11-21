/* REQUEST LIMITS - the answer to a load test on the LEGO Battles port's lobby
 * server (its GitHub issue #2), whose findings applied here too: one address
 * could register lobbies without end, every registration posted to Discord,
 * and a host could list itself at any address it named.
 *
 * The game's own traffic, for scale: a host registers once per lobby and
 * heartbeats every 5 s (port/frontend/forksession.cpp); the list and room
 * codes are fetched when a player asks. Every limit sits far above that; past
 * one the answer is 429 with Retry-After (503 when the directory is full),
 * which the game shows as a refusal.
 *
 * WHERE THE COUNTS LIVE. A Worker keeps no memory between requests that it can
 * rely on, so the counters are rows in D1's `limits` table: a fixed window per
 * kind and address, and token buckets for the Discord channel. Made here on
 * first use (and the servers table's `peer` column added), so a deploy needs
 * no migration step. The cron prunes old rows. */

export const MAX_LOBBIES = 200;          // listed at once, every address together
export const MAX_LOBBIES_PER_IP = 3;     // listed at once from one address
export const MAX_LIST = 100;             // lobbies in one list reply
// Per address: [requests, ms].
export const RATE_GET = [120, 60000];
export const RATE_WRITE = [120, 60000];  // POST, PUT and DELETE
export const RATE_REGISTER = [6, 600000];
// The Discord channel: token buckets, every line together and per source
// address (the lobby's host): [burst, ms per token].
export const DISCORD_GLOBAL = [30, 2000];
export const DISCORD_PER_SOURCE = [10, 30000];

let schemaReady = null;

export function ensureSchema(env) {
  if (!schemaReady) {
    schemaReady = (async () => {
      await env.DB.prepare(
        "CREATE TABLE IF NOT EXISTS limits " +
        "(key TEXT PRIMARY KEY, a REAL NOT NULL, b REAL NOT NULL, c REAL NOT NULL DEFAULT 0)"
      ).run();
      // The address a lobby was registered from, never shown: the per-address
      // cap counts it. Fails harmlessly once the column exists.
      try {
        await env.DB.prepare("ALTER TABLE servers ADD COLUMN peer TEXT NOT NULL DEFAULT ''").run();
      } catch (e) {}
    })().catch(e => { schemaReady = null; throw e; });
  }
  return schemaReady;
}

// TEST_LIMIT_SCALE: a test's `wrangler dev --var`, never set live.
function scale(env) {
  const n = Number(env.TEST_LIMIT_SCALE || 1);
  return Number.isFinite(n) && n >= 1 ? n : 1;
}

/* True when `addr` is over `limit` for `kind` in the current window. One
 * statement: the window restarts once it is older than its length. */
export async function overLimit(env, kind, addr, [count, ms]) {
  const now = Date.now();
  const row = await env.DB.prepare(
    "INSERT INTO limits (key, a, b) VALUES (?1, ?2, 1) " +
    "ON CONFLICT(key) DO UPDATE SET " +
    "b = CASE WHEN ?2 - a >= ?3 THEN 1 ELSE b + 1 END, " +
    "a = CASE WHEN ?2 - a >= ?3 THEN ?2 ELSE a END " +
    "RETURNING b"
  ).bind(`w:${kind}:${addr}`, now, ms).first();
  return !!row && row.b > count * scale(env);
}

/* One token from the bucket `key`: false when it is empty. A bucket holds at
 * most `burst` and gains one every `every` ms.
 *
 * ONE STATEMENT, so two lines sent at once cannot both spend the last token
 * (a read and then a write let 11 of 16 through a burst of 10). SET reads the
 * old row, so `refill` is the same in all three; `c` records whether this call
 * took one. */
export async function take(env, key, [burst, every]) {
  const refill = "MIN(?2, a + (?3 - b) / ?4)";
  const row = await env.DB.prepare(
    "INSERT INTO limits (key, a, b, c) VALUES (?1, ?2 - 1, ?3, 1) " +
    "ON CONFLICT(key) DO UPDATE SET " +
    `c = (${refill} >= 1), a = ${refill} - (${refill} >= 1), b = ?3 ` +
    "RETURNING c"
  ).bind("t:" + key, burst, Date.now(), every).first();
  return !!row && row.c >= 1;
}

/* Discord lines held back: count one, or take the count when a line may say
 * so (at most every 10 minutes). */
export async function holdLine(env) {
  await env.DB.prepare(
    "INSERT INTO limits (key, a, b) VALUES ('held', 1, 0) ON CONFLICT(key) DO UPDATE SET a = a + 1"
  ).run();
}

export async function takeHeld(env) {
  const now = Date.now();
  const row = await env.DB.prepare("SELECT a, b FROM limits WHERE key='held'").first();
  if (!row || row.a < 1 || now - row.b < 600000) return 0;
  await env.DB.prepare("UPDATE limits SET a = 0, b = ? WHERE key='held'").bind(now).run();
  return row.a;
}

// Rows nobody has touched for an hour: windows long over, buckets full again.
export async function pruneLimits(env) {
  await env.DB.prepare(
    "DELETE FROM limits WHERE key != 'held' AND " +
    "((key LIKE 'w:%' AND a < ?1) OR (key LIKE 't:%' AND b < ?1))"
  ).bind(Date.now() - 3600000).run();
}

export function slowDown(ms) {
  return new Response(JSON.stringify({error: "slow down"}), {
    status: 429,
    headers: {"content-type": "application/json; charset=utf-8",
              "retry-after": String(Math.ceil(ms / 1000))},
  });
}

const V4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
export function isV4(s) { return V4.test(String(s == null ? "" : s)); }
export function isPublicV4(s) {
  if (!isV4(s)) return false;
  const [a, b] = s.split(".").map(Number);
  if (a === 0 || a === 10 || a === 127 || a >= 224) return false;
  if ((a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254)) return false;
  return !(a === 100 && b >= 64 && b <= 127);
}

/* THE ADDRESS A LOBBY IS LISTED AT. A host may name its public address (the
 * game learns it from UPnP); it is believed only when it is the address this
 * server sees, or when the server cannot check it because the host reached it
 * over IPv6. Anything else is listed at the address seen, so a listing can
 * never point joiners at a third party. */
export function hostAddress(claimed, peer) {
  const c = String(claimed == null ? "" : claimed).trim();
  if (c && c === peer) return c.slice(0, 64);
  if (isPublicV4(c) && String(peer).includes(":")) return c;
  return String(peer).slice(0, 64);
}
