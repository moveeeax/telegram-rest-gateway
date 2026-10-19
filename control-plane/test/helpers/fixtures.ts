import type { Account } from "../../src/store.js";
import type { Config } from "../../src/config.js";

export function account(id: string, patch: Partial<Account> = {}): Account {
  return {
    accountId: id, status: "onboarding", telegramUserId: null, username: null,
    archiveEnabled: false, exportEnabled: false, clientTokenHash: "h-" + id, invitedBy: "owner",
    createdAt: new Date("2026-10-18T10:00:00Z"), activatedAt: null, tokenShownAt: null,
    deleteStep: null, secretRev: 1, ...patch,
  };
}

export function config(patch: Partial<Config> = {}): Config {
  return {
    publicPort: 8095, adminPort: 8096, pgUrl: "postgres://x", adminToken: "cp-admin",
    registryMcpToken: "reg-mcp", registryArchiverToken: "reg-arch", workerNamespace: "tgw-accounts",
    workerImage: "ghcr.io/moveeeax/telegram-rest-gateway:abc1234", sharedSecretName: "telegram-rest-gateway",
    workerConfigMap: "tgw-worker-config", kafkaBrokers: "kafka.kafka.svc:9092",
    s3: { endpoint: "http://minio", region: "us-east-1", bucket: "tgw-s3-bucket", accessKeyId: "k", secretAccessKey: "s", forcePathStyle: true },
    joinBaseUrl: "https://join.tarassov.me", apiBaseUrl: "https://tg.tarassov.me", mcpBaseUrl: "",
    reconcileIntervalMs: 30000, archiverAdminUrl: "", archiverAdminToken: "", ...patch,
  };
}
