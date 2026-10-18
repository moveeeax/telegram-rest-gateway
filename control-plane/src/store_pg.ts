// Postgres-реализация Store (база tgw_control в CNPG). Схема применяется на старте.
import pg from "pg";
import type { Account, ClaimResult, Invite, InviteKind, Store } from "./store.js";

// Общий ключ advisory lock на claim: подсчет занятых сессий и захват идут под одной блокировкой.
const CLAIM_LOCK = 727001;

export const SCHEMA = `
CREATE TABLE IF NOT EXISTS accounts (
  account_id        text PRIMARY KEY,
  status            text NOT NULL CHECK (status IN ('onboarding','active','needs_relogin','deleting')),
  telegram_user_id  text,
  username          text,
  archive_enabled   boolean NOT NULL DEFAULT false,
  export_enabled    boolean NOT NULL DEFAULT false,
  client_token_hash text NOT NULL,
  invited_by        text NOT NULL,
  created_at        timestamptz NOT NULL,
  activated_at      timestamptz,
  token_shown_at    timestamptz,
  delete_step       text,
  secret_rev        integer NOT NULL DEFAULT 1
);
CREATE UNIQUE INDEX IF NOT EXISTS accounts_active_tg ON accounts (telegram_user_id) WHERE status = 'active';
CREATE TABLE IF NOT EXISTS invites (
  code_hash          text PRIMARY KEY,
  kind               text NOT NULL CHECK (kind IN ('new','relogin','token')),
  account_id         text,
  expires_at         timestamptz NOT NULL,
  claimed_at         timestamptz,
  session_hash       text,
  session_expires_at timestamptz,
  used_at            timestamptz,
  code_attempts      integer NOT NULL DEFAULT 0,
  password_attempts  integer NOT NULL DEFAULT 0,
  outcome            text
);
CREATE INDEX IF NOT EXISTS invites_session ON invites (session_hash);
`;

const ACCOUNT_COLS: Record<keyof Omit<Account, "accountId">, string> = {
  status: "status", telegramUserId: "telegram_user_id", username: "username", archiveEnabled: "archive_enabled",
  exportEnabled: "export_enabled", clientTokenHash: "client_token_hash", invitedBy: "invited_by",
  createdAt: "created_at", activatedAt: "activated_at", tokenShownAt: "token_shown_at",
  deleteStep: "delete_step", secretRev: "secret_rev",
};

type Row = Record<string, unknown>;

function toAccount(r: Row): Account {
  return {
    accountId: r.account_id as string, status: r.status as Account["status"],
    telegramUserId: (r.telegram_user_id as string | null) ?? null, username: (r.username as string | null) ?? null,
    archiveEnabled: r.archive_enabled as boolean, exportEnabled: r.export_enabled as boolean,
    clientTokenHash: r.client_token_hash as string, invitedBy: r.invited_by as string,
    createdAt: r.created_at as Date, activatedAt: (r.activated_at as Date | null) ?? null,
    tokenShownAt: (r.token_shown_at as Date | null) ?? null, deleteStep: (r.delete_step as string | null) ?? null,
    secretRev: r.secret_rev as number,
  };
}

function toInvite(r: Row): Invite {
  return {
    codeHash: r.code_hash as string, kind: r.kind as InviteKind, accountId: (r.account_id as string | null) ?? null,
    expiresAt: r.expires_at as Date, claimedAt: (r.claimed_at as Date | null) ?? null,
    sessionHash: (r.session_hash as string | null) ?? null, sessionExpiresAt: (r.session_expires_at as Date | null) ?? null,
    usedAt: (r.used_at as Date | null) ?? null, codeAttempts: r.code_attempts as number,
    passwordAttempts: r.password_attempts as number, outcome: (r.outcome as string | null) ?? null,
  };
}

export class PgStore implements Store {
  private constructor(private pool: pg.Pool) {}

  static async connect(url: string): Promise<PgStore> {
    const pool = new pg.Pool({ connectionString: url, max: 5 });
    await pool.query(SCHEMA);
    return new PgStore(pool);
  }

  async close(): Promise<void> {
    await this.pool.end();
  }

  async resetForTests(): Promise<void> {
    await this.pool.query("TRUNCATE accounts, invites");
  }

  async createInvite(i: { codeHash: string; kind: InviteKind; accountId: string | null; ttlMs: number; now: Date }): Promise<Invite> {
    const r = await this.pool.query(
      "INSERT INTO invites (code_hash, kind, account_id, expires_at) VALUES ($1,$2,$3,$4) RETURNING *",
      [i.codeHash, i.kind, i.accountId, new Date(i.now.getTime() + i.ttlMs)],
    );
    return toInvite(r.rows[0]);
  }

  async getInvite(codeHash: string): Promise<Invite | null> {
    const r = await this.pool.query("SELECT * FROM invites WHERE code_hash = $1", [codeHash]);
    return r.rows[0] ? toInvite(r.rows[0]) : null;
  }

