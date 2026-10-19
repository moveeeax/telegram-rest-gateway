import { test } from "node:test";
import assert from "node:assert/strict";
import { reconcileOnce, startReconciler } from "../src/reconcile.js";
import { MemStore } from "../src/store_mem.js";
import { secretManifest, LABEL_ACCOUNT, LABEL_MANAGED } from "../src/manifests.js";
import { FakeKube } from "./helpers/fake_kube.js";
import { account, config } from "./helpers/fixtures.js";

const ID = "a1b2c3d4e5f";
const NOW = new Date("2026-10-18T10:05:00Z");

test("new onboarding account gets StatefulSet, Service, NetworkPolicy", async () => {
  const store = new MemStore();
  const kube = new FakeKube();
  await store.insertAccount(account(ID));
  const r = await reconcileOnce(store, kube, config(), NOW);
  assert.equal(r.applied, 3);
  assert.deepEqual(kube.applied.sort(), [`NetworkPolicy/tgw-${ID}`, `Service/tgw-${ID}`, `StatefulSet/tgw-${ID}`]);
});

test("labelled objects without a db record are deleted, but never the Secret", async () => {
  const store = new MemStore();
  const kube = new FakeKube();
  await kube.apply(secretManifest("azzzzzzzzzz", { clientToken: "c", adminToken: "a", mcpServiceToken: "m", archiverServiceToken: "r", databaseEncryptionKey: "k" }, 1, config()));
  await kube.apply({ apiVersion: "v1", kind: "Service", metadata: { name: "tgw-azzzzzzzzzz", namespace: "tgw-accounts", labels: { [LABEL_ACCOUNT]: "azzzzzzzzzz", [LABEL_MANAGED]: "control-plane" } } });
  const r = await reconcileOnce(store, kube, config(), NOW);
  // Secret держит ключ шифрования БД: его удаляет только lifecycle после зачистки S3.
  assert.equal(r.deleted, 1);
  assert.deepEqual([...kube.objects.keys()], ["Secret/tgw-azzzzzzzzzz"]);
});

test("objects without managed-by label are never touched", async () => {
  const store = new MemStore();
  const kube = new FakeKube();
  kube.putForeign("Service", "tgw-1000000001", "1000000001");
  await reconcileOnce(store, kube, config(), NOW);
  assert.deepEqual(kube.deleted, []);
  assert.ok(kube.objects.has("Service/tgw-1000000001"));
});

test("db read failure: nothing applied, nothing deleted", async () => {
  const store = new MemStore();
  const kube = new FakeKube();
  await kube.apply(secretManifest("azzzzzzzzzz", { clientToken: "c", adminToken: "a", mcpServiceToken: "m", archiverServiceToken: "r", databaseEncryptionKey: "k" }, 1, config()));
  kube.applied = [];
  store.failNextList = new Error("connection refused");
  await assert.rejects(reconcileOnce(store, kube, config(), NOW), /connection refused/);
  assert.deepEqual(kube.deleted, []);
  assert.deepEqual(kube.applied, []);
});

test("restart mid-onboarding keeps the onboarding objects", async () => {
  const store = new MemStore();
  const kube = new FakeKube();
  await store.insertAccount(account(ID, { createdAt: new Date("2026-10-18T10:00:00Z") }));
  await reconcileOnce(store, kube, config(), NOW);
  // новый процесс: тот же store и кластер, свежий reconciler
  const again = await reconcileOnce(store, kube, config(), new Date("2026-10-18T10:10:00Z"));
  assert.equal(again.deleted, 0);
  assert.ok(kube.objects.has(`StatefulSet/tgw-${ID}`));
});

test("active -> needs_relogin drops IngressRoute, keeps StatefulSet and Secret", async () => {
  const store = new MemStore();
  const kube = new FakeKube();
  await store.insertAccount(account(ID, { status: "active" }));
  await kube.apply(secretManifest(ID, { clientToken: "c", adminToken: "a", mcpServiceToken: "m", archiverServiceToken: "r", databaseEncryptionKey: "k" }, 1, config()));
  await reconcileOnce(store, kube, config(), NOW);
  assert.ok(kube.objects.has(`IngressRoute/tgw-${ID}`));
  await store.updateAccount(ID, { status: "needs_relogin" });
  await reconcileOnce(store, kube, config(), NOW);
  assert.equal(kube.objects.has(`IngressRoute/tgw-${ID}`), false);
  assert.ok(kube.objects.has(`StatefulSet/tgw-${ID}`));
  assert.ok(kube.objects.has(`Secret/tgw-${ID}`));
});

test("kick coalesces: five kicks, one pass at a time", async () => {
  const store = new MemStore();
  const kube = new FakeKube();
  let passes = 0;
  let concurrent = 0;
  let maxConcurrent = 0;
  const orig = store.listAccounts.bind(store);
  store.listAccounts = async () => {
    passes++;
    concurrent++;
    maxConcurrent = Math.max(maxConcurrent, concurrent);
    await new Promise((r) => setTimeout(r, 20));
    concurrent--;
    return orig();
  };
  const rec = startReconciler(store, kube, config({ reconcileIntervalMs: 3_600_000 }), { hooks: [] });
  for (let i = 0; i < 5; i++) rec.kick();
  await new Promise((r) => setTimeout(r, 150));
  rec.stop();
  assert.equal(maxConcurrent, 1);
  assert.ok(passes <= 2, `passes=${passes}`);
  assert.ok(passes >= 1);
});
