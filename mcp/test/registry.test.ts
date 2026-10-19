import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import type { AddressInfo } from "node:net";
import { createHash } from "node:crypto";
import { RegistryClient } from "../src/registry.js";

const sha = (s: string) => createHash("sha256").update(s).digest("hex");
const A = { accountId: "aaaaaaaaaa1", gatewayUrl: "http://a", serviceToken: "svc" };
let entries: any[] = [];
let status = 200;
let hits = 0;
let lastAuth = "";
let url = "";
const srv = http.createServer((req, res) => {
  hits++;
  lastAuth = String(req.headers.authorization ?? "");
  setTimeout(() => {
    res.writeHead(status, { "content-type": "application/json" });
    res.end(status === 200 ? JSON.stringify({ tokens: entries }) : "{}");
  }, 20);
});
before(async () => {
  url = await new Promise((r) => srv.listen(0, "127.0.0.1", () => r(`http://127.0.0.1:${(srv.address() as AddressInfo).port}/internal/registry`)));
});
after(() => { srv.close(); });

function client() {
  let t = 1_000_000;
  const c = new RegistryClient(url, "reg-tok", { ttlMs: 30_000, staleMs: 300_000, now: () => t });
  return { c, advance: (ms: number) => { t += ms; } };
}

test("known token resolves to its set, unknown to null, registry token is sent", async () => {
  entries = [{ sha256: sha("tok-a"), accounts: [A] }]; status = 200;
  const { c } = client();
  assert.deepEqual(await c.resolve("tok-a"), { tokenHash: sha("tok-a"), accounts: [A] });
  assert.equal(await c.resolve("nope"), null);
  assert.equal(lastAuth, "Bearer reg-tok");
});

test("cache: no second fetch within ttl; revoked token disappears after ttl", async () => {
  entries = [{ sha256: sha("tok-a"), accounts: [A] }]; status = 200;
  const { c, advance } = client();
  hits = 0;
  await c.resolve("tok-a");
  await c.resolve("tok-a");
  assert.equal(hits, 1);
  entries = [];
  advance(29_000);
  assert.ok(await c.resolve("tok-a"));
  advance(2_000);
  assert.equal(await c.resolve("tok-a"), null);
  assert.equal(hits, 2);
});

test("registry down: last snapshot serves known tokens up to staleMs, unknown is unavailable", async () => {
  entries = [{ sha256: sha("tok-a"), accounts: [A] }]; status = 200;
  const { c, advance } = client();
  await c.resolve("tok-a");
  status = 500;
  advance(31_000);
  assert.ok(await c.resolve("tok-a"));
  assert.equal(await c.resolve("nope"), "unavailable");
  advance(300_000);
  assert.equal(await c.resolve("tok-a"), "unavailable");
});

test("registry down from the start: unavailable", async () => {
  status = 503;
  const { c } = client();
  assert.equal(await c.resolve("tok-a"), "unavailable");
});

test("parallel resolves share one fetch", async () => {
  entries = [{ sha256: sha("tok-a"), accounts: [A] }]; status = 200;
  const { c } = client();
  hits = 0;
  await Promise.all([1, 2, 3, 4, 5].map(() => c.resolve("tok-a")));
  assert.equal(hits, 1);
});

test("empty token never resolves", async () => {
  entries = [{ sha256: sha(""), accounts: [A] }]; status = 200;
  assert.equal(await client().c.resolve(""), null);
});
