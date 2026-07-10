/**
 * Абстракция хранилища архива. Два бэкенда:
 *  - SqliteStore  — встроенный (better-sqlite3 + FTS5), дефолт;
 *  - PostgresStore — внешний (pg + tsvector/GIN), если задан ARCHIVER_PG_URL.
 * Интерфейс асинхронный (ради pg); sqlite оборачивается тривиально.
 */
import Database from "better-sqlite3";
import { Pool } from "pg";

export type MessageRow = {
  session_id: string;
  chat_id: string;
  message_id: string;
  date: number | null;
  is_outgoing: number;
  type: string;
  text: string;
  file_id: string | null;
  file_name: string | null;
  mime_type: string | null;
};

export type SearchArgs = { q: string; chat_id?: string | null; session_id?: string | null; limit: number };
export type SearchRow = {
  session_id: string;
  chat_id: string;
  chat_title: string | null;
  message_id: string;
  date: number | null;
  is_outgoing: number | boolean;
  type: string;
  text: string;
  file_id: string | null;
  media_url: string | null;
  deleted: number | boolean;
  snippet: string;
};

export interface Store {
  init(): Promise<void>;
  upsertMessage(r: MessageRow): Promise<void>;
  deleteRow(session_id: string, chat_id: string, message_id: string): Promise<void>;
  updateText(a: { session_id: string; chat_id: string; message_id: string; text: string; type: string; file_id: string | null }): Promise<void>;
  markEdited(edit_date: number | null, session_id: string, chat_id: string, message_id: string): Promise<void>;
  markDeleted(session_id: string, chat_id: string, message_id: string): Promise<void>;
  upsertChat(session_id: string, chat_id: string, title: string, type: string): Promise<void>;
  setMediaUrl(session_id: string, chat_id: string, message_id: string, url: string): Promise<void>;
  getSeq(session_id: string): Promise<number | undefined>;
  setSeq(session_id: string, seq: number): Promise<void>;
  search(a: SearchArgs): Promise<SearchRow[]>;
  stats(): Promise<{ messages: number; chats: number }>;
}

