// Конфигурация control plane из переменных окружения CP_*.
export type S3Settings = {
  endpoint: string;
  region: string;
  bucket: string;
  accessKeyId: string;
  secretAccessKey: string;
  forcePathStyle: boolean;
};

export type Config = {
  publicPort: number;
  adminPort: number;
  pgUrl: string;
  adminToken: string;
  registryMcpToken: string;
  registryArchiverToken: string;
  workerNamespace: string;
  workerImage: string;
  sharedSecretName: string;
  workerConfigMap: string;
  kafkaBrokers: string;
  s3: S3Settings;
  joinBaseUrl: string;
  apiBaseUrl: string;
  // Пусто, пока нет единого MCP: страница онбординга тогда не показывает блок MCP.
  mcpBaseUrl: string;
  reconcileIntervalMs: number;
};

export function loadConfig(env: NodeJS.ProcessEnv): Config {
  const required = (name: string): string => {
    const v = env[name];
    if (v === undefined || v.trim() === "") throw new Error(`${name} is required`);
    return v;
  };
  const optional = (name: string, def: string): string => env[name] ?? def;
  const int = (name: string, def: number): number => {
    const raw = env[name];
    if (raw === undefined || raw === "") return def;
    const n = Number(raw);
    if (!Number.isInteger(n) || n <= 0) throw new Error(`${name} must be a positive integer`);
    return n;
  };
  return {
    pgUrl: required("CP_PG_URL"),
    adminToken: required("CP_ADMIN_TOKEN"),
    registryMcpToken: required("CP_REGISTRY_MCP_TOKEN"),
    registryArchiverToken: required("CP_REGISTRY_ARCHIVER_TOKEN"),
    workerImage: required("CP_WORKER_IMAGE"),
    publicPort: int("CP_PUBLIC_PORT", 8095),
    adminPort: int("CP_ADMIN_PORT", 8096),
    workerNamespace: optional("CP_WORKER_NAMESPACE", "tgw-accounts"),
    sharedSecretName: optional("CP_SHARED_SECRET", "telegram-rest-gateway"),
    workerConfigMap: optional("CP_WORKER_CONFIGMAP", "tgw-worker-config"),
    kafkaBrokers: optional("CP_KAFKA_BROKERS", ""),
    s3: {
      endpoint: required("CP_S3_ENDPOINT"),
      region: optional("CP_S3_REGION", "us-east-1"),
      bucket: required("CP_S3_BUCKET"),
      accessKeyId: required("CP_S3_ACCESS_KEY_ID"),
      secretAccessKey: required("CP_S3_SECRET_ACCESS_KEY"),
      forcePathStyle: optional("CP_S3_FORCE_PATH_STYLE", "true") !== "false",
    },
    joinBaseUrl: optional("CP_JOIN_BASE_URL", "https://join.tarassov.me"),
    apiBaseUrl: optional("CP_API_BASE_URL", "https://tg.tarassov.me"),
    mcpBaseUrl: optional("CP_MCP_BASE_URL", ""),
    reconcileIntervalMs: int("CP_RECONCILE_INTERVAL_MS", 30000),
  };
}
