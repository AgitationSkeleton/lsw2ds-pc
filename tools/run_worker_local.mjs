/* Run services/master-server (the Cloudflare worker) locally, with no wrangler
 * and no npm dependencies, so it can be held to the same conformance test the
 * self-host server passes.
 *
 * WHY THIS EXISTS. The hosted directory was found rejecting every
 * registration the game client made, and the rewrite that followed was recorded
 * honestly as READ, NOT RUN - there was no JS runtime here. "Read" is the
 * weakest kind of evidence this project accepts, and it is exactly the kind
 * that let the original divergence ship: two implementations of one protocol,
 * assumed equivalent because they had "the same routes".
 *
 * WHAT IT SHIMS. The worker needs two things from Cloudflare that Node does not
 * provide:
 *
 *   env.DB   a D1 binding - prepare().bind().run()/first()/all()
 *            backed here by node:sqlite, which is built in
 *   fetch    the Request/Response entry point
 *            backed here by a plain http server that converts both ways
 *
 * Everything else the worker uses - crypto.randomUUID, crypto.subtle,
 * crypto.getRandomValues, URL, Response, btoa - is standard in Node 18+.
 *
 * THIS IS A HARNESS, NOT A DEPLOYMENT. It proves the worker's LOGIC answers the
 * client's protocol. It does not exercise Cloudflare's routing, its D1
 * transaction semantics, or its edge behaviour, and a pass here is not a
 * substitute for a wrangler run before deploying.
 *
 *   node tools/run_worker_local.mjs [port] [schema.sql] [worker.js]
 *
 * The worker and schema are parameters so an OLD revision can be run as a
 * control. A conformance test that passes everything it is pointed at proves
 * nothing; pointing it at the version known to be broken is what shows it
 * can tell the difference.
 */

import { createServer } from "node:http";
import { DatabaseSync } from "node:sqlite";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const port = Number(process.argv[2] || 28556);
const schemaPath = process.argv[3] || resolve(here, "../services/master-server/schema.sql");
const workerPath = process.argv[4] ||
  resolve(here, "../services/master-server/src/index.js");

const db = new DatabaseSync(":memory:");
/* The schema is the one the worker ships. Applying it here rather than an
 * inlined copy means a column the worker expects and the schema forgets shows
 * up as a failure, which is the sort of drift this whole exercise is about. */
/* Executed whole rather than split on ";". Splitting looks harmless and is not:
 * a statement carrying its own comments comes apart at the wrong places and
 * sqlite reports "incomplete input" for something the file gets right. */
db.exec(readFileSync(schemaPath, "utf8"));

/* D1's prepare/bind/run/first/all over node:sqlite. D1 returns {results} from
 * all(); first() returns the row or null; run() reports success. */
const DB = {
  prepare(sql) {
    return {
      bind(...args) {
        const params = args.map(v => (v === undefined ? null : v));
        return {
          run() { db.prepare(sql).run(...params); return {success: true}; },
          first() { return db.prepare(sql).get(...params) ?? null; },
          all() { return {results: db.prepare(sql).all(...params)}; },
        };
      },
      /* Unbound forms, used by nothing here but part of the D1 surface. */
      run() { db.prepare(sql).run(); return {success: true}; },
      first() { return db.prepare(sql).get() ?? null; },
      all() { return {results: db.prepare(sql).all()}; },
    };
  },
};

const worker = (await import("file://" + workerPath.replace(/\\/g, "/"))).default;
const env = {DB};

createServer(async (req, res) => {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const body = Buffer.concat(chunks);

  /* CLOUDFLARE INJECTS cf-connecting-ip, AND THE WORKER DEPENDS ON IT.
   * register() derives the advertised host address from
   * `value.address || request.headers.get("cf-connecting-ip")`, and the client
   * does not send an address - the self-host server takes it from the socket
   * for the same reason. Without this the worker correctly answers
   * "public address unavailable", which is right behaviour and a wrong test.
   * The harness supplies what the edge would. */
  const headers = {...req.headers};
  if (!headers["cf-connecting-ip"]) {
    headers["cf-connecting-ip"] =
      (req.socket.remoteAddress || "127.0.0.1").replace(/^::ffff:/, "");
  }

  const request = new Request("http://127.0.0.1:" + port + req.url, {
    method: req.method,
    headers,
    body: (req.method === "GET" || req.method === "HEAD") ? undefined : body,
  });

  let response;
  try {
    response = await worker.fetch(request, env);
  } catch (error) {
    response = new Response(JSON.stringify({error: String(error)}), {status: 500});
  }

  const text = await response.text();
  const outHeaders = {};
  response.headers.forEach((v, k) => { outHeaders[k] = v; });
  res.writeHead(response.status, outHeaders);
  res.end(text);
}).listen(port, "127.0.0.1", () => {
  console.log("worker harness on http://127.0.0.1:" + port);
  console.log("  worker: " + workerPath);
  console.log("  schema: " + schemaPath);
});
