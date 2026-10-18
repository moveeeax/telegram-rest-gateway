import { Onboarding, type S3Ops } from "../../src/onboarding.js";
import { MemStore } from "../../src/store_mem.js";
import { WorkerClient } from "../../src/worker_client.js";
import { sha256hex } from "../../src/ids.js";
import { FakeKube } from "./fake_kube.js";
import { FakeWorker } from "./fake_worker.js";
import { config } from "./fixtures.js";
import type { Config } from "../../src/config.js";

export class FakeS3 implements S3Ops {
  metas = new Map<string, any>();
  deleted: string[] = [];
  async putMeta(accountId: string, meta: object) { this.metas.set(accountId, meta); }
  async deletePrefix(prefix: string) { this.deleted.push(prefix); return 1; }
}

// Онбординг на MemStore, FakeKube и одной заглушке воркера на все аккаунты.
export async function onbEnv(cfgPatch: Partial<Config> = {}) {
  const store = new MemStore();
  const kube = new FakeKube();
  const s3 = new FakeS3();
  const worker = new FakeWorker();
  const url = await worker.start();
  let clock = new Date("2026-10-18T12:00:00Z");
  let kicks = 0;
  const cfg = config(cfgPatch);
  const onb = new Onboarding({
    store, kube, cfg, s3,
    worker: (_id, admin) => new WorkerClient(url, admin, 2000),
    kick: () => { kicks++; },
    now: () => clock,
  });
  const invite = async (kind: "new" | "relogin" = "new", accountId: string | null = null) => {
    const code = "code-" + Math.random().toString(36).slice(2);
    await store.createInvite({ codeHash: sha256hex(code), kind, accountId, ttlMs: 24 * 3600e3, now: clock });
    return code;
  };
  // Открыть ссылку и вернуть cookie сессии.
  const open = async (code: string, cookie: string | null = null) => {
    const r = await onb.open(code, cookie);
    if (r.kind !== "redirect") return { r, cookie: null as string | null };
    return { r, cookie: r.cookie };
  };
  return {
    store, kube, s3, worker, onb, cfg, invite, open,
    advance: (ms: number) => { clock = new Date(clock.getTime() + ms); },
    now: () => clock,
    kicks: () => kicks,
    stop: () => worker.stop(),
  };
}
