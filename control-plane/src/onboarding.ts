// Онбординг по инвайту: занять ссылку, поднять воркер, провести логин, выдать токен один раз.
// Состояние живет в базе и в воркере; в памяти только кэш SVG по QR-ссылке.
import QRCode from "qrcode";
import type { Config } from "./config.js";
import { clientToken, newAccountId, newCode, newSecret, sha256hex } from "./ids.js";
import type { Kube } from "./kube.js";
import { SECRET_KEYS, secretManifest, workerName } from "./manifests.js";
import type { S3Ops } from "./s3.js";
import type { Account, Invite, Store } from "./store.js";
import type { AuthState, CodeInfo, WorkerClient } from "./worker_client.js";

export type { S3Ops } from "./s3.js";

export const INVITE_TTL_MS = 24 * 3600 * 1000;
export const SESSION_TTL_MS = 15 * 60 * 1000;
export const MAX_CONCURRENT = 3;
export const MAX_ATTEMPTS = 5;
// Номера телефона на сессию: каждый вызов шлет код через общий API_ID, флуд грозит его блокировкой.
export const MAX_PHONE_ATTEMPTS = 3;
export const COOKIE_NAME = "tgw_onb";

export const TEXT = {
  inUse: "ссылка уже используется",
  busy: "Сейчас подключается несколько человек, попробуйте через несколько минут.",
  invalid: "Ссылка недействительна или истекла.",
  duplicate: "этот аккаунт уже подключен",
  wrongAccount: "Вы вошли другим аккаунтом Telegram. Повторите вход нужным аккаунтом по новой ссылке.",
  tooMany: "Слишком много неверных попыток. Попросите новую ссылку.",
} as const;

export type Phase = "starting" | "qr" | "code" | "password" | "ready" | "duplicate" | "expired" | "error";

export type OnboardView = {
  phase: Phase;
  qr_svg?: string;
  code_info?: CodeInfo;
  error?: string;
  token?: string;
  api_url?: string;
  mcp_url?: string;
  claude_mcp_add?: string;
};

export type OpenResult =
  | { kind: "redirect"; cookie: string; setCookie: string }
  | { kind: "page"; message: string; status: number };

export type OnboardingDeps = {
  store: Store;
  kube: Kube;
  cfg: Config;
  worker: (accountId: string, adminToken: string) => WorkerClient;
  s3: S3Ops;
  kick: () => void;
  now?: () => Date;
};

type Session = { invite: Invite; account: Account; worker: WorkerClient; secrets: Record<string, string> };

export function sessionCookie(value: string): string {
  return `${COOKIE_NAME}=${value}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=${SESSION_TTL_MS / 1000}`;
}

export class Onboarding {
  private now: () => Date;
  private qrCache = new Map<string, string>();

  constructor(private d: OnboardingDeps) {
    this.now = d.now ?? (() => new Date());
  }

  async open(code: string, cookie: string | null): Promise<OpenResult> {
    const now = this.now();
    // Cookie переиспользуется только для той же ссылки (перезагрузка). Cookie другой ссылки, например
    // недавнего онбординга в этом же браузере, привязал бы новую ссылку к чужой завершенной сессии.
    const codeHash = sha256hex(code);
    const bound = cookie ? await this.d.store.inviteBySession(sha256hex(cookie), now) : null;
    const value = cookie && (bound === null || bound.codeHash === codeHash) ? cookie : newCode();
    const claim = await this.d.store.claimInvite(codeHash, sha256hex(value), SESSION_TTL_MS, MAX_CONCURRENT, now);
    if (!claim.ok) {
      if (claim.reason === "in_use" || claim.reason === "used") return { kind: "page", status: 409, message: TEXT.inUse };
      if (claim.reason === "busy") return { kind: "page", status: 503, message: TEXT.busy };
      return { kind: "page", status: 404, message: TEXT.invalid };
    }
    const inv = claim.invite;
    if (claim.abandonedAccountId) {
      const old = await this.d.store.getAccount(claim.abandonedAccountId);
      if (old?.status === "onboarding") await this.d.store.updateAccount(old.accountId, { status: "deleting" });
    }
    if (inv.kind === "token") return { kind: "page", status: 404, message: TEXT.invalid };
    if (inv.kind === "new" && !inv.accountId) await this.createAccount(inv, now);
    if (inv.kind === "relogin") {
      const a = inv.accountId ? await this.d.store.getAccount(inv.accountId) : null;
      if (!a || a.status !== "needs_relogin") {
        await this.d.store.releaseInvite(inv.codeHash);
        return { kind: "page", status: 409, message: TEXT.inUse };
      }
    }
    this.d.kick();
    return { kind: "redirect", cookie: value, setCookie: sessionCookie(value) };
  }

