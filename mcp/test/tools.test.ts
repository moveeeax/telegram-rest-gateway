import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import type { AddressInfo } from "node:net";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { buildServer } from "../src/tools.js";
import type { AccountSet } from "../src/accounts.js";

type Req = { worker: string; method: string; path: string; auth: string };
const log: Req[] = [];
const servers: http.Server[] = [];

function fakeWorker(name: string, meId: string): Promise<string> {
  const s = http.createServer((req, res) => {
    log.push({ worker: name, method: req.method ?? "", path: req.url ?? "", auth: String(req.headers.authorization ?? "") });
    req.resume();
    req.on("end", () => {
      res.writeHead(200, { "content-type": "application/json" });
      if (req.url === "/v1/me") return res.end(JSON.stringify({ ok: true, data: { id: meId, first_name: name, username: name } }));
      res.end(JSON.stringify({ ok: true, data: { id: "tmp-1" } }));
    });
  });
  servers.push(s);
  return new Promise((r) => s.listen(0, "127.0.0.1", () => r(`http://127.0.0.1:${(s.address() as AddressInfo).port}`)));
}

let set: AccountSet;
let archiverUrl = "";
const archiverCalls: string[] = [];

before(async () => {
  const ua = await fakeWorker("alpha", "1001");
  const ub = await fakeWorker("beta", "1002");
  set = { tokenHash: "h", accounts: [
    { accountId: "aaaaaaaaaa1", gatewayUrl: ua, serviceToken: "svc-a" },
    { accountId: "abbbbbbbbb2", gatewayUrl: ub, serviceToken: "svc-b" },
  ] };
  const arch = http.createServer((req, res) => {
    archiverCalls.push(req.url ?? "");
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, results: [] }));
  });
  servers.push(arch);
  archiverUrl = await new Promise((r) => arch.listen(0, "127.0.0.1", () => r(`http://127.0.0.1:${(arch.address() as AddressInfo).port}`)));
});
after(() => { for (const s of servers) s.close(); });

async function connect(accounts: () => AccountSet | null) {
  const server = buildServer({ accounts, archiverUrl, archiverToken: "arch-tok" });
  const [ct, st] = InMemoryTransport.createLinkedPair();
  await server.connect(st);
  const client = new Client({ name: "t", version: "1" });
  await client.connect(ct);
  return client;
}
const text = (r: any) => JSON.parse(r.content[0].text);

test("get_me goes to the chosen account with its service token", async () => {
  const c = await connect(() => set);
  log.length = 0;
  const r: any = await c.callTool({ name: "telegram_get_me", arguments: { account: "abbbbbbbbb2" } });
  assert.equal(r.isError, undefined);
  assert.equal(text(r).account, "abbbbbbbbb2");
  assert.equal(text(r).id, "1002");
  assert.deepEqual(log.map((l) => [l.worker, l.path, l.auth]), [["beta", "/v1/me", "Bearer svc-b"]]);
});

test("write tool without account on a multi-account token sends nothing", async () => {
  const c = await connect(() => set);
  log.length = 0;
  const r: any = await c.callTool({ name: "telegram_send_message", arguments: { chat_id: "1", text: "hi" } });
  assert.equal(r.isError, true);
  assert.match(r.content[0].text, /account is required/);
  assert.equal(log.length, 0);
});

test("list_accounts returns every account of the token", async () => {
  const c = await connect(() => set);
  const r: any = await c.callTool({ name: "telegram_list_accounts", arguments: {} });
  assert.deepEqual(text(r).accounts.map((x: any) => [x.account, x.id]), [["aaaaaaaaaa1", "1001"], ["abbbbbbbbb2", "1002"]]);
});

test("search_history sends the chosen account as session_id", async () => {
  const c = await connect(() => set);
  archiverCalls.length = 0;
  await c.callTool({ name: "telegram_search_history", arguments: { query: "x", account: "aaaaaaaaaa1" } });
  assert.equal(new URLSearchParams(archiverCalls[0].split("?")[1]).get("session_id"), "aaaaaaaaaa1");
});

test("no resolved accounts: tool call fails, nothing sent", async () => {
  const c = await connect(() => null);
  log.length = 0;
  const r: any = await c.callTool({ name: "telegram_get_me", arguments: {} });
  assert.equal(r.isError, true);
  assert.equal(log.length, 0);
});

test("single-account set: account may be omitted", async () => {
  const c = await connect(() => ({ tokenHash: "h1", accounts: [set.accounts[0]] }));
  const r: any = await c.callTool({ name: "telegram_get_me", arguments: {} });
  assert.equal(text(r).account, "aaaaaaaaaa1");
});
