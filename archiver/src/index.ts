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
const GW_TEMPLATE = process.env.ARCHIVER_GATEWAY_TEMPLATE ?? "";
const MAX_BODY_BYTES = 64 * 1024;

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

/** Clamp limit like C++ queryInt: invalid/<1 → default; cap at max. */
function queryLimit(raw: string | null, def = 20, max = 100): number {
  const n = Number(raw ?? String(def));
  if (!Number.isFinite(n) || n < 1) return def;
  return Math.min(Math.floor(n), max);
}

/** Allow only the configured gateway template host (anti-SSRF for /backfill). */
function isAllowedGatewayUrl(url: string, sessionId: string): boolean {
  let parsed: URL;
  try {
    parsed = new URL(url);
  } catch {
    return false;
  }
  if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return false;

  if (GW_TEMPLATE) {
    const expected = GW_TEMPLATE.replaceAll("{sessionId}", sessionId).replace(/\/$/, "");
    let expectedUrl: URL;
    try {
      expectedUrl = new URL(expected);
    } catch {
      return false;
    }
    return (
      parsed.protocol === expectedUrl.protocol &&
      parsed.hostname === expectedUrl.hostname &&
      (parsed.port || defaultPort(parsed.protocol)) === (expectedUrl.port || defaultPort(expectedUrl.protocol)) &&
      (parsed.pathname === "/" || parsed.pathname === "")
    );
  }

  // Без шаблона — только localhost / cluster DNS / private IP (не открытый интернет).
  const host = parsed.hostname.toLowerCase();
  if (host === "localhost" || host === "127.0.0.1" || host === "::1") return true;
  if (host.endsWith(".svc") || host.endsWith(".svc.cluster.local") || host.endsWith(".cluster.local")) return true;
  if (isPrivateIp(host)) return true;
  return false;
}

function defaultPort(protocol: string): string {
  return protocol === "https:" ? "443" : "80";
}

function isPrivateIp(host: string): boolean {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
  if (!m) return false;
  const [a, b] = [Number(m[1]), Number(m[2])];
  if (a === 10) return true;
  if (a === 127) return true;
  if (a === 192 && b === 168) return true;
  if (a === 172 && b >= 16 && b <= 31) return true;
  if (a === 169 && b === 254) return true;
  return false;
}

async function handleEvent(frame: any): Promise<void> {
  const sessionId = String(frame.session_id ?? "default");
  const data = frame.data ?? {};

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
      // unknown type — still advance seq below if present
      break;
  }

  // seq после успешной обработки, иначе transient PG-ошибка + redelivery ломает gap-detect
  if (typeof frame.seq === "number") {
    const prev = await store.getSeq(sessionId);
    if (prev !== undefined && frame.seq > prev + 1) console.warn(`seq gap for ${sessionId}: ${prev} -> ${frame.seq}`);
    if (prev === undefined || frame.seq > prev) await store.setSeq(sessionId, frame.seq);
  }
  processed += 1;
}

// ---------------- Backfill ----------------
type BackfillState = { running: boolean; session_id?: string; chats_total: number; chats_done: number; messages_added: number; started_at?: number; finished_at?: number; error?: string };
let backfill: BackfillState = { running: false, chats_total: 0, chats_done: 0, messages_added: 0 };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function gwGet(base: string, token: string, path: string): Promise<any> {
  const resp = await fetch(`${base}${path}`, { headers: { Authorization: `Bearer ${token}` }, signal: AbortSignal.timeout(30000) });
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

async function readBody(req: http.IncomingMessage, maxBytes: number): Promise<{ ok: true; body: string } | { ok: false; tooLarge: true }> {
  let body = "";
  for await (const chunk of req) {
    body += typeof chunk === "string" ? chunk : Buffer.from(chunk).toString("utf8");
    if (body.length > maxBytes) return { ok: false, tooLarge: true };
  }
  return { ok: true, body };
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
          limit: queryLimit(url.searchParams.get("limit"), 20, 100),
        });
        return send(200, { ok: true, results: rows });
      }
      if (url.pathname === "/backfill" && req.method === "POST") {
        if (backfill.running) return send(409, { ok: false, error: "backfill already running", state: backfill });
        const raw = await readBody(req, MAX_BODY_BYTES);
        if (!raw.ok) return send(413, { ok: false, error: "body too large" });
        let cfg: any = {}; try { cfg = JSON.parse(raw.body || "{}"); } catch { return send(400, { ok: false, error: "bad json body" }); }
        if (!cfg.gateway_url || !cfg.token || !cfg.session_id) return send(400, { ok: false, error: "gateway_url, token, session_id are required" });
        const sessionId = String(cfg.session_id);
        const gatewayUrl = String(cfg.gateway_url).replace(/\/$/, "");
        if (!isAllowedGatewayUrl(gatewayUrl, sessionId)) {
          return send(400, {
            ok: false,
            error: GW_TEMPLATE
              ? "gateway_url must match ARCHIVER_GATEWAY_TEMPLATE for this session_id"
              : "gateway_url must be localhost, cluster DNS, or private IP",
          });
        }
        const throttleMs = queryLimit(String(cfg.throttle_ms ?? "300"), 300, 60_000);
        const maxPerChat = queryLimit(String(cfg.max_per_chat ?? "1000000"), 1_000_000, 5_000_000);
        void runBackfill(gatewayUrl, String(cfg.token), sessionId, throttleMs, maxPerChat);
        return send(200, { ok: true, started: true, session_id: sessionId });
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
      let frame: unknown;
      try {
        frame = JSON.parse(message.value.toString());
      } catch (e) {
        // poison pill — коммитим, иначе consumer зациклится на битом сообщении
        console.error("bad event (invalid json):", e);
        return;
      }
      // storage/transient: retry with backoff, then rethrow so kafkajs does not commit
      const attempts = 5;
      for (let i = 0; i < attempts; i++) {
        try {
          await handleEvent(frame);
          return;
        } catch (e) {
          console.error(`event handling failed (attempt ${i + 1}/${attempts}):`, e);
          if (i === attempts - 1) throw e;
          await sleep(1000 * (i + 1));
        }
      }
    },
  });
}

for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, async () => { try { await consumer.disconnect(); media?.stop(); } finally { process.exit(0); } });
}

main().catch((e) => { console.error("fatal:", e); process.exit(1); });
