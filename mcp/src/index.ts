#!/usr/bin/env node
/**
 * MCP-сервер поверх telegram-rest-gateway: агент получает инструменты Telegram-аккаунта.
 *
 * Конфиг через env:
 *   TGW_BASE_URL     — адрес гейтвея (default http://127.0.0.1:8080)
 *   TGW_BEARER_TOKEN — Bearer-токен API (обязателен)
 *
 * Один MCP-сервер = один аккаунт (инстанс гейтвея). Транспорт stdio.
 */
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { isInitializeRequest } from "@modelcontextprotocol/sdk/types.js";
import { createHash, randomUUID, timingSafeEqual } from "node:crypto";
import express from "express";
import { z } from "zod";

const BASE_URL = (process.env.TGW_BASE_URL ?? "http://127.0.0.1:8080").replace(/\/$/, "");
const TOKEN = process.env.TGW_BEARER_TOKEN ?? "";
if (!TOKEN) {
  console.error("TGW_BEARER_TOKEN is required");
  process.exit(1);
}

async function api(
  path: string,
  method: string = "GET",
  body?: unknown,
  headers: Record<string, string> = {},
): Promise<any> {
  const resp = await fetch(`${BASE_URL}${path}`, {
    method,
    headers: {
      Authorization: `Bearer ${TOKEN}`,
      ...(body !== undefined ? { "Content-Type": "application/json" } : {}),
      ...headers,
    },
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  const json = await resp.json().catch(() => ({ ok: false, error: { message: `HTTP ${resp.status}` } }));
  if (!json.ok) {
    const err = json.error ?? {};
    throw new Error(`${err.code ?? resp.status}: ${err.message ?? "request failed"}`);
  }
  return json;
}

function textResult(data: unknown) {
  return { content: [{ type: "text" as const, text: JSON.stringify(data, null, 2) }] };
}

const MAX_INLINE_BYTES = 1024 * 1024; // 1 МиБ — крупнее отдаём ссылкой на REST, а не base64 в ответе

// Маркер "файл больше лимита" — отличаем от прочих сбоев чтения потока.
class DownloadTooLargeError extends Error {}

// Читаем тело ответа потоково, не давая ему разрастись сверх лимита: под гейтвея живёт
// с лимитом памяти (128Mi), а Buffer.from(await resp.arrayBuffer()) буферизовал файл целиком
// ещё до проверки размера — большое видео валило под по OOMKill.
async function readLimited(
  body: ReadableStream<Uint8Array> | null,
  limit: number,
  controller: AbortController,
): Promise<Buffer> {
  if (!body) return Buffer.alloc(0);
  const reader = body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > limit) {
        // Обрываем закачку немедленно, не дочитывая остаток — именно это и защищает от OOM.
        controller.abort();
        throw new DownloadTooLargeError();
      }
      chunks.push(value);
    }
  } finally {
    reader.releaseLock();
  }
  return Buffer.concat(chunks);
}

