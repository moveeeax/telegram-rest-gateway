import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import type { AddressInfo } from "node:net";
import { createHash } from "node:crypto";
import { mcpApp } from "../src/http.js";
import type { AccountSet } from "../src/accounts.js";

const sha = (s: string) => createHash("sha256").update(s).digest("hex");
let gw = "";
const servers: http.Server[] = [];
const known = new Map<string, AccountSet>();
let registryDown = false;

before(async () => {
  const g = http.createServer((req, res) => {
    req.resume();
    req.on("end", () => {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, data: { id: "1001", first_name: "a" } }));
    });
  });
  servers.push(g);
  gw = await new Promise((r) => g.listen(0, "127.0.0.1", () => r(`http://127.0.0.1:${(g.address() as AddressInfo).port}`)));
  known.set("tok-a", { tokenHash: sha("tok-a"), accounts: [{ accountId: "aaaaaaaaaa1", gatewayUrl: gw, serviceToken: "svc" }] });
  known.set("tok-b", { tokenHash: sha("tok-b"), accounts: [{ accountId: "abbbbbbbbb2", gatewayUrl: gw, serviceToken: "svc" }] });
  const app = mcpApp({
    resolve: async (t) => (registryDown ? "unavailable" : known.get(t) ?? null),
    allowedOrigins: ["https://claude.ai", "https://claude.com"],
    archiverUrl: "", archiverToken: "",
  });
  const s = app.listen(0, "127.0.0.1");
  await new Promise((r) => s.once("listening", r));
  servers.push(s);
  base = `http://127.0.0.1:${(s.address() as AddressInfo).port}/mcp`;
});
after(() => { for (const s of servers) s.close(); });
let base = "";

const H = { "content-type": "application/json", accept: "application/json, text/event-stream" };
function post(body: unknown, headers: Record<string, string> = {}) {
  return fetch(base, { method: "POST", headers: { ...H, ...headers }, body: JSON.stringify(body) });
}
const init = { jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "t", version: "1" } } };
const call = (id: number) => ({ jsonrpc: "2.0", id, method: "tools/call", params: { name: "telegram_get_me", arguments: {} } });

async function session(headers: Record<string, string> = {}): Promise<string> {
  const r = await post(init, headers);
  assert.equal(r.status, 200);
  await r.text();
  const sid = r.headers.get("mcp-session-id")!;
  await (await post({ jsonrpc: "2.0", method: "notifications/initialized" }, { "mcp-session-id": sid, ...headers })).text();
  return sid;
}

test("handshake without token opens a session; tools/call without token is 401", async () => {
  const sid = await session();
  assert.ok(sid);
  const r = await post(call(2), { "mcp-session-id": sid });
  assert.equal(r.status, 401);
});

test("session binds to the first token; another token in it gets 404", async () => {
  const sid = await session();
  const ok = await post(call(2), { "mcp-session-id": sid, authorization: "Bearer tok-a" });
  assert.equal(ok.status, 200);
  assert.match(await ok.text(), /aaaaaaaaaa1/);
  const other = await post(call(3), { "mcp-session-id": sid, authorization: "Bearer tok-b" });
  assert.equal(other.status, 404);
  const again = await post(call(4), { "mcp-session-id": sid, "x-api-key": "tok-a" });
  assert.equal(again.status, 200);
});

test("unknown token 401, unknown session 404 on POST, GET and DELETE", async () => {
  const sid = await session();
  assert.equal((await post(call(2), { "mcp-session-id": sid, authorization: "Bearer nope" })).status, 401);
  assert.equal((await post(call(2), { "mcp-session-id": "00000000-0000-0000-0000-000000000000", authorization: "Bearer tok-a" })).status, 404);
  for (const method of ["GET", "DELETE"]) {
    const r = await fetch(base, { method, headers: { ...H, "mcp-session-id": "00000000-0000-0000-0000-000000000000", authorization: "Bearer tok-a" } });
    assert.equal(r.status, 404, method);
  }
});

test("registry unavailable gives 503", async () => {
  const sid = await session();
  registryDown = true;
  try {
    assert.equal((await post(call(2), { "mcp-session-id": sid, authorization: "Bearer tok-a" })).status, 503);
  } finally { registryDown = false; }
});

test("Origin outside the allowlist is 403; allowed or absent passes", async () => {
  assert.equal((await post(init, { origin: "https://evil.example" })).status, 403);
  assert.equal((await post(init, { origin: "https://claude.ai" })).status, 200);
  assert.equal((await post(init)).status, 200);
});

test("revoked token stops working in an open session", async () => {
  known.set("tok-c", { tokenHash: sha("tok-c"), accounts: [{ accountId: "acccccccccc", gatewayUrl: gw, serviceToken: "svc" }] });
  const sid = await session({ authorization: "Bearer tok-c" });
  assert.equal((await post(call(2), { "mcp-session-id": sid, authorization: "Bearer tok-c" })).status, 200);
  known.delete("tok-c");
  assert.equal((await post(call(3), { "mcp-session-id": sid, authorization: "Bearer tok-c" })).status, 401);
});
