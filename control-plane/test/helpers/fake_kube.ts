import type { Kube } from "../../src/kube.js";
import { LABEL_ACCOUNT, LABEL_MANAGED, type KubeObject } from "../../src/manifests.js";

// k8s в памяти: объекты по kind/name, журнал вызовов, программируемые поды.
export class FakeKube implements Kube {
  objects = new Map<string, KubeObject>();
  applied: string[] = [];
  deleted: string[] = [];
  pods = new Set<string>(); // accountId живых подов
  podGoneAfter = new Map<string, number>(); // accountId -> сколько раз ответить "под еще жив"

  key(kind: string, name: string) {
    return `${kind}/${name}`;
  }
  async apply(obj: KubeObject): Promise<void> {
    const k = this.key(obj.kind, obj.metadata.name);
    this.applied.push(k);
    this.objects.set(k, structuredClone(obj));
  }
  async delete(kind: string, name: string): Promise<void> {
    const k = this.key(kind, name);
    this.deleted.push(k);
    this.objects.delete(k);
  }
  async listManaged(kind: string): Promise<KubeObject[]> {
    return [...this.objects.values()].filter((o) => o.kind === kind && o.metadata.labels?.[LABEL_MANAGED] === "control-plane");
  }
  async getSecret(name: string): Promise<Record<string, string> | null> {
    const s = this.objects.get(this.key("Secret", name)) as any;
    return s ? { ...s.stringData } : null;
  }
  async podGone(accountId: string): Promise<boolean> {
    const left = this.podGoneAfter.get(accountId) ?? 0;
    if (left > 0) {
      this.podGoneAfter.set(accountId, left - 1);
      return false;
    }
    return !this.pods.has(accountId);
  }
  // Объект, созданный не control plane (например Deployment из чарта tgw).
  putForeign(kind: string, name: string, accountId: string) {
    this.objects.set(this.key(kind, name), {
      apiVersion: "v1", kind, metadata: { name, namespace: "tgw-accounts", labels: { [LABEL_ACCOUNT]: accountId } },
    });
  }
}
