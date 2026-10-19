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
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { isInitializeRequest } from "@modelcontextprotocol/sdk/types.js";
import { createHash, randomUUID, timingSafeEqual } from "node:crypto";
import express from "express";
import { archiveSearchEnabled } from "./archive_search.js";
import type { AccountSet } from "./accounts.js";
import { buildServer as buildToolsServer, type ToolContext } from "./tools.js";

const BASE_URL = (process.env.TGW_BASE_URL ?? "http://127.0.0.1:8080").replace(/\/$/, "");
const TOKEN = process.env.TGW_BEARER_TOKEN ?? "";
if (!TOKEN) {
  console.error("TGW_BEARER_TOKEN is required");
  process.exit(1);
}

// Однопользовательский режим: один аккаунт из env. TGW_SESSION_ID нужен поиску по архиву
// (session_id аккаунта); без него поиск не регистрируется (fail closed).
const ARCHIVER_URL = (process.env.TGW_ARCHIVER_URL ?? "").replace(/\/$/, "");
const SESSION_ID = process.env.TGW_SESSION_ID ?? "";
if (ARCHIVER_URL && !SESSION_ID) {
  console.error("TGW_ARCHIVER_URL задан без TGW_SESSION_ID: telegram_search_history выключен");
}
const singleSet: AccountSet = {
  tokenHash: "",
  accounts: [{ accountId: SESSION_ID || "default", gatewayUrl: BASE_URL, serviceToken: TOKEN }],
};
const singleCtx: ToolContext = {
  accounts: () => singleSet,
  archiverUrl: archiveSearchEnabled(ARCHIVER_URL, SESSION_ID) ? ARCHIVER_URL : "",
  archiverToken: process.env.TGW_ARCHIVER_TOKEN ?? "",
};
const buildServer = () => buildToolsServer(singleCtx);

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

  // Токен принимается из двух мест: `Authorization: Bearer <t>` (Claude Code, curl) и
  // `X-API-Key: <t>` (custom connector claude.ai: там Authorization занят его собственным
  // OAuth-токеном, а из пользовательских заголовков одобрен X-API-Key). Токен один и тот же.
  const presentedToken = (req: express.Request): string => {
    const auth = req.headers.authorization ?? "";
    if (auth.startsWith("Bearer ")) return auth.slice("Bearer ".length);
    const key = req.headers["x-api-key"];
    if (typeof key === "string") return key;
    if (Array.isArray(key) && key.length === 1) return key[0];
    return "";
  };

  // Рукопожатие протокола без ключа: claude.ai проверяет custom connector пробным POST /mcp
  // БЕЗ пользовательских заголовков, и на 401 уходит в OAuth-обнаружение, которого здесь нет.
  // Поэтому initialize/ping/tools/list (ничего про аккаунт не раскрывают) пропускаются без
  // токена, а любой вызов инструмента и всё остальное — только с ним.
  const HANDSHAKE_METHODS = new Set(["initialize", "notifications/initialized", "ping", "tools/list"]);
  const isHandshakeOnly = (body: unknown): boolean => {
    const msgs = Array.isArray(body) ? body : [body];
    return (
      msgs.length > 0 &&
      msgs.every(
        (m) => m !== null && typeof m === "object" && HANDSHAKE_METHODS.has((m as { method?: unknown }).method as string),
      )
    );
  };

  const unauthorized = (req: express.Request, res: express.Response): boolean => {
    if (!HTTP_TOKEN) return false; // явный опт-аут через MCP_HTTP_ALLOW_INSECURE=1
    if (req.method === "POST" && isHandshakeOnly(req.body)) return false;
    // Сравниваем не сами строки, а их SHA-256-дайджесты фиксированной длины: так timingSafeEqual
    // не бросает исключение при разной длине токенов и не даёт судить о ней по времени ответа.
    const got = createHash("sha256").update(presentedToken(req)).digest();
    const want = createHash("sha256").update(HTTP_TOKEN).digest();
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
