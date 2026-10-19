// Точка входа control plane: база, k8s, reconcile, публичный и admin-серверы.
import { adminApp, tokenReveal } from "./admin_server.js";
import { loadConfig } from "./config.js";
import { K8sKube } from "./kube.js";
import { advanceAllDeletions, pollActive } from "./lifecycle.js";
import { Onboarding } from "./onboarding.js";
import { publicApp } from "./public_server.js";
import { startReconciler } from "./reconcile.js";
import { S3Store } from "./s3.js";
import { PgStore } from "./store_pg.js";
import { WorkerClient, workerBaseUrl } from "./worker_client.js";

async function main() {
  const cfg = loadConfig(process.env);
  const store = await PgStore.connect(cfg.pgUrl);
  const kube = new K8sKube(cfg.workerNamespace);
  const s3 = new S3Store(cfg.s3);
  const worker = (accountId: string, adminToken: string) => new WorkerClient(workerBaseUrl(accountId, cfg), adminToken);

  // kick нужен онбордингу и admin API до создания reconciler: связываем через ссылку.
  let kick = () => {};
  const deps = { store, kube, cfg, s3, worker, kick: () => kick() };
  const onb = new Onboarding(deps);
  const rec = startReconciler(store, kube, cfg, {
    hooks: [(now) => onb.expire(now), () => advanceAllDeletions(deps), () => pollActive(deps)],
  });
  kick = rec.kick;
  rec.kick();

  const pub = publicApp(onb, tokenReveal(deps)).listen(cfg.publicPort, () => console.error(`control-plane: public on :${cfg.publicPort}`));
  const adm = adminApp(deps).listen(cfg.adminPort, () => console.error(`control-plane: admin on :${cfg.adminPort}`));

  const shutdown = (sig: string) => {
    console.error(`control-plane: ${sig}, shutting down`);
    rec.stop();
    pub.close();
    adm.close();
    void rec.idle().then(() => store.close()).finally(() => process.exit(0));
  };
  process.on("SIGTERM", () => shutdown("SIGTERM"));
  process.on("SIGINT", () => shutdown("SIGINT"));
}

main().catch((e) => {
  console.error(`control-plane: fatal: ${(e as Error).message}`);
  process.exit(1);
});
