import { test } from "node:test";
import assert from "node:assert/strict";
import type { AddressInfo } from "node:net";
import { publicApp } from "../src/public_server.js";
import { onbEnv } from "./helpers/onb_env.js";

async function serve(e: Awaited<ReturnType<typeof onbEnv>>) {
  const app = publicApp(e.onb);
  const srv = app.listen(0, "127.0.0.1");
  await new Promise((r) => srv.once("listening", r));
  const base = `http://127.0.0.1:${(srv.address() as AddressInfo).port}`;
  return { base, close: () => new Promise((r) => srv.close(r)) };
}

const claimI = (base: string, code: string, cookie?: string) =>
  fetch(`${base}/i/${code}`, { method: "POST", headers: { "content-type": "application/json", ...(cookie ? { cookie } : {}) }, body: "{}" });

test("GET of an invite link (link preview) claims nothing", async () => {
  const e = await onbEnv();
  const s = await serve(e);
  try {
    const code = await e.invite();
    for (let i = 0; i < 3; i++) {
      const r = await fetch(`${s.base}/i/${code}`);
      assert.equal(r.status, 200);
      assert.equal(r.headers.get("set-cookie"), null);
      assert.ok((await r.text()).includes("Подключить"));
    }
    assert.equal((await e.store.listAccounts()).length, 0);
    assert.equal((await claimI(s.base, code)).status, 200);
    assert.equal((await e.store.listAccounts()).length, 1);
  } finally { await s.close(); await e.stop(); }
});

test("claim sets a strict session cookie; another browser gets in_use", async () => {
  const e = await onbEnv();
  const s = await serve(e);
  try {
    const code = await e.invite();
    const r = await claimI(s.base, code);
    assert.equal(r.status, 200);
    assert.deepEqual(await r.json(), { redirect: "/onboard" });
    const c = r.headers.get("set-cookie") ?? "";
    for (const part of ["tgw_onb=", "HttpOnly", "Secure", "SameSite=Strict", "Path=/"]) assert.ok(c.includes(part), part);
    assert.equal(r.headers.get("cache-control"), "no-store");
    // повторное открытие из Telegram (cookie Strict не пришел на навигацию, но пришел на fetch)
    const again = await claimI(s.base, code, c.split(";")[0]);
    assert.equal(again.status, 200);
    const other = await claimI(s.base, code);
    assert.equal(other.status, 409);
    assert.ok(JSON.stringify(await other.json()).includes("ссылка уже используется"));
    const form = await fetch(`${s.base}/i/${code}`, { method: "POST", headers: { "content-type": "application/x-www-form-urlencoded" }, body: "" });
    assert.equal(form.status, 415);
  } finally { await s.close(); await e.stop(); }
});

test("GET of a token link shows a button, POST reveals once", async () => {
  const e = await onbEnv();
  let reveals = 0;
  const app = (await import("../src/public_server.js")).publicApp(e.onb, async () => {
    reveals++;
    return reveals === 1 ? { token: "a1b2c3d4e5f.T", apiUrl: "https://tg.tarassov.me" } : { status: 409, message: "ссылка уже используется" };
  });
  const srv = app.listen(0, "127.0.0.1");
  await new Promise((r) => srv.once("listening", r));
  const base = `http://127.0.0.1:${(srv.address() as AddressInfo).port}`;
  try {
    const page = await fetch(`${base}/t/somecode`);
    assert.equal(page.status, 200);
    assert.equal((await page.text()).includes("a1b2c3d4e5f.T"), false);
    assert.equal(reveals, 0);
    const post = (await fetch(`${base}/t/somecode`, { method: "POST", headers: { "content-type": "application/json" }, body: "{}" }));
    assert.deepEqual(await post.json(), { token: "a1b2c3d4e5f.T", api_url: "https://tg.tarassov.me" });
    const second = await fetch(`${base}/t/somecode`, { method: "POST", headers: { "content-type": "application/json" }, body: "{}" });
    assert.equal(second.status, 409);
  } finally { await new Promise((r) => srv.close(r)); await e.stop(); }
});

test("state and submit endpoints use the cookie; admin token never leaks", async () => {
  const e = await onbEnv();
  const s = await serve(e);
  const bodies: string[] = [];
  try {
    const r = await claimI(s.base, await e.invite());
    const cookie = (r.headers.get("set-cookie") ?? "").split(";")[0];
    const page = await fetch(`${s.base}/onboard`, { headers: { cookie } });
    const html = await page.text();
    bodies.push(html);
    assert.ok(html.includes("Telegram → Настройки → Устройства → Подключить устройство"));
    assert.match(page.headers.get("content-security-policy") ?? "", /default-src 'none'/);
    const st = await fetch(`${s.base}/onboard/state`, { headers: { cookie } });
    const j = await st.json();
    bodies.push(JSON.stringify(j));
    assert.equal(j.phase, "qr");
    const ph = await fetch(`${s.base}/onboard/phone`, {
      method: "POST", headers: { cookie, "content-type": "application/json" }, body: JSON.stringify({ value: "+15550100" }),
    });
    const pj = await ph.json();
    bodies.push(JSON.stringify(pj));
    assert.equal(pj.phase, "code");
    // форма без JSON отклоняется: защита от отправки с чужой страницы
    const form = await fetch(`${s.base}/onboard/code`, {
      method: "POST", headers: { cookie, "content-type": "application/x-www-form-urlencoded" }, body: "value=1",
    });
    assert.equal(form.status, 415);
    e.worker.state = { authorization_state: "ready", ready: true };
    const ready = await (await fetch(`${s.base}/onboard/state`, { headers: { cookie } })).json();
    bodies.push(JSON.stringify(ready));
    assert.equal(ready.phase, "ready");
    assert.ok(ready.token);
    const id = (await e.store.listAccounts())[0].accountId;
    const admin = (await e.kube.getSecret(`tgw-${id}`))!.ADMIN_TOKEN;
    for (const b of bodies) assert.equal(b.includes(admin), false);
  } finally { await s.close(); await e.stop(); }
});

test("no cookie: expired; unknown paths 404; admin paths absent", async () => {
  const e = await onbEnv();
  const s = await serve(e);
  try {
    const st = await (await fetch(`${s.base}/onboard/state`)).json();
    assert.equal(st.phase, "expired");
    for (const p of ["/admin/accounts", "/internal/registry", "/", "/__tgw-edge"]) {
      assert.equal((await fetch(`${s.base}${p}`)).status, 404, p);
    }
    assert.equal((await fetch(`${s.base}/healthz`)).status, 200);
  } finally { await s.close(); await e.stop(); }
});
