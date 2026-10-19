import { test } from "node:test";
import assert from "node:assert/strict";
import { bearerTokensValue, desiredObjects, secretManifest, workerName, type KubeObject } from "../src/manifests.js";
import { account, config } from "./helpers/fixtures.js";

const ID = "a1b2c3d4e5f";
const LABELS = { "tgw.tarassov.me/account-id": ID, "tgw.tarassov.me/managed-by": "control-plane" };

function byKind(objs: KubeObject[], kind: string): any {
  return objs.find((o) => o.kind === kind);
}
function env(sts: any): Record<string, any> {
  const c = sts.spec.template.spec.containers[0];
  return Object.fromEntries(c.env.map((e: any) => [e.name, e.value ?? e.valueFrom]));
}
const secrets = { clientToken: `${ID}.CLIENT`, adminToken: "ADMIN", mcpServiceToken: "MCPSVC", archiverServiceToken: "ARCHSVC", databaseEncryptionKey: "DBKEY" };

test("labels and namespace on every object", () => {
  for (const o of [...desiredObjects(account(ID, { status: "active" }), config()), secretManifest(ID, secrets, 1, config())]) {
    assert.equal(o.metadata.namespace, "tgw-accounts", o.kind);
    for (const [k, v] of Object.entries(LABELS)) assert.equal(o.metadata.labels[k], v, `${o.kind} ${k}`);
    assert.equal(o.metadata.name, workerName(ID));
  }
});

test("onboarding: no IngressRoute; active: IngressRoute", () => {
  assert.deepEqual(desiredObjects(account(ID), config()).map((o) => o.kind).sort(), ["NetworkPolicy", "Service", "StatefulSet"]);
  assert.deepEqual(desiredObjects(account(ID, { status: "active" }), config()).map((o) => o.kind).sort(),
    ["IngressRoute", "NetworkPolicy", "Service", "StatefulSet"]);
  assert.equal(byKind(desiredObjects(account(ID, { status: "needs_relogin" }), config()), "IngressRoute"), undefined);
});

test("replicas: 1 normally and until logout, 0 after logout while deleting", () => {
  const reps = (p: Parameters<typeof account>[1]) => byKind(desiredObjects(account(ID, p), config()), "StatefulSet").spec.replicas;
  assert.equal(reps({}), 1);
  // под нужен живым, пока lifecycle не сделал logout
  assert.equal(reps({ status: "deleting", deleteStep: null }), 1);
  assert.equal(reps({ status: "deleting", deleteStep: "logout_wait:2" }), 1);
  for (const s of ["logged_out", "scaled", "pod_gone", "s3_deleted"]) assert.equal(reps({ status: "deleting", deleteStep: s }), 0, s);
});

test("worker env: per-account secrets override shared, export off, kafka by archive flag", () => {
  const sts = byKind(desiredObjects(account(ID), config()), "StatefulSet");
  const e = env(sts);
  assert.equal(e.TGW_SESSION_ID, ID);
  assert.deepEqual(e.BEARER_TOKENS, { secretKeyRef: { name: workerName(ID), key: "BEARER_TOKENS" } });
  assert.deepEqual(e.DATABASE_ENCRYPTION_KEY, { secretKeyRef: { name: workerName(ID), key: "DATABASE_ENCRYPTION_KEY" } });
  assert.equal(e.TGW_SESSION_EXPORT_ENABLED, "false");
  assert.equal(e.TGW_KAFKA_BROKERS, "");
  const c = sts.spec.template.spec.containers[0];
  assert.deepEqual(c.envFrom, [{ configMapRef: { name: "tgw-worker-config" } }, { secretRef: { name: "telegram-rest-gateway" } }]);
  assert.equal(c.image, "ghcr.io/moveeeax/telegram-rest-gateway:abc1234");
  assert.deepEqual(c.resources, { requests: { cpu: "50m", memory: "64Mi" }, limits: { cpu: "500m", memory: "256Mi" } });
  const on = env(byKind(desiredObjects(account(ID, { archiveEnabled: true, exportEnabled: true }), config()), "StatefulSet"));
  assert.equal(on.TGW_KAFKA_BROKERS, "kafka.kafka.svc:9092");
  assert.equal(on.TGW_SESSION_EXPORT_ENABLED, "true");
});

test("secret revision annotation restarts the pod", () => {
  const rev = (n: number) => byKind(desiredObjects(account(ID, { secretRev: n }), config()), "StatefulSet")
    .spec.template.metadata.annotations["tgw.tarassov.me/secret-rev"];
  assert.equal(rev(1), "1");
  assert.equal(rev(2), "2");
});

test("IngressRoute rule: own prefix, public paths only", () => {
  const ir = byKind(desiredObjects(account(ID, { status: "active" }), config()), "IngressRoute");
  const route = ir.spec.routes[0];
  assert.match(route.match, /Host\(`tg\.tarassov\.me`\)/);
  assert.ok(route.match.includes("bearer a1b2c3d4e5f\\."));
  assert.ok(route.match.includes("bearer\\.a1b2c3d4e5f\\."));
  for (const bad of ["auth", "webhooks", "ui", "metrics"]) assert.equal(route.match.includes(bad), false, bad);
  assert.deepEqual(route.services, [{ name: workerName(ID), port: 8080 }]);
  assert.deepEqual(ir.spec.tls, {});
  assert.deepEqual(ir.spec.entryPoints, ["websecure"]);
});

test("NetworkPolicy admits only traefik and the tgw namespace", () => {
  const np = byKind(desiredObjects(account(ID), config()), "NetworkPolicy");
  assert.deepEqual(np.spec.podSelector, { matchLabels: { "tgw.tarassov.me/account-id": ID } });
  assert.deepEqual(np.spec.policyTypes, ["Ingress"]);
  const from = np.spec.ingress[0].from;
  assert.equal(from.length, 2);
  assert.deepEqual(np.spec.ingress[0].ports, [{ port: 8080, protocol: "TCP" }]);
});

test("BEARER_TOKENS: client token has no admin scope", () => {
  const lines = bearerTokensValue(secrets).split("\n");
  assert.deepEqual(lines, [`${ID}.CLIENT read,write`, "ADMIN", "MCPSVC read,write", "ARCHSVC read"]);
  const s = secretManifest(ID, secrets, 1, config()) as any;
  assert.equal(s.stringData.BEARER_TOKENS, bearerTokensValue(secrets));
  assert.equal(s.stringData.CLIENT_TOKEN, `${ID}.CLIENT`);
  assert.equal(s.stringData.ADMIN_TOKEN, "ADMIN");
  assert.equal(s.stringData.DATABASE_ENCRYPTION_KEY, "DBKEY");
});

test("unsafe account id is rejected", () => {
  assert.throws(() => desiredObjects(account("../x"), config()));
  assert.throws(() => secretManifest("../x", secrets, 1, config()));
});
