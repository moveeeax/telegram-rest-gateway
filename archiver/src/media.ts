/**
 * Оффлоад медиа в S3. При событии с file_id: качаем файл из гейтвея (GET /v1/files/{id},
 * файл валиден в рамках сессии гейтвея), кладём в S3 и пишем media_url в строку сообщения.
 * Работает выделенным воркером с очередью — не блокирует consumer/бэкфилл.
 *
 * Env:
 *   ARCHIVER_S3_ENDPOINT/_REGION/_BUCKET/_ACCESS_KEY_ID/_SECRET_ACCESS_KEY/_PREFIX/_PUBLIC_BASE
 *   ARCHIVER_S3_FORCE_PATH_STYLE — "true"/"false"; default: true (false → virtual-hosted AWS)
 *   ARCHIVER_GATEWAY_TEMPLATE — напр. http://tgw-{sessionId}:8080 (per-account сервис)
 *   ARCHIVER_GATEWAY_TOKEN    — Bearer (read) для скачивания файлов
 *   ARCHIVER_MEDIA_MAX_BYTES  — не грузить файлы больше (default 100 MiB)
 */
import { PutObjectCommand, S3Client } from "@aws-sdk/client-s3";
import type { Store } from "./store.js";

export type MediaJob = {
  session_id: string;
  chat_id: string;
  message_id: string;
  file_id: string;
  file_name?: string;
  mime_type?: string;
  /** Сколько раз process() уже завершался retriable-неудачей (0 = первая попытка). */
  attempts?: number;
  /** Не брать в работу раньше этого времени (epoch ms) — бэкофф между requeue. */
  notBefore?: number;
};

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const QUEUE_CAP = 50000;
/** Максимум вызовов process() на job (1 первая + requeue). */
const MAX_JOB_ATTEMPTS = 5;

type ProcessResult = "done" | "retry" | "skip";

/** Permanent: 401/403/404 и явные non-retriable. Не requeue'им, ключ остаётся в seen. */
class MediaError extends Error {
  constructor(
    message: string,
    readonly permanent: boolean,
  ) {
    super(message);
    this.name = "MediaError";
  }
}

function envBool(name: string): boolean | undefined {
  const v = process.env[name];
  if (v === undefined || v === "") return undefined;
  return v === "1" || v.toLowerCase() === "true";
}

function isPermanentError(e: unknown): boolean {
  return e instanceof MediaError && e.permanent;
}

export class MediaOffloader {
  private queue: MediaJob[] = [];
  private seen = new Set<string>();
  private running = true;
  private s3: S3Client;
  private bucket: string;
  private prefix: string;
  private publicBase: string;
  private gwTemplate: string;
  private gwToken: string;
  private maxBytes: number;
  uploaded = 0;
  failed = 0;
  lastError: string | null = null;

  static fromEnv(store: Store): MediaOffloader | null {
    const bucket = process.env.ARCHIVER_S3_BUCKET ?? "";
    const gwTemplate = process.env.ARCHIVER_GATEWAY_TEMPLATE ?? "";
    if (!bucket || !gwTemplate) return null; // медиа-оффлоад выключен
    const endpoint = process.env.ARCHIVER_S3_ENDPOINT ?? "";
    return new MediaOffloader(store, {
      endpoint,
      region: process.env.ARCHIVER_S3_REGION ?? "us-east-1",
      bucket,
      accessKeyId: process.env.ARCHIVER_S3_ACCESS_KEY_ID ?? "",
      secretAccessKey: process.env.ARCHIVER_S3_SECRET_ACCESS_KEY ?? "",
      prefix: (process.env.ARCHIVER_S3_PREFIX ?? "media/").replace(/^\/+/, ""),
      publicBase: (process.env.ARCHIVER_S3_PUBLIC_BASE ?? "").replace(/\/$/, ""),
      // path-style по умолчанию (исторический дефолт; virtual-hosted ломает бакеты
      // с точками в имени на чистом AWS) — virtual-hosted только явным опт-аутом
      forcePathStyle: envBool("ARCHIVER_S3_FORCE_PATH_STYLE") ?? true,
      gwTemplate,
      gwToken: process.env.ARCHIVER_GATEWAY_TOKEN ?? "",
      maxBytes: Number(process.env.ARCHIVER_MEDIA_MAX_BYTES ?? String(100 * 1024 * 1024)),
    });
  }

  constructor(private store: Store, cfg: any) {
    this.s3 = new S3Client({
      endpoint: cfg.endpoint || undefined,
      region: cfg.region,
      forcePathStyle: Boolean(cfg.forcePathStyle),
      credentials: { accessKeyId: cfg.accessKeyId, secretAccessKey: cfg.secretAccessKey },
    });
    this.bucket = cfg.bucket;
    this.prefix = cfg.prefix;
    this.publicBase = cfg.publicBase;
    this.gwTemplate = cfg.gwTemplate;
    this.gwToken = cfg.gwToken;
    this.maxBytes = cfg.maxBytes;
    void this.loop();
  }

  private jobKey(job: MediaJob): string {
    return `${job.session_id}:${job.chat_id}:${job.message_id}`;
  }

  enqueue(job: MediaJob): void {
    const key = this.jobKey(job);
    if (this.seen.has(key)) return; // уже в работе/обработано в этом процессе
    if (this.queue.length >= QUEUE_CAP) {
      console.error(`media: queue full (${QUEUE_CAP}), not enqueueing ${key}`);
      return; // не помечаем seen — попробуем при следующем событии
    }
    this.seen.add(key);
    this.queue.push(job);
  }