function buildServer(): McpServer {
const server = new McpServer({
  name: "telegram-rest-gateway",
  version: "0.1.0",
});

server.tool(
  "telegram_get_me",
  "Информация об аккаунте Telegram, от имени которого работает гейтвей.",
  {},
  async () => textResult((await api("/v1/me")).data),
);

server.tool(
  "telegram_get_chats",
  "Список чатов аккаунта (главный список, по порядку). Возвращает id (строки!), title, type, unread_count.",
  {
    limit: z.number().int().min(1).max(100).default(20).describe("Сколько чатов вернуть"),
    offset: z.number().int().min(0).max(900).default(0).describe("Смещение для пагинации"),
  },
  async ({ limit, offset }) => {
    const r = await api(`/v1/chats?limit=${limit}&offset=${offset}`);
    return textResult({ chats: r.data, next_offset: r.meta?.next_offset ?? null });
  },
);

server.tool(
  "telegram_get_history",
  "История сообщений чата, от новых к старым. Медиа-сообщения содержат file_id — его можно скачать telegram_download_file.",
  {
    chat_id: z.string().describe("id чата (строка)"),
    limit: z.number().int().min(1).max(100).default(20),
    from_id: z.string().optional().describe("Курсор: id сообщения, с которого продолжить (meta.next_cursor прошлого вызова)"),
  },
  async ({ chat_id, limit, from_id }) => {
    const cursor = from_id ? `&from_id=${encodeURIComponent(from_id)}` : "";
    const r = await api(`/v1/chats/${encodeURIComponent(chat_id)}/messages?limit=${limit}${cursor}`);
    return textResult({ messages: r.data, next_cursor: r.meta?.next_cursor ?? null });
  },
);

server.tool(
  "telegram_send_message",
  "Отправить текстовое сообщение в чат. Возвращает временный id (202); финальный id появится в истории. parse_mode=markdown — это MarkdownV2 Telegram: спецсимволы _ * [ ] ( ) ~ ` > # + - = | { } . ! вне разметки ОБЯЗАТЕЛЬНО экранировать обратным слэшем, иначе VALIDATION_ERROR. Без parse_mode текст уходит как есть (надёжнее для обычных сообщений).",
  {
    chat_id: z.string().describe("id чата (строка). Для юзера по @username сначала telegram_resolve_username"),
    text: z.string().min(1),
    parse_mode: z.enum(["markdown", "html"]).optional(),
    reply_to_message_id: z.string().optional(),
    idempotency_key: z.string().optional().describe("Ключ идемпотентности для безопасных ретраев"),
  },
  async ({ chat_id, text, parse_mode, reply_to_message_id, idempotency_key }) => {
    const headers: Record<string, string> = {};
    if (idempotency_key) headers["Idempotency-Key"] = idempotency_key;
    const r = await api(
      `/v1/chats/${encodeURIComponent(chat_id)}/messages`,
      "POST",
      { text, ...(parse_mode ? { parse_mode } : {}), ...(reply_to_message_id ? { reply_to_message_id } : {}) },
      headers,
    );
    return textResult(r.data);
  },
);

server.tool(
  "telegram_edit_message",
  "Отредактировать текст своего сообщения.",
  {
    chat_id: z.string(),
    message_id: z.string(),
    text: z.string().min(1),
    parse_mode: z.enum(["markdown", "html"]).optional(),
  },
  async ({ chat_id, message_id, text, parse_mode }) => {
    const r = await api(
      `/v1/chats/${encodeURIComponent(chat_id)}/messages/${encodeURIComponent(message_id)}`,
      "PATCH",
      { text, ...(parse_mode ? { parse_mode } : {}) },
    );
    return textResult(r.data);
  },
);

server.tool(
  "telegram_delete_messages",
  "Удалить сообщения. revoke=true (default) — у всех участников; false — только у себя.",
  {
    chat_id: z.string(),
    message_ids: z.array(z.string()).min(1),
    revoke: z.boolean().default(true),
  },
  async ({ chat_id, message_ids, revoke }) => {
    await api(`/v1/chats/${encodeURIComponent(chat_id)}/messages`, "DELETE", { message_ids, revoke });
    return textResult({ deleted: message_ids.length });
  },
);

server.tool(
  "telegram_forward_messages",
  "Переслать сообщения из одного чата в другой.",
  {
    to_chat_id: z.string(),
    from_chat_id: z.string(),
    message_ids: z.array(z.string()).min(1),
    send_copy: z.boolean().default(false).describe("true — копия без пометки 'forwarded from'"),
  },
  async ({ to_chat_id, from_chat_id, message_ids, send_copy }) => {
    const r = await api(`/v1/chats/${encodeURIComponent(to_chat_id)}/messages/forward`, "POST", {
      from_chat_id,
      message_ids,
      send_copy,
    });
    return textResult(r.data);
  },
);

server.tool(
  "telegram_react",
  "Поставить или снять emoji-реакцию на сообщение.",
  {
    chat_id: z.string(),
    message_id: z.string(),
    emoji: z.string().describe("Например 👍 ❤️ 🔥"),
    remove: z.boolean().default(false),
  },
  async ({ chat_id, message_id, emoji, remove }) => {
    await api(
      `/v1/chats/${encodeURIComponent(chat_id)}/messages/${encodeURIComponent(message_id)}/reactions`,
      remove ? "DELETE" : "POST",
      { emoji },
    );
    return textResult({ ok: true, emoji, removed: remove });
  },
);

server.tool(
  "telegram_search_live",
  "ЖИВОЙ поиск Telegram (серверный, searchMessages/searchChatMessages) — всегда актуальный, без индексатора. Без chat_id — глобально по всем чатам; с chat_id — внутри чата. Отличие от telegram_search_history: этот идёт напрямую в Telegram (свежие данные, но по правилам поиска Telegram и без удалённых сообщений).",
  {
    query: z.string().min(1).describe("Поисковая строка"),
    chat_id: z.string().optional().describe("Ограничить одним чатом (иначе — глобально)"),
    limit: z.number().int().min(1).max(100).default(20),
    cursor: z.string().optional().describe("Курсор следующей страницы: next_offset (глобально) или next_from_id (по чату)"),
  },
  async ({ query, chat_id, limit, cursor }) => {
    let path: string;
    if (chat_id) {
      const c = cursor ? `&from_id=${encodeURIComponent(cursor)}` : "";
      path = `/v1/chats/${encodeURIComponent(chat_id)}/search?q=${encodeURIComponent(query)}&limit=${limit}${c}`;
    } else {
      const c = cursor ? `&offset=${encodeURIComponent(cursor)}` : "";
      path = `/v1/search?q=${encodeURIComponent(query)}&limit=${limit}${c}`;
    }
    const r = await api(path);
    return textResult({ results: r.data, total_count: r.meta?.total_count, next_cursor: r.meta?.next_from_id ?? r.meta?.next_offset ?? null });
  },
);

server.tool(
  "telegram_resolve_username",
  "Найти публичный чат/канал/пользователя по @username. Возвращает chat_id для остальных инструментов.",
  { username: z.string().describe("С @ или без") },
  async ({ username }) => textResult((await api(`/v1/resolve?username=${encodeURIComponent(username)}`)).data),
);

server.tool(
  "telegram_get_chat",
  "Карточка чата по id (тип, название). Работает для чатов, известных клиенту (из списка/resolve).",
  { chat_id: z.string() },
  async ({ chat_id }) => textResult((await api(`/v1/chats/${encodeURIComponent(chat_id)}`)).data),
);

server.tool(
  "telegram_get_user",
  "Карточка пользователя: имя, @username, телефон, онлайн-статус, premium.",
  { user_id: z.string() },
  async ({ user_id }) => textResult((await api(`/v1/users/${encodeURIComponent(user_id)}`)).data),
);

server.tool(
  "telegram_get_contacts",
  "Список id контактов аккаунта (карточки — через telegram_get_user).",
  {},
  async () => textResult((await api("/v1/contacts")).data),
);

server.tool(
  "telegram_download_file",
  "Скачать файл по file_id из медиа-сообщения. Картинки возвращаются как изображение (агент их видит), прочее — метаданные + base64 (до 1 МБ). Если файл ещё не докачан гейтвеем — вернёт статус, повтори через пару секунд.",
  { file_id: z.string() },
  async ({ file_id }) => {
    const controller = new AbortController();
    const resp = await fetch(`${BASE_URL}/v1/files/${encodeURIComponent(file_id)}`, {
      headers: { Authorization: `Bearer ${TOKEN}` },
      // maxBytes посреди потока (controller) совмещаем с таймаутом запроса,
      // чтобы зависший гейтвей не подвешивал инструмент бессрочно.
      signal: AbortSignal.any([AbortSignal.timeout(60000), controller.signal]),
    });
    const ctype = resp.headers.get("content-type") ?? "";
    if (resp.status === 202 || ctype.includes("application/json")) {
      const json: any = await resp.json();
      return textResult(json.data ?? json);
    }
    if (!resp.ok) {
      throw new Error(`download failed: HTTP ${resp.status}`);
    }
    const tooLargeResult = (size?: number) =>
      textResult({
        mime_type: ctype,
        ...(size !== undefined ? { size } : {}),
        note: "файл больше 1 МБ — скачай через REST: GET /v1/files/" + file_id,
      });
    // Content-Length известен заранее — отказываемся, не читая тело вообще.
    const declaredLength = Number(resp.headers.get("content-length"));
    if (Number.isFinite(declaredLength) && declaredLength > MAX_INLINE_BYTES) {
      await resp.body?.cancel().catch(() => {});
      return tooLargeResult(declaredLength);
    }
    let buf: Buffer;
    try {
      buf = await readLimited(resp.body, MAX_INLINE_BYTES, controller);
    } catch (e) {
      if (e instanceof DownloadTooLargeError) return tooLargeResult();
      throw e;
    }
    if (ctype.startsWith("image/")) {
      return { content: [{ type: "image" as const, data: buf.toString("base64"), mimeType: ctype }] };
    }
    return {
      content: [
        {
          type: "text" as const,
          text: JSON.stringify({ mime_type: ctype, size: buf.length, base64: buf.toString("base64") }),
        },
      ],
    };
  },
);

server.tool(
  "telegram_mark_read",
  "Отметить сообщения чата прочитанными.",
  { chat_id: z.string(), message_ids: z.array(z.string()).min(1) },
  async ({ chat_id, message_ids }) => {
    await api(`/v1/chats/${encodeURIComponent(chat_id)}/messages/read`, "POST", { message_ids });
    return textResult({ ok: true });
  },
);

// Поиск по архиву переписки (архиватор: Kafka -> SQLite FTS5). Регистрируется только
// если задан TGW_ARCHIVER_URL — гейтвей сам историю не индексирует.
const ARCHIVER_URL = (process.env.TGW_ARCHIVER_URL ?? "").replace(/\/$/, "");
if (ARCHIVER_URL) {
  server.tool(  // eslint-disable-line
    "telegram_search_history",
    "Полнотекстовый поиск по архиву переписки (все чаты, вся сохранённая история). Возвращает сниппеты с подсветкой <<...>>, chat_id и message_id для перехода к контексту через telegram_get_history.",
    {
      query: z.string().min(1).describe("Поисковые слова (AND-семантика)"),
      chat_id: z.string().optional().describe("Ограничить одним чатом"),
      limit: z.number().int().min(1).max(100).default(20),
    },
    async ({ query, chat_id, limit }) => {
      const params = new URLSearchParams({ q: query, limit: String(limit) });
      if (chat_id) params.set("chat_id", chat_id);
      const headers: Record<string, string> = {};
      if (process.env.TGW_ARCHIVER_TOKEN) {
        headers.Authorization = `Bearer ${process.env.TGW_ARCHIVER_TOKEN}`;
      }
      const resp = await fetch(`${ARCHIVER_URL}/search?${params}`, { headers });
      const json: any = await resp.json();
      if (!json.ok) throw new Error(json.error ?? `HTTP ${resp.status}`);
      return textResult(json.results);
    },
  );
}

  return server;
}

