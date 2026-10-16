/* The directory's log for its owner: one Discord message per thing worth
 * knowing - a lobby hosted, a player joining or leaving, a game started (with
 * its settings) or over, a lobby disbanded or gone quiet.
 *
 * TWO CHANNELS, ONE PER KIND: the co-op lobbies of the base port and the bounty
 * hunter arenas of the fork. The webhooks are the DISCORD_WEBHOOK_COOP and
 * DISCORD_WEBHOOK_BOUNTY secrets (`wrangler secret put ...`), never in the
 * repository, which is public: whoever has a webhook can post to its channel.
 * Without one, that kind is simply not reported. A message that fails (Discord
 * down, rate limited) is let go: the directory never waits on it.
 *
 * NO ADDRESSES ARE POSTED. A host's address is in the public list because
 * joining needs it; the log is for people, and it does not need it. */

// Names as they are, not as Discord's markdown would take them.
export function plain(s) {
  return String(s == null ? "" : s).replace(/[\r\n]+/g, " ")
    .replace(/[\\*_~`|>#[\]()<:@-]/g, "\\$&").slice(0, 100);
}

// How long, for people: "40 s", "12 min", "1 h 5 min".
export function lasted(seconds) {
  const s = Math.max(0, Math.round(seconds));
  if (s < 60) return s + " s";
  const m = Math.round(s / 60);
  return m < 60 ? m + " min" : Math.floor(m / 60) + " h " + (m % 60) + " min";
}

export function kindLabel(kind) {
  return kind === "coop" ? "co-op" : "Bounty Hunter";
}

/* The rules a bounty game runs with, as the client reports them
 * (port/frontend/fork.cpp, master_status_tick). Anything absent is left out. */
export function describeConfig(config, withMatch) {
  if (!config || typeof config !== "object") return "";
  const parts = [];
  if (withMatch && config.map) parts.push("Map: " + plain(config.map));
  if (withMatch && config.mode) parts.push("Mode: " + plain(config.mode));
  if (config.roster) {
    parts.push("Roster: " + plain(config.roster) +
               (config.character ? " (" + plain(config.character) + ")" : ""));
  }
  if (config.customization) parts.push("Customization: " + plain(config.customization));
  if (config.controls) parts.push("Controls: " + plain(config.controls));
  if (config.music) parts.push("Music: " + plain(config.music));
  if (config.lobby_music) parts.push("Lobby music: " + plain(config.lobby_music));
  if (config.lobby_time) parts.push("Lobby timer: " + plain(config.lobby_time));
  if (Array.isArray(config.extras)) {
    parts.push("Extras: " + (config.extras.length
      ? config.extras.slice(0, 18).map(plain).join(", ") : "none"));
  }
  return parts.join(" · ");
}

export function names(roster) {
  return (Array.isArray(roster) ? roster : [])
    .map(entry => String((entry && entry.name) || "").slice(0, 48))
    .filter(Boolean);
}

/* One message to the kind's channel. ctx: whatever keeps the Worker running
 * until it is sent - its waitUntil. */
export function announce(env, ctx, kind, text) {
  const hook = kind === "coop" ? env.DISCORD_WEBHOOK_COOP : env.DISCORD_WEBHOOK_BOUNTY;
  if (!hook) return;
  const send = fetch(hook, {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    // (no one is pinged, whatever a name says)
    body: JSON.stringify({
      username: kind === "coop" ? "LSW2 DS co-op lobbies" : "LSW2 DS Bounty Hunter lobbies",
      content: String(text).slice(0, 1900),
      allowed_mentions: {parse: []},
    }),
  }).catch(() => {});
  if (ctx && ctx.waitUntil) ctx.waitUntil(send);
}
