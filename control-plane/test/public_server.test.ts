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

test("invite link sets a strict session cookie and redirects", async () => {
  const e = await onbEnv();
  const s = await serve(e);
  try {
    const code = await e.invite();
    const r = await fetch(`${s.base}/i/${code}`, { redirect: "manual" });
    assert.equal(r.status, 303);
    assert.equal(r.headers.get("location"), "/onboard");
    const c = r.headers.get("set-cookie") ?? "";
    for (const part of ["tgw_onb=", "HttpOnly", "Secure", "SameSite=Strict", "Path=/"]) assert.ok(c.includes(part), part);
    assert.equal(r.headers.get("cache-control"), "no-store");

    const other = await fetch(`${s.base}/i/${code}`, { redirect: "manual" });
    assert.equal(other.status, 409);
    assert.ok((await other.text()).includes("ссылка уже используется"));
  } finally { await s.close(); await e.stop(); }
});

test("state and submit endpoints use the cookie; admin token never leaks", async () => {
  const e = await onbEnv();
  const s = await serve(e);
  const bodies: string[] = [];
  try {
    const r = await fetch(`${s.base}/i/${await e.invite()}`, { redirect: "manual" });
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
