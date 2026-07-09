/**
 * Архиватор: консьюмит события гейтвея из Kafka и складывает сообщения в SQLite (FTS5).
 *
 * Env:
 *   ARCHIVER_KAFKA_BROKERS — bootstrap (default redpanda:9092)
 *   ARCHIVER_KAFKA_TOPIC   — топик событий (default tgw.updates)
 *   ARCHIVER_KAFKA_GROUP   — consumer group (default tgw-archiver)
 *   ARCHIVER_DB_PATH       — путь к базе (default /data/archive.db)
 *   ARCHIVER_HTTP_PORT     — порт поиска (default 8090)
 *   ARCHIVER_TOKEN         — Bearer для HTTP (пусто = без auth, для внутренней сети)
 *
 * Семантика: Kafka даёт at-least-once — все записи идемпотентны
 * (PK session_id+chat_id+message_id, upsert). Дыры seq только логируем.
 */
import Database from "better-sqlite3";
import http from "node:http";
import { Kafka, logLevel } from "kafkajs";

const BROKERS = (process.env.ARCHIVER_KAFKA_BROKERS ?? "redpanda:9092").split(",");
const TOPIC = process.env.ARCHIVER_KAFKA_TOPIC ?? "tgw.updates";
const GROUP = process.env.ARCHIVER_KAFKA_GROUP ?? "tgw-archiver";
const DB_PATH = process.env.ARCHIVER_DB_PATH ?? "/data/archive.db";
const PORT = Number(process.env.ARCHIVER_HTTP_PORT ?? "8090");
const TOKEN = process.env.ARCHIVER_TOKEN ?? "";

// --- SQLite ---
const db = new Database(DB_PATH);
db.pragma("journal_mode = WAL");
db.exec(`
CREATE TABLE IF NOT EXISTS messages (
  session_id  TEXT NOT NULL,
  chat_id     TEXT NOT NULL,
  message_id  TEXT NOT NULL,
  date        INTEGER,
  is_outgoing INTEGER DEFAULT 0,
  type        TEXT,
  text        TEXT DEFAULT '',
  file_id     TEXT,
  file_name   TEXT,
  mime_type   TEXT,
  edited_at   INTEGER,
  deleted     INTEGER DEFAULT 0,
  PRIMARY KEY (session_id, chat_id, message_id)
);
CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
  text, content='messages', content_rowid='rowid', tokenize='unicode61'
);
CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
  INSERT INTO messages_fts(rowid, text) VALUES (new.rowid, new.text);
END;
CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
  INSERT INTO messages_fts(messages_fts, rowid, text) VALUES ('delete', old.rowid, old.text);
END;
CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE OF text ON messages BEGIN
  INSERT INTO messages_fts(messages_fts, rowid, text) VALUES ('delete', old.rowid, old.text);
  INSERT INTO messages_fts(rowid, text) VALUES (new.rowid, new.text);
END;
CREATE TABLE IF NOT EXISTS chats (
  session_id TEXT NOT NULL,
  chat_id    TEXT NOT NULL,
  title      TEXT,
  type       TEXT,
  PRIMARY KEY (session_id, chat_id)
);
CREATE TABLE IF NOT EXISTS seq_state (
  session_id TEXT PRIMARY KEY,
  last_seq   INTEGER
);
`);

