// Публичный сервер: только пути онбординга. Admin API и реестр живут на другом порту.
import { randomBytes } from "node:crypto";
import express from "express";
import { COOKIE_NAME, type Onboarding } from "./onboarding.js";
import { messagePage, onboardPage, tokenPage } from "./pages.js";

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
  const STATIC_CSP = "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

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

  app.get("/i/:code", wrap(async (req, res) => {
    const r = await onb.open(req.params.code, readCookie(req.headers.cookie));
    if (r.kind === "page") return void html(res, r.status, messagePage(r.message), STATIC_CSP);
    res.set("set-cookie", r.setCookie).redirect(303, "/onboard");
  }));

  app.get("/onboard", (_req, res) => {
    const nonce = randomBytes(16).toString("base64");
    const csp = `default-src 'none'; script-src 'nonce-${nonce}'; style-src 'unsafe-inline'; img-src data:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`;
    html(res, 200, onboardPage(nonce), csp);
  });

  app.get("/onboard/state", wrap(async (req, res) => {
    const c = readCookie(req.headers.cookie);
    res.json(c ? await onb.view(c) : { phase: "expired" });
  }));

  // Только JSON: обычная HTML-форма с чужого сайта сюда не отправит (плюс SameSite=Strict).
  const json = express.json({ limit: "4kb" });
  const submit = (fn: (c: string, v: string) => ReturnType<Onboarding["view"]>) =>
    wrap(async (req, res) => {
      if (!req.is("application/json")) return void res.status(415).json({ phase: "error", error: "unsupported media type" });
      const c = readCookie(req.headers.cookie);
      const value = typeof req.body?.value === "string" ? req.body.value.trim() : "";
      if (!c) return void res.json({ phase: "expired" });
      if (!value || value.length > 256) return void res.status(400).json({ ...(await onb.view(c)), error: "Пустое значение." });
      res.json(await fn(c, value));
    });
  app.post("/onboard/phone", json, submit((c, v) => onb.submitPhone(c, v)));
  app.post("/onboard/code", json, submit((c, v) => onb.submitCode(c, v)));
  app.post("/onboard/password", json, submit((c, v) => onb.submitPassword(c, v)));

  app.get("/t/:code", wrap(async (req, res) => {
    if (!revealToken) return void html(res, 404, messagePage("Ссылка недействительна или истекла."), STATIC_CSP);
    const r = await revealToken(req.params.code);
    if ("token" in r) return void html(res, 200, tokenPage(r.token, r.apiUrl), STATIC_CSP);
    html(res, r.status, messagePage(r.message), STATIC_CSP);
  }));

  app.use((_req, res) => {
    res.status(404).type("text").send("not found");
  });
  return app;
}
