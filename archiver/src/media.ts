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
/**
 * Верхняя граница размера `seen` (LRU на Map, вытеснение самого старого ключа). Дедуп —
 * лишь оптимизация (повторный S3-put идемпотентен), поэтому неограниченный рост Set'а ради
 * него не оправдан: за недели аптайма он набирал сотни MB против лимита пода 256Mi (5.6).
 * 50–100k ключей ("session:chat:message" — десятки байт) — единицы MB, с запасом на активный
 * рабочий набор чатов между перезапусками.
 */
const SEEN_MAX_SIZE = 100_000;

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

/** Маркер "файл больше maxBytes" — отличаем от прочих сбоев чтения потока. */
class DownloadTooLargeError extends Error {}

// Читаем тело ответа потоково, не давая ему разрастись сверх лимита: Buffer.from(await
// resp.arrayBuffer()) буферизует файл целиком ещё до проверки размера — большое видео валит
// воркер по OOM (ARCHIVER_MEDIA_MAX_BYTES тут не спасает, т.к. проверяется постфактум).
async function readLimited(
  body: ReadableStream<Uint8Array> | null,
  limit: number,
  controller: AbortController,
): Promise<Buffer> {
  if (!body) return Buffer.alloc(0);
  const reader = body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > limit) {
        // Обрываем закачку немедленно, не дочитывая остаток — именно это и защищает от OOM.
        controller.abort();
        throw new DownloadTooLargeError();
      }
      chunks.push(value);
    }
  } finally {
    reader.releaseLock();
  }
  return Buffer.concat(chunks);
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
  // LRU на базе Map: порядок ключей — порядок вставки, "тронуть" ключ = delete+set (двигает его
  // в конец, most-recently-used), переполнение — удаляем самый старый (первый) ключ. См. SEEN_MAX_SIZE.
  private seen = new Map<string, true>();
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
    if (this.seen.has(key)) {
      this.touchSeen(key); // повторная ссылка — двигаем в MRU, защищаем от вытеснения
      return; // уже в работе/обработано в этом процессе
    }
    if (this.queue.length >= QUEUE_CAP) {
      console.error(`media: queue full (${QUEUE_CAP}), not enqueueing ${key}`);
      return; // не помечаем seen — попробуем при следующем событии
    }
    this.markSeen(key);
    this.queue.push(job);
  }

  /** Помечает ключ виденным, вытесняя старейший при переполнении (LRU, см. SEEN_MAX_SIZE). */
  private markSeen(key: string): void {
    if (this.seen.size >= SEEN_MAX_SIZE) {
      const oldest = this.seen.keys().next().value;
      if (oldest !== undefined) this.seen.delete(oldest);
    }
    this.seen.set(key, true);
  }

  /** Сдвигает существующий ключ в конец (most-recently-used) порядка Map. */
  private touchSeen(key: string): void {
    this.seen.delete(key);
    this.seen.set(key, true);
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
    const dedupKey = this.jobKey(job);
    // Скачиваем файл из гейтвея; 202 = ещё качается TDLib — несколько ретраев внутри process.
    let bytes: Buffer | null = null;
    let contentType = job.mime_type || "application/octet-stream";
    for (let attempt = 0; attempt < 6; attempt++) {
      let resp: Response;
      // Свой AbortController: им readLimited ниже обрывает закачку немедленно при превышении
      // maxBytes посреди потока — совмещаем с таймаутом запроса через AbortSignal.any.
      const controller = new AbortController();
      try {
        resp = await fetch(`${base}/v1/files/${encodeURIComponent(job.file_id)}`, {
          headers: this.gwToken ? { Authorization: `Bearer ${this.gwToken}` } : {},
          signal: AbortSignal.any([AbortSignal.timeout(60000), controller.signal]), // не морозим воркер на зависшем скачивании
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
      // Content-Length известен заранее — отказываемся, не читая тело вообще: раньше
      // resp.arrayBuffer() буферизовал файл целиком ещё до проверки размера, и 1.5 GB видео
      // валило воркер по OOM (после рестарта job снова в очереди — crash-loop). Превышение —
      // это "skip" (как и раньше для oversized), а не ошибка: не крашим и не ретраим (5.4).
      const declaredLength = Number(resp.headers.get("content-length"));
      if (Number.isFinite(declaredLength) && declaredLength > this.maxBytes) {
        await resp.body?.cancel().catch(() => {});
        console.error(`media: skip oversized ${dedupKey} (content-length ${declaredLength} > ${this.maxBytes})`);
        return "skip";
      }
      // Content-Length может отсутствовать или занижать фактический размер — читаем потоково
      // и обрываем закачку сразу по достижении лимита, не давая буферу разрастись.
      let ab: Buffer;
      try {
        ab = await readLimited(resp.body, this.maxBytes, controller);
      } catch (e) {
        if (e instanceof DownloadTooLargeError) {
          console.error(`media: skip oversized ${dedupKey} (> ${this.maxBytes})`);
          return "skip";
        }
        throw new MediaError(`download stream: ${String((e as any)?.message ?? e)}`, false);
      }
      bytes = ab;
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