const upsertMessage = db.prepare(`
INSERT INTO messages (session_id, chat_id, message_id, date, is_outgoing, type, text,
                      file_id, file_name, mime_type)
VALUES (@session_id, @chat_id, @message_id, @date, @is_outgoing, @type, @text,
        @file_id, @file_name, @mime_type)
ON CONFLICT(session_id, chat_id, message_id) DO UPDATE SET
  date = excluded.date, is_outgoing = excluded.is_outgoing, type = excluded.type,
  text = excluded.text, file_id = excluded.file_id, file_name = excluded.file_name,
  mime_type = excluded.mime_type
`);
const deleteRow = db.prepare(
  `DELETE FROM messages WHERE session_id = ? AND chat_id = ? AND message_id = ?`,
);
const updateText = db.prepare(
  `UPDATE messages SET text = @text, type = @type, file_id = @file_id
   WHERE session_id = @session_id AND chat_id = @chat_id AND message_id = @message_id`,
);
const markEdited = db.prepare(
  `UPDATE messages SET edited_at = ? WHERE session_id = ? AND chat_id = ? AND message_id = ?`,
);
const markDeleted = db.prepare(
  `UPDATE messages SET deleted = 1 WHERE session_id = ? AND chat_id = ? AND message_id = ?`,
);
const upsertChat = db.prepare(`
INSERT INTO chats (session_id, chat_id, title, type) VALUES (?, ?, ?, ?)
ON CONFLICT(session_id, chat_id) DO UPDATE SET title = excluded.title, type = excluded.type
`);
const getSeq = db.prepare(`SELECT last_seq FROM seq_state WHERE session_id = ?`);
const setSeq = db.prepare(`
INSERT INTO seq_state (session_id, last_seq) VALUES (?, ?)
ON CONFLICT(session_id) DO UPDATE SET last_seq = excluded.last_seq
`);

type Content = {
  type?: string;
  text?: string;
  caption?: string;
  file_id?: string;
  file_name?: string;
  mime_type?: string;
};

function messageRow(sessionId: string, msg: any) {
  const content: Content = msg.content ?? {};
  return {
    session_id: sessionId,
    chat_id: String(msg.chat_id ?? ""),
    message_id: String(msg.id ?? ""),
    date: msg.date ?? null,
    is_outgoing: msg.is_outgoing ? 1 : 0,
    type: content.type ?? "unknown",
    text: content.text ?? content.caption ?? "",
    file_id: content.file_id ?? null,
    file_name: content.file_name ?? null,
    mime_type: content.mime_type ?? null,
  };
}

let processed = 0;

function handleEvent(frame: any): void {
  const sessionId = String(frame.session_id ?? "default");
  const data = frame.data ?? {};

  // Детекция дыр в seq (потери в канале — метрика tgw_kafka_dropped_total на гейтвее).
  if (typeof frame.seq === "number") {
    const prev = getSeq.get(sessionId) as { last_seq: number } | undefined;
    if (prev && frame.seq > prev.last_seq + 1) {
      console.warn(`seq gap for ${sessionId}: ${prev.last_seq} -> ${frame.seq}`);
    }
    if (!prev || frame.seq > prev.last_seq) {
      setSeq.run(sessionId, frame.seq);
    }
  }

  switch (frame.update_type) {
    case "updateNewMessage": {
      const row = messageRow(sessionId, data);
      if (row.chat_id && row.message_id) upsertMessage.run(row);
      break;
    }
    case "updateMessageSendSucceeded": {
      // Временный id заменяется финальным: старую строку убираем, новую пишем.
      const msg = data.message ?? {};
      if (data.old_message_id && msg.chat_id) {
        deleteRow.run(sessionId, String(msg.chat_id), String(data.old_message_id));
      }
      const row = messageRow(sessionId, msg);
      if (row.chat_id && row.message_id) upsertMessage.run(row);
      break;
    }
    case "updateMessageContent": {
      const content: Content = data.new_content ?? {};
      updateText.run({
        session_id: sessionId,
        chat_id: String(data.chat_id ?? ""),
        message_id: String(data.message_id ?? ""),
        text: content.text ?? content.caption ?? "",
        type: content.type ?? "unknown",
        file_id: content.file_id ?? null,
      });
      break;
    }
    case "updateMessageEdited":
      markEdited.run(data.edit_date ?? null, sessionId, String(data.chat_id ?? ""),
                     String(data.message_id ?? ""));
      break;
    case "updateDeleteMessages":
      for (const id of data.message_ids ?? []) {
        markDeleted.run(sessionId, String(data.chat_id ?? ""), String(id));
      }
      break;
    case "updateNewChat":
      upsertChat.run(sessionId, String(data.id ?? ""), data.title ?? "", data.type ?? "");
      break;
    default:
      return; // interaction info / user status / typing — не архивируем
  }
  processed += 1;
}

