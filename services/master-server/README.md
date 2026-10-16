# Master server (Cloudflare Worker)

The public lobby directory for the PC port. It runs as a Cloudflare Worker
over D1 at **`lsw2server.viosarcade.xyz`**. The game's default master address
is that host, on port 80. Hosts advertise their lobbies here, and players
browse them or join by room code. Game traffic never passes through the
directory: players connect to the host directly.

`services/master-server-selfhost` is the same protocol as a single Python
process, for running a private directory (`127.0.0.1:28553` by default).

## Protocol

The game speaks plain HTTP/1.1 on port 80 (`port/net/lsw2_master.cpp`). It has
no TLS, so the zone must keep answering plain HTTP. Do not turn on "Always use
HTTPS" for this hostname.

| Request | What it does |
|---|---|
| `GET /v1/health` | Returns `{ok, service, ttl}`. |
| `GET /v1/servers?kind=coop\|bounty` | Lists the live lobbies of one kind. |
| `POST /v1/servers` | Registers a lobby. The body is `{name, port, players, kind, roster, state, config}`; the reply carries `{id, token, code, expires_in}`. |
| `PUT /v1/servers/<id>` | Heartbeat. Send `X-Token`; the body is `{token, players, roster, state, config}`. |
| `DELETE /v1/servers/<id>` | Disbands the lobby. Send `X-Token`. |
| `GET /v1/codes/<code>` | Looks up a lobby by its four-character room code. |

- **Tokens and expiry.** A token is returned once and stored only as a SHA-256
  hash. A lobby expires 20 s after its last heartbeat, and hosts beat every
  5 s.
- **`state` and `config`.** `state` is `lobby` or `match`. `config` holds the
  host's rules as short strings, and the server keeps only whitelisted keys
  (`sanitiseConfig`).
- **Cleanup.** A once-a-minute cron (`scheduled`) removes expired lobbies.

## Discord log

The server posts one message to Discord for each of these events:

- a lobby hosted (with its room code and rules)
- a player joining or leaving
- a game starting (with the map, mode and rules)
- a game ending
- a lobby disbanded
- a lobby closed because its host stopped answering

Co-op and Bounty Hunter lobbies post to separate channels. The webhooks are
**Worker secrets**, never in this repository:

    npx wrangler secret put DISCORD_WEBHOOK_COOP
    npx wrangler secret put DISCORD_WEBHOOK_BOUNTY

If a kind has no secret set, that kind is simply not logged. Posts never
include host addresses, and they never ping anyone (`allowed_mentions` is
empty).

## Deploying

    npm install
    npx wrangler d1 create lsw2ds-master-server     # once; put the id in wrangler.jsonc
    npx wrangler d1 execute lsw2ds-master-server --remote --file schema.sql
    npx wrangler deploy

On the first deploy, wrangler creates the custom-domain route and its DNS
record.

**Testing locally.** Run `npm run dev` and point the game's master address at
the printed local URL. `npm run conformance` runs the Worker locally against
the client's protocol tests.

## NAT

The directory only publishes the address and port a host registered from. It
cannot make an unreachable host reachable. `/v1/turn-credentials` hands out
short-lived coturn REST credentials when the `TURN_SHARED_SECRET` and
`TURN_URL` secrets are set (see `services/turn-relay`). `STUN_URL` optionally
adds a STUN server.
