import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import type { AddressInfo } from "node:net";
import { ArchiverRegistry } from "../src/registry.js";

let status = 200;
let hits = 0;
let auth = "";
let url = "";
const srv = http.createServer((req, res) => {
  hits++;
  auth = String(req.headers.authorization ?? "");
  res.writeHead(status, { "content-type": "application/json" });
  res.end(status === 200 ? JSON.stringify({ accounts: {
    aaaaaaaaaa1: { gatewayUrl: "http://gw-a", archiverGatewayToken: "tok-a", archive_enabled: true },
    abbbbbbbbb2: { gatewayUrl: "http://gw-b", archiverGatewayToken: "tok-b", archive_enabled: false },
  } }) : "{}");
});
before(async () => {
  url = await new Promise((r) => srv.listen(0, "127.0.0.1", () => r(`http://127.0.0.1:${(srv.address() as AddressInfo).port}/internal/registry`)));
});
after(() => { srv.close(); });

function reg() {
  let t = 1_000_000;
  return { r: new ArchiverRegistry(url, "reg-arch", { ttlMs: 30_000, staleMs: 300_000, now: () => t }), advance: (ms: number) => { t += ms; } };
}

test("targets per account, unknown is null, registry token sent", async () => {
  status = 200;
  const { r } = reg();
  assert.equal(r.ready(), false);
  assert.deepEqual(await r.get("aaaaaaaaaa1"), { gatewayUrl: "http://gw-a", token: "tok-a", archiveEnabled: true });
  assert.deepEqual(await r.get("abbbbbbbbb2"), { gatewayUrl: "http://gw-b", token: "tok-b", archiveEnabled: false });
  assert.equal(await r.get("acccccccccc"), null);
  assert.equal(r.ready(), true);
  assert.equal(auth, "Bearer reg-arch");
});

test("cache within ttl; stale snapshot survives an outage up to staleMs", async () => {
  status = 200;
  const { r, advance } = reg();
  hits = 0;
  await r.get("aaaaaaaaaa1");
  await r.get("aaaaaaaaaa1");
  assert.equal(hits, 1);
  status = 500;
  advance(31_000);
  assert.ok(await r.get("aaaaaaaaaa1"));
  advance(300_000);
  assert.equal(await r.get("aaaaaaaaaa1"), "unavailable");
});

test("never loaded: unavailable and not ready", async () => {
  status = 503;
  const { r } = reg();
  assert.equal(await r.get("aaaaaaaaaa1"), "unavailable");
  assert.equal(r.ready(), false);
});
