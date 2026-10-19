// Клиент реестра control plane: токен MCP -> набор аккаунтов. Снимок реестра кэшируется на ttl;
// при недоступном реестре уже известные токены живут на последнем удачном снимке до staleMs,
// неизвестные получают "unavailable" (503), а не null (401): ключ мог быть верным.
import { createHash } from "node:crypto";
import type { AccountRef, AccountSet } from "./accounts.js";

type Snapshot = { at: number; byHash: Map<string, AccountRef[]> };

export class RegistryClient {
  private snap: Snapshot | null = null;
  private inflight: Promise<void> | null = null;
  private failedAt = 0;
  private ttlMs: number;
  private staleMs: number;
  private now: () => number;

  constructor(private url: string, private token: string, opts: { ttlMs?: number; staleMs?: number; now?: () => number } = {}) {
    this.ttlMs = opts.ttlMs ?? 30_000;
    this.staleMs = opts.staleMs ?? 300_000;
    this.now = opts.now ?? Date.now;
  }

  async resolve(token: string): Promise<AccountSet | null | "unavailable"> {
    if (!token) return null;
    const t = this.now();
    const fresh = this.snap !== null && t - this.snap.at < this.ttlMs;
    // После неудачи не долбим реестр на каждом запросе: следующая попытка через секунду.
    if (!fresh && t - this.failedAt >= 1000) await this.refresh();
    const s = this.snap;
    const age = s ? this.now() - s.at : Infinity;
    const hash = createHash("sha256").update(token).digest("hex");
    if (!s || age >= this.staleMs) return "unavailable";
    const accounts = s.byHash.get(hash);
    if (accounts) return { tokenHash: hash, accounts };
    return age < this.ttlMs ? null : "unavailable";
  }

  private refresh(): Promise<void> {
    if (!this.inflight) {
      this.inflight = this.fetchSnapshot().finally(() => {
        this.inflight = null;
      });
    }
    return this.inflight;
  }

  private async fetchSnapshot(): Promise<void> {
    try {
      const r = await fetch(this.url, { headers: { authorization: `Bearer ${this.token}` }, signal: AbortSignal.timeout(5000) });
      if (!r.ok) throw new Error(`registry http ${r.status}`);
      const body = (await r.json()) as { tokens?: { sha256: string; accounts: AccountRef[] }[] };
      const byHash = new Map<string, AccountRef[]>();
      for (const e of body.tokens ?? []) {
        if (typeof e.sha256 === "string" && Array.isArray(e.accounts) && e.accounts.length) byHash.set(e.sha256, e.accounts);
      }
      this.snap = { at: this.now(), byHash };
    } catch (e) {
      this.failedAt = this.now();
      console.error(`tgw-mcp: registry refresh failed: ${(e as Error).message}`);
    }
  }
}
