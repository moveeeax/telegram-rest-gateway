import { test } from "node:test";
import assert from "node:assert/strict";
import { retryMedia } from "../src/media_retry.js";
import type { MediaJob } from "../src/media.js";

const rows: MediaJob[] = [
  { session_id: "aaaaaaaaaa1", chat_id: "-1", message_id: "10", file_id: "f10" },
  { session_id: "aaaaaaaaaa1", chat_id: "-1", message_id: "11", file_id: "f11" },
];

function fakes() {
  const calls: { session: string | null; since: number; limit: number }[] = [];
  const queued: string[] = [];
  const forgotten: string[] = [];
  const store = { pendingMedia: async (session: string | null, since: number, limit: number) => { calls.push({ session, since, limit }); return rows; } };
  const media = { forget: (j: MediaJob) => { forgotten.push(j.message_id); }, enqueue: (j: MediaJob) => { queued.push(j.message_id); } };
  return { store, media, calls, queued, forgotten };
}

test("rows without media_url are re-queued past the dedup", async () => {
  const f = fakes();
  const n = await retryMedia(f.store as any, f.media as any, { session_id: "aaaaaaaaaa1" }, 1_700_000_000_000);
  assert.equal(n, 2);
  assert.deepEqual(f.forgotten, ["10", "11"]);
  assert.deepEqual(f.queued, ["10", "11"]);
  assert.equal(f.calls[0].session, "aaaaaaaaaa1");
});

test("since defaults to 7 days back, limit capped", async () => {
  const f = fakes();
  const now = 1_700_000_000_000;
  await retryMedia(f.store as any, f.media as any, {}, now);
  assert.equal(f.calls[0].session, null);
  assert.equal(f.calls[0].since, Math.floor(now / 1000) - 7 * 86400);
  assert.equal(f.calls[0].limit, 10000);
});

test("bad input is rejected", async () => {
  const f = fakes();
  await assert.rejects(retryMedia(f.store as any, f.media as any, { session_id: "../x" }, 0), /session_id/);
  await assert.rejects(retryMedia(f.store as any, f.media as any, { since: "yesterday" as any }, 0), /since/);
});