// ---------------- SQLite ----------------
export class SqliteStore implements Store {
  private db: Database.Database;
  constructor(path: string) {
    this.db = new Database(path);
    this.db.pragma("journal_mode = WAL");
  }
  async init(): Promise<void> {
    this.db.exec(`
CREATE TABLE IF NOT EXISTS messages (
  session_id TEXT NOT NULL, chat_id TEXT NOT NULL, message_id TEXT NOT NULL,
  date INTEGER, is_outgoing INTEGER DEFAULT 0, type TEXT, text TEXT DEFAULT '',
  file_id TEXT, file_name TEXT, mime_type TEXT, media_url TEXT,
  edited_at INTEGER, deleted INTEGER DEFAULT 0,
  PRIMARY KEY (session_id, chat_id, message_id)
);
CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(text, content='messages', content_rowid='rowid', tokenize='unicode61');
CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
  INSERT INTO messages_fts(rowid, text) VALUES (new.rowid, new.text); END;
CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
  INSERT INTO messages_fts(messages_fts, rowid, text) VALUES ('delete', old.rowid, old.text); END;
CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE OF text ON messages BEGIN
  INSERT INTO messages_fts(messages_fts, rowid, text) VALUES ('delete', old.rowid, old.text);
  INSERT INTO messages_fts(rowid, text) VALUES (new.rowid, new.text); END;
CREATE TABLE IF NOT EXISTS chats (session_id TEXT NOT NULL, chat_id TEXT NOT NULL, title TEXT, type TEXT, PRIMARY KEY (session_id, chat_id));
CREATE TABLE IF NOT EXISTS seq_state (session_id TEXT PRIMARY KEY, last_seq INTEGER);
`);
    // media_url мог отсутствовать в старой БД — добавим при апгрейде.
    try { this.db.exec(`ALTER TABLE messages ADD COLUMN media_url TEXT`); } catch { /* уже есть */ }
  }
  async upsertMessage(r: MessageRow): Promise<void> {
    this.db.prepare(`INSERT INTO messages (session_id,chat_id,message_id,date,is_outgoing,type,text,file_id,file_name,mime_type)
      VALUES (@session_id,@chat_id,@message_id,@date,@is_outgoing,@type,@text,@file_id,@file_name,@mime_type)
      ON CONFLICT(session_id,chat_id,message_id) DO UPDATE SET date=excluded.date,is_outgoing=excluded.is_outgoing,
        type=excluded.type,text=excluded.text,file_id=excluded.file_id,file_name=excluded.file_name,mime_type=excluded.mime_type`).run(r);
  }
  async deleteRow(s: string, c: string, m: string): Promise<void> {
    this.db.prepare(`DELETE FROM messages WHERE session_id=? AND chat_id=? AND message_id=?`).run(s, c, m);
  }
  async updateText(a: any): Promise<void> {
    this.db.prepare(`UPDATE messages SET text=@text,type=@type,file_id=@file_id WHERE session_id=@session_id AND chat_id=@chat_id AND message_id=@message_id`).run(a);
  }
  async markEdited(d: number | null, s: string, c: string, m: string): Promise<void> {
    this.db.prepare(`UPDATE messages SET edited_at=? WHERE session_id=? AND chat_id=? AND message_id=?`).run(d, s, c, m);
  }
  async markDeleted(s: string, c: string, m: string): Promise<void> {
    this.db.prepare(`UPDATE messages SET deleted=1 WHERE session_id=? AND chat_id=? AND message_id=?`).run(s, c, m);
  }
  async upsertChat(s: string, c: string, t: string, ty: string): Promise<void> {
    this.db.prepare(`INSERT INTO chats (session_id,chat_id,title,type) VALUES (?,?,?,?)
      ON CONFLICT(session_id,chat_id) DO UPDATE SET title=excluded.title,type=excluded.type`).run(s, c, t, ty);
  }
  async setMediaUrl(s: string, c: string, m: string, url: string): Promise<void> {
    this.db.prepare(`UPDATE messages SET media_url=? WHERE session_id=? AND chat_id=? AND message_id=?`).run(url, s, c, m);
  }
  async getSeq(s: string): Promise<number | undefined> {
    const r = this.db.prepare(`SELECT last_seq FROM seq_state WHERE session_id=?`).get(s) as any;
    return r?.last_seq;
  }
  async setSeq(s: string, seq: number): Promise<void> {
    this.db.prepare(`INSERT INTO seq_state (session_id,last_seq) VALUES (?,?) ON CONFLICT(session_id) DO UPDATE SET last_seq=excluded.last_seq`).run(s, seq);
  }
  async search(a: SearchArgs): Promise<SearchRow[]> {
    const q = a.q.split(/\s+/).filter(Boolean).map((t) => `"${t.replaceAll('"', '""')}"`).join(" ");
    return this.db.prepare(`
      SELECT m.session_id,m.chat_id,c.title AS chat_title,m.message_id,m.date,m.is_outgoing,m.type,m.text,m.file_id,m.media_url,m.deleted,
             snippet(messages_fts,0,'<<','>>','…',12) AS snippet
      FROM messages_fts f JOIN messages m ON m.rowid=f.rowid
      LEFT JOIN chats c ON c.session_id=m.session_id AND c.chat_id=m.chat_id
      WHERE messages_fts MATCH @q AND (@chat_id IS NULL OR m.chat_id=@chat_id) AND (@session_id IS NULL OR m.session_id=@session_id)
      ORDER BY m.date DESC LIMIT @limit`).all({ q, chat_id: a.chat_id ?? null, session_id: a.session_id ?? null, limit: a.limit }) as SearchRow[];
  }
  async stats(): Promise<{ messages: number; chats: number }> {
    const m = (this.db.prepare(`SELECT COUNT(*) n FROM messages`).get() as any).n;
    const c = (this.db.prepare(`SELECT COUNT(*) n FROM chats`).get() as any).n;
    return { messages: m, chats: c };
  }
}

