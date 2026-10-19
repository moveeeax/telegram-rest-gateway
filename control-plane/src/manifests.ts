// Объекты k8s воркера аккаунта. Чистые функции: reconcile применяет их server-side apply.
// Шаблон пода повторяет deploy/helm/telegram-rest-gateway/templates/deployment.yaml.
import type { Config } from "./config.js";
import { isAccountId } from "./ids.js";
import type { Account } from "./store.js";

export const LABEL_ACCOUNT = "tgw.tarassov.me/account-id";
export const LABEL_MANAGED = "tgw.tarassov.me/managed-by";
export const MANAGED_VALUE = "control-plane";

export type KubeObject = {
  apiVersion: string;
  kind: string;
  metadata: { name: string; namespace: string; labels: Record<string, string>; annotations?: Record<string, string> };
  [k: string]: unknown;
};

export type WorkerSecrets = {
  clientToken: string;
  adminToken: string;
  mcpServiceToken: string;
  archiverServiceToken: string;
  databaseEncryptionKey: string;
};

// Ключи Secret аккаунта. Токены в открытом виде нужны гейтвею (он сам хэширует BEARER_TOKENS)
// и control plane (admin-токен для auth-вызовов, клиентский для показа один раз).
export const SECRET_KEYS = {
  bearerTokens: "BEARER_TOKENS",
  dbKey: "DATABASE_ENCRYPTION_KEY",
  client: "CLIENT_TOKEN",
  admin: "ADMIN_TOKEN",
  mcp: "MCP_SERVICE_TOKEN",
  archiver: "ARCHIVER_SERVICE_TOKEN",
} as const;

// Публичные префиксы REST на общем хосте. /v1/auth, /v1/webhooks, /ui, /metrics сюда не входят.
const PUBLIC_PATHS = "(chats|me|users|contacts|resolve|search|files|updates|health)";
export const API_HOST_DEFAULT = "tg.tarassov.me";

// Шаги удаления (lifecycle.ts), на которых воркер уже вышел из Telegram и должен быть остановлен.
export const SCALED_DOWN_STEPS = new Set(["logged_out", "scaled", "pod_gone", "s3_deleted"]);

export function workerName(accountId: string): string {
  if (!isAccountId(accountId)) throw new Error(`invalid accountId: ${accountId}`);
  return `tgw-${accountId}`;
}

function meta(accountId: string, cfg: Config) {
  return {
    name: workerName(accountId),
    namespace: cfg.workerNamespace,
    labels: { [LABEL_ACCOUNT]: accountId, [LABEL_MANAGED]: MANAGED_VALUE },
  };
}

// Строки BEARER_TOKENS: клиент read,write; admin без скоупа (полный); сервисные урезаны.
export function bearerTokensValue(s: WorkerSecrets): string {
  return [`${s.clientToken} read,write`, s.adminToken, `${s.mcpServiceToken} read,write`, `${s.archiverServiceToken} read`].join("\n");
}

export function secretManifest(accountId: string, s: WorkerSecrets, secretRev: number, cfg: Config): KubeObject {
  const m = meta(accountId, cfg);
  return {
    apiVersion: "v1",
    kind: "Secret",
    metadata: { ...m, annotations: { "tgw.tarassov.me/secret-rev": String(secretRev) } },
    type: "Opaque",
    stringData: {
      [SECRET_KEYS.bearerTokens]: bearerTokensValue(s),
      [SECRET_KEYS.dbKey]: s.databaseEncryptionKey,
      [SECRET_KEYS.client]: s.clientToken,
      [SECRET_KEYS.admin]: s.adminToken,
      [SECRET_KEYS.mcp]: s.mcpServiceToken,
      [SECRET_KEYS.archiver]: s.archiverServiceToken,
    },
  };
}

function apiHost(cfg: Config): string {
  try {
    return new URL(cfg.apiBaseUrl).host;
  } catch {
    return API_HOST_DEFAULT;
  }
}