// Транспорт: MCP_HTTP_PORT => Streamable HTTP (для кластера, удалённые агенты);
// иначе stdio (локальный запуск как сабпроцесс MCP-клиента).
const HTTP_PORT = process.env.MCP_HTTP_PORT;
if (HTTP_PORT) {
  const HTTP_TOKEN = process.env.MCP_HTTP_TOKEN ?? "";
  const ALLOW_INSECURE = process.env.MCP_HTTP_ALLOW_INSECURE === "1";
  if (!HTTP_TOKEN) {
    // Fail-closed: без токена /mcp был бы открыт всем и молча проксировал бы к гейтвею с его
    // собственным read+write токеном — это полный захват аккаунта с сети. Опт-аут только явный.
    if (!ALLOW_INSECURE) {
      console.error(
        "tgw-mcp: MCP_HTTP_TOKEN не задан. В HTTP-режиме авторизация обязательна — иначе /mcp " +
          "открыт всем в сети и проксирует полный доступ гейтвея. Задайте MCP_HTTP_TOKEN, либо " +
          "явно примите риск через MCP_HTTP_ALLOW_INSECURE=1.",
      );
      process.exit(1);
    }
    console.error(
      "tgw-mcp: ВНИМАНИЕ — MCP_HTTP_TOKEN не задан, MCP_HTTP_ALLOW_INSECURE=1: /mcp работает " +
        "БЕЗ авторизации. Используйте только в доверенной сети/для отладки.",
    );
  }
  const app = express();
  app.use(express.json({ limit: "8mb" }));

  const unauthorized = (req: express.Request, res: express.Response): boolean => {
    if (!HTTP_TOKEN) return false; // явный опт-аут через MCP_HTTP_ALLOW_INSECURE=1
    // Сравниваем не сами строки, а их SHA-256-дайджесты фиксированной длины: так timingSafeEqual
    // не бросает исключение при разной длине токенов и не даёт судить о ней по времени ответа.
    const got = createHash("sha256").update(req.headers.authorization ?? "").digest();
    const want = createHash("sha256").update(`Bearer ${HTTP_TOKEN}`).digest();
    if (!timingSafeEqual(got, want)) {
      res.status(401).json({ jsonrpc: "2.0", error: { code: -32001, message: "unauthorized" }, id: null });
      return true;
    }
    return false;
  };

  app.get("/healthz", (_req, res) => {
    res.json({ ok: true });
  });

  // Stateful Streamable HTTP: сессия создаётся на initialize, живёт по mcp-session-id
  // (так работают удалённые MCP-клиенты — initialize -> session id -> дальнейшие запросы).
  const transports: Record<string, StreamableHTTPServerTransport> = {};
  // Время последней активности сессии — по нему реапер ниже подчищает забытые/зависшие сессии,
  // иначе transports растёт без границ (клиенты не всегда шлют DELETE /mcp).
  const lastActivity: Record<string, number> = {};
  const touch = (id: string) => {
    lastActivity[id] = Date.now();
  };

  const rawTtl = Number(process.env.MCP_SESSION_TTL_MS);
  const SESSION_TTL_MS = Number.isFinite(rawTtl) && rawTtl > 0 ? rawTtl : 30 * 60 * 1000;
  const reaper = setInterval(() => {
    const now = Date.now();
    for (const id of Object.keys(lastActivity)) {
      if (now - lastActivity[id] > SESSION_TTL_MS) {
        transports[id]?.close(); // триггерит onclose ниже — он и уберёт запись из обеих map
      }
    }
  }, Math.min(60_000, SESSION_TTL_MS));
  reaper.unref(); // фоновая уборка не должна держать процесс живым

  app.post("/mcp", async (req, res) => {
    if (unauthorized(req, res)) return;
    const sessionId = req.headers["mcp-session-id"] as string | undefined;
    let transport = sessionId ? transports[sessionId] : undefined;
    if (transport && sessionId) touch(sessionId);

    if (!transport && isInitializeRequest(req.body)) {
      transport = new StreamableHTTPServerTransport({
        sessionIdGenerator: () => randomUUID(),
        onsessioninitialized: (id) => {
          transports[id] = transport!;
          touch(id);
        },
      });
      transport.onclose = () => {
        if (transport!.sessionId) {
          delete transports[transport!.sessionId];
          delete lastActivity[transport!.sessionId];
        }
      };
      await buildServer().connect(transport);
    } else if (!transport) {
      res.status(400).json({ jsonrpc: "2.0", error: { code: -32000, message: "no valid session" }, id: null });
      return;
    }
    await transport.handleRequest(req, res, req.body);
  });

  // GET (SSE server->client) и DELETE (закрытие сессии) — по существующему session id.
  const bySession = async (req: express.Request, res: express.Response) => {
    if (unauthorized(req, res)) return;
    const sessionId = req.headers["mcp-session-id"] as string | undefined;
    const transport = sessionId ? transports[sessionId] : undefined;
    if (!transport) {
      res.status(400).send("invalid or missing session id");
      return;
    }
    if (sessionId) touch(sessionId);
    await transport.handleRequest(req, res);
  };
  app.get("/mcp", bySession);
  app.delete("/mcp", bySession);

  app.listen(Number(HTTP_PORT), () => console.error(`tgw-mcp: streamable-http on :${HTTP_PORT}`));
} else {
  const transport = new StdioServerTransport();
  await buildServer().connect(transport);
  console.error(`tgw-mcp: stdio connected to ${BASE_URL}`);
}
