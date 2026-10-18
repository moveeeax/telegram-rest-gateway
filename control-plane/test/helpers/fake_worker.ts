import http from "node:http";
import type { AddressInfo } from "node:net";

// Заглушка гейтвея: программируемое состояние авторизации и журнал запросов.
export class FakeWorker {
  server: http.Server;
  requests: { method: string; path: string; auth: string; body: any }[] = [];
  healthy = true;
  state: any = { authorization_state: "wait_phone_number", ready: false };
  me: any = { id: "777", first_name: "T", last_name: "", username: "tester" };
  failNext: { status: number; body: any } | null = null;
  delayMs = 0;
  onQr: () => void = () => {
    this.state = { authorization_state: "wait_other_device_confirmation", ready: false, qr_link: "tg://login?token=AAA" };
  };

  constructor() {
    this.server = http.createServer((req, res) => {
      let raw = "";
      req.on("data", (c) => (raw += c));
      req.on("end", () => setTimeout(() => this.handle(req, res, raw), this.delayMs));
    });
  }

  private handle(req: http.IncomingMessage, res: http.ServerResponse, raw: string) {
    const path = req.url ?? "";
    const body = raw ? JSON.parse(raw) : null;
    this.requests.push({ method: req.method ?? "", path, auth: String(req.headers.authorization ?? ""), body });
    const send = (status: number, obj: any) => {
      res.writeHead(status, { "content-type": "application/json" });
      res.end(JSON.stringify(obj));
    };
    if (path === "/v1/health") return this.healthy ? send(200, { ok: true, status: "alive" }) : send(503, {});
    if (this.failNext) {
      const f = this.failNext;
      this.failNext = null;
      return send(f.status, f.body);
    }
    if (path === "/v1/auth/state") return send(200, { ok: true, data: this.state });
    if (path === "/v1/auth/qr") {
      this.onQr();
      return send(200, { ok: true, data: this.state });
    }
    if (path === "/v1/auth/phone") {
      this.state = { authorization_state: "wait_code", ready: false, code_info: { type: "sms", next_type: "", length: 5, resend_timeout: 60 } };
      return send(200, { ok: true, data: this.state });
    }
    if (path === "/v1/auth/code") return send(200, { ok: true, data: this.state });
    if (path === "/v1/auth/password") return send(200, { ok: true, data: this.state });
    if (path === "/v1/me") return send(200, { ok: true, data: this.me });
    if (path === "/v1/auth/logout") {
      this.state = { authorization_state: "closed", ready: false };
      return send(200, { ok: true, data: { logged_out: true } });
    }
    send(404, { ok: false, error: { code: "NOT_FOUND", message: "no route" } });
  }

  async start(): Promise<string> {
    await new Promise<void>((r) => this.server.listen(0, "127.0.0.1", r));
    return `http://127.0.0.1:${(this.server.address() as AddressInfo).port}`;
  }
  async stop() {
    await new Promise<void>((r) => this.server.close(() => r()));
  }
}
