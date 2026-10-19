import { test } from "node:test";
import assert from "node:assert/strict";
import { onbEnv } from "./helpers/onb_env.js";
import { secretManifest } from "../src/manifests.js";
import { account } from "./helpers/fixtures.js";

test("QR path to ready: worker created, token shown exactly once", async () => {
  const e = await onbEnv();
  try {
    const code = await e.invite();
    const { r, cookie } = await e.open(code);
    assert.equal(r.kind, "redirect");
    assert.ok(cookie);
    const accs = await e.store.listAccounts();
    assert.equal(accs.length, 1);
    const id = accs[0].accountId;
    assert.match(id, /^a[a-z0-9]{10}$/);
    assert.equal(accs[0].status, "onboarding");
    assert.ok(e.kube.objects.has(`Secret/tgw-${id}`));
    assert.ok(e.kicks() >= 1);

    e.worker.healthy = false;
    assert.equal((await e.onb.view(cookie!)).phase, "starting");
    e.worker.healthy = true;
    const qr = await e.onb.view(cookie!);
    assert.equal(qr.phase, "qr");
    assert.match(qr.qr_svg ?? "", /^<svg/);
    assert.ok(e.worker.requests.some((q) => q.path === "/v1/auth/qr"));

    e.worker.state = { authorization_state: "ready", ready: true };
    const ready = await e.onb.view(cookie!);
    assert.equal(ready.phase, "ready");
    assert.match(ready.token ?? "", new RegExp(`^${id}\\.`));
    assert.equal(ready.api_url, "https://tg.tarassov.me");
    const again = await e.onb.view(cookie!);
    assert.equal(again.phase, "ready");
    assert.equal(again.token, undefined);

    const acc = await e.store.getAccount(id);
    assert.equal(acc?.status, "active");
    // архив у всех тенантов включен по умолчанию (решение владельца 2026-10-19)
    assert.equal(acc?.archiveEnabled, true);
    assert.equal(acc?.telegramUserId, "777");
    assert.equal(acc?.username, "tester");
    assert.equal(e.s3.metas.get(id).telegram_user_id, "777");
  } finally { await e.stop(); }
});

test("reload with the same cookie keeps one account; another browser gets in_use", async () => {
  const e = await onbEnv();
  try {
    const code = await e.invite();
    const first = await e.open(code);
    const reload = await e.open(code, first.cookie);
    assert.equal(reload.r.kind, "redirect");
    assert.equal((await e.store.listAccounts()).length, 1);
    const other = await e.onb.open(code, null);
    assert.equal(other.kind, "page");
    assert.equal(other.kind === "page" && other.status, 409);
    assert.ok(other.kind === "page" && other.message.includes("ссылка уже используется"));
  } finally { await e.stop(); }
});

test("cookie of a finished onboarding does not leak into the next invite", async () => {
  const e = await onbEnv();
  try {
    const first = await e.open(await e.invite());
    e.worker.state = { authorization_state: "ready", ready: true };
    assert.equal((await e.onb.view(first.cookie!)).phase, "ready");
    // тот же браузер открывает следующую ссылку с прежним cookie
    e.worker.state = { authorization_state: "wait_phone_number", ready: false };
    const second = await e.open(await e.invite(), first.cookie);
    assert.equal(second.r.kind, "redirect");
    assert.notEqual(second.cookie, first.cookie);
    const v = await e.onb.view(second.cookie!);
    assert.equal(v.phase, "qr");
    assert.equal((await e.store.listAccounts()).filter((a) => a.status === "onboarding").length, 1);
  } finally { await e.stop(); }
});

test("fourth concurrent onboarding is told to retry later", async () => {
  const e = await onbEnv();
  try {
    for (let i = 0; i < 3; i++) await e.open(await e.invite());
    const r = await e.onb.open(await e.invite(), null);
    assert.equal(r.kind === "page" && r.status, 503);
    assert.ok(r.kind === "page" && r.message.includes("попробуйте через несколько минут"));
    assert.equal((await e.store.listAccounts()).length, 3);
  } finally { await e.stop(); }
});

