import { test } from "node:test";
import assert from "node:assert/strict";
import {
  accountIdFromToken, clientToken, isAccountId, newAccountId, newCode, newSecret, sha256hex,
} from "../src/ids.js";

test("newAccountId: slug format, no repeats", () => {
  const seen = new Set<string>();
  for (let i = 0; i < 1000; i++) {
    const id = newAccountId();
    assert.match(id, /^a[a-z0-9]{10}$/);
    seen.add(id);
  }
  assert.equal(seen.size, 1000);
});

test("isAccountId: slug and numeric legacy ids only", () => {
  assert.equal(isAccountId("1214732998"), true);
  assert.equal(isAccountId(newAccountId()), true);
  assert.equal(isAccountId("A1234567890"), false);
  assert.equal(isAccountId("a123"), false);
  assert.equal(isAccountId("../x"), false);
  assert.equal(isAccountId(""), false);
});

test("client token carries its account id", () => {
  const id = newAccountId();
  const t = clientToken(id);
  assert.equal(accountIdFromToken(t), id);
  assert.equal(accountIdFromToken("nodot"), null);
  assert.equal(accountIdFromToken("../x.secret"), null);
});

test("newSecret: 32 bytes base64url", () => {
  const s = newSecret();
  assert.equal(s.length, 43);
  assert.match(s, /^[A-Za-z0-9_-]+$/);
  assert.notEqual(s, newSecret());
});

test("newCode and sha256hex", () => {
  assert.match(newCode(), /^[A-Za-z0-9_-]{32}$/);
  assert.equal(sha256hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
});
