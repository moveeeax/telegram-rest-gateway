// Источник истины control plane: аккаунты и приглашения. В базе только хэши токенов и кодов.
export type AccountStatus = "onboarding" | "active" | "needs_relogin" | "deleting";

export type Account = {
  accountId: string;
  status: AccountStatus;
  telegramUserId: string | null;
  username: string | null;
  archiveEnabled: boolean;
  exportEnabled: boolean;
  clientTokenHash: string;
  invitedBy: string;
  createdAt: Date;
  activatedAt: Date | null;
  tokenShownAt: Date | null;
  // Шаг пайплайна удаления (lifecycle.ts), null вне статуса deleting.
  deleteStep: string | null;
  // Ревизия Secret аккаунта: растет при перевыпуске токена, ее смена перезапускает под.
  secretRev: number;
};

export type InviteKind = "new" | "relogin" | "token";
export type AttemptKind = "code" | "password" | "phone";

export type Invite = {
  codeHash: string;
  kind: InviteKind;
  accountId: string | null;
  expiresAt: Date;
  claimedAt: Date | null;
  sessionHash: string | null;
  sessionExpiresAt: Date | null;
  usedAt: Date | null;
  codeAttempts: number;
  passwordAttempts: number;
  phoneAttempts: number;
  // Чем кончилась сессия, если не успехом: "duplicate", "wrong_account", "too_many_attempts".
  outcome: string | null;
};

export type ClaimResult =
  // abandonedAccountId: аккаунт истекшей сессии, которую этот claim перехватил (его надо удалить).
  | { ok: true; invite: Invite; abandonedAccountId?: string | null }
  | { ok: false; reason: "not_found" | "expired" | "in_use" | "busy" | "used" };

export interface Store {
  createInvite(i: { codeHash: string; kind: InviteKind; accountId: string | null; ttlMs: number; now: Date }): Promise<Invite>;
  getInvite(codeHash: string): Promise<Invite | null>;
  // Атомарно занимает инвайт: не истек, не использован, не занят другой сессией, и занятых
  // сессий меньше maxConcurrent. Та же сессия может занять свой инвайт повторно (перезагрузка).
  claimInvite(codeHash: string, sessionHash: string, sessionTtlMs: number, maxConcurrent: number, now: Date): Promise<ClaimResult>;
  inviteBySession(sessionHash: string, now: Date): Promise<Invite | null>;
  attachAccount(codeHash: string, accountId: string): Promise<void>;
  // Сбрасывает занятость (истекшая сессия): ссылка снова работает до своего expiresAt.
  releaseInvite(codeHash: string): Promise<void>;
  markInviteUsed(codeHash: string, now: Date): Promise<void>;
  bumpAttempt(codeHash: string, kind: AttemptKind): Promise<number>;
  setOutcome(codeHash: string, outcome: string): Promise<void>;
  // Атомарно ставит tokenShownAt, если он пуст. true ровно у одного вызывающего.
  claimTokenShow(accountId: string, now: Date): Promise<boolean>;
  // Занятые, неиспользованные инвайты, чья сессия истекла.
  expiredSessions(now: Date): Promise<Invite[]>;
  insertAccount(a: Account): Promise<void>;
  getAccount(accountId: string): Promise<Account | null>;
  listAccounts(): Promise<Account[]>;
  updateAccount(accountId: string, patch: Partial<Omit<Account, "accountId">>): Promise<void>;
  deleteAccount(accountId: string): Promise<void>;
  // Только status = 'active': аккаунты needs_relogin в проверке дубликата не участвуют.
  activeByTelegramId(telegramUserId: string): Promise<Account | null>;
}