  private async createAccount(inv: Invite, now: Date): Promise<void> {
    const accountId = newAccountId();
    const secrets = {
      clientToken: clientToken(accountId), adminToken: newSecret(), mcpServiceToken: newSecret(),
      archiverServiceToken: newSecret(), databaseEncryptionKey: newSecret(),
    };
    await this.d.store.insertAccount({
      accountId, status: "onboarding", telegramUserId: null, username: null, archiveEnabled: true,
      exportEnabled: false, clientTokenHash: sha256hex(secrets.clientToken), invitedBy: "owner", createdAt: now,
      activatedAt: null, tokenShownAt: null, deleteStep: null, secretRev: 1,
    });
    // Secret до attach: reconcile, увидев аккаунт, поднимет под уже с токенами и ключом.
    await this.d.kube.apply(secretManifest(accountId, secrets, 1, this.d.cfg));
    await this.d.store.attachAccount(inv.codeHash, accountId);
  }

  private async session(cookie: string): Promise<Session | Phase> {
    const invite = await this.d.store.inviteBySession(sha256hex(cookie), this.now());
    if (!invite || !invite.accountId) return "expired";
    const account = await this.d.store.getAccount(invite.accountId);
    if (!account) return "expired";
    const secrets = await this.d.kube.getSecret(workerName(account.accountId));
    if (!secrets?.[SECRET_KEYS.admin]) return "starting";
    return { invite, account, secrets, worker: this.d.worker(account.accountId, secrets[SECRET_KEYS.admin]) };
  }

  private outcomeView(inv: Invite): OnboardView | null {
    if (inv.outcome === "duplicate") return { phase: "duplicate", error: TEXT.duplicate };
    if (inv.outcome === "wrong_account") return { phase: "error", error: TEXT.wrongAccount };
    if (inv.outcome === "too_many_attempts") return { phase: "error", error: TEXT.tooMany };
    return null;
  }

  async view(cookie: string): Promise<OnboardView> {
    const s = await this.session(cookie);
    if (typeof s === "string") return { phase: s };
    const done = this.outcomeView(s.invite);
    if (done) return done;
    if (s.account.status === "deleting") return { phase: "expired" };
    if (s.account.status === "active" && s.invite.usedAt) return this.readyView(s);
    if (!(await s.worker.health())) return { phase: "starting" };
    const st = await s.worker.state();
    if (!st.ok) return { phase: "starting" };
    return this.fromState(s, st.value, true);
  }

  private async fromState(s: Session, st: AuthState, mayStartQr: boolean): Promise<OnboardView> {
    switch (st.authorization_state) {
      case "wait_phone_number": {
        if (!mayStartQr) return { phase: "qr" };
        const r = await s.worker.startQr();
        return r.ok ? this.fromState(s, r.value, false) : { phase: "starting" };
      }
      case "wait_other_device_confirmation":
        return st.qr_link ? { phase: "qr", qr_svg: await this.svg(st.qr_link) } : { phase: "starting" };
      case "wait_code":
        return { phase: "code", code_info: st.code_info };
      case "wait_password":
        return { phase: "password" };
      case "ready":
        return this.finish(s);
      case "logging_out":
      case "closing":
      case "closed":
        return { phase: "error", error: "Сессия закрыта. Попросите новую ссылку." };
      default:
        return { phase: "starting" };
    }
  }

  private async svg(link: string): Promise<string> {
    let out = this.qrCache.get(link);
    if (!out) {
      out = await QRCode.toString(link, { type: "svg", margin: 1 });
      if (this.qrCache.size > 50) this.qrCache.clear();
      this.qrCache.set(link, out);
    }
    return out;
  }

