-- The hosted directory's table, matching the protocol the game client speaks.
--
-- REWRITTEN alongside src/index.js. The previous schema required
-- map_key and game_mode NOT NULL, and the client has never sent either - it
-- sends {name, port, players, kind, roster}. Registration was therefore
-- rejected outright by validConfig before it ever reached SQL. See the header
-- of src/index.js for the full divergence list.
--
-- The reference for these columns is services/master-server-selfhost, which
-- has been verified end to end against a live client.

CREATE TABLE IF NOT EXISTS servers (
  id TEXT PRIMARY KEY,
  token_hash TEXT NOT NULL,
  name TEXT NOT NULL,
  address TEXT NOT NULL,
  port INTEGER NOT NULL CHECK(port BETWEEN 1024 AND 65535),

  -- The four-character room code. UNIQUE because
  -- /v1/codes/<code> resolves to exactly one lobby; the worker retries on
  -- collision rather than handing out a duplicate.
  code TEXT NOT NULL UNIQUE,

  -- 'bounty' or 'coop'. The client filters the list on it.
  kind TEXT NOT NULL DEFAULT 'bounty',

  players INTEGER NOT NULL DEFAULT 1 CHECK(players BETWEEN 0 AND 4),
  max_players INTEGER NOT NULL DEFAULT 4 CHECK(max_players BETWEEN 1 AND 4),
  config_json TEXT NOT NULL DEFAULT '{}',

  -- Slots and names only, never player addresses: /v1/servers is
  -- unauthenticated by design, so anything here is public to anyone who can
  -- reach the directory. A host advertising its own address chose to; the
  -- people who joined it did not.
  roster_json TEXT NOT NULL DEFAULT '[]',

  -- 'lobby' or 'match', as the host reports it; the change is what the
  -- Discord log calls a game starting or ending.
  state TEXT NOT NULL DEFAULT 'lobby',

  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS servers_expires_at ON servers(expires_at);
CREATE INDEX IF NOT EXISTS servers_kind ON servers(kind);