// ---------------- PostgreSQL ----------------
export class PostgresStore implements Store {
  private pool: Pool;
  constructor(url: string) {
    // keepAlive + idleTimeout: не даём простаивающему соединению «протухнуть» (сеть/CNPG рвут
    // idle-TCP молча). query/statement_timeout: висящий на мёртвом коннекте запрос падает, а не
    // блокирует хендлер навсегда. pool.on('error'): фоновая ошибка коннекта не копится.
    this.pool = new Pool({
      connectionString: url,
      max: 8,
      idleTimeoutMillis: 10000,
      connectionTimeoutMillis: 5000,
      keepAlive: true,
      keepAliveInitialDelayMillis: 5000,
      statement_timeout: 20000,
      query_timeout: 20000,
    });
    this.pool.on("error", (err) => console.error("pg pool error:", err.message));
  }
  async init(): Promise<void> {
    await this.pool.query(`
CREATE TABLE IF NOT EXISTS messages (
  session_id text NOT NULL, chat_id text NOT NULL, message_id text NOT NULL,
  date bigint, is_outgoing boolean DEFAULT false, type text, text text DEFAULT '',
  file_id text, file_name text, mime_type text, media_url text,
  edited_at bigint, deleted boolean DEFAULT false,
  tsv tsvector GENERATED ALWAYS AS (to_tsvector('simple', coalesce(text,''))) STORED,
  PRIMARY KEY (session_id, chat_id, message_id)
);
CREATE INDEX IF NOT EXISTS messages_tsv_idx ON messages USING GIN (tsv);
CREATE INDEX IF NOT EXISTS messages_date_idx ON messages (date DESC);
CREATE TABLE IF NOT EXISTS chats (session_id text NOT NULL, chat_id text NOT NULL, title text, type text, PRIMARY KEY (session_id, chat_id));
CREATE TABLE IF NOT EXISTS seq_state (session_id text PRIMARY KEY, last_seq bigint);
`);
  }
  async upsertMessage(r: MessageRow): Promise<void> {
    await this.pool.query(
      `INSERT INTO messages (session_id,chat_id,message_id,date,is_outgoing,type,text,file_id,file_name,mime_type)
       VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)
       ON CONFLICT (session_id,chat_id,message_id) DO UPDATE SET date=EXCLUDED.date,is_outgoing=EXCLUDED.is_outgoing,
         type=EXCLUDED.type,text=EXCLUDED.text,file_id=EXCLUDED.file_id,file_name=EXCLUDED.file_name,mime_type=EXCLUDED.mime_type`,
      [r.session_id, r.chat_id, r.message_id, r.date, !!r.is_outgoing, r.type, r.text, r.file_id, r.file_name, r.mime_type],
    );
  }
  async deleteRow(s: string, c: string, m: string): Promise<void> {
    await this.pool.query(`DELETE FROM messages WHERE session_id=$1 AND chat_id=$2 AND message_id=$3`, [s, c, m]);
  }
  async updateText(a: any): Promise<void> {
    await this.pool.query(`UPDATE messages SET text=$4,type=$5,file_id=$6 WHERE session_id=$1 AND chat_id=$2 AND message_id=$3`,
      [a.session_id, a.chat_id, a.message_id, a.text, a.type, a.file_id]);
  }
  async markEdited(d: number | null, s: string, c: string, m: string): Promise<void> {
    await this.pool.query(`UPDATE messages SET edited_at=$1 WHERE session_id=$2 AND chat_id=$3 AND message_id=$4`, [d, s, c, m]);
  }
  async markDeleted(s: string, c: string, m: string): Promise<void> {
    await this.pool.query(`UPDATE messages SET deleted=true WHERE session_id=$1 AND chat_id=$2 AND message_id=$3`, [s, c, m]);
  }
  async upsertChat(s: string, c: string, t: string, ty: string): Promise<void> {
    await this.pool.query(`INSERT INTO chats (session_id,chat_id,title,type) VALUES ($1,$2,$3,$4)
      ON CONFLICT (session_id,chat_id) DO UPDATE SET title=EXCLUDED.title,type=EXCLUDED.type`, [s, c, t, ty]);
  }
  async setMediaUrl(s: string, c: string, m: string, url: string): Promise<void> {
    await this.pool.query(`UPDATE messages SET media_url=$1 WHERE session_id=$2 AND chat_id=$3 AND message_id=$4`, [url, s, c, m]);
  }
  async getSeq(s: string): Promise<number | undefined> {
    const r = await this.pool.query(`SELECT last_seq FROM seq_state WHERE session_id=$1`, [s]);
    return r.rows[0] ? Number(r.rows[0].last_seq) : undefined;
  }
  async setSeq(s: string, seq: number): Promise<void> {
    await this.pool.query(`INSERT INTO seq_state (session_id,last_seq) VALUES ($1,$2)
      ON CONFLICT (session_id) DO UPDATE SET last_seq=EXCLUDED.last_seq`, [s, seq]);
  }
  async search(a: SearchArgs): Promise<SearchRow[]> {
    const r = await this.pool.query(
      `SELECT m.session_id,m.chat_id,c.title AS chat_title,m.message_id,m.date,m.is_outgoing,m.type,m.text,m.file_id,m.media_url,m.deleted,
              ts_headline('simple', m.text, websearch_to_tsquery('simple',$1), 'StartSel=<<,StopSel=>>,MaxFragments=1,MaxWords=14,MinWords=3') AS snippet
       FROM messages m LEFT JOIN chats c ON c.session_id=m.session_id AND c.chat_id=m.chat_id
       WHERE m.tsv @@ websearch_to_tsquery('simple',$1)
         AND ($2::text IS NULL OR m.chat_id=$2) AND ($3::text IS NULL OR m.session_id=$3)
       ORDER BY m.date DESC NULLS LAST LIMIT $4`,
      [a.q, a.chat_id ?? null, a.session_id ?? null, a.limit],
    );
    return r.rows as SearchRow[];
  }
  async stats(): Promise<{ messages: number; chats: number }> {
    const m = await this.pool.query(`SELECT COUNT(*)::int n FROM messages`);
    const c = await this.pool.query(`SELECT COUNT(*)::int n FROM chats`);
    return { messages: m.rows[0].n, chats: c.rows[0].n };
  }
}

export function makeStore(): Store {
  const pg = process.env.ARCHIVER_PG_URL;
  if (pg) {
    console.error("archiver: store = PostgreSQL");
    return new PostgresStore(pg);
  }
  console.error("archiver: store = SQLite");
  return new SqliteStore(process.env.ARCHIVER_DB_PATH ?? "/data/archive.db");
}
