import { test } from "node:test";
import assert from "node:assert/strict";
import { advanceDeletion, detectRelogin, pollActive } from "../src/lifecycle.js";
import { secretManifest } from "../src/manifests.js";
import { MemStore } from "../src/store_mem.js";
import { WorkerClient } from "../src/worker_client.js";
import { FakeKube } from "./helpers/fake_kube.js";
import { FakeWorker } from "./helpers/fake_worker.js";
import { FakeS3 } from "./helpers/onb_env.js";
import { account, config } from "./helpers/fixtures.js";

const ID = "adeleteme01";
const SECRETS = { clientToken: `${ID}.C`, adminToken: "ADM", mcpServiceToken: "M", archiverServiceToken: "R", databaseEncryptionKey: "K" };

async function env(url: string) {
  const store = new MemStore();
  const kube = new FakeKube();
  const s3 = new FakeS3();
  let kicks = 0;
  const deps = { store, kube, cfg: config(), s3, worker: (_: string, admin: string) => new WorkerClient(url, admin, 300), kick: () => { kicks++; } };
  return { store, kube, s3, deps, kicks: () => kicks };
}

test("deletion: logout, wait for pod, then S3, then the record", async () => {
  const w = new FakeWorker();
  const url = await w.start();
  try {
    const e = await env(url);
    w.state = { authorization_state: "ready", ready: true };
    await e.store.insertAccount(account(ID, { status: "deleting" }));
    await e.kube.apply(secretManifest(ID, SECRETS, 1, config()));
    e.kube.pods.add(ID);
    e.kube.podGoneAfter.set(ID, 3);
    await advanceDeletion((await e.store.getAccount(ID))!, e.deps);
    assert.ok(w.requests.some((r) => r.path === "/v1/auth/logout" && r.auth === "Bearer ADM"));
    assert.deepEqual(e.s3.deleted, []);
    e.kube.pods.delete(ID);
    await advanceDeletion((await e.store.getAccount(ID))!, e.deps);
    assert.deepEqual(e.s3.deleted, []); // podGone еще false
    await advanceDeletion((await e.store.getAccount(ID))!, e.deps);
    assert.deepEqual(e.s3.deleted, []); // и тут
    await advanceDeletion((await e.store.getAccount(ID))!, e.deps);
    assert.deepEqual(e.s3.deleted, [`telegram-sessions/${ID}/`]);
    assert.equal(await e.store.getAccount(ID), null);
    assert.equal(e.kube.objects.has(`Secret/tgw-${ID}`), false);
    assert.ok(e.kicks() >= 1);
  } finally { await w.stop(); }
});

test("deletion proceeds when the worker never answers", async () => {
  const e = await env("http://127.0.0.1:1");
  await e.store.insertAccount(account(ID, { status: "deleting" }));
  await e.kube.apply(secretManifest(ID, SECRETS, 1, config()));
  for (let i = 0; i < 5; i++) {
    const a = await e.store.getAccount(ID);
    if (!a) break;
    await advanceDeletion(a, e.deps);
  }
  assert.equal(await e.store.getAccount(ID), null);
  assert.deepEqual(e.s3.deleted, [`telegram-sessions/${ID}/`]);
});

test("deletion after duplicate skips logout", async () => {
  const w = new FakeWorker();
  const url = await w.start();
  try {
    const e = await env(url);
    await e.store.insertAccount(account(ID, { status: "deleting", deleteStep: "logged_out" }));
    await advanceDeletion((await e.store.getAccount(ID))!, e.deps);
    assert.equal(w.requests.some((r) => r.path === "/v1/auth/logout"), false);
    assert.equal(await e.store.getAccount(ID), null);
  } finally { await w.stop(); }
});

test("detectRelogin", () => {
  const a = account(ID, { status: "active" });
  for (const s of ["wait_phone_number", "closed", "logging_out"]) {
    assert.equal(detectRelogin(a, { authorization_state: s, ready: false }), "needs_relogin", s);
  }
  for (const s of ["ready", "wait_tdlib_parameters", "unknown"]) {
    assert.equal(detectRelogin(a, { authorization_state: s, ready: s === "ready" }), "active", s);
  }
  assert.equal(detectRelogin(a, null), "active");
  assert.equal(detectRelogin(account(ID, { status: "onboarding" }), { authorization_state: "wait_phone_number", ready: false }), "onboarding");
});

test("pollActive marks a logged-out worker needs_relogin", async () => {
  const w = new FakeWorker();
  const url = await w.start();
  try {
    const e = await env(url);
    await e.store.insertAccount(account(ID, { status: "active" }));
    await e.kube.apply(secretManifest(ID, SECRETS, 1, config()));
    w.state = { authorization_state: "wait_phone_number", ready: false };
    await pollActive(e.deps);
    assert.equal((await e.store.getAccount(ID))?.status, "needs_relogin");
  } finally { await w.stop(); }
});

test("deletion purges the archive after S3 and only then drops the record", async () => {
  const http = await import("node:http");
  const calls: string[] = [];
  let fail = true;
  const arch = http.createServer((req, res) => {
    calls.push(`${req.method} ${req.url} ${req.headers.authorization}`);
    res.writeHead(fail ? 500 : 200, { "content-type": "application/json" });
    res.end(fail ? "{}" : '{"ok":true}');
  });
  const url: string = await new Promise((r) => arch.listen(0, "127.0.0.1", () => r(`http://127.0.0.1:${(arch.address() as any).port}`)));
  try {
    const e = await env("http://127.0.0.1:1");
    (e.deps as any).cfg = config({ archiverAdminUrl: url, archiverAdminToken: "arch-adm" });
    await e.store.insertAccount(account(ID, { status: "deleting", deleteStep: "pod_gone" }));
    await advanceDeletion((await e.store.getAccount(ID))!, e.deps);
    assert.deepEqual(e.s3.deleted, [`telegram-sessions/${ID}/`]);
    assert.deepEqual(calls, [`DELETE /accounts/${ID} Bearer arch-adm`]);
    // purge не прошел: запись остается, шаг повторится
    assert.equal((await e.store.getAccount(ID))?.deleteStep, "s3_deleted");
    fail = false;
    await advanceDeletion((await e.store.getAccount(ID))!, e.deps);
    assert.equal(await e.store.getAccount(ID), null);
  } finally { arch.close(); }
});
