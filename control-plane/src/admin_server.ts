// Admin API владельца и реестр для MCP и архиватора. Слушает отдельный порт, в Ingress не попадает.
import { createHash, timingSafeEqual } from "node:crypto";
import express from "express";
import type { Config } from "./config.js";
import { clientToken, isAccountId, newCode, newSecret, sha256hex } from "./ids.js";
import type { Kube } from "./kube.js";
import { SECRET_KEYS, secretManifest, workerName } from "./manifests.js";
import { INVITE_TTL_MS, TEXT } from "./onboarding.js";
import type { Account, Store } from "./store.js";
import { workerBaseUrl, type WorkerClient } from "./worker_client.js";

export type AdminDeps = {
  store: Store;
  kube: Kube;
  cfg: Config;
  worker: (accountId: string, adminToken: string) => WorkerClient;
  kick: () => void;
  now?: () => Date;
};

function digest(s: string): Buffer {
  return createHash("sha256").update(s).digest();
}

// Сравнение по хэшам фиксированной длины, без раннего выхода.
export function tokenMatches(presented: string, expected: string): boolean {
  return expected !== "" && timingSafeEqual(digest(presented), digest(expected));
}

function bearer(req: express.Request): string {
  const h = req.headers.authorization ?? "";
  return h.startsWith("Bearer ") ? h.slice(7) : "";
}

function publicAccount(a: Account) {
  return {
    accountId: a.accountId, status: a.status, telegramUserId: a.telegramUserId, username: a.username,
    archiveEnabled: a.archiveEnabled, exportEnabled: a.exportEnabled, createdAt: a.createdAt, activatedAt: a.activatedAt,
  };
}

// Ссылка /t/<code>: новый токен показывается один раз.
export function tokenReveal(d: AdminDeps) {
  const now = d.now ?? (() => new Date());
  return async (code: string): Promise<{ token: string; apiUrl: string } | { status: number; message: string }> => {
    const claim = await d.store.claimInvite(sha256hex(code), sha256hex(newCode()), 60_000, Number.POSITIVE_INFINITY, now());
    if (!claim.ok) {
      if (claim.reason === "used" || claim.reason === "in_use") return { status: 409, message: TEXT.inUse };
      return { status: 404, message: TEXT.invalid };
    }
    const inv = claim.invite;
    if (inv.kind !== "token" || !inv.accountId) {
      await d.store.releaseInvite(inv.codeHash);
      return { status: 404, message: TEXT.invalid };
    }
    await d.store.markInviteUsed(inv.codeHash, now());
    const secrets = await d.kube.getSecret(workerName(inv.accountId));
    const token = secrets?.[SECRET_KEYS.client];
    if (!token) return { status: 404, message: TEXT.invalid };
    return { token, apiUrl: d.cfg.apiBaseUrl };
  };
}