test("session that never became healthy expires; link works again", async () => {
  const e = await onbEnv();
  try {
    e.worker.healthy = false;
    const code = await e.invite();
    const { cookie } = await e.open(code);
    const id = (await e.store.listAccounts())[0].accountId;
    e.advance(16 * 60e3);
    await e.onb.expire(e.now());
    assert.equal((await e.store.getAccount(id))?.status, "deleting");
    assert.equal((await e.onb.view(cookie!)).phase, "expired");
    const fresh = await e.open(code);
    assert.equal(fresh.r.kind, "redirect");
    assert.equal((await e.store.listAccounts()).filter((a) => a.status === "onboarding").length, 1);
  } finally { await e.stop(); }
});

test("rotated QR link gives a new SVG", async () => {
  const e = await onbEnv();
  try {
    const { cookie } = await e.open(await e.invite());
    const a = await e.onb.view(cookie!);
    e.worker.state = { authorization_state: "wait_other_device_confirmation", ready: false, qr_link: "tg://login?token=BBB" };
    const b = await e.onb.view(cookie!);
    assert.equal(b.phase, "qr");
    assert.notEqual(a.qr_svg, b.qr_svg);
  } finally { await e.stop(); }
});

test("duplicate telegram account: existing untouched, new one logged out and deleting", async () => {
  const e = await onbEnv();
  try {
    await e.store.insertAccount(account("aexisting01", { status: "active", telegramUserId: "777" }));
    const { cookie } = await e.open(await e.invite());
    const id = (await e.store.listAccounts()).find((a) => a.accountId !== "aexisting01")!.accountId;
    e.worker.state = { authorization_state: "ready", ready: true };
    const v = await e.onb.view(cookie!);
    assert.equal(v.phase, "duplicate");
    assert.equal(v.token, undefined);
    assert.ok(e.worker.requests.some((q) => q.path === "/v1/auth/logout"));
    assert.equal((await e.store.getAccount("aexisting01"))?.status, "active");
    const dup = await e.store.getAccount(id);
    assert.equal(dup?.status, "deleting");
    assert.equal(dup?.deleteStep, "logged_out");
    assert.equal((await e.onb.view(cookie!)).phase, "duplicate");
  } finally { await e.stop(); }
});

test("phone, code and 2FA password are forwarded", async () => {
  const e = await onbEnv();
  try {
    const { cookie } = await e.open(await e.invite());
    await e.onb.view(cookie!);
    const c = await e.onb.submitPhone(cookie!, "+15550100");
    assert.equal(c.phase, "code");
    assert.equal(c.code_info?.length, 5);
    e.worker.state = { authorization_state: "wait_password", ready: false };
    assert.equal((await e.onb.submitCode(cookie!, "12345")).phase, "password");
    await e.onb.submitPassword(cookie!, "secret-pw");
    const sent = e.worker.requests.find((q) => q.path === "/v1/auth/password");
    assert.deepEqual(sent?.body, { password: "secret-pw" });
  } finally { await e.stop(); }
});

test("sixth code attempt ends the session with an error", async () => {
  const e = await onbEnv();
  try {
    const { cookie } = await e.open(await e.invite());
    await e.onb.view(cookie!);
    await e.onb.submitPhone(cookie!, "+15550100");
    for (let i = 0; i < 5; i++) await e.onb.submitCode(cookie!, "00000");
    const v = await e.onb.submitCode(cookie!, "00000");
    assert.equal(v.phase, "error");
    assert.equal(e.worker.requests.filter((q) => q.path === "/v1/auth/code").length, 5);
    assert.equal((await e.onb.view(cookie!)).phase, "error");
  } finally { await e.stop(); }
});

test("fourth phone number ends the session (shared API_ID protection)", async () => {
  const e = await onbEnv();
  try {
    const { cookie } = await e.open(await e.invite());
    await e.onb.view(cookie!);
    for (let i = 0; i < 3; i++) await e.onb.submitPhone(cookie!, "+1555010" + i);
    const v = await e.onb.submitPhone(cookie!, "+15550109");
    assert.equal(v.phase, "error");
    assert.equal(e.worker.requests.filter((q) => q.path === "/v1/auth/phone").length, 3);
  } finally { await e.stop(); }
});

