// Streamable HTTP для MCP. Токен из Authorization: Bearer или X-API-Key резолвится в набор
// аккаунтов (реестр control plane или единственный токен однопользовательского режима).
// Сессия привязывается к хэшу токена первого авторизованного запроса: запрос с другим токеном в
// этой сессии получает 404, клиент по спецификации MCP обязан начать новую сессию.
import { randomUUID } from "node:crypto";
import express from "express";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { isInitializeRequest } from "@modelcontextprotocol/sdk/types.js";
import type { AccountSet } from "./accounts.js";
import { buildServer } from "./tools.js";

export type Resolve = (token: string) => Promise<AccountSet | null | "unavailable">;

export type McpAppOptions = {
  resolve: Resolve;
  allowedOrigins: string[];
  archiverUrl: string;
  archiverToken: string;
  sessionTtlMs?: number;
  // Только однопользовательский режим с MCP_HTTP_ALLOW_INSECURE=1: запрос без токена получает этот набор.
  anonymous?: AccountSet;
};

type Session = { transport: StreamableHTTPServerTransport; boundHash: string | null; holder: { set: AccountSet | null }; last: number };

// Рукопожатие без ключа: claude.ai проверяет custom connector пробным POST /mcp без
// пользовательских заголовков и на 401 уходит в OAuth-обнаружение, которого здесь нет.
// initialize/ping/tools/list ничего про аккаунт не раскрывают и пропускаются без токена.
const HANDSHAKE_METHODS = new Set(["initialize", "notifications/initialized", "ping", "tools/list"]);
function isHandshakeOnly(body: unknown): boolean {
  const msgs = Array.isArray(body) ? body : [body];
  return msgs.length > 0 && msgs.every((m) => m !== null && typeof m === "object" && HANDSHAKE_METHODS.has((m as { method?: unknown }).method as string));
}

// Токен принимается из Authorization: Bearer (Claude Code, curl) и X-API-Key (custom connector
// claude.ai: там Authorization занят его собственным OAuth-токеном).
function presentedToken(req: express.Request): string {
  const auth = req.headers.authorization ?? "";
  if (auth.startsWith("Bearer ")) return auth.slice("Bearer ".length);
  const key = req.headers["x-api-key"];
  if (typeof key === "string") return key;
  if (Array.isArray(key) && key.length === 1) return key[0];
  return "";
}

const rpcError = (res: express.Response, status: number, code: number, message: string) =>
  res.status(status).json({ jsonrpc: "2.0", error: { code, message }, id: null });

export function mcpApp(opts: McpAppOptions): express.Express {
  const app = express();
  app.disable("x-powered-by");
  app.use(express.json({ limit: "8mb" }));
  const sessions = new Map<string, Session>();

  const ttl = opts.sessionTtlMs ?? 30 * 60 * 1000;
  const reaper = setInterval(() => {
    const now = Date.now();
    for (const [id, s] of sessions) if (now - s.last > ttl) s.transport.close();
  }, Math.min(60_000, ttl));
  reaper.unref();

  app.get("/healthz", (_req, res) => {
    res.json({ ok: true });
  });

  // Проверка Origin (защита от DNS rebinding из браузера). Серверные клиенты Origin не шлют.
  app.use("/mcp", (req, res, next) => {
    const origin = req.headers.origin;
    if (origin && !opts.allowedOrigins.includes(origin)) return void rpcError(res, 403, -32003, "origin not allowed");
    next();
  });

  // Авторизация запроса к сессии. Возвращает false, если ответ уже отправлен.
  async function authorize(req: express.Request, res: express.Response, s: Session | undefined, handshake: boolean): Promise<AccountSet | null | false> {
    const token = presentedToken(req);
    if (!token) {
      if (opts.anonymous) return opts.anonymous;
      if (handshake) return null;
      rpcError(res, 401, -32001, "unauthorized");
      return false;
    }
    const r = await opts.resolve(token);
    if (r === "unavailable") {
      rpcError(res, 503, -32002, "account registry unavailable, retry later");
      return false;
    }
    if (r === null) {
      rpcError(res, 401, -32001, "unauthorized");
      return false;
    }
    if (s && s.boundHash && s.boundHash !== r.tokenHash) {
      rpcError(res, 404, -32001, "session belongs to another token");
      return false;
    }
    return r;
  }

  const bind = (s: Session, set: AccountSet | null) => {
    if (!set) return;
    s.boundHash = set.tokenHash;
    s.holder.set = set; // каждый запрос обновляет набор: отзыв доходит до открытых сессий
  };

  app.post("/mcp", async (req, res) => {
    const sid = req.headers["mcp-session-id"] as string | undefined;
    const existing = sid ? sessions.get(sid) : undefined;
    if (sid && !existing) return void rpcError(res, 404, -32001, "session not found");
    const set = await authorize(req, res, existing, isHandshakeOnly(req.body));
    if (set === false) return;
    if (existing) {
      bind(existing, set);
      existing.last = Date.now();
      return void (await existing.transport.handleRequest(req, res, req.body));
    }
    if (!isInitializeRequest(req.body)) return void rpcError(res, 400, -32000, "no valid session");
    const holder: { set: AccountSet | null } = { set: null };
    const s: Session = { transport: undefined as unknown as StreamableHTTPServerTransport, boundHash: null, holder, last: Date.now() };
    const transport = new StreamableHTTPServerTransport({
      sessionIdGenerator: () => randomUUID(),
      onsessioninitialized: (id) => {
        sessions.set(id, s);
      },
    });
    s.transport = transport;
    transport.onclose = () => {
      if (transport.sessionId) sessions.delete(transport.sessionId);
    };
    bind(s, set);
    await buildServer({ accounts: () => holder.set, archiverUrl: opts.archiverUrl, archiverToken: opts.archiverToken }).connect(transport);
    await transport.handleRequest(req, res, req.body);
  });

  // GET (SSE server->client) и DELETE (закрытие сессии) по существующему session id.
  const bySession = async (req: express.Request, res: express.Response) => {
    const sid = req.headers["mcp-session-id"] as string | undefined;
    const s = sid ? sessions.get(sid) : undefined;
    if (!s) return void rpcError(res, 404, -32001, "session not found");
    const set = await authorize(req, res, s, false);
    if (set === false) return;
    bind(s, set);
    s.last = Date.now();
    await s.transport.handleRequest(req, res);
  };
  app.get("/mcp", bySession);
  app.delete("/mcp", bySession);
  return app;
}
