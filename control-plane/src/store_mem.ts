// Хранилище в памяти для тестов. Семантика совпадает с PgStore, включая атомарность claim.
import type { Account, AttemptKind, ClaimResult, Invite, InviteKind, OwnerToken, Store } from "./store.js";

export class MemStore implements Store {
  private invites = new Map<string, Invite>();
  private accounts = new Map<string, Account>();
  private owners = new Map<string, OwnerToken>();
  // Сбой базы для тестов reconcile: следующий listAccounts бросит эту ошибку.
  failNextList: Error | null = null;

  async createInvite(i: { codeHash: string; kind: InviteKind; accountId: string | null; ttlMs: number; now: Date }): Promise<Invite> {
    const inv: Invite = {
      codeHash: i.codeHash, kind: i.kind, accountId: i.accountId, expiresAt: new Date(i.now.getTime() + i.ttlMs),
      claimedAt: null, sessionHash: null, sessionExpiresAt: null, usedAt: null, codeAttempts: 0, passwordAttempts: 0, phoneAttempts: 0, outcome: null,
    };
    this.invites.set(i.codeHash, inv);
    return { ...inv };
  }

  async getInvite(codeHash: string): Promise<Invite | null> {
    const i = this.invites.get(codeHash);
    return i ? { ...i } : null;
  }

  async claimInvite(codeHash: string, sessionHash: string, sessionTtlMs: number, maxConcurrent: number, now: Date): Promise<ClaimResult> {
    const inv = this.invites.get(codeHash);
    if (!inv) return { ok: false, reason: "not_found" };
    if (inv.usedAt) return { ok: false, reason: "used" };
    if (inv.expiresAt <= now) return { ok: false, reason: "expired" };
    const live = (i: Invite) => i.claimedAt !== null && i.usedAt === null && i.sessionExpiresAt !== null && i.sessionExpiresAt > now;
    if (live(inv)) {
      return inv.sessionHash === sessionHash ? { ok: true, invite: { ...inv } } : { ok: false, reason: "in_use" };
    }
    const busy = [...this.invites.values()].filter(live).length;
    if (busy >= maxConcurrent) return { ok: false, reason: "busy" };
    // Истекшая, но еще не освобожденная сессия: начинаем с чистого листа, как после releaseInvite.
    const abandonedAccountId = inv.claimedAt !== null && inv.kind === "new" ? inv.accountId : null;
    if (inv.claimedAt !== null) this.reset(inv);
    inv.claimedAt = now;
    inv.sessionHash = sessionHash;
    inv.sessionExpiresAt = new Date(now.getTime() + sessionTtlMs);
    return { ok: true, invite: { ...inv }, abandonedAccountId };
  }

  async inviteBySession(sessionHash: string, now: Date): Promise<Invite | null> {
    for (const i of this.invites.values()) {
      if (i.sessionHash === sessionHash && i.sessionExpiresAt !== null && i.sessionExpiresAt > now) return { ...i };
    }
    return null;
  }

  async attachAccount(codeHash: string, accountId: string): Promise<void> {
    const i = this.invites.get(codeHash);
    if (i) i.accountId = accountId;
  }

  private reset(i: Invite) {
    i.claimedAt = null;
    i.sessionHash = null;
    i.sessionExpiresAt = null;
    i.codeAttempts = 0;
    i.passwordAttempts = 0;
    i.phoneAttempts = 0;
    i.outcome = null;
    if (i.kind === "new") i.accountId = null;
  }

  async releaseInvite(codeHash: string): Promise<void> {
    const i = this.invites.get(codeHash);
    if (i) this.reset(i);
  }

  async markInviteUsed(codeHash: string, now: Date): Promise<void> {
    const i = this.invites.get(codeHash);
    if (i) i.usedAt = now;
  }

  async bumpAttempt(codeHash: string, kind: AttemptKind): Promise<number> {
    const i = this.invites.get(codeHash);
    if (!i) return 0;
    if (kind === "code") return ++i.codeAttempts;
    if (kind === "phone") return ++i.phoneAttempts;
    return ++i.passwordAttempts;
  }

  async setOutcome(codeHash: string, outcome: string): Promise<void> {
    const i = this.invites.get(codeHash);
    if (i) i.outcome = outcome;
  }

  async claimTokenShow(accountId: string, now: Date): Promise<boolean> {
    const a = this.accounts.get(accountId);
    if (!a || a.tokenShownAt) return false;
    a.tokenShownAt = now;
    return true;
  }

  async expiredSessions(now: Date): Promise<Invite[]> {
    return [...this.invites.values()]
      .filter((i) => i.claimedAt !== null && i.usedAt === null && i.sessionExpiresAt !== null && i.sessionExpiresAt <= now)
      .map((i) => ({ ...i }));
  }

  async insertAccount(a: Account): Promise<void> {
    if (this.accounts.has(a.accountId)) throw new Error(`account exists: ${a.accountId}`);
    this.accounts.set(a.accountId, { ...a });
  }

  async getAccount(accountId: string): Promise<Account | null> {
    const a = this.accounts.get(accountId);
    return a ? { ...a } : null;
  }

  async listAccounts(): Promise<Account[]> {
    if (this.failNextList) {
      const e = this.failNextList;
      this.failNextList = null;
      throw e;
    }
    return [...this.accounts.values()].map((a) => ({ ...a }));
  }

  async updateAccount(accountId: string, patch: Partial<Omit<Account, "accountId">>): Promise<void> {
    const a = this.accounts.get(accountId);
    if (!a) throw new Error(`no account: ${accountId}`);
    if (patch.status === "active" && (patch.telegramUserId ?? a.telegramUserId)) {
      const tg = patch.telegramUserId ?? a.telegramUserId;
      for (const o of this.accounts.values()) {
        if (o.accountId !== accountId && o.status === "active" && o.telegramUserId === tg) {
          throw new Error("duplicate active telegram user");
        }
      }
    }
    Object.assign(a, patch);
  }

  async deleteAccount(accountId: string): Promise<void> {
    this.accounts.delete(accountId);
  }

  async insertOwnerToken(t: OwnerToken): Promise<void> {
    if (this.owners.has(t.label) || [...this.owners.values()].some((o) => o.tokenHash === t.tokenHash)) {
      throw new Error(`owner token exists: ${t.label}`);
    }
    this.owners.set(t.label, { ...t, accountIds: [...t.accountIds] });
  }

  async listOwnerTokens(): Promise<OwnerToken[]> {
    return [...this.owners.values()].map((o) => ({ ...o, accountIds: [...o.accountIds] }));
  }

  async deleteOwnerToken(label: string): Promise<boolean> {
    return this.owners.delete(label);
  }

  async activeByTelegramId(telegramUserId: string): Promise<Account | null> {
    for (const a of this.accounts.values()) {
      if (a.status === "active" && a.telegramUserId === telegramUserId) return { ...a };
    }
    return null;
  }
}
