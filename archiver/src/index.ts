/**
 * Архиватор: консьюмит события гейтвея из Kafka в хранилище (SQLite или PostgreSQL),
 * опционально оффлоадит медиа в S3 (в истории — ссылка), и умеет бэкфиллить историю.
 *
 * Env: ARCHIVER_KAFKA_BROKERS/_TOPIC/_GROUP, ARCHIVER_HTTP_PORT, ARCHIVER_TOKEN,
 *      ARCHIVER_PG_URL (иначе SQLite ARCHIVER_DB_PATH),
 *      ARCHIVER_S3_* + ARCHIVER_GATEWAY_TEMPLATE/_TOKEN (медиа-оффлоад).
 */
import http from "node:http";
import { Kafka, logLevel } from "kafkajs";
import { makeStore, type MessageRow } from "./store.js";
import { MediaOffloader } from "./media.js";

const BROKERS = (process.env.ARCHIVER_KAFKA_BROKERS ?? "redpanda:9092").split(",");
const TOPIC = process.env.ARCHIVER_KAFKA_TOPIC ?? "tgw.updates";
const GROUP = process.env.ARCHIVER_KAFKA_GROUP ?? "tgw-archiver";
const PORT = Number(process.env.ARCHIVER_HTTP_PORT ?? "8090");
const TOKEN = process.env.ARCHIVER_TOKEN ?? "";

const store = makeStore();
await store.init();
const media = MediaOffloader.fromEnv(store);
if (media) console.error("archiver: media offload enabled (S3)");

let processed = 0;

type Content = { type?: string; text?: string; caption?: string; file_id?: string; file_name?: string; mime_type?: string };

