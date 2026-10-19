import { test } from "node:test";
import assert from "node:assert/strict";
import type { AddressInfo } from "node:net";
import { adminApp } from "../src/admin_server.js";
import { tokenReveal } from "../src/admin_server.js";
import { secretManifest } from "../src/manifests.js";
import { sha256hex } from "../src/ids.js";
import { MemStore } from "../src/store_mem.js";
import { WorkerClient } from "../src/worker_client.js";
import { FakeKube } from "./helpers/fake_kube.js";
import { FakeWorker } from "./helpers/fake_worker.js";
import { account, config } from "./helpers/fixtures.js";

const ID = "aadmintest1";
const SECRETS = { clientToken: `${ID}.OLD`, adminToken: "ADM", mcpServiceToken: "MCPSVC", archiverServiceToken: "ARCHSVC", databaseEncryptionKey: "K" };

async function env() {
  const w = new FakeWorker();
  const url = await w.start();
  const store = new MemStore();
  const kube = new FakeKube();
  const cfg = config();
  let kicks = 0;
  const deps = { store, kube, cfg, worker: (_: string, admin: string) => new WorkerClient(url, admin, 500), kick: () => { kicks++; }, now: () => new Date("2026-10-18T12:00:00Z") };
  const srv = adminApp(deps).listen(0, "127.0.0.1");
  await new Promise((r) => srv.once("listening", r));
  const base = `http://127.0.0.1:${(srv.address() as AddressInfo).port}`;
  const call = (method: string, path: string, token: string | null, body?: unknown) =>
    fetch(`${base}${path}`, {
      method,
      headers: { ...(token ? { authorization: `Bearer ${token}` } : {}), ...(body ? { "content-type": "application/json" } : {}) },
      body: body ? JSON.stringify(body) : undefined,
    });
  return { w, store, kube, cfg, deps, call, kicks: () => kicks, stop: async () => { await new Promise((r) => srv.close(r)); await w.stop(); } };
}

test("admin requires the control plane admin token", async () => {
  const e = await env();
  try {
    assert.equal((await e.call("GET", "/admin/accounts", null)).status, 401);
    assert.equal((await e.call("GET", "/admin/accounts", `${ID}.OLD`)).status, 401);
    assert.equal((await e.call("GET", "/admin/accounts", "reg-mcp")).status, 401);
    assert.equal((await e.call("GET", "/admin/accounts", "cp-admin")).status, 200);
  } finally { await e.stop(); }
});

test("create invite returns a join link valid for 24h", async () => {
  const e = await env();
  try {
    const r = await e.call("POST", "/admin/invites", "cp-admin");
    assert.equal(r.status, 201);
    const j = await r.json();
    assert.match(j.url, /^https:\/\/join\.tarassov\.me\/i\/[A-Za-z0-9_-]{32}$/);
    assert.equal(j.expires_at, "2026-10-19T12:00:00.000Z");
    const code = j.url.split("/i/")[1];
    assert.equal((await e.store.getInvite(sha256hex(code)))?.kind, "new");
  } finally { await e.stop(); }
});

test("list accounts: no hashes, tgw_ready from the worker", async () => {
  const e = await env();
  try {
    await e.store.insertAccount(account(ID, { status: "active", telegramUserId: "777" }));
    await e.kube.apply(secretManifest(ID, SECRETS, 1, e.cfg));
    e.w.state = { authorization_state: "ready", ready: true };
    const j = await (await e.call("GET", "/admin/accounts", "cp-admin")).json();
    assert.equal(j.accounts.length, 1);
    assert.equal(j.accounts[0].accountId, ID);
    assert.equal(j.accounts[0].tgw_ready, true);
    assert.equal(JSON.stringify(j).includes("h-" + ID), false);
  } finally { await e.stop(); }
});

