// Клиент auth-API воркера. Ходит только изнутри кластера, с admin-токеном аккаунта.
// Ответы гейтвея: {"ok":true,"data":...} или {"ok":false,"error":{"code","message"}}.
import type { Config } from "./config.js";
import { workerName } from "./manifests.js";

export type CodeInfo = { type: string; next_type: string; length: number; resend_timeout: number };
export type AuthState = { authorization_state: string; ready: boolean; qr_link?: string; code_info?: CodeInfo };
export type WorkerResult<T> = { ok: true; value: T } | { ok: false; status: number; error: string };
export type Me = { id: string; username?: string; first_name: string; last_name: string };

export function workerBaseUrl(accountId: string, cfg: Config): string {
  return `http://${workerName(accountId)}.${cfg.workerNamespace}.svc:8080`;
}

export class WorkerClient {
  constructor(private baseUrl: string, private adminToken: string, private timeoutMs = 10000) {}

  async health(): Promise<boolean> {
    try {
      const r = await fetch(`${this.baseUrl}/v1/health`, { signal: AbortSignal.timeout(this.timeoutMs) });
      return r.ok;
    } catch {
      return false;
    }
  }

  state(): Promise<WorkerResult<AuthState>> {
    return this.call("GET", "/v1/auth/state");
  }
  startQr(): Promise<WorkerResult<AuthState>> {
    return this.call("POST", "/v1/auth/qr");
  }
  phone(phone_number: string): Promise<WorkerResult<AuthState>> {
    return this.call("POST", "/v1/auth/phone", { phone_number });
  }
  code(code: string): Promise<WorkerResult<AuthState>> {
    return this.call("POST", "/v1/auth/code", { code });
  }
  password(password: string): Promise<WorkerResult<AuthState>> {
    return this.call("POST", "/v1/auth/password", { password });
  }
  me(): Promise<WorkerResult<Me>> {
    return this.call("GET", "/v1/me");
  }
  logout(): Promise<WorkerResult<{ logged_out: boolean }>> {
    // logout ждет Closed до 10 с на стороне гейтвея.
    return this.call("POST", "/v1/auth/logout", undefined, Math.max(this.timeoutMs, 15000));
  }

  private async call<T>(method: string, path: string, body?: unknown, timeoutMs = this.timeoutMs): Promise<WorkerResult<T>> {
    try {
      const r = await fetch(`${this.baseUrl}${path}`, {
        method,
        headers: {
          authorization: `Bearer ${this.adminToken}`,
          ...(body === undefined ? {} : { "content-type": "application/json" }),
        },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: AbortSignal.timeout(timeoutMs),
      });
      const json = (await r.json().catch(() => null)) as { ok?: boolean; data?: T; error?: { code?: string; message?: string } } | null;
      if (r.ok && json?.ok) return { ok: true, value: json.data as T };
      const err = json?.error ? `${json.error.code ?? "ERROR"}: ${json.error.message ?? ""}` : `http ${r.status}`;
      return { ok: false, status: r.status, error: err };
    } catch (e) {
      return { ok: false, status: 0, error: (e as Error).name === "TimeoutError" ? "timeout" : (e as Error).message };
    }
  }
}
