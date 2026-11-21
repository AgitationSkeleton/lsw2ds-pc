// The request limits (src/limits.js) against a local `wrangler dev`:
//   npx wrangler dev --local --port 8798
//   node test/limits.mjs http://127.0.0.1:8798
// Each check sends CF-Connecting-IP to stand for a different player, so one run
// can be several addresses. Lobbies are removed again at the end.
const base = process.argv[2] || "http://127.0.0.1:8798";
let failed = 0;
const made = [];

function check(name, ok, detail = "") {
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? " - " + detail : ""}`);
  if (!ok) failed++;
}

async function call(method, path, ip, body, token) {
  const headers = {"content-type": "application/json", "cf-connecting-ip": ip};
  if (token) headers["x-token"] = token;
  const r = await fetch(base + path, {method, headers, body: body ? JSON.stringify(body) : undefined});
  const text = method === "HEAD" ? "" : await r.text();
  let json = {};
  try { json = JSON.parse(text); } catch (e) {}
  return {status: r.status, json, headers: r.headers, text};
}

async function register(ip, extra = {}) {
  const r = await call("POST", "/v1/servers", ip, {name: "limit test", port: 28550, players: 1, ...extra});
  if (r.status === 201) made.push(r.json);
  return r;
}

const run = Date.now() % 100000;
const ipA = `203.0.113.${run % 200 + 1}`;
const ipB = `198.51.100.${run % 200 + 1}`;

// Three lobbies from one address, then a 429 with Retry-After.
for (let i = 0; i < 3; i++) {
  const r = await register(ipA);
  check(`lobby ${i + 1} from one address`, r.status === 201, String(r.status));
}
const fourth = await register(ipA);
check("a fourth lobby from that address is refused", fourth.status === 429 && !!fourth.headers.get("retry-after"),
      `${fourth.status} ${fourth.text}`);

// A named address that is not the one seen is not believed.
const spoof = await register(ipB, {address: "8.8.8.8"});
check("a lobby that names another address", spoof.status === 201, String(spoof.status));
const list = await call("GET", "/v1/servers", ipB);
const listed = (list.json.servers || []).find(s => s.id === spoof.json.id);
check("is listed at the address seen", listed && listed.address === ipB, listed && listed.address);
check("the list says how many there are", typeof list.json.total === "number", String(list.json.total));

// HEAD: headers only, the status a GET would get.
const head = await call("HEAD", "/v1/health", ipB);
check("HEAD /v1/health", head.status === 200 && head.text === "", String(head.status));

// Registrations per address: six in ten minutes (the fourth above counted too).
const ipC = `192.0.2.${run % 200 + 1}`;
let refusedAt = 0;
for (let i = 1; i <= 7; i++) {
  const r = await register(ipC);
  if (r.status === 201) {
    await call("DELETE", `/v1/servers/${r.json.id}`, ipC, null, r.json.token);
    made.pop();
  } else if (!refusedAt) refusedAt = i;
}
check("the seventh registration in ten minutes is refused", refusedAt === 7, `refused at ${refusedAt}`);

// Reads per address: 120 a minute.
const ipD = `192.0.2.${(run + 50) % 200 + 1}`;
let readRefused = 0;
for (let i = 1; i <= 125 && !readRefused; i++) {
  const r = await call("GET", "/v1/servers", ipD);
  if (r.status === 429) readRefused = i;
}
check("the 121st read in a minute is refused", readRefused === 121, `refused at ${readRefused}`);

for (const m of made) {
  await call("DELETE", `/v1/servers/${m.id}`, m.ip || ipA, null, m.token);
}
console.log(failed ? `${failed} failed` : "all passed");
process.exit(failed ? 1 : 0);