  async claimInvite(codeHash: string, sessionHash: string, sessionTtlMs: number, maxConcurrent: number, now: Date): Promise<ClaimResult> {
    const c = await this.pool.connect();
    try {
      await c.query("BEGIN");
      await c.query("SELECT pg_advisory_xact_lock($1)", [CLAIM_LOCK]);
      const r = await c.query("SELECT * FROM invites WHERE code_hash = $1", [codeHash]);
      const inv = r.rows[0] ? toInvite(r.rows[0]) : null;
      let result: ClaimResult;
      const live = (i: Invite) => i.claimedAt !== null && i.sessionExpiresAt !== null && i.sessionExpiresAt > now;
      if (!inv) result = { ok: false, reason: "not_found" };
      else if (inv.usedAt) result = { ok: false, reason: "used" };
      else if (inv.expiresAt <= now) result = { ok: false, reason: "expired" };
      else if (live(inv)) {
        result = inv.sessionHash === sessionHash ? { ok: true, invite: inv } : { ok: false, reason: "in_use" };
      } else {
        const busy = await c.query(
          "SELECT count(*)::int AS n FROM invites WHERE claimed_at IS NOT NULL AND used_at IS NULL AND session_expires_at > $1",
          [now],
        );
        if (busy.rows[0].n >= maxConcurrent) result = { ok: false, reason: "busy" };
        else {
          const u = await c.query(
            "UPDATE invites SET claimed_at = $2, session_hash = $3, session_expires_at = $4 WHERE code_hash = $1 RETURNING *",
            [codeHash, now, sessionHash, new Date(now.getTime() + sessionTtlMs)],
          );
          result = { ok: true, invite: toInvite(u.rows[0]) };
        }
      }
      await c.query("COMMIT");
      return result;
    } catch (e) {
      await c.query("ROLLBACK");
      throw e;
    } finally {
      c.release();
    }
  }

  async inviteBySession(sessionHash: string, now: Date): Promise<Invite | null> {
    const r = await this.pool.query(
      "SELECT * FROM invites WHERE session_hash = $1 AND session_expires_at > $2", [sessionHash, now]);
    return r.rows[0] ? toInvite(r.rows[0]) : null;
  }

  async attachAccount(codeHash: string, accountId: string): Promise<void> {
    await this.pool.query("UPDATE invites SET account_id = $2 WHERE code_hash = $1", [codeHash, accountId]);
  }

  async releaseInvite(codeHash: string): Promise<void> {
    await this.pool.query(
      `UPDATE invites SET claimed_at = NULL, session_hash = NULL, session_expires_at = NULL,
         code_attempts = 0, password_attempts = 0,
         account_id = CASE WHEN kind = 'new' THEN NULL ELSE account_id END
       WHERE code_hash = $1`, [codeHash]);
  }

  async markInviteUsed(codeHash: string, now: Date): Promise<void> {
    await this.pool.query("UPDATE invites SET used_at = $2 WHERE code_hash = $1", [codeHash, now]);
  }

  async bumpAttempt(codeHash: string, kind: "code" | "password"): Promise<number> {
    const col = kind === "code" ? "code_attempts" : "password_attempts";
    const r = await this.pool.query(
      `UPDATE invites SET ${col} = ${col} + 1 WHERE code_hash = $1 RETURNING ${col} AS n`, [codeHash]);
    return r.rows[0]?.n ?? 0;
  }

  async setOutcome(codeHash: string, outcome: string): Promise<void> {
    await this.pool.query("UPDATE invites SET outcome = $2 WHERE code_hash = $1", [codeHash, outcome]);
  }

  async claimTokenShow(accountId: string, now: Date): Promise<boolean> {
    const r = await this.pool.query(
      "UPDATE accounts SET token_shown_at = $2 WHERE account_id = $1 AND token_shown_at IS NULL", [accountId, now]);
    return r.rowCount === 1;
  }

  async expiredSessions(now: Date): Promise<Invite[]> {
    const r = await this.pool.query(
      "SELECT * FROM invites WHERE claimed_at IS NOT NULL AND used_at IS NULL AND session_expires_at <= $1", [now]);
    return r.rows.map(toInvite);
  }

  async insertAccount(a: Account): Promise<void> {
    const keys = Object.keys(ACCOUNT_COLS) as (keyof typeof ACCOUNT_COLS)[];
    const cols = ["account_id", ...keys.map((k) => ACCOUNT_COLS[k])];
    const vals = [a.accountId, ...keys.map((k) => a[k])];
    await this.pool.query(
      `INSERT INTO accounts (${cols.join(",")}) VALUES (${cols.map((_, i) => "$" + (i + 1)).join(",")})`, vals);
  }

  async getAccount(accountId: string): Promise<Account | null> {
    const r = await this.pool.query("SELECT * FROM accounts WHERE account_id = $1", [accountId]);
    return r.rows[0] ? toAccount(r.rows[0]) : null;
  }

  async listAccounts(): Promise<Account[]> {
    const r = await this.pool.query("SELECT * FROM accounts ORDER BY created_at");
    return r.rows.map(toAccount);
  }

  async updateAccount(accountId: string, patch: Partial<Omit<Account, "accountId">>): Promise<void> {
    const keys = (Object.keys(patch) as (keyof typeof ACCOUNT_COLS)[]).filter((k) => k in ACCOUNT_COLS);
    if (keys.length === 0) return;
    const sets = keys.map((k, i) => `${ACCOUNT_COLS[k]} = $${i + 2}`);
    const r = await this.pool.query(
      `UPDATE accounts SET ${sets.join(", ")} WHERE account_id = $1`, [accountId, ...keys.map((k) => patch[k])]);
    if (r.rowCount === 0) throw new Error(`no account: ${accountId}`);
  }

  async deleteAccount(accountId: string): Promise<void> {
    await this.pool.query("DELETE FROM accounts WHERE account_id = $1", [accountId]);
  }

  async activeByTelegramId(telegramUserId: string): Promise<Account | null> {
    const r = await this.pool.query(
      "SELECT * FROM accounts WHERE status = 'active' AND telegram_user_id = $1", [telegramUserId]);
    return r.rows[0] ? toAccount(r.rows[0]) : null;
  }
}
