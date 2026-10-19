// Тенантность архиватора: какие кадры писать, кто может звать админские пути, разбор id для purge.
import { createHash, timingSafeEqual } from "node:crypto";
import type { ArchiveTarget } from "./registry.js";

export type Lookup = (sessionId: string) => Promise<ArchiveTarget | null | "unavailable">;

// "store": аккаунт с archive_enabled; "drop": архив выключен или аккаунта нет в реестре;
// "wait": реестр недоступен, кадр нельзя ни писать, ни терять (consumer не коммитит offset).
export async function admitFrame(sessionId: string, lookup: Lookup | null): Promise<"store" | "drop" | "wait"> {
  if (!lookup) return "store"; // без реестра (локальный запуск) архивируем все
  const t = await lookup(sessionId);
  if (t === "unavailable") return "wait";
  return t !== null && t.archiveEnabled ? "store" : "drop";
}

export type AccessLevel = "admin" | "search" | "none";

function same(presented: string, expected: string): boolean {
  if (!expected) return false;
  return timingSafeEqual(createHash("sha256").update(presented).digest(), createHash("sha256").update(`Bearer ${expected}`).digest());
}

// search: токен MCP (ARCHIVER_TOKEN); admin: ARCHIVER_ADMIN_TOKEN. Пустой токен в конфиге никого не пускает.
export function accessLevel(authorization: string | undefined, tokens: { search: string; admin: string }): AccessLevel {
  const h = authorization ?? "";
  if (same(h, tokens.admin)) return "admin";
  if (same(h, tokens.search)) return "search";
  return "none";
}

// 403 для токена поиска: клиент MCP не должен решить, что его ключ неверный.
export function guardAdmin(level: AccessLevel): null | 401 | 403 {
  if (level === "admin") return null;
  return level === "search" ? 403 : 401;
}

const ACCOUNT_ID = /^(a[a-z0-9]{10}|[0-9]{1,20})$/;

export function parsePurgeId(pathname: string): string | null {
  const m = /^\/accounts\/([^/]+)$/.exec(pathname);
  if (!m) return null;
  let id: string;
  try { id = decodeURIComponent(m[1]); } catch { return null; }
  return ACCOUNT_ID.test(id) ? id : null;
}
