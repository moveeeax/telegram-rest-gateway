// Повтор оффлоада медиа: строки с file_id без media_url ставятся в очередь заново, мимо дедупа
// процесса (там могли остаться ключи, ранее помеченные permanent fail).
import type { MediaJob } from "./media.js";

const DEFAULT_WINDOW_S = 7 * 86400;
const MAX_ROWS = 10000;
const SESSION_ID = /^(a[a-z0-9]{10}|[0-9]{1,20})$/;

export type RetryStore = { pendingMedia(sessionId: string | null, sinceEpoch: number, limit: number): Promise<MediaJob[]> };
export type RetryMedia = { forget(job: MediaJob): void; enqueue(job: MediaJob): void };

export async function retryMedia(
  store: RetryStore, media: RetryMedia, body: { session_id?: unknown; since?: unknown }, nowMs: number,
): Promise<number> {
  const session = body.session_id === undefined || body.session_id === null ? null : String(body.session_id);
  if (session !== null && !SESSION_ID.test(session)) throw new Error("session_id is not a valid account id");
  let since = Math.floor(nowMs / 1000) - DEFAULT_WINDOW_S;
  if (body.since !== undefined) {
    if (typeof body.since !== "number" || !Number.isFinite(body.since)) throw new Error("since must be epoch seconds");
    since = Math.floor(body.since);
  }
  const rows = await store.pendingMedia(session, since, MAX_ROWS);
  for (const job of rows) {
    media.forget(job);
    media.enqueue(job);
  }
  return rows.length;
}
