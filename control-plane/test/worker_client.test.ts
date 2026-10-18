import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { WorkerClient, workerBaseUrl } from "../src/worker_client.js";
import { FakeWorker } from "./helpers/fake_worker.js";
import { config } from "./helpers/fixtures.js";

const w = new FakeWorker();
let url = "";
before(async () => { url = await w.start(); });
after(async () => { await w.stop(); });

test("each call hits the right path with the admin token", async () => {
  const c = new WorkerClient(url, "ADMIN");
  w.requests = [];
  assert.equal(await c.health(), true);
  assert.equal((await c.startQr()).ok, true);
  const st = await c.state();
  assert.ok(st.ok && st.value.qr_link === "tg://login?token=AAA");
  await c.phone("+15550100");
  await c.code("12345");
  await c.password("pw");
  const me = await c.me();
  assert.ok(me.ok && me.value.id === "777");
  const lo = await c.logout();
  assert.ok(lo.ok && lo.value.logged_out === true);
  assert.deepEqual(w.requests.map((r) => `${r.method} ${r.path}`), [
    "GET /v1/health", "POST /v1/auth/qr", "GET /v1/auth/state", "POST /v1/auth/phone", "POST /v1/auth/code",
    "POST /v1/auth/password", "GET /v1/me", "POST /v1/auth/logout",
  ]);
  for (const r of w.requests.slice(1)) assert.equal(r.auth, "Bearer ADMIN");
  assert.deepEqual(w.requests[3].body, { phone_number: "+15550100" });
  assert.deepEqual(w.requests[4].body, { code: "12345" });
  assert.deepEqual(w.requests[5].body, { password: "pw" });
});

test("gateway error is returned, not thrown", async () => {
  const c = new WorkerClient(url, "ADMIN");
  w.failNext = { status: 400, body: { ok: false, error: { code: "PHONE_CODE_INVALID", message: "bad code" } } };
  const r = await c.code("0");
  assert.deepEqual(r, { ok: false, status: 400, error: "PHONE_CODE_INVALID: bad code" });
});

test("timeout gives status 0", async () => {
  const c = new WorkerClient(url, "ADMIN", 50);
  w.delayMs = 200;
  const r = await c.state();
  w.delayMs = 0;
  assert.equal(r.ok, false);
  assert.equal(!r.ok && r.status, 0);
});

test("health on a closed port is false", async () => {
  assert.equal(await new WorkerClient("http://127.0.0.1:1", "ADMIN", 200).health(), false);
});

test("workerBaseUrl points at the in-cluster service", () => {
  assert.equal(workerBaseUrl("a1b2c3d4e5f", config()), "http://tgw-a1b2c3d4e5f.tgw-accounts.svc:8080");
});
