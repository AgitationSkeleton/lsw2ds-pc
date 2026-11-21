// The Discord flood limit (src/discord.js + src/limits.js) against a local
// `wrangler dev` whose webhook is a capture server this script runs:
//   node test/discord_flood.mjs serve 8799        (prints nothing; captures)
//   npx wrangler dev --local --port 8798 --var DISCORD_WEBHOOK_BOUNTY:http://127.0.0.1:8799/hook
//   node test/discord_flood.mjs run http://127.0.0.1:8798 http://127.0.0.1:8799
// Nothing reaches a real channel.
import http from "node:http";

const [mode, a, b] = process.argv.slice(2);

if (mode === "serve") {
  const lines = [];
  http.createServer((req, res) => {
    let body = "";
    req.on("data", c => body += c);
    req.on("end", () => {
      if (req.method === "POST") {
        try { lines.push(JSON.parse(body).content); } catch (e) {}
        res.writeHead(204).end();
      } else if (req.url === "/count") {
        res.writeHead(200, {"content-type": "application/json"}).end(JSON.stringify(lines));
      } else if (req.url === "/reset") {
        lines.length = 0;
        res.writeHead(204).end();
      } else res.writeHead(404).end();
    });
  }).listen(Number(a || 8799), "127.0.0.1");
} else {
  const base = a || "http://127.0.0.1:8798", hook = b || "http://127.0.0.1:8799";
  let failed = 0;
  const check = (name, ok, detail = "") => {
    console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? " - " + detail : ""}`);
    if (!ok) failed++;
  };
  const call = async (method, path, ip, body, token) => {
    const headers = {"content-type": "application/json", "cf-connecting-ip": ip};
    if (token) headers["x-token"] = token;
    const r = await fetch(base + path, {method, headers, body: body ? JSON.stringify(body) : undefined});
    let json = {};
    try { json = await r.json(); } catch (e) {}
    return {status: r.status, json};
  };
  const lines = async () => (await fetch(hook + "/count")).json();
  const settle = () => new Promise(r => setTimeout(r, 1500));
  const run = Date.now() % 1000;
  const made = [];

  // One address: a lobby whose roster changes 15 times = 16 lines; 10 go.
  await fetch(hook + "/reset");
  const ip = `203.0.113.${run % 200 + 1}`;
  const lobby = await call("POST", "/v1/servers", ip, {name: "flood", port: 28550, players: 1});
  made.push([lobby.json, ip]);
  for (let i = 0; i < 15; i++) {
    const roster = [{slot: 0, name: "Host"}].concat(i % 2 ? [] : [{slot: 1, name: "Guest"}]);
    await call("PUT", `/v1/servers/${lobby.json.id}`, ip, {players: roster.length, roster}, lobby.json.token);
  }
  await settle();
  const one = await lines();
  check("one address gets at most 10 lines at once", one.length === 10, `${one.length} of 16`);

  // Many addresses: 36 lobbies from 12 addresses; the channel takes 30 at once.
  await fetch(hook + "/reset");
  for (let k = 0; k < 12; k++) {
    const ipk = `198.51.${run % 200 + 1}.${k + 1}`;
    for (let i = 0; i < 3; i++) {
      const r = await call("POST", "/v1/servers", ipk, {name: `f${k}-${i}`, port: 28550, players: 1});
      if (r.status === 201) made.push([r.json, ipk]);
    }
  }
  await settle();
  const all = await lines();
  // The global bucket was partly spent by the 10 above; it refills one every 2 s.
  check("the channel is capped across addresses", all.length < 36 && all.length <= 30,
        `${all.length} of 36`);

  // A line that went after some were held back says so (once in ten minutes,
  // so it is the first line to go after the first one held).
  check("a later line says lines were held back", one.concat(all).some(l => /held back/.test(l)));

  for (const [m, mip] of made) await call("DELETE", `/v1/servers/${m.id}`, mip, null, m.token);
  console.log(failed ? `${failed} failed` : "all passed");
  process.exit(failed ? 1 : 0);
}
