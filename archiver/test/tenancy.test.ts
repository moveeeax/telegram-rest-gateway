import { test } from "node:test";
import assert from "node:assert/strict";
import { admitFrame, accessLevel, guardAdmin, parsePurgeId } from "../src/tenancy.js";
import type { ArchiveTarget } from "../src/registry.js";

const on: ArchiveTarget = { gatewayUrl: "http://a", token: "t", archiveEnabled: true };
const off: ArchiveTarget = { ...on, archiveEnabled: false };

test("admitFrame: archived stored, disabled/unknown dropped, registry not ready waits", async () => {
  assert.equal(await admitFrame("a1", async () => on), "store");
  assert.equal(await admitFrame("a1", async () => off), "drop");
  assert.equal(await admitFrame("a1", async () => null), "drop");
  assert.equal(await admitFrame("a1", async () => "unavailable"), "wait");
  // без реестра (локальный запуск) пишем все
  assert.equal(await admitFrame("a1", null), "store");
});

test("accessLevel and guardAdmin: MCP token on admin paths is 403, none is 401", () => {
  const tokens = { search: "s-tok", admin: "a-tok" };
  assert.equal(accessLevel("Bearer a-tok", tokens), "admin");
  assert.equal(accessLevel("Bearer s-tok", tokens), "search");
  assert.equal(accessLevel("Bearer nope", tokens), "none");
  assert.equal(accessLevel(undefined, tokens), "none");
  assert.equal(guardAdmin("admin"), null);
  assert.equal(guardAdmin("search"), 403);
  assert.equal(guardAdmin("none"), 401);
  // без admin-токена в конфиге админские пути закрыты для всех
  assert.equal(accessLevel("Bearer ", { search: "s-tok", admin: "" }), "none");
});

test("parsePurgeId accepts account ids only", () => {
  assert.equal(parsePurgeId("/accounts/a1b2c3d4e5f"), "a1b2c3d4e5f");
  assert.equal(parsePurgeId("/accounts/1000000001"), "1000000001");
  for (const p of ["/accounts/", "/accounts/..%2Fx", "/accounts/../x", "/accounts/a1/x", "/accounts/A1B2C3D4E5F"]) {
    assert.equal(parsePurgeId(p), null, p);
  }
});

import { waitForAdmission } from "../src/tenancy.js";
import { buildSessionIndex } from "../src/pg_index.js";

test("waitForAdmission: waits while unavailable, then admits", async () => {
  let n = 0;
  const lookup = async () => (++n < 3 ? "unavailable" : { gatewayUrl: "g", token: "t", archiveEnabled: true }) as any;
  let beats = 0;
  const v = await waitForAdmission("a1", lookup, { heartbeat: async () => { beats++; }, stopping: () => false, sleep: async () => {} });
  assert.equal(v, "store");
  assert.equal(beats, 2);
});

test("waitForAdmission: stops on shutdown and rethrows heartbeat errors (rebalance)", async () => {
  const lookup = async () => "unavailable" as const;
  let stop = false;
  const p = waitForAdmission("a1", lookup, { heartbeat: async () => { stop = true; }, stopping: () => stop, sleep: async () => {} });
  await assert.rejects(p, /stopping/);
  await assert.rejects(
    waitForAdmission("a1", lookup, { heartbeat: async () => { throw new Error("REBALANCE_IN_PROGRESS"); }, stopping: () => false, sleep: async () => {} }),
    /REBALANCE_IN_PROGRESS/,
  );
});

test("buildSessionIndex: dedicated client without query_timeout, CONCURRENTLY, always closed", async () => {
  const configs: any[] = [];
  const queries: string[] = [];
  let ended = 0;
  const make = (cfg: any) => {
    configs.push(cfg);
    return { connect: async () => {}, query: async (q: string) => { queries.push(q); return {}; }, end: async () => { ended++; } };
  };
  await buildSessionIndex("postgres://x", make);
  assert.equal(configs[0].connectionString, "postgres://x");
  assert.equal(configs[0].query_timeout, undefined);
  assert.equal(configs[0].statement_timeout, undefined);
  assert.deepEqual(queries, ["CREATE INDEX CONCURRENTLY IF NOT EXISTS messages_session_date_idx ON messages (session_id, date DESC)"]);
  assert.equal(ended, 1);
  const failing = () => ({ connect: async () => {}, query: async () => { throw new Error("boom"); }, end: async () => { ended++; } });
  await assert.rejects(buildSessionIndex("postgres://x", failing as any), /boom/);
  assert.equal(ended, 2);
});
