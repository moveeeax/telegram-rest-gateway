// Реестр control plane для архиватора: accountId -> адрес гейтвея, токен архиватора, archive_enabled.
// Снимок кэшируется на ttl; при недоступном реестре последний удачный снимок живет до staleMs.
export type ArchiveTarget = { gatewayUrl: string; token: string; archiveEnabled: boolean };

type Snapshot = { at: number; byId: Map<string, ArchiveTarget> };

export class ArchiverRegistry {
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

  // Хотя бы один снимок загружен: до этого кадры Kafka нельзя ни писать, ни отбрасывать.
  ready(): boolean {
    return this.snap !== null;
  }

  async get(sessionId: string): Promise<ArchiveTarget | null | "unavailable"> {
    const t = this.now();
    const fresh = this.snap !== null && t - this.snap.at < this.ttlMs;
    if (!fresh && t - this.failedAt >= 1000) await this.refresh();
    const s = this.snap;
    if (!s || this.now() - s.at >= this.staleMs) return "unavailable";
    return s.byId.get(sessionId) ?? null;
  }

  private refresh(): Promise<void> {
    if (!this.inflight) this.inflight = this.load().finally(() => { this.inflight = null; });
    return this.inflight;
  }

  private async load(): Promise<void> {
    try {
      const r = await fetch(this.url, { headers: { authorization: `Bearer ${this.token}` }, signal: AbortSignal.timeout(5000) });
      if (!r.ok) throw new Error(`registry http ${r.status}`);
      const body = (await r.json()) as { accounts?: Record<string, { gatewayUrl: string; archiverGatewayToken: string; archive_enabled: boolean }> };
      const byId = new Map<string, ArchiveTarget>();
      for (const [id, a] of Object.entries(body.accounts ?? {})) {
        if (a && typeof a.gatewayUrl === "string" && typeof a.archiverGatewayToken === "string") {
          byId.set(id, { gatewayUrl: a.gatewayUrl.replace(/\/$/, ""), token: a.archiverGatewayToken, archiveEnabled: a.archive_enabled === true });
        }
      }
      this.snap = { at: this.now(), byId };
    } catch (e) {
      this.failedAt = this.now();
      console.error(`archiver: registry refresh failed: ${(e as Error).message}`);
    }
  }
}