// --- HTTP: /health, /stats, /search ---
function authorized(req: http.IncomingMessage): boolean {
  if (!TOKEN) return true;
  return req.headers.authorization === `Bearer ${TOKEN}`;
}

// FTS5-запрос из пользовательской строки: каждый токен в кавычках (инъекция синтаксиса
// FTS невозможна), соединение по AND.
function ftsQuery(q: string): string {
  return q
    .split(/\s+/)
    .filter(Boolean)
    .map((t) => `"${t.replaceAll('"', '""')}"`)
    .join(" ");
}

const searchStmt = db.prepare(`
SELECT m.session_id, m.chat_id, c.title AS chat_title, m.message_id, m.date, m.is_outgoing,
       m.type, m.text, m.file_id, m.deleted,
       snippet(messages_fts, 0, '<<', '>>', '…', 12) AS snippet
FROM messages_fts f
JOIN messages m ON m.rowid = f.rowid
LEFT JOIN chats c ON c.session_id = m.session_id AND c.chat_id = m.chat_id
WHERE messages_fts MATCH @q
  AND (@chat_id IS NULL OR m.chat_id = @chat_id)
  AND (@session_id IS NULL OR m.session_id = @session_id)
ORDER BY m.date DESC
LIMIT @limit
`);

const server = http.createServer((req, res) => {
  const url = new URL(req.url ?? "/", "http://localhost");
  res.setHeader("Content-Type", "application/json; charset=utf-8");

  if (url.pathname === "/health") {
    res.end(JSON.stringify({ ok: true }));
    return;
  }
  if (!authorized(req)) {
    res.statusCode = 401;
    res.end(JSON.stringify({ ok: false, error: "unauthorized" }));
    return;
  }
  if (url.pathname === "/stats") {
    const messages = (db.prepare(`SELECT COUNT(*) AS n FROM messages`).get() as any).n;
    const chats = (db.prepare(`SELECT COUNT(*) AS n FROM chats`).get() as any).n;
    res.end(JSON.stringify({ ok: true, messages, chats, processed_events: processed }));
    return;
  }
  if (url.pathname === "/search") {
    const q = url.searchParams.get("q") ?? "";
    if (!q.trim()) {
      res.statusCode = 400;
      res.end(JSON.stringify({ ok: false, error: "query param 'q' is required" }));
      return;
    }
    try {
      const rows = searchStmt.all({
        q: ftsQuery(q),
        chat_id: url.searchParams.get("chat_id"),
        session_id: url.searchParams.get("session_id"),
        limit: Math.min(Number(url.searchParams.get("limit") ?? "20"), 100),
      });
      res.end(JSON.stringify({ ok: true, results: rows }));
    } catch (e: any) {
      res.statusCode = 400;
      res.end(JSON.stringify({ ok: false, error: String(e?.message ?? e) }));
    }
    return;
  }
  res.statusCode = 404;
  res.end(JSON.stringify({ ok: false, error: "not found" }));
});
server.listen(PORT, () => console.error(`archiver: http on :${PORT}, db ${DB_PATH}`));

// --- Kafka consumer ---
const kafka = new Kafka({ clientId: "tgw-archiver", brokers: BROKERS, logLevel: logLevel.WARN });
const consumer = kafka.consumer({ groupId: GROUP });

async function main() {
  await consumer.connect();
  await consumer.subscribe({ topic: TOPIC, fromBeginning: true });
  console.error(`archiver: consuming ${TOPIC} from ${BROKERS.join(",")} (group ${GROUP})`);
  await consumer.run({
    eachMessage: async ({ message }) => {
      if (!message.value) return;
      try {
        handleEvent(JSON.parse(message.value.toString()));
      } catch (e) {
        console.error("bad event:", e);
      }
    },
  });
}

for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, async () => {
    try {
      await consumer.disconnect();
    } finally {
      db.close();
      process.exit(0);
    }
  });
}

main().catch((e) => {
  console.error("fatal:", e);
  process.exit(1);
});
