#!/usr/bin/env node
/**
 * MCP-сервер поверх telegram-rest-gateway: агент получает инструменты Telegram-аккаунтов.
 *
 * Режимы:
 *   Многоаккаунтный (единый MCP): MCP_REGISTRY_URL + MCP_REGISTRY_TOKEN. Токен клиента (Bearer или
 *     X-API-Key) резолвится через реестр control plane в набор аккаунтов; в гейтвей MCP ходит
 *     сервисным токеном воркера.
 *   Однопользовательский: TGW_BASE_URL + TGW_BEARER_TOKEN, в HTTP-режиме ключ MCP_HTTP_TOKEN.
 * Транспорт: MCP_HTTP_PORT => Streamable HTTP, иначе stdio (только однопользовательский).
 */
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { createHash, timingSafeEqual } from "node:crypto";
import { archiveSearchEnabled } from "./archive_search.js";
import type { AccountSet } from "./accounts.js";
import { mcpApp, type Resolve } from "./http.js";
import { RegistryClient } from "./registry.js";
import { buildServer } from "./tools.js";

const env = process.env;
const ARCHIVER_URL = (env.TGW_ARCHIVER_URL ?? "").replace(/\/$/, "");
const ARCHIVER_TOKEN = env.TGW_ARCHIVER_TOKEN ?? "";
const HTTP_PORT = env.MCP_HTTP_PORT;
const REGISTRY_URL = env.MCP_REGISTRY_URL ?? "";
const ALLOWED_ORIGINS = (env.MCP_ALLOWED_ORIGINS ?? "https://claude.ai,https://claude.com").split(",").map((s) => s.trim()).filter(Boolean);
const rawTtl = Number(env.MCP_SESSION_TTL_MS);
const SESSION_TTL_MS = Number.isFinite(rawTtl) && rawTtl > 0 ? rawTtl : 30 * 60 * 1000;

function fail(message: string): never {
  console.error(`tgw-mcp: ${message}`);
  process.exit(1);
}

const digest = (s: string) => createHash("sha256").update(s).digest();

if (REGISTRY_URL) {
  if (!HTTP_PORT) fail("MCP_REGISTRY_URL работает только в HTTP-режиме (MCP_HTTP_PORT)");
  const registryToken = env.MCP_REGISTRY_TOKEN ?? "";
  if (!registryToken) fail("MCP_REGISTRY_TOKEN обязателен вместе с MCP_REGISTRY_URL");
  const registry = new RegistryClient(REGISTRY_URL, registryToken);
  const app = mcpApp({
    resolve: (t) => registry.resolve(t), allowedOrigins: ALLOWED_ORIGINS,
    archiverUrl: ARCHIVER_URL, archiverToken: ARCHIVER_TOKEN, sessionTtlMs: SESSION_TTL_MS,
  });
  app.listen(Number(HTTP_PORT), () => console.error(`tgw-mcp: multi-account streamable-http on :${HTTP_PORT}`));
} else {
  const BASE_URL = (env.TGW_BASE_URL ?? "http://127.0.0.1:8080").replace(/\/$/, "");
  const TOKEN = env.TGW_BEARER_TOKEN ?? "";
  if (!TOKEN) fail("TGW_BEARER_TOKEN is required");
  // TGW_SESSION_ID нужен поиску по архиву (session_id аккаунта); без него поиск не регистрируется.
  const SESSION_ID = env.TGW_SESSION_ID ?? "";
  if (ARCHIVER_URL && !SESSION_ID) console.error("tgw-mcp: TGW_ARCHIVER_URL задан без TGW_SESSION_ID: telegram_search_history выключен");
  const archiverUrl = archiveSearchEnabled(ARCHIVER_URL, SESSION_ID) ? ARCHIVER_URL : "";
  const single: AccountSet = { tokenHash: "", accounts: [{ accountId: SESSION_ID || "default", gatewayUrl: BASE_URL, serviceToken: TOKEN }] };

  if (HTTP_PORT) {
    const HTTP_TOKEN = env.MCP_HTTP_TOKEN ?? "";
    const insecure = env.MCP_HTTP_ALLOW_INSECURE === "1";
    // Fail-closed: без ключа /mcp был бы открыт всем и проксировал бы полный доступ гейтвея.
    if (!HTTP_TOKEN && !insecure) {
      fail("MCP_HTTP_TOKEN не задан. В HTTP-режиме авторизация обязательна; явный опт-аут MCP_HTTP_ALLOW_INSECURE=1.");
    }
    if (!HTTP_TOKEN) console.error("tgw-mcp: ВНИМАНИЕ — MCP_HTTP_TOKEN не задан, /mcp работает БЕЗ авторизации.");
    const want = digest(HTTP_TOKEN);
    const set: AccountSet = { ...single, tokenHash: digest(HTTP_TOKEN).toString("hex") };
    const resolve: Resolve = async (t) => (HTTP_TOKEN && timingSafeEqual(digest(t), want) ? set : null);
    const app = mcpApp({
      resolve, allowedOrigins: ALLOWED_ORIGINS, archiverUrl, archiverToken: ARCHIVER_TOKEN,
      sessionTtlMs: SESSION_TTL_MS, anonymous: HTTP_TOKEN ? undefined : set,
    });
    app.listen(Number(HTTP_PORT), () => console.error(`tgw-mcp: streamable-http on :${HTTP_PORT}`));
  } else {
    await buildServer({ accounts: () => single, archiverUrl, archiverToken: ARCHIVER_TOKEN }).connect(new StdioServerTransport());
    console.error(`tgw-mcp: stdio connected to ${BASE_URL}`);
  }
}
