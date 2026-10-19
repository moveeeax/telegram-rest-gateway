// Доступ к k8s: только namespace воркеров и только объекты с лейблом managed-by=control-plane.
import * as k8s from "@kubernetes/client-node";
import { ObjectSerializer } from "@kubernetes/client-node/dist/serializer.js";
import { getSerializationType } from "@kubernetes/client-node/dist/util.js";
import { LABEL_ACCOUNT, LABEL_MANAGED, MANAGED_VALUE, type KubeObject } from "./manifests.js";

export interface Kube {
  // Server-side apply, fieldManager "tgw-control-plane", force.
  apply(obj: KubeObject): Promise<void>;
  // 404 не ошибка.
  delete(kind: string, name: string): Promise<void>;
  listManaged(kind: string): Promise<KubeObject[]>;
  // Расшифрованные значения Secret, null если его нет.
  getSecret(name: string): Promise<Record<string, string> | null>;
  // Нет ни одного пода аккаунта (включая Terminating).
  podGone(accountId: string): Promise<boolean>;
}

// Secret в общий список не входит: в нем ключ шифрования БД TDLib, и reconcile его не удаляет
// никогда. Удаляет его только lifecycle, когда S3 уже зачищен (lifecycle.ts).
// KubernetesObjectApi.patch сериализует тело по модели типа (V1NetworkPolicy и т.п.), где часть
// полей названа иначе, чем в JSON k8s: `from` в модели это `_from`. Объект в JSON-форме при этом
// теряет такие поля молча (так NetworkPolicy ушла в кластер без from и пускала всех). Поэтому
// перед patch переводим объект в форму модели; сериализатор вернет ровно исходный JSON. Тип без
// модели (CRD, например IngressRoute) проходит как есть.
export function toWire<T extends KubeObject>(obj: T): T {
  return ObjectSerializer.deserialize(JSON.parse(JSON.stringify(obj)), getSerializationType(obj.apiVersion, obj.kind)) as T;
}

export const MANAGED_KINDS = ["StatefulSet", "Service", "NetworkPolicy", "IngressRoute"] as const;

const API_VERSION: Record<string, string> = {
  StatefulSet: "apps/v1",
  Service: "v1",
  NetworkPolicy: "networking.k8s.io/v1",
  IngressRoute: "traefik.io/v1alpha1",
  Secret: "v1",
  Pod: "v1",
};

const FIELD_MANAGER = "tgw-control-plane";
const MANAGED_SELECTOR = `${LABEL_MANAGED}=${MANAGED_VALUE}`;

function isNotFound(e: unknown): boolean {
  const code = (e as { code?: number; statusCode?: number }).code ?? (e as { statusCode?: number }).statusCode;
  return code === 404;
}

export class K8sKube implements Kube {
  private api: k8s.KubernetesObjectApi;

  constructor(private namespace: string, kc?: k8s.KubeConfig) {
    const config = kc ?? new k8s.KubeConfig();
    if (!kc) config.loadFromDefault();
    this.api = k8s.KubernetesObjectApi.makeApiClient(config);
  }

  async apply(obj: KubeObject): Promise<void> {
    if (obj.metadata.namespace !== this.namespace) throw new Error(`refusing to apply outside ${this.namespace}`);
    await this.api.patch(toWire(obj) as k8s.KubernetesObject, undefined, undefined, FIELD_MANAGER, true, k8s.PatchStrategy.ServerSideApply);
  }

  async delete(kind: string, name: string): Promise<void> {
    try {
      await this.api.delete({ apiVersion: API_VERSION[kind], kind, metadata: { name, namespace: this.namespace } });
    } catch (e) {
      if (!isNotFound(e)) throw e;
    }
  }

  async listManaged(kind: string): Promise<KubeObject[]> {
    const r = await this.api.list(API_VERSION[kind], kind, this.namespace, undefined, undefined, undefined, undefined, MANAGED_SELECTOR);
    return r.items as unknown as KubeObject[];
  }

  async getSecret(name: string): Promise<Record<string, string> | null> {
    try {
      const s = await this.api.read<k8s.V1Secret>({ apiVersion: "v1", kind: "Secret", metadata: { name, namespace: this.namespace } });
      const out: Record<string, string> = {};
      for (const [k, v] of Object.entries(s.data ?? {})) out[k] = Buffer.from(v, "base64").toString("utf8");
      return out;
    } catch (e) {
      if (isNotFound(e)) return null;
      throw e;
    }
  }

  async podGone(accountId: string): Promise<boolean> {
    const r = await this.api.list("v1", "Pod", this.namespace, undefined, undefined, undefined, undefined, `${LABEL_ACCOUNT}=${accountId}`);
    return r.items.length === 0;
  }
}