function statefulSet(a: Account, cfg: Config): KubeObject {
  const m = meta(a.accountId, cfg);
  const fromSecret = (key: string) => ({ valueFrom: { secretKeyRef: { name: m.name, key } } });
  return {
    apiVersion: "apps/v1",
    kind: "StatefulSet",
    metadata: m,
    spec: {
      // Одна сессия Telegram = один под. StatefulSet не создает замену, пока старый под не удален.
      // При удалении под нужен живым до logout: гасим только после шага logged_out.
      replicas: a.status === "deleting" && SCALED_DOWN_STEPS.has(a.deleteStep ?? "") ? 0 : 1,
      serviceName: m.name,
      selector: { matchLabels: { [LABEL_ACCOUNT]: a.accountId } },
      template: {
        metadata: {
          labels: { ...m.labels },
          annotations: { "tgw.tarassov.me/secret-rev": String(a.secretRev) },
        },
        spec: {
          terminationGracePeriodSeconds: 120,
          automountServiceAccountToken: false,
          securityContext: { fsGroup: 65532 },
          containers: [
            {
              name: "gateway",
              image: cfg.workerImage,
              imagePullPolicy: "IfNotPresent",
              securityContext: {
                runAsNonRoot: true, runAsUser: 65532, runAsGroup: 65532, readOnlyRootFilesystem: false,
                allowPrivilegeEscalation: false, capabilities: { drop: ["ALL"] },
              },
              ports: [{ name: "http", containerPort: 8080 }],
              envFrom: [{ configMapRef: { name: cfg.workerConfigMap } }, { secretRef: { name: cfg.sharedSecretName } }],
              // env перекрывает envFrom: токены и ключ БД берутся из Secret аккаунта, не из общего.
              env: [
                { name: "TGW_SESSION_ID", value: a.accountId },
                { name: "BEARER_TOKENS", ...fromSecret(SECRET_KEYS.bearerTokens) },
                { name: "DATABASE_ENCRYPTION_KEY", ...fromSecret(SECRET_KEYS.dbKey) },
                { name: "TGW_SESSION_EXPORT_ENABLED", value: a.exportEnabled ? "true" : "false" },
                // Пустой список брокеров выключает Kafka: без archive_enabled события в архив не идут.
                { name: "TGW_KAFKA_BROKERS", value: a.archiveEnabled ? cfg.kafkaBrokers : "" },
              ],
              // Readiness на /health: неавторизованный воркер должен отвечать на auth-вызовы онбординга.
              readinessProbe: { httpGet: { path: "/v1/health", port: "http" }, initialDelaySeconds: 3, periodSeconds: 10 },
              livenessProbe: { httpGet: { path: "/v1/health", port: "http" }, initialDelaySeconds: 10, periodSeconds: 15, failureThreshold: 3 },
              resources: { requests: { cpu: "50m", memory: "64Mi" }, limits: { cpu: "500m", memory: "256Mi" } },
              volumeMounts: [{ name: "data", mountPath: "/data" }],
            },
          ],
          // Эфемерно: сессия восстанавливается из S3 на старте и пушится туда же.
          volumes: [{ name: "data", emptyDir: {} }],
        },
      },
    },
  };
}

function service(a: Account, cfg: Config): KubeObject {
  return {
    apiVersion: "v1",
    kind: "Service",
    metadata: meta(a.accountId, cfg),
    spec: {
      selector: { [LABEL_ACCOUNT]: a.accountId },
      ports: [{ name: "http", port: 8080, targetPort: "http" }],
    },
  };
}

function networkPolicy(a: Account, cfg: Config): KubeObject {
  const ns = (name: string) => ({ "kubernetes.io/metadata.name": name });
  return {
    apiVersion: "networking.k8s.io/v1",
    kind: "NetworkPolicy",
    metadata: meta(a.accountId, cfg),
    spec: {
      podSelector: { matchLabels: { [LABEL_ACCOUNT]: a.accountId } },
      policyTypes: ["Ingress"],
      ingress: [
        {
          from: [
            { namespaceSelector: { matchLabels: ns("kube-system") }, podSelector: { matchLabels: { "app.kubernetes.io/name": "traefik" } } },
            // control plane, MCP, архиватор
            { namespaceSelector: { matchLabels: ns("tgw") } },
          ],
          ports: [{ port: 8080, protocol: "TCP" }],
        },
      ],
    },
  };
}

function ingressRoute(a: Account, cfg: Config): KubeObject {
  const id = a.accountId;
  const match =
    `Host(\`${apiHost(cfg)}\`) && ` +
    `(HeaderRegexp(\`Authorization\`, \`(?i)^bearer ${id}\\.\`) || HeaderRegexp(\`Sec-WebSocket-Protocol\`, \`(?i)^bearer\\.${id}\\.\`)) && ` +
    `PathRegexp(\`(?i)^/v1/${PUBLIC_PATHS}(/|$)\`)`;
  return {
    apiVersion: "traefik.io/v1alpha1",
    kind: "IngressRoute",
    metadata: meta(id, cfg),
    spec: {
      entryPoints: ["websecure"],
      routes: [{ kind: "Rule", match, services: [{ name: workerName(id), port: 8080 }] }],
      // Сертификат берется из пула Traefik по SNI (его держит Ingress tgw-edge в namespace tgw).
      tls: {},
    },
  };
}

// Желаемые объекты аккаунта без Secret: Secret создается в онбординге и при перевыпуске токена.
export function desiredObjects(a: Account, cfg: Config): KubeObject[] {
  workerName(a.accountId);
  const out = [statefulSet(a, cfg), service(a, cfg), networkPolicy(a, cfg)];
  if (a.status === "active") out.push(ingressRoute(a, cfg));
  return out;
}
