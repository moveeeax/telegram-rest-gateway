import { test } from "node:test";
import assert from "node:assert/strict";
import { pickAccount, WRITE_TOOLS, type AccountSet } from "../src/accounts.js";

const a = { accountId: "aaaaaaaaaa1", gatewayUrl: "http://a", serviceToken: "ta" };
const b = { accountId: "abbbbbbbbb2", gatewayUrl: "http://b", serviceToken: "tb" };
const one: AccountSet = { tokenHash: "h1", accounts: [a] };
const two: AccountSet = { tokenHash: "h2", accounts: [a, b] };

test("single account: empty or own id picks it, another id is refused", () => {
  assert.deepEqual(pickAccount(one, undefined, "telegram_get_me"), { ok: true, account: a });
  assert.deepEqual(pickAccount(one, "aaaaaaaaaa1", "telegram_get_me"), { ok: true, account: a });
  const r = pickAccount(one, "abbbbbbbbb2", "telegram_get_me");
  assert.deepEqual(r, { ok: false, error: "account abbbbbbbbb2 is not available for this token" });
});

test("several accounts: account is required for every tool, no default", () => {
  for (const tool of ["telegram_get_me", "telegram_send_message"]) {
    assert.deepEqual(pickAccount(two, undefined, tool), { ok: false, error: "account is required: one of aaaaaaaaaa1, abbbbbbbbb2" });
  }
  assert.deepEqual(pickAccount(two, "abbbbbbbbb2", "telegram_send_message"), { ok: true, account: b });
  assert.equal(pickAccount(two, "acccccccccc", "telegram_get_me").ok, false);
});

test("write tools list", () => {
  assert.deepEqual([...WRITE_TOOLS].sort(), [
    "telegram_delete_messages", "telegram_edit_message", "telegram_forward_messages", "telegram_mark_read", "telegram_react", "telegram_send_message",
  ]);
});