test("abandoned account of an expired session goes to deleting when the link is reused", async () => {
  const e = await onbEnv();
  try {
    const code = await e.invite();
    await e.open(code);
    const first = (await e.store.listAccounts())[0].accountId;
    e.advance(16 * 60e3); // expire() еще не прошел
    const again = await e.open(code);
    assert.equal(again.r.kind, "redirect");
    assert.equal((await e.store.getAccount(first))?.status, "deleting");
    const accs = await e.store.listAccounts();
    assert.equal(accs.filter((a) => a.status === "onboarding").length, 1);
    assert.notEqual(accs.find((a) => a.status === "onboarding")!.accountId, first);
  } finally { await e.stop(); }
});

test("relogin: same telegram user returns to active without a new token", async () => {
  const e = await onbEnv();
  try {
    const id = "areloginacc";
    await e.store.insertAccount(account(id, { status: "needs_relogin", telegramUserId: "777", tokenShownAt: new Date() }));
    await e.kube.apply(secretManifest(id, { clientToken: `${id}.C`, adminToken: "ADM", mcpServiceToken: "m", archiverServiceToken: "r", databaseEncryptionKey: "k" }, 1, e.cfg));
    const { cookie } = await e.open(await e.invite("relogin", id));
    assert.equal((await e.store.listAccounts()).length, 1);
    e.worker.state = { authorization_state: "ready", ready: true };
    const v = await e.onb.view(cookie!);
    assert.equal(v.phase, "ready");
    assert.equal(v.token, undefined);
    assert.equal((await e.store.getAccount(id))?.status, "active");
  } finally { await e.stop(); }
});

test("relogin with a different telegram user is refused and logged out", async () => {
  const e = await onbEnv();
  try {
    const id = "areloginbad";
    await e.store.insertAccount(account(id, { status: "needs_relogin", telegramUserId: "999" }));
    await e.kube.apply(secretManifest(id, { clientToken: `${id}.C`, adminToken: "ADM", mcpServiceToken: "m", archiverServiceToken: "r", databaseEncryptionKey: "k" }, 1, e.cfg));
    const { cookie } = await e.open(await e.invite("relogin", id));
    e.worker.state = { authorization_state: "ready", ready: true };
    const v = await e.onb.view(cookie!);
    assert.equal(v.phase, "error");
    assert.match(v.error ?? "", /другим аккаунтом/);
    assert.ok(e.worker.requests.some((q) => q.path === "/v1/auth/logout"));
    const acc = await e.store.getAccount(id);
    assert.equal(acc?.status, "needs_relogin");
    assert.equal(acc?.secretRev, 2);
  } finally { await e.stop(); }
});

test("ready view shows MCP fields only when an MCP url is configured", async () => {
  const e = await onbEnv({ mcpBaseUrl: "https://mcp.tarassov.me" });
  try {
    const { cookie } = await e.open(await e.invite());
    e.worker.state = { authorization_state: "ready", ready: true };
    const v = await e.onb.view(cookie!);
    assert.equal(v.mcp_url, "https://mcp.tarassov.me/mcp");
    assert.ok(v.claude_mcp_add?.includes(v.token!));
  } finally { await e.stop(); }
  const f = await onbEnv();
  try {
    const { cookie } = await f.open(await f.invite());
    f.worker.state = { authorization_state: "ready", ready: true };
    const v = await f.onb.view(cookie!);
    assert.equal(v.mcp_url, undefined);
    assert.equal(v.claude_mcp_add, undefined);
  } finally { await f.stop(); }
});

test("unknown cookie or code", async () => {
  const e = await onbEnv();
  try {
    assert.equal((await e.onb.view("nope")).phase, "expired");
    const r = await e.onb.open("nope", null);
    assert.equal(r.kind === "page" && r.status, 404);
  } finally { await e.stop(); }
});
