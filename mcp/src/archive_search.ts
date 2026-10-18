// Поиск по архиву всегда в срезе своего аккаунта: архиватор хранит переписку всех аккаунтов,
// и запрос без session_id вернул бы чужие сообщения.

export type SearchArgs = { query: string; chat_id?: string; limit: number };

export function buildSearchParams(sessionId: string, args: SearchArgs): URLSearchParams {
  const params = new URLSearchParams({ q: args.query, limit: String(args.limit), session_id: sessionId });
  if (args.chat_id) params.set("chat_id", args.chat_id);
  return params;
}

// Без session_id инструмент не регистрируется вовсе (fail closed), а не ищет по всем аккаунтам.
export function archiveSearchEnabled(archiverUrl: string, sessionId: string): boolean {
  return archiverUrl !== "" && sessionId !== "";
}
