// Сверка таблицы accounts с объектами в namespace воркеров. Источник истины база: если ее
// прочитать не удалось, проход прерывается до любого apply и delete.
import type { Config } from "./config.js";
import { MANAGED_KINDS, type Kube } from "./kube.js";
import { desiredObjects, LABEL_ACCOUNT } from "./manifests.js";
import type { Store } from "./store.js";

export type PassResult = { applied: number; deleted: number };

export async function reconcileOnce(store: Store, kube: Kube, cfg: Config, _now: Date): Promise<PassResult> {
  const accounts = await store.listAccounts();
  const byId = new Map(accounts.map((a) => [a.accountId, a]));
  let applied = 0;
  for (const a of accounts) {
    try {
      for (const obj of desiredObjects(a, cfg)) {
        await kube.apply(obj);
        applied++;
      }
    } catch (e) {
      // Ошибка одного аккаунта не останавливает остальные, следующий проход повторит.
      console.error(`reconcile: apply ${a.accountId} failed: ${(e as Error).message}`);
    }
  }
  let deleted = 0;
  for (const kind of MANAGED_KINDS) {
    for (const o of await kube.listManaged(kind)) {
      const id = o.metadata.labels?.[LABEL_ACCOUNT];
      const a = id ? byId.get(id) : undefined;
      // Secret и прочее удаляются только без записи в базе; маршрут живет только у active.
      const orphan = !a;
      const staleRoute = kind === "IngressRoute" && a !== undefined && a.status !== "active";
      if (orphan || staleRoute) {
        await kube.delete(kind, o.metadata.name);
        deleted++;
      }
    }
  }
  return { applied, deleted };
}

// Задачи, которые идут в каждом проходе после reconcile: истечение онбордингов, удаление,
// опрос состояния воркеров.
export type PassHook = (now: Date) => Promise<void>;

export function startReconciler(
  store: Store, kube: Kube, cfg: Config, opts: { hooks: PassHook[] },
): { kick(): void; stop(): void; idle(): Promise<void> } {
  let running = false;
  let pending = false;
  let stopped = false;
  let idleWaiters: (() => void)[] = [];

  const pass = async () => {
    running = true;
    do {
      pending = false;
      const now = new Date();
      try {
        const r = await reconcileOnce(store, kube, cfg, now);
        if (r.applied || r.deleted) console.error(`reconcile: applied ${r.applied} deleted ${r.deleted}`);
        for (const h of opts.hooks) {
          try {
            await h(now);
          } catch (e) {
            console.error(`reconcile hook failed: ${(e as Error).message}`);
          }
        }
      } catch (e) {
        console.error(`reconcile: pass aborted: ${(e as Error).message}`);
      }
    } while (pending && !stopped);
    running = false;
    const w = idleWaiters;
    idleWaiters = [];
    w.forEach((f) => f());
  };

  const kick = () => {
    if (stopped) return;
    if (running) {
      pending = true;
      return;
    }
    void pass();
  };
  const timer = setInterval(kick, cfg.reconcileIntervalMs);
  timer.unref();
  return {
    kick,
    stop() {
      stopped = true;
      clearInterval(timer);
    },
    idle: () => (running ? new Promise<void>((r) => idleWaiters.push(r)) : Promise.resolve()),
  };
}