function messageRow(sessionId: string, msg: any): MessageRow {
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

function maybeOffload(r: MessageRow): void {
  if (media && r.file_id && r.chat_id && r.message_id) {
    media.enqueue({
      session_id: r.session_id,
      chat_id: r.chat_id,
      message_id: r.message_id,
      file_id: r.file_id,
      file_name: r.file_name ?? undefined,
      mime_type: r.mime_type ?? undefined,
    });
  }
}

async function handleEvent(frame: any): Promise<void> {
  const sessionId = String(frame.session_id ?? "default");
  const data = frame.data ?? {};

  if (typeof frame.seq === "number") {
    const prev = await store.getSeq(sessionId);
    if (prev !== undefined && frame.seq > prev + 1) console.warn(`seq gap for ${sessionId}: ${prev} -> ${frame.seq}`);
    if (prev === undefined || frame.seq > prev) await store.setSeq(sessionId, frame.seq);
  }

  switch (frame.update_type) {
    case "updateNewMessage": {
      const row = messageRow(sessionId, data);
      if (row.chat_id && row.message_id) { await store.upsertMessage(row); maybeOffload(row); }
      break;
    }
    case "updateMessageSendSucceeded": {
      const msg = data.message ?? {};
      if (data.old_message_id && msg.chat_id) await store.deleteRow(sessionId, String(msg.chat_id), String(data.old_message_id));
      const row = messageRow(sessionId, msg);
      if (row.chat_id && row.message_id) { await store.upsertMessage(row); maybeOffload(row); }
      break;
    }
    case "updateMessageContent": {
      const content: Content = data.new_content ?? {};
      await store.updateText({
        session_id: sessionId, chat_id: String(data.chat_id ?? ""), message_id: String(data.message_id ?? ""),
        text: content.text ?? content.caption ?? "", type: content.type ?? "unknown", file_id: content.file_id ?? null,
      });
      if (content.file_id) maybeOffload({ ...messageRow(sessionId, { id: data.message_id, chat_id: data.chat_id, content }) });
      break;
    }
    case "updateMessageEdited":
      await store.markEdited(data.edit_date ?? null, sessionId, String(data.chat_id ?? ""), String(data.message_id ?? ""));
      break;
    case "updateDeleteMessages":
      for (const id of data.message_ids ?? []) await store.markDeleted(sessionId, String(data.chat_id ?? ""), String(id));
      break;
    case "updateNewChat":
      await store.upsertChat(sessionId, String(data.id ?? ""), data.title ?? "", data.type ?? "");
      break;
    default:
      return;
  }
  processed += 1;
}

// ---------------- Backfill ----------------
type BackfillState = { running: boolean; session_id?: string; chats_total: number; chats_done: number; messages_added: number; started_at?: number; finished_at?: number; error?: string };
let backfill: BackfillState = { running: false, chats_total: 0, chats_done: 0, messages_added: 0 };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function gwGet(base: string, token: string, path: string): Promise<any> {
  const resp = await fetch(`${base}${path}`, { headers: { Authorization: `Bearer ${token}` } });
  const json: any = await resp.json();
  if (!json.ok) throw new Error(`${json.error?.code ?? resp.status}: ${json.error?.message ?? "gw error"}`);
  return json;
}

async function runBackfill(base: string, token: string, sessionId: string, throttleMs: number, maxPerChat: number) {
  backfill = { running: true, session_id: sessionId, chats_total: 0, chats_done: 0, messages_added: 0, started_at: Date.now() };
  try {
    const chatIds: string[] = [];
    for (let offset = 0; offset <= 900; offset += 100) {
      const r = await gwGet(base, token, `/v1/chats?limit=100&offset=${offset}`);
      const page = r.data ?? [];
      for (const c of page) { chatIds.push(String(c.id)); await store.upsertChat(sessionId, String(c.id), c.title ?? "", c.type ?? ""); }
      if (page.length < 100) break;
      await sleep(throttleMs);
    }
    backfill.chats_total = chatIds.length;
    for (const cid of chatIds) {
      let cursor = "0", got = 0, emptyRetries = 0;
      while (got < maxPerChat) {
        let page: any[] = [], next: string | null = null;
        try {
          const r = await gwGet(base, token, `/v1/chats/${encodeURIComponent(cid)}/messages?limit=100&from_id=${encodeURIComponent(cursor)}`);
          page = r.data ?? []; next = r.meta?.next_cursor ?? null;
        } catch { await sleep(throttleMs * 5); break; }
        if (page.length === 0) { if (emptyRetries < 3 && cursor !== "0") { emptyRetries++; await sleep(throttleMs * 2); continue; } break; }
        emptyRetries = 0;
        for (const m of page) {
          const row = messageRow(sessionId, m);
          if (row.chat_id && row.message_id) { await store.upsertMessage(row); maybeOffload(row); backfill.messages_added++; }
        }
        got += page.length;
        if (!next || next === cursor) break;
        cursor = next;
        await sleep(throttleMs);
      }
      backfill.chats_done++;
    }
  } catch (e: any) {
    backfill.error = String(e?.message ?? e);
  } finally {
    backfill.running = false;
    backfill.finished_at = Date.now();
  }
}

// ---------------- HTTP ----------------
function authorized(req: http.IncomingMessage): boolean {
  if (!TOKEN) return true;
  return req.headers.authorization === `Bearer ${TOKEN}`;
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url ?? "/", "http://localhost");
  res.setHeader("Content-Type", "application/json; charset=utf-8");
  const send = (code: number, obj: unknown) => { res.statusCode = code; res.end(JSON.stringify(obj)); };

  void (async () => {
    try {
      if (url.pathname === "/health") return send(200, { ok: true });
      if (!authorized(req)) return send(401, { ok: false, error: "unauthorized" });

      if (url.pathname === "/stats") {
        const s = await store.stats();
        return send(200, { ok: true, ...s, processed_events: processed, media: media?.stats() ?? null });
      }
      if (url.pathname === "/search") {
        const q = url.searchParams.get("q") ?? "";
        if (!q.trim()) return send(400, { ok: false, error: "query param 'q' is required" });
        const rows = await store.search({
          q, chat_id: url.searchParams.get("chat_id"), session_id: url.searchParams.get("session_id"),
          limit: Math.min(Number(url.searchParams.get("limit") ?? "20"), 100),
        });
        return send(200, { ok: true, results: rows });
      }
      if (url.pathname === "/backfill" && req.method === "POST") {
        if (backfill.running) return send(409, { ok: false, error: "backfill already running", state: backfill });
        let body = ""; for await (const c of req) body += c;
        let cfg: any = {}; try { cfg = JSON.parse(body || "{}"); } catch { return send(400, { ok: false, error: "bad json body" }); }
        if (!cfg.gateway_url || !cfg.token || !cfg.session_id) return send(400, { ok: false, error: "gateway_url, token, session_id are required" });
        void runBackfill(String(cfg.gateway_url).replace(/\/$/, ""), String(cfg.token), String(cfg.session_id), Number(cfg.throttle_ms ?? 300), Number(cfg.max_per_chat ?? 1000000));
        return send(200, { ok: true, started: true, session_id: cfg.session_id });
      }
      if (url.pathname === "/backfill") return send(200, { ok: true, backfill, media: media?.stats() ?? null });
      return send(404, { ok: false, error: "not found" });
    } catch (e: any) {
      send(500, { ok: false, error: String(e?.message ?? e) });
    }
  })();
});
server.listen(PORT, () => console.error(`archiver: http on :${PORT}`));

// ---------------- Kafka ----------------
const kafka = new Kafka({ clientId: "tgw-archiver", brokers: BROKERS, logLevel: logLevel.WARN });
const consumer = kafka.consumer({ groupId: GROUP });

async function main() {
  await consumer.connect();
  await consumer.subscribe({ topic: TOPIC, fromBeginning: true });
  console.error(`archiver: consuming ${TOPIC} from ${BROKERS.join(",")} (group ${GROUP})`);
  await consumer.run({
    eachMessage: async ({ message }) => {
      if (!message.value) return;
      try { await handleEvent(JSON.parse(message.value.toString())); }
      catch (e) { console.error("bad event:", e); }
    },
  });
}

for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, async () => { try { await consumer.disconnect(); media?.stop(); } finally { process.exit(0); } });
}

main().catch((e) => { console.error("fatal:", e); process.exit(1); });
