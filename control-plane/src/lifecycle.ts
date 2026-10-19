// Жизненный цикл аккаунта после онбординга: удаление по шагам и обнаружение отозванной сессии.
import type { Config } from "./config.js";
import type { Kube } from "./kube.js";
import { SECRET_KEYS, workerName } from "./manifests.js";
import { sessionPrefix, type S3Ops } from "./s3.js";
import type { Account, AccountStatus, Store } from "./store.js";
import { workerBaseUrl, type AuthState, type WorkerClient } from "./worker_client.js";

export type LifecycleDeps = {
  store: Store;
  kube: Kube;
  cfg: Config;
  s3: S3Ops;
  worker: (accountId: string, adminToken: string) => WorkerClient;
  kick: () => void;
};

// Сколько проходов ждать ответа воркера на logout, прежде чем удалять без него.
export const LOGOUT_WAIT_PASSES = 3;

// Шаги: (null | logout_wait:N) -> logged_out -> scaled -> pod_gone -> s3_deleted -> запись удалена.
// replicas: 0 ставит reconcile по статусу deleting. S3 чистится строго после остановки пода,
// иначе финальный push на shutdown вернул бы бинлог.
export async function advanceDeletion(a: Account, d: LifecycleDeps): Promise<void> {
  const id = a.accountId;
  let step = a.deleteStep;
  const set = async (s: string) => {
    step = s;
    await d.store.updateAccount(id, { deleteStep: s });
  };
  if (step === null || step.startsWith("logout_wait:")) {
    const waited = step ? Number(step.split(":")[1]) : 0;
    const secrets = await d.kube.getSecret(workerName(id));
    const admin = secrets?.[SECRET_KEYS.admin];
    let done = !admin;
    if (admin) {
      const w = d.worker(id, admin);
      if (await w.health()) {
        const r = await w.logout();
        // 409: сессии нет (онбординг не дошел до ready), выходить не из чего.
        done = r.ok || r.status === 409;
      }
      if (!done && waited + 1 >= LOGOUT_WAIT_PASSES) {
        console.error(`lifecycle: ${id}: worker did not log out after ${LOGOUT_WAIT_PASSES} passes, deleting anyway`);
        done = true;
      }
    }
    if (!done) return set(`logout_wait:${waited + 1}`);
    await set("logged_out");
  }
  if (step === "logged_out") {
    d.kick();
    await set("scaled");
  }
  if (step === "scaled") {
    if (!(await d.kube.podGone(id))) return;
    await set("pod_gone");
  }
  if (step === "pod_gone") {
    await d.s3.deletePrefix(sessionPrefix(id));
    await set("s3_deleted");
  }
  if (step === "s3_deleted") {
    // Purge архива добавит план архиватора (шаг archive_purged).
    await d.store.deleteAccount(id);
    d.kick();
  }
}

// Состояния TDLib, при которых активный аккаунт потерял сессию.
const LOST = new Set(["wait_phone_number", "closed", "logging_out"]);

export function detectRelogin(a: Account, state: AuthState | null): AccountStatus {
  if (a.status !== "active" || state === null) return a.status;
  return LOST.has(state.authorization_state) ? "needs_relogin" : "active";
}

// Проход по активным аккаунтам: спросить состояние воркера и пометить потерянные сессии.
export async function pollActive(d: LifecycleDeps): Promise<void> {
  for (const a of await d.store.listAccounts()) {
    if (a.status !== "active") continue;
    const admin = (await d.kube.getSecret(workerName(a.accountId)))?.[SECRET_KEYS.admin];
    if (!admin) continue;
    const w = d.worker(a.accountId, admin);
    const r = (await w.health()) ? await w.state() : null;
    const next = detectRelogin(a, r && r.ok ? r.value : null);
    if (next !== a.status) {
      console.error(`lifecycle: ${a.accountId}: ${a.status} -> ${next}`);
      await d.store.updateAccount(a.accountId, { status: next });
      d.kick();
    }
  }
}

export async function advanceAllDeletions(d: LifecycleDeps): Promise<void> {
  for (const a of await d.store.listAccounts()) {
    if (a.status !== "deleting") continue;
    try {
      await advanceDeletion(a, d);
    } catch (e) {
      console.error(`lifecycle: delete ${a.accountId}: ${(e as Error).message}`);
    }
  }
}

export { workerBaseUrl };
