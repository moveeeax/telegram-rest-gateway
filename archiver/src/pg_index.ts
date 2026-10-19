// Индекс (session_id, date DESC) для поиска в срезе аккаунта. Отдельный клиент без query_timeout
// пула (20 с не хватает на большой таблице) и CONCURRENTLY: построение не блокирует запись,
// в том числе от старого пода во время выкатки.
import pg from "pg";

type MiniClient = { connect(): Promise<unknown>; query(q: string): Promise<unknown>; end(): Promise<unknown> };
export type MakeClient = (cfg: { connectionString: string }) => MiniClient;

const defaultMake: MakeClient = (cfg) => new pg.Client(cfg);

export async function buildSessionIndex(url: string, make: MakeClient = defaultMake): Promise<void> {
  const c = make({ connectionString: url });
  try {
    await c.connect();
    await c.query("CREATE INDEX CONCURRENTLY IF NOT EXISTS messages_session_date_idx ON messages (session_id, date DESC)");
  } finally {
    await c.end().catch(() => {});
  }
}
