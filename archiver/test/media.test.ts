import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import type { AddressInfo } from "node:net";
import { MediaOffloader, type MediaJob } from "../src/media.js";
import type { ArchiveTarget } from "../src/registry.js";

let gwStatus = 200;
const gwCalls: { path: string; auth: string }[] = [];
let gwUrl = "";
const gw = http.createServer((req, res) => {
  gwCalls.push({ path: req.url ?? "", auth: String(req.headers.authorization ?? "") });
  if (gwStatus !== 200) {
    res.writeHead(gwStatus, { "content-type": "application/json" });
    return res.end('{"ok":false}');
  }
  res.writeHead(200, { "content-type": "image/jpeg", "content-length": "3" });
  res.end("abc");
});
before(async () => {
  gwUrl = await new Promise((r) => gw.listen(0, "127.0.0.1", () => r(`http://127.0.0.1:${(gw.address() as AddressInfo).port}`)));
});
after(() => { gw.close(); });

const job = (session: string, msg: string): MediaJob => ({ session_id: session, chat_id: "-1", message_id: msg, file_id: "f-" + msg, file_name: "p.jpg" });
const until = async (pred: () => boolean, ms = 4000) => {
  const end = Date.now() + ms;
  while (Date.now() < end) { if (pred()) return true; await new Promise((r) => setTimeout(r, 25)); }
  return false;
};

function offloader(targets: (s: string) => Promise<ArchiveTarget | null | "unavailable">) {
  const urls: string[] = [];
  const puts: string[] = [];
  const store: any = { setMediaUrl: async (_s: string, _c: string, _m: string, u: string) => { urls.push(u); } };
  const m = new MediaOffloader(store, {
    bucket: "b", prefix: "media/", publicBase: "", maxBytes: 1000, targets,
    put: async (key: string) => { puts.push(key); },
  });
  return { m, urls, puts };
}

test("media uses the account's gateway and token", async () => {
  gwStatus = 200; gwCalls.length = 0;
  const { m, urls, puts } = offloader(async (s) => (s === "aaaaaaaaaa1" ? { gatewayUrl: gwUrl, token: "tok-a", archiveEnabled: true } : null));
  m.enqueue(job("aaaaaaaaaa1", "1"));
  assert.ok(await until(() => urls.length === 1));
  m.stop();
  assert.deepEqual(gwCalls.map((c) => [c.path, c.auth]), [["/v1/files/f-1", "Bearer tok-a"]]);
  assert.deepEqual(puts, ["media/aaaaaaaaaa1/-1/1/p.jpg"]);
});

test("401 is retried, not a permanent failure", async () => {
  gwStatus = 401; gwCalls.length = 0;
  const { m } = offloader(async () => ({ gatewayUrl: gwUrl, token: "stale", archiveEnabled: true }));
  m.enqueue(job("aaaaaaaaaa1", "2"));
  assert.ok(await until(() => gwCalls.length === 1 && m.stats().pending === 1));
  m.stop();
  assert.equal(m.stats().failed, 0);
});

test("404 (stale file_id) is permanent and not looped", async () => {
  gwStatus = 404; gwCalls.length = 0;
  const { m } = offloader(async () => ({ gatewayUrl: gwUrl, token: "t", archiveEnabled: true }));
  m.enqueue(job("aaaaaaaaaa1", "3"));
  assert.ok(await until(() => m.stats().failed === 1));
  m.stop();
  assert.equal(m.stats().pending, 0);
  assert.equal(gwCalls.length, 1);
});

test("registry unavailable retries; unknown account skips", async () => {
  gwCalls.length = 0;
  const a = offloader(async () => "unavailable");
  a.m.enqueue(job("aaaaaaaaaa1", "4"));
  assert.ok(await until(() => a.m.stats().pending === 1));
  a.m.stop();
  const b = offloader(async () => null);
  b.m.enqueue(job("agone000001", "5"));
  await until(() => false, 700);
  b.m.stop();
  assert.equal(b.m.stats().pending, 0);
  assert.equal(b.m.stats().failed, 0);
  assert.equal(gwCalls.length, 0);
});