  stats() {
    return {
      pending: this.queue.length,
      uploaded: this.uploaded,
      failed: this.failed,
      last_error: this.lastError,
    };
  }

  stop() {
    this.running = false;
  }

  private urlFor(key: string): string {
    if (this.publicBase) return `${this.publicBase}/${key}`;
    // Путь-стайл к S3-объекту (доступ по правам бакета/пресайну).
    return `s3://${this.bucket}/${key}`;
  }

  /** In-queue requeue для 202 / 5xx / network / S3; после MAX_JOB_ATTEMPTS — give-up. */
  private requeueOrGiveUp(job: MediaJob, key: string, reason: string): void {
    const tries = (job.attempts ?? 0) + 1;
    if (tries < MAX_JOB_ATTEMPTS) {
      // растущая пауза между попытками: иначе на пустой очереди весь бюджет
      // сгорает за минуты, а TDLib может качать большой файл десятки минут
      this.queue.push({ ...job, attempts: tries, notBefore: Date.now() + 60_000 * tries });
      return;
    }
    this.failed++;
    this.lastError = `${key}: gave up after ${tries} attempts: ${reason}`;
    console.error(`media: gave up on ${key} after ${tries} attempts: ${reason}`);
    // снимаем seen — повтор возможен при следующем Kafka/backfill событии
    this.seen.delete(key);
  }

  private markPermanentFail(key: string, reason: string): void {
    this.failed++;
    this.lastError = `${key}: ${reason}`;
    console.error(`media: permanent fail ${key}: ${reason}`);
    // ключ остаётся в seen — не крутим 401/404 бесконечно
  }

  private async loop(): Promise<void> {
    while (this.running) {
      const job = this.queue.shift();
      if (!job) {
        await sleep(500);
        continue;
      }
      if (job.notBefore && job.notBefore > Date.now()) {
        // бэкофф ещё не истёк — в конец очереди (ротация не мешает due-джобам)
        this.queue.push(job);
        await sleep(500);
        continue;
      }
      const key = this.jobKey(job);
      try {
        const result = await this.process(job);
        if (result === "retry") {
          // 202 / ещё не докачался — requeue в конец очереди
          this.requeueOrGiveUp(job, key, "download still pending (202)");
        }
        // "done" и "skip" (oversized) оставляют ключ в seen
      } catch (e: any) {
        const msg = String(e?.message ?? e);
        if (isPermanentError(e)) {
          this.markPermanentFail(key, msg);
        } else {
          // 5xx / network / timeout / S3 — тот же in-queue requeue, что и для 202
          console.error(`media: retriable fail ${key}:`, e);
          this.requeueOrGiveUp(job, key, msg);
        }
      }
      await sleep(50); // мягкий троттлинг
    }
  }

  private async process(job: MediaJob): Promise<ProcessResult> {
    const base = this.gwTemplate.replace("{sessionId}", job.session_id).replace(/\/$/, "");
    // Скачиваем файл из гейтвея; 202 = ещё качается TDLib — несколько ретраев внутри process.
    let bytes: Buffer | null = null;
    let contentType = job.mime_type || "application/octet-stream";
    for (let attempt = 0; attempt < 6; attempt++) {
      let resp: Response;
      try {
        resp = await fetch(`${base}/v1/files/${encodeURIComponent(job.file_id)}`, {
          headers: this.gwToken ? { Authorization: `Bearer ${this.gwToken}` } : {},
          signal: AbortSignal.timeout(60000), // не морозим воркер на зависшем скачивании
        });
      } catch (e: any) {
        // network / timeout — retriable на уровне job (requeue)
        throw new MediaError(`download network: ${String(e?.message ?? e)}`, false);
      }
      // Только 202 = «ещё качается». JSON-ошибки (401/404/500) не маскируем под pending.
      if (resp.status === 202) {
        await sleep(2000 * (attempt + 1));
        continue;
      }
      if (!resp.ok) {
        const body = await resp.text().catch(() => "");
        const detail = body ? `: ${body.slice(0, 200)}` : "";
        const permanent = resp.status === 401 || resp.status === 403 || resp.status === 404;
        throw new MediaError(`download ${resp.status}${detail}`, permanent);
      }
      // 200 + application/json — легитимный файл (пользователь прислал .json):
      // ошибки гейтвея приходят с не-2xx статусом и отсечены выше.
      const ct = resp.headers.get("content-type") ?? "";
      const ab = await resp.arrayBuffer();
      if (ab.byteLength > this.maxBytes) {
        console.error(`media: skip oversized ${this.jobKey(job)} (${ab.byteLength} > ${this.maxBytes})`);
        return "skip";
      }
      bytes = Buffer.from(ab);
      if (ct) contentType = ct;
      break;
    }
    // так и не докачался (серия 202) — caller requeue'ит job в очередь
    if (!bytes) return "retry";

    const safeName = (job.file_name || "file").replace(/[^\w.\-]+/g, "_");
    const key = `${this.prefix}${job.session_id}/${job.chat_id}/${job.message_id}/${safeName}`;
    try {
      await this.s3.send(new PutObjectCommand({ Bucket: this.bucket, Key: key, Body: bytes, ContentType: contentType }));
    } catch (e: any) {
      throw new MediaError(`s3 put: ${String(e?.message ?? e)}`, false);
    }
    await this.store.setMediaUrl(job.session_id, job.chat_id, job.message_id, this.urlFor(key));
    this.uploaded++;
    return "done";
  }
}
