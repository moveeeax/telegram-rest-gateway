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