  // Воркер в ready: проверить дубликат или совпадение аккаунта, активировать.
  private async finish(s: Session): Promise<OnboardView> {
    const me = await s.worker.me();
    if (!me.ok) return { phase: "starting" };
    const now = this.now();
    const { store } = this.d;
    const id = s.account.accountId;
    const tg = me.value.id;
    if (s.invite.kind === "relogin") {
      if (s.account.telegramUserId !== tg) {
        await s.worker.logout();
        await store.setOutcome(s.invite.codeHash, "wrong_account");
        await store.markInviteUsed(s.invite.codeHash, now);
        // Новая ревизия перезапускает под: после logout он закрыт и ждет SIGTERM.
        await store.updateAccount(id, { secretRev: s.account.secretRev + 1 });
        this.d.kick();
        return { phase: "error", error: TEXT.wrongAccount };
      }
      await store.updateAccount(id, { status: "active", username: me.value.username ?? null });
      await store.markInviteUsed(s.invite.codeHash, now);
      this.d.kick();
      return { phase: "ready", api_url: this.d.cfg.apiBaseUrl };
    }
    const dup = await store.activeByTelegramId(tg);
    let isDup = dup !== null && dup.accountId !== id;
    if (!isDup) {
      try {
        await this.d.s3.putMeta(id, { account_id: id, telegram_user_id: tg, username: me.value.username ?? null, activated_at: now.toISOString() });
        await store.updateAccount(id, { status: "active", telegramUserId: tg, username: me.value.username ?? null, activatedAt: now });
      } catch (e) {
        // Уникальный индекс по активному telegram id: параллельный онбординг того же человека.
        if (!/duplicate/i.test((e as Error).message)) throw e;
        isDup = true;
      }
    }
    if (isDup) {
      await s.worker.logout();
      await store.setOutcome(s.invite.codeHash, "duplicate");
      await store.markInviteUsed(s.invite.codeHash, now);
      await store.updateAccount(id, { status: "deleting", deleteStep: "logged_out" });
      this.d.kick();
      return { phase: "duplicate", error: TEXT.duplicate };
    }
    await store.markInviteUsed(s.invite.codeHash, now);
    this.d.kick();
    const fresh = (await store.getAccount(id))!;
    return this.readyView({ ...s, account: fresh });
  }

  private async readyView(s: Session): Promise<OnboardView> {
    const v: OnboardView = { phase: "ready", api_url: this.d.cfg.apiBaseUrl };
    if (s.invite.kind !== "new") return v;
    if (!(await this.d.store.claimTokenShow(s.account.accountId, this.now()))) return v;
    const token = s.secrets[SECRET_KEYS.client];
    v.token = token;
    if (this.d.cfg.mcpBaseUrl) {
      v.mcp_url = `${this.d.cfg.mcpBaseUrl}/mcp`;
      v.claude_mcp_add = `claude mcp add --transport http telegram ${v.mcp_url} --header "X-API-Key: ${token}"`;
    }
    return v;
  }

  submitPhone(cookie: string, phone: string): Promise<OnboardView> {
    return this.submit(cookie, "phone", (w) => w.phone(phone));
  }
  submitCode(cookie: string, code: string): Promise<OnboardView> {
    return this.submit(cookie, "code", (w) => w.code(code));
  }
  submitPassword(cookie: string, password: string): Promise<OnboardView> {
    return this.submit(cookie, "password", (w) => w.password(password));
  }

  private async submit(
    cookie: string, attempt: "code" | "password" | "phone",
    call: (w: WorkerClient) => Promise<{ ok: true; value: AuthState } | { ok: false; status: number; error: string }>,
  ): Promise<OnboardView> {
    const s = await this.session(cookie);
    if (typeof s === "string") return { phase: s };
    const done = this.outcomeView(s.invite);
    if (done) return done;
    const limit = attempt === "phone" ? MAX_PHONE_ATTEMPTS : MAX_ATTEMPTS;
    if ((await this.d.store.bumpAttempt(s.invite.codeHash, attempt)) > limit) {
      await this.d.store.setOutcome(s.invite.codeHash, "too_many_attempts");
      await this.d.store.markInviteUsed(s.invite.codeHash, this.now());
      if (s.account.status === "onboarding") await this.d.store.updateAccount(s.account.accountId, { status: "deleting" });
      this.d.kick();
      return { phase: "error", error: TEXT.tooMany };
    }
    const r = await call(s.worker);
    if (!r.ok) {
      const v = await this.view(cookie);
      return { ...v, error: r.status === 0 ? "Сервер не ответил, попробуйте еще раз." : `Telegram: ${r.error}` };
    }
    return this.fromState(s, r.value, false);
  }

  // Истекшие сессии: новый аккаунт без логина уходит в удаление, ссылка снова доступна.
  async expire(now: Date): Promise<void> {
    for (const inv of await this.d.store.expiredSessions(now)) {
      if (inv.accountId && inv.kind === "new") {
        const a = await this.d.store.getAccount(inv.accountId);
        if (a?.status === "onboarding") await this.d.store.updateAccount(a.accountId, { status: "deleting" });
      }
      await this.d.store.releaseInvite(inv.codeHash);
    }
  }
}