export function adminApp(d: AdminDeps): express.Express {
  const now = d.now ?? (() => new Date());
  const { store, kube, cfg } = d;
  const app = express();
  app.disable("x-powered-by");
  app.use(express.json({ limit: "16kb" }));

  const wrap = (fn: (req: express.Request, res: express.Response) => Promise<void>) =>
    (req: express.Request, res: express.Response) => {
      fn(req, res).catch((e) => {
        console.error(`admin: ${req.method} ${req.path}: ${(e as Error).message}`);
        if (!res.headersSent) res.status(500).json({ error: "internal" });
      });
    };

  app.get("/healthz", (_req, res) => {
    res.json({ ok: true });
  });

  app.get("/internal/registry", wrap(async (req, res) => {
    const t = bearer(req);
    const isMcp = tokenMatches(t, cfg.registryMcpToken);
    const isArchiver = tokenMatches(t, cfg.registryArchiverToken);
    if (!isMcp && !isArchiver) return void res.status(401).json({ error: "unauthorized" });
    const active = (await store.listAccounts()).filter((a) => a.status === "active");
    if (isMcp) {
      const tokens = [];
      for (const a of active) {
        const s = await kube.getSecret(workerName(a.accountId));
        if (!s?.[SECRET_KEYS.mcp]) continue;
        tokens.push({ sha256: a.clientTokenHash, accountIds: [a.accountId], gatewayUrl: workerBaseUrl(a.accountId, cfg), serviceToken: s[SECRET_KEYS.mcp] });
      }
      return void res.json({ tokens });
    }
    const accounts: Record<string, unknown> = {};
    for (const a of active) {
      const s = await kube.getSecret(workerName(a.accountId));
      if (!s?.[SECRET_KEYS.archiver]) continue;
      accounts[a.accountId] = { gatewayUrl: workerBaseUrl(a.accountId, cfg), archiverGatewayToken: s[SECRET_KEYS.archiver], archive_enabled: a.archiveEnabled };
    }
    res.json({ accounts });
  }));

  app.use("/admin", (req, res, next) => {
    if (!tokenMatches(bearer(req), cfg.adminToken)) return void res.status(401).json({ error: "unauthorized" });
    next();
  });

  const inviteUrl = (path: string, code: string) => `${cfg.joinBaseUrl}/${path}/${code}`;

  app.post("/admin/invites", wrap(async (_req, res) => {
    const code = newCode();
    const inv = await store.createInvite({ codeHash: sha256hex(code), kind: "new", accountId: null, ttlMs: INVITE_TTL_MS, now: now() });
    res.status(201).json({ url: inviteUrl("i", code), expires_at: inv.expiresAt.toISOString() });
  }));

  app.get("/admin/accounts", wrap(async (_req, res) => {
    const accounts = await store.listAccounts();
    const out = await Promise.all(accounts.map(async (a) => {
      let ready: boolean | null = null;
      const admin = (await kube.getSecret(workerName(a.accountId)))?.[SECRET_KEYS.admin];
      if (admin) {
        const r = await d.worker(a.accountId, admin).state();
        ready = r.ok ? r.value.ready : null;
      }
      return { ...publicAccount(a), tgw_ready: ready };
    }));
    res.json({ accounts: out });
  }));

  // Аккаунт по :id или 404 (заодно отсекает небезопасные id).
  const load = async (req: express.Request, res: express.Response): Promise<Account | null> => {
    const id = String(req.params.id);
    const a = isAccountId(id) ? await store.getAccount(id) : null;
    if (!a) res.status(404).json({ error: "not_found" });
    return a;
  };

  app.patch("/admin/accounts/:id", wrap(async (req, res) => {
    const a = await load(req, res);
    if (!a) return;
    const body = req.body ?? {};
    const patch: Partial<Account> = {};
    for (const [k, field] of [["archive_enabled", "archiveEnabled"], ["export_enabled", "exportEnabled"]] as const) {
      if (body[k] === undefined) continue;
      if (typeof body[k] !== "boolean") return void res.status(400).json({ error: `${k} must be boolean` });
      patch[field] = body[k];
    }
    await store.updateAccount(a.accountId, patch);
    d.kick();
    res.json(publicAccount((await store.getAccount(a.accountId))!));
  }));

  app.post("/admin/accounts/:id/relogin", wrap(async (req, res) => {
    const a = await load(req, res);
    if (!a) return;
    if (a.status !== "needs_relogin") return void res.status(409).json({ error: `status is ${a.status}` });
    const code = newCode();
    const inv = await store.createInvite({ codeHash: sha256hex(code), kind: "relogin", accountId: a.accountId, ttlMs: INVITE_TTL_MS, now: now() });
    res.status(201).json({ url: inviteUrl("i", code), expires_at: inv.expiresAt.toISOString() });
  }));

  app.post("/admin/accounts/:id/token", wrap(async (req, res) => {
    const a = await load(req, res);
    if (!a) return;
    if (a.status !== "active") return void res.status(409).json({ error: `status is ${a.status}` });
    const old = await kube.getSecret(workerName(a.accountId));
    if (!old?.[SECRET_KEYS.admin] || !old[SECRET_KEYS.dbKey]) return void res.status(409).json({ error: "account secret missing" });
    const token = clientToken(a.accountId);
    const rev = a.secretRev + 1;
    // Ключ БД и служебные токены прежние: меняется только клиентский токен.
    await kube.apply(secretManifest(a.accountId, {
      clientToken: token, adminToken: old[SECRET_KEYS.admin], databaseEncryptionKey: old[SECRET_KEYS.dbKey],
      mcpServiceToken: old[SECRET_KEYS.mcp] ?? newSecret(), archiverServiceToken: old[SECRET_KEYS.archiver] ?? newSecret(),
      legacyBearerTokens: old[SECRET_KEYS.legacy] ? old[SECRET_KEYS.legacy].split("\n") : undefined,
    }, rev, cfg));
    await store.updateAccount(a.accountId, { clientTokenHash: sha256hex(token), secretRev: rev });
    const code = newCode();
    const inv = await store.createInvite({ codeHash: sha256hex(code), kind: "token", accountId: a.accountId, ttlMs: INVITE_TTL_MS, now: now() });
    d.kick();
    res.status(201).json({ url: inviteUrl("t", code), expires_at: inv.expiresAt.toISOString() });
  }));

  // Перенос аккаунта из чарта tgw: тот же числовой id (он же сегмент S3 с сессией), тот же ключ
  // шифрования БД TDLib и старые токены владельца. Вызывать только когда старый под уже остановлен:
  // reconcile сразу поднимет воркер, и два процесса на одну сессию отзовут ее (AUTH_KEY_DUPLICATED).
  app.post("/admin/accounts/import", wrap(async (req, res) => {
    const b = req.body ?? {};
    const id = typeof b.account_id === "string" ? b.account_id : "";
    const key = typeof b.database_encryption_key === "string" ? b.database_encryption_key : "";
    const legacy = b.legacy_bearer_tokens ?? [];
    const bad = (m: string) => void res.status(400).json({ error: m });
    if (!/^[0-9]{1,20}$/.test(id)) return bad("account_id must be the numeric id of an existing account");
    if (!key) return bad("database_encryption_key is required");
    if (!Array.isArray(legacy) || legacy.some((t) => typeof t !== "string" || !t.trim() || /[\r\n]/.test(t))) {
      return bad("legacy_bearer_tokens must be an array of single-line strings");
    }
    if (b.archive_enabled !== undefined && typeof b.archive_enabled !== "boolean") return bad("archive_enabled must be boolean");
    if (await store.getAccount(id)) return void res.status(409).json({ error: "account exists" });
    const token = clientToken(id);
    await kube.apply(secretManifest(id, {
      clientToken: token, adminToken: newSecret(), mcpServiceToken: newSecret(), archiverServiceToken: newSecret(),
      databaseEncryptionKey: key, legacyBearerTokens: legacy.length ? legacy : undefined,
    }, 1, cfg));
    await store.insertAccount({
      accountId: id, status: "active", telegramUserId: id, username: null, archiveEnabled: b.archive_enabled ?? true,
      exportEnabled: false, clientTokenHash: sha256hex(token), invitedBy: "import", createdAt: now(), activatedAt: now(),
      tokenShownAt: now(), deleteStep: null, secretRev: 1,
    });
    d.kick();
    res.status(201).json({ accountId: id, client_token: token });
  }));

  app.delete("/admin/accounts/:id", wrap(async (req, res) => {
    const a = await load(req, res);
    if (!a) return;
    if (a.status !== "deleting") await store.updateAccount(a.accountId, { status: "deleting", deleteStep: null });
    d.kick();
    res.status(202).json({ accountId: a.accountId, status: "deleting" });
  }));

  app.use((_req, res) => {
    res.status(404).json({ error: "not_found" });
  });
  return app;
}
