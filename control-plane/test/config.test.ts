import { test } from "node:test";
import assert from "node:assert/strict";
import { loadConfig } from "../src/config.js";

const full = {
  CP_PG_URL: "postgres://u:p@h/db",
  CP_ADMIN_TOKEN: "adm",
  CP_REGISTRY_MCP_TOKEN: "rm",
  CP_REGISTRY_ARCHIVER_TOKEN: "ra",
  CP_WORKER_IMAGE: "ghcr.io/x/gw:abc",
  CP_S3_ENDPOINT: "http://minio:9000",
  CP_S3_BUCKET: "tgw-s3-bucket",
  CP_S3_ACCESS_KEY_ID: "k",
  CP_S3_SECRET_ACCESS_KEY: "s",
};

test("missing required variable is named", () => {
  assert.throws(() => loadConfig({}), /CP_PG_URL/);
});

test("empty admin token is an error", () => {
  assert.throws(() => loadConfig({ ...full, CP_ADMIN_TOKEN: "" }), /CP_ADMIN_TOKEN/);
});

test("defaults", () => {
  const c = loadConfig(full);
  assert.equal(c.publicPort, 8095);
  assert.equal(c.adminPort, 8096);
  assert.equal(c.workerNamespace, "tgw-accounts");
  assert.equal(c.sharedSecretName, "telegram-rest-gateway");
  assert.equal(c.workerConfigMap, "tgw-worker-config");
  assert.equal(c.joinBaseUrl, "https://join.tarassov.me");
  assert.equal(c.apiBaseUrl, "https://tg.tarassov.me");
  assert.equal(c.mcpBaseUrl, "");
  assert.equal(c.reconcileIntervalMs, 30000);
  assert.equal(c.kafkaBrokers, "");
  assert.equal(c.s3.region, "us-east-1");
  assert.equal(c.s3.forcePathStyle, true);
});