test("patch flags, delete, relogin only for needs_relogin", async () => {
  const e = await env();
  try {
    await e.store.insertAccount(account(ID, { status: "active" }));
    assert.equal((await e.call("PATCH", `/admin/accounts/${ID}`, "cp-admin", { archive_enabled: true })).status, 200);
    assert.equal((await e.store.getAccount(ID))?.archiveEnabled, true);
    assert.equal((await e.call("PATCH", `/admin/accounts/${ID}`, "cp-admin", { archive_enabled: "yes" })).status, 400);
    assert.equal((await e.call("POST", `/admin/accounts/${ID}/relogin`, "cp-admin")).status, 409);
    await e.store.updateAccount(ID, { status: "needs_relogin" });
    const rl = await e.call("POST", `/admin/accounts/${ID}/relogin`, "cp-admin");
    assert.equal(rl.status, 201);
    const code = (await rl.json()).url.split("/i/")[1];
    const inv = await e.store.getInvite(sha256hex(code));
    assert.equal(inv?.kind, "relogin");
    assert.equal(inv?.accountId, ID);
    assert.equal((await e.call("DELETE", `/admin/accounts/${ID}`, "cp-admin")).status, 202);
    assert.equal((await e.store.getAccount(ID))?.status, "deleting");
    assert.equal((await e.call("DELETE", "/admin/accounts/anotthere01", "cp-admin")).status, 404);
    assert.equal((await e.call("DELETE", "/admin/accounts/..%2Fx", "cp-admin")).status, 404);
  } finally { await e.stop(); }
});

test("token reissue: new hash, revision bump, one-time reveal", async () => {
  const e = await env();
  try {
    await e.store.insertAccount(account(ID, { status: "active", clientTokenHash: sha256hex(SECRETS.clientToken) }));
    await e.kube.apply(secretManifest(ID, SECRETS, 1, e.cfg));
    const r = await e.call("POST", `/admin/accounts/${ID}/token`, "cp-admin");
    assert.equal(r.status, 201);
    const url = (await r.json()).url as string;
    assert.match(url, /^https:\/\/join\.tarassov\.me\/t\//);
    const acc = (await e.store.getAccount(ID))!;
    assert.equal(acc.secretRev, 2);
    assert.notEqual(acc.clientTokenHash, sha256hex(SECRETS.clientToken));
    const sec = (await e.kube.getSecret(`tgw-${ID}`))!;
    assert.equal(sha256hex(sec.CLIENT_TOKEN), acc.clientTokenHash);
    assert.equal(sec.ADMIN_TOKEN, "ADM");
    assert.equal(sec.DATABASE_ENCRYPTION_KEY, "K");
    assert.ok(sec.BEARER_TOKENS.startsWith(`${sec.CLIENT_TOKEN} read,write`));
    const reveal = tokenReveal(e.deps);
    const code = url.split("/t/")[1];
    const first = await reveal(code);
    assert.ok("token" in first && first.token === sec.CLIENT_TOKEN);
    const second = await reveal(code);
    assert.ok("status" in second && second.status === 409);
  } finally { await e.stop(); }
});

test("registry: each consumer sees only its part", async () => {
  const e = await env();
  try {
    await e.store.insertAccount(account(ID, { status: "active", clientTokenHash: "HASH", archiveEnabled: true }));
    await e.store.insertAccount(account("aonboarding", { status: "onboarding" }));
    await e.kube.apply(secretManifest(ID, SECRETS, 1, e.cfg));
    const m = await (await e.call("GET", "/internal/registry", "reg-mcp")).json();
    assert.deepEqual(m, { tokens: [{ sha256: "HASH", accountIds: [ID], gatewayUrl: `http://tgw-${ID}.tgw-accounts.svc:8080`, serviceToken: "MCPSVC" }] });
    const a = await (await e.call("GET", "/internal/registry", "reg-arch")).json();
    assert.deepEqual(a, { accounts: { [ID]: { gatewayUrl: `http://tgw-${ID}.tgw-accounts.svc:8080`, archiverGatewayToken: "ARCHSVC", archive_enabled: true } } });
    assert.equal(JSON.stringify(m).includes("ARCHSVC"), false);
    assert.equal(JSON.stringify(a).includes("MCPSVC"), false);
    assert.equal((await e.call("GET", "/internal/registry", "cp-admin")).status, 401);
    assert.equal((await e.call("GET", "/internal/registry", null)).status, 401);
  } finally { await e.stop(); }
});
