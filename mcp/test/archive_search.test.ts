import { test } from "node:test";
import assert from "node:assert/strict";
import { buildSearchParams, archiveSearchEnabled } from "../src/archive_search.js";

test("session_id always set", () => {
  const p = buildSearchParams("111", { query: "hi", limit: 20 });
  assert.equal(p.get("session_id"), "111");
  assert.equal(p.get("q"), "hi");
  assert.equal(p.get("limit"), "20");
  assert.equal(p.has("chat_id"), false);
});

test("chat_id passed through", () => {
  assert.equal(buildSearchParams("111", { query: "x", chat_id: "-100", limit: 5 }).get("chat_id"), "-100");
});

test("no session id -> disabled", () => {
  assert.equal(archiveSearchEnabled("http://a:8090", ""), false);
  assert.equal(archiveSearchEnabled("", "111"), false);
  assert.equal(archiveSearchEnabled("http://a:8090", "111"), true);
});
