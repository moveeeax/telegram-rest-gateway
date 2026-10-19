// HTTP-клиент гейтвея одного аккаунта. Токен сервисный (воркер аккаунта), клиентский токен
// MCP в гейтвей не передается.
export class GatewayClient {
  constructor(private baseUrl: string, private token: string) {
    this.baseUrl = baseUrl.replace(/\/$/, "");
  }

  async api(path: string, method: string = "GET", body?: unknown, headers: Record<string, string> = {}): Promise<any> {
    const resp = await fetch(`${this.baseUrl}${path}`, {
      method,
      headers: {
        Authorization: `Bearer ${this.token}`,
        ...(body !== undefined ? { "Content-Type": "application/json" } : {}),
        ...headers,
      },
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const json = await resp.json().catch(() => ({ ok: false, error: { message: `HTTP ${resp.status}` } }));
    if (!json.ok) {
      const err = json.error ?? {};
      throw new Error(`${err.code ?? resp.status}: ${err.message ?? "request failed"}`);
    }
    return json;
  }

  fetchFile(fileId: string, signal: AbortSignal): Promise<Response> {
    return fetch(`${this.baseUrl}/v1/files/${encodeURIComponent(fileId)}`, {
      headers: { Authorization: `Bearer ${this.token}` },
      signal,
    });
  }
}
