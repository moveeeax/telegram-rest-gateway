// Публичный сервер: только пути онбординга. Admin API и реестр живут на другом порту.
import { randomBytes } from "node:crypto";
import express from "express";
import { COOKIE_NAME, type Onboarding } from "./onboarding.js";
import { invitePage, onboardPage, tokenRevealPage } from "./pages.js";

export type TokenReveal = (code: string) => Promise<{ token: string; apiUrl: string } | { status: number; message: string }>;

function readCookie(header: string | undefined): string | null {
  for (const part of (header ?? "").split(";")) {
    const [k, ...v] = part.trim().split("=");
    if (k === COOKIE_NAME && v.length) return v.join("=");
  }
  return null;
}

export function publicApp(onb: Onboarding, revealToken?: TokenReveal): express.Express {
  const app = express();
  app.disable("x-powered-by");
  app.set("trust proxy", false);
  app.use((_req, res, next) => {
    res.set({
      "cache-control": "no-store",
      "x-frame-options": "DENY",
      "x-content-type-options": "nosniff",
      "referrer-policy": "no-referrer",
    });
    next();
  });

  const html = (res: express.Response, status: number, body: string, csp: string) =>
    res.status(status).set("content-security-policy", csp).type("html").send(body);

  const wrap = (fn: (req: express.Request, res: express.Response) => Promise<void>) =>
    (req: express.Request, res: express.Response) => {
      fn(req, res).catch((e) => {
        console.error(`public: ${req.method} ${req.path}: ${(e as Error).message}`);
        if (!res.headersSent) res.status(500).json({ phase: "error", error: "Внутренняя ошибка, попробуйте еще раз." });
      });
    };

  app.get("/healthz", (_req, res) => {
    res.json({ ok: true });
  });

  const scripted = (res: express.Response, page: (nonce: string) => string) => {
    const nonce = randomBytes(16).toString("base64");
    const csp = `default-src 'none'; script-src 'nonce-${nonce}'; style-src 'unsafe-inline'; img-src data:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`;
    html(res, 200, page(nonce), csp);
  };

  // Только JSON: обычная HTML-форма с чужого сайта сюда не отправит (плюс SameSite=Strict).
  const json = express.json({ limit: "4kb" });
  const jsonOnly = (req: express.Request, res: express.Response): boolean => {
    if (req.is("application/json")) return true;
    res.status(415).json({ phase: "error", error: "unsupported media type" });
    return false;
  };

  // GET ничего не занимает (превью ссылок в мессенджерах), занимает POST с этой же страницы.
  app.get("/i/:code", (_req, res) => scripted(res, invitePage));
  app.post("/i/:code", json, wrap(async (req, res) => {
    if (!jsonOnly(req, res)) return;
    const r = await onb.open(String(req.params.code), readCookie(req.headers.cookie));
    if (r.kind === "page") return void res.status(r.status).json({ error: r.message });
    res.set("set-cookie", r.setCookie).json({ redirect: "/onboard" });
  }));

  app.get("/onboard", (_req, res) => scripted(res, onboardPage));

  app.get("/onboard/state", wrap(async (req, res) => {
    const c = readCookie(req.headers.cookie);
    res.json(c ? await onb.view(c) : { phase: "expired" });
  }));

  const submit = (fn: (c: string, v: string) => ReturnType<Onboarding["view"]>) =>
    wrap(async (req, res) => {
      if (!jsonOnly(req, res)) return;
      const c = readCookie(req.headers.cookie);
      const value = typeof req.body?.value === "string" ? req.body.value.trim() : "";
      if (!c) return void res.json({ phase: "expired" });
      if (!value || value.length > 256) return void res.status(400).json({ ...(await onb.view(c)), error: "Пустое значение." });
      res.json(await fn(c, value));
    });
  app.post("/onboard/phone", json, submit((c, v) => onb.submitPhone(c, v)));
  app.post("/onboard/code", json, submit((c, v) => onb.submitCode(c, v)));
  app.post("/onboard/password", json, submit((c, v) => onb.submitPassword(c, v)));

  app.get("/t/:code", (_req, res) => scripted(res, tokenRevealPage));
  app.post("/t/:code", json, wrap(async (req, res) => {
    if (!jsonOnly(req, res)) return;
    if (!revealToken) return void res.status(404).json({ error: "Ссылка недействительна или истекла." });
    const r = await revealToken(String(req.params.code));
    if ("token" in r) return void res.json({ token: r.token, api_url: r.apiUrl });
    res.status(r.status).json({ error: r.message });
  }));

  app.use((_req, res) => {
    res.status(404).type("text").send("not found");
  });
  return app;
}
