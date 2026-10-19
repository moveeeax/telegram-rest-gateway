// Инструменты MCP. Каждый вызов выбирает аккаунт из набора токена (параметр account) и ходит в
// гейтвей этого аккаунта его сервисным токеном. В ответе всегда поле account.
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z, type ZodRawShape } from "zod";
import { pickAccount, type AccountRef, type AccountSet } from "./accounts.js";
import { buildSearchParams } from "./archive_search.js";
import { GatewayClient } from "./gateway.js";

export type ToolContext = { accounts: () => AccountSet | null; archiverUrl: string; archiverToken: string };

function textResult(data: unknown) {
  return { content: [{ type: "text" as const, text: JSON.stringify(data, null, 2) }] };
}

// Поле account в ответе: объект дополняется им, остальное заворачивается в {account, result}.
function withAccount(result: any, account: string): any {
  const isError = result?.isError === true;
  if (isError) return result;
  const first = result?.content?.[0];
  if (first?.type === "text") {
    try {
      const data = JSON.parse(first.text);
      const merged = data !== null && typeof data === "object" && !Array.isArray(data) ? { account, ...data } : { account, result: data };
      return { ...result, content: [{ type: "text" as const, text: JSON.stringify(merged, null, 2) }, ...result.content.slice(1)] };
    } catch {
      return result;
    }
  }
  return { ...result, content: [{ type: "text" as const, text: JSON.stringify({ account }) }, ...(result?.content ?? [])] };
}

