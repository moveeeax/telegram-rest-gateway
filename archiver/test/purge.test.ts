import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

// SQLite нужен нативный better-sqlite3: на машине без собранного модуля тест пропускается (CI его собирает).
let SqliteStore: any = null;
try { ({ SqliteStore } = await import("../src/store.js")); new SqliteStore(":memory:"); } catch { SqliteStore = null; }

test("purgeSession removes one account's rows only; search requires its session", { skip: SqliteStore ? false : "better-sqlite3 not built" }, async () => {
  const s = new SqliteStore(join(mkdtempSync(join(tmpdir(), "arch-")), "a.db"));
  await s.init();
  for (const sid of ["a1b2c3d4e5f", "azzzzzzzzzz"]) {
    await s.upsertMessage({ session_id: sid, chat_id: "-1", message_id: "1", date: 100, is_outgoing: 0, type: "text", text: "hello world", file_id: null, file_name: null, mime_type: null });
    await s.upsertChat(sid, "-1", "chat", "group");
    await s.setSeq(sid, 5);
  }
  const r = await s.purgeSession("a1b2c3d4e5f");
  assert.deepEqual(r, { messages: 1, chats: 1 });
  assert.equal((await s.search({ q: "hello", session_id: "a1b2c3d4e5f", limit: 10 })).length, 0);
  assert.equal((await s.search({ q: "hello", session_id: "azzzzzzzzzz", limit: 10 })).length, 1);
  assert.equal(await s.getSeq("a1b2c3d4e5f"), undefined);
  assert.equal(await s.getSeq("azzzzzzzzzz"), 5);
});