function errorResult(message: string) {
  return { isError: true, content: [{ type: "text" as const, text: message }] };
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

export function buildServer(ctx: ToolContext): McpServer {
  const server = new McpServer({ name: "telegram-rest-gateway", version: "0.2.0" });
  const accountParam = z.string().optional().describe(
    "Аккаунт (accountId из telegram_list_accounts). Обязателен, если токену доступно больше одного аккаунта.",
  );

  // Обертка над server.tool: параметр account, выбор аккаунта, клиент гейтвея, поле account в ответе.
  function tool<S extends ZodRawShape>(
    name: string, description: string, shape: S,
    handler: (args: z.objectOutputType<S, z.ZodTypeAny>, gw: GatewayClient, account: AccountRef) => Promise<any>,
  ) {
    server.tool(name, description, { ...shape, account: accountParam }, (async (args: any) => {
      const set = ctx.accounts();
      if (!set) return errorResult("unauthorized: token is not resolved");
      const pick = pickAccount(set, args.account, name);
      if (!pick.ok) return errorResult(pick.error);
      const { account: _drop, ...rest } = args;
      const gw = new GatewayClient(pick.account.gatewayUrl, pick.account.serviceToken);
      return withAccount(await handler(rest, gw, pick.account), pick.account.accountId);
    }) as any);
  }

  server.tool(
    "telegram_list_accounts",
    "Аккаунты Telegram, доступные этому ключу: accountId (его передавать в параметр account остальных инструментов), id и имя.",
    {},
    async () => {
      const set = ctx.accounts();
      if (!set) return errorResult("unauthorized: token is not resolved");
      const accounts = await Promise.all(set.accounts.map(async (a) => {
        try {
          const me = (await new GatewayClient(a.gatewayUrl, a.serviceToken).api("/v1/me")).data;
          return { account: a.accountId, id: me.id, username: me.username ?? null, first_name: me.first_name ?? null };
        } catch (e) {
          return { account: a.accountId, error: (e as Error).message };
        }
      }));
      return textResult({ accounts });
    },
  );

tool(
  "telegram_get_me",
  "Информация об аккаунте Telegram, от имени которого работает гейтвей.",
  {},
  async (_args, gw) => textResult((await gw.api("/v1/me")).data),
);

tool(
  "telegram_get_chats",
  "Список чатов аккаунта (главный список, по порядку). Возвращает id (строки!), title, type, unread_count.",
  {
    limit: z.number().int().min(1).max(100).default(20).describe("Сколько чатов вернуть"),
    offset: z.number().int().min(0).max(900).default(0).describe("Смещение для пагинации"),
  },
  async ({ limit, offset }, gw) => {
    const r = await gw.api(`/v1/chats?limit=${limit}&offset=${offset}`);
    return textResult({ chats: r.data, next_offset: r.meta?.next_offset ?? null });
  },
);

tool(
  "telegram_get_history",
  "История сообщений чата, от новых к старым. Медиа-сообщения содержат file_id — его можно скачать telegram_download_file.",
  {
    chat_id: z.string().describe("id чата (строка)"),
    limit: z.number().int().min(1).max(100).default(20),
    from_id: z.string().optional().describe("Курсор: id сообщения, с которого продолжить (meta.next_cursor прошлого вызова)"),
  },
  async ({ chat_id, limit, from_id }, gw) => {
    const cursor = from_id ? `&from_id=${encodeURIComponent(from_id)}` : "";
    const r = await gw.api(`/v1/chats/${encodeURIComponent(chat_id)}/messages?limit=${limit}${cursor}`);
    return textResult({ messages: r.data, next_cursor: r.meta?.next_cursor ?? null });
  },
);

tool(
  "telegram_send_message",
  "Отправить текстовое сообщение в чат. Возвращает временный id (202); финальный id появится в истории. parse_mode=markdown — это MarkdownV2 Telegram: спецсимволы _ * [ ] ( ) ~ ` > # + - = | { } . ! вне разметки ОБЯЗАТЕЛЬНО экранировать обратным слэшем, иначе VALIDATION_ERROR. Без parse_mode текст уходит как есть (надёжнее для обычных сообщений).",
  {
    chat_id: z.string().describe("id чата (строка). Для юзера по @username сначала telegram_resolve_username"),
    text: z.string().min(1),
    parse_mode: z.enum(["markdown", "html"]).optional(),
    reply_to_message_id: z.string().optional(),
    idempotency_key: z.string().optional().describe("Ключ идемпотентности для безопасных ретраев"),
  },
  async ({ chat_id, text, parse_mode, reply_to_message_id, idempotency_key }, gw) => {
    const headers: Record<string, string> = {};
    if (idempotency_key) headers["Idempotency-Key"] = idempotency_key;
    const r = await gw.api(
      `/v1/chats/${encodeURIComponent(chat_id)}/messages`,
      "POST",
      { text, ...(parse_mode ? { parse_mode } : {}), ...(reply_to_message_id ? { reply_to_message_id } : {}) },
      headers,
    );
    return textResult(r.data);
  },
);

tool(
  "telegram_edit_message",
  "Отредактировать текст своего сообщения.",
  {
    chat_id: z.string(),
    message_id: z.string(),
    text: z.string().min(1),
    parse_mode: z.enum(["markdown", "html"]).optional(),
  },
  async ({ chat_id, message_id, text, parse_mode }, gw) => {
    const r = await gw.api(
      `/v1/chats/${encodeURIComponent(chat_id)}/messages/${encodeURIComponent(message_id)}`,
      "PATCH",
      { text, ...(parse_mode ? { parse_mode } : {}) },
    );
    return textResult(r.data);
  },
);

tool(
  "telegram_delete_messages",
  "Удалить сообщения. revoke=true (default) — у всех участников; false — только у себя.",
  {
    chat_id: z.string(),
    message_ids: z.array(z.string()).min(1),
    revoke: z.boolean().default(true),
  },
  async ({ chat_id, message_ids, revoke }, gw) => {
    await gw.api(`/v1/chats/${encodeURIComponent(chat_id)}/messages`, "DELETE", { message_ids, revoke });
    return textResult({ deleted: message_ids.length });
  },
);

tool(
  "telegram_forward_messages",
  "Переслать сообщения из одного чата в другой.",
  {
    to_chat_id: z.string(),
    from_chat_id: z.string(),
    message_ids: z.array(z.string()).min(1),
    send_copy: z.boolean().default(false).describe("true — копия без пометки 'forwarded from'"),
  },
  async ({ to_chat_id, from_chat_id, message_ids, send_copy }, gw) => {
    const r = await gw.api(`/v1/chats/${encodeURIComponent(to_chat_id)}/messages/forward`, "POST", {
      from_chat_id,
      message_ids,
      send_copy,
    });
    return textResult(r.data);
  },
);

tool(
  "telegram_react",
  "Поставить или снять emoji-реакцию на сообщение.",
  {
    chat_id: z.string(),
    message_id: z.string(),
    emoji: z.string().describe("Например 👍 ❤️ 🔥"),
    remove: z.boolean().default(false),
  },
  async ({ chat_id, message_id, emoji, remove }, gw) => {
    await gw.api(
      `/v1/chats/${encodeURIComponent(chat_id)}/messages/${encodeURIComponent(message_id)}/reactions`,
      remove ? "DELETE" : "POST",
      { emoji },
    );
    return textResult({ ok: true, emoji, removed: remove });
  },
);

tool(
  "telegram_search_live",
  "ЖИВОЙ поиск Telegram (серверный, searchMessages/searchChatMessages) — всегда актуальный, без индексатора. Без chat_id — глобально по всем чатам; с chat_id — внутри чата. Отличие от telegram_search_history: этот идёт напрямую в Telegram (свежие данные, но по правилам поиска Telegram и без удалённых сообщений).",
  {
    query: z.string().min(1).describe("Поисковая строка"),
    chat_id: z.string().optional().describe("Ограничить одним чатом (иначе — глобально)"),
    limit: z.number().int().min(1).max(100).default(20),
    cursor: z.string().optional().describe("Курсор следующей страницы: next_offset (глобально) или next_from_id (по чату)"),
  },
  async ({ query, chat_id, limit, cursor }, gw) => {
    let path: string;
    if (chat_id) {
      const c = cursor ? `&from_id=${encodeURIComponent(cursor)}` : "";
      path = `/v1/chats/${encodeURIComponent(chat_id)}/search?q=${encodeURIComponent(query)}&limit=${limit}${c}`;
    } else {
      const c = cursor ? `&offset=${encodeURIComponent(cursor)}` : "";
      path = `/v1/search?q=${encodeURIComponent(query)}&limit=${limit}${c}`;
    }
    const r = await gw.api(path);
    return textResult({ results: r.data, total_count: r.meta?.total_count, next_cursor: r.meta?.next_from_id ?? r.meta?.next_offset ?? null });
  },
);

tool(
  "telegram_resolve_username",
  "Найти публичный чат/канал/пользователя по @username. Возвращает chat_id для остальных инструментов.",
  { username: z.string().describe("С @ или без") },
  async ({ username }, gw) => textResult((await gw.api(`/v1/resolve?username=${encodeURIComponent(username)}`)).data),
);

tool(
  "telegram_get_chat",
  "Карточка чата по id (тип, название). Работает для чатов, известных клиенту (из списка/resolve).",
  { chat_id: z.string() },
  async ({ chat_id }, gw) => textResult((await gw.api(`/v1/chats/${encodeURIComponent(chat_id)}`)).data),
);

tool(
  "telegram_get_user",
  "Карточка пользователя: имя, @username, телефон, онлайн-статус, premium.",
  { user_id: z.string() },
  async ({ user_id }, gw) => textResult((await gw.api(`/v1/users/${encodeURIComponent(user_id)}`)).data),
);

tool(
  "telegram_get_contacts",
  "Список id контактов аккаунта (карточки — через telegram_get_user).",
  {},
  async (_args, gw) => textResult((await gw.api("/v1/contacts")).data),
);

tool(
  "telegram_download_file",
  "Скачать файл по file_id из медиа-сообщения. Картинки возвращаются как изображение (агент их видит), прочее — метаданные + base64 (до 1 МБ). Если файл ещё не докачан гейтвеем — вернёт статус, повтори через пару секунд.",
  { file_id: z.string() },
  async ({ file_id }, gw) => {
    const controller = new AbortController();
    // maxBytes посреди потока (controller) совмещаем с таймаутом запроса,
    // чтобы зависший гейтвей не подвешивал инструмент бессрочно.
    const resp = await gw.fetchFile(file_id, AbortSignal.any([AbortSignal.timeout(60000), controller.signal]));
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

tool(
  "telegram_mark_read",
  "Отметить сообщения чата прочитанными.",
  { chat_id: z.string(), message_ids: z.array(z.string()).min(1) },
  async ({ chat_id, message_ids }, gw) => {
    await gw.api(`/v1/chats/${encodeURIComponent(chat_id)}/messages/read`, "POST", { message_ids });
    return textResult({ ok: true });
  },
);

// Поиск по архиву переписки (архиватор: Kafka -> Postgres). Регистрируется, если задан архиватор.
// session_id всегда равен выбранному аккаунту, модели этот параметр недоступен.
if (ctx.archiverUrl) {
  tool(
    "telegram_search_history",
    "Полнотекстовый поиск по архиву переписки (все чаты, вся сохранённая история). Возвращает сниппеты с подсветкой <<...>>, chat_id и message_id для перехода к контексту через telegram_get_history.",
    {
      query: z.string().min(1).describe("Поисковые слова (AND-семантика)"),
      chat_id: z.string().optional().describe("Ограничить одним чатом"),
      limit: z.number().int().min(1).max(100).default(20),
    },
    async ({ query, chat_id, limit }, _gw, account) => {
      const params = buildSearchParams(account.accountId, { query, chat_id, limit });
      const headers: Record<string, string> = {};
      if (ctx.archiverToken) headers.Authorization = `Bearer ${ctx.archiverToken}`;
      const resp = await fetch(`${ctx.archiverUrl.replace(/\/$/, "")}/search?${params}`, { headers });
      const json: any = await resp.json();
      if (!json.ok) throw new Error(json.error ?? `HTTP ${resp.status}`);
      return textResult(json.results);
    },
  );
}

  return server;
}
