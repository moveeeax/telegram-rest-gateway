/**
 * Оффлоад медиа в S3. При событии с file_id: качаем файл из гейтвея (GET /v1/files/{id},
 * файл валиден в рамках сессии гейтвея), кладём в S3 и пишем media_url в строку сообщения.
 * Работает выделенным воркером с очередью — не блокирует consumer/бэкфилл.
 *
 * Env:
 *   ARCHIVER_S3_ENDPOINT/_REGION/_BUCKET/_ACCESS_KEY_ID/_SECRET_ACCESS_KEY/_PREFIX/_PUBLIC_BASE
 *   ARCHIVER_GATEWAY_TEMPLATE — напр. http://tgw-{sessionId}:8080 (per-account сервис)
 *   ARCHIVER_GATEWAY_TOKEN    — Bearer (read) для скачивания файлов
 *   ARCHIVER_MEDIA_MAX_BYTES  — не грузить файлы больше (default 100 MiB)
 */
import { PutObjectCommand, S3Client } from "@aws-sdk/client-s3";
import type { Store } from "./store.js";

export type MediaJob = { session_id: string; chat_id: string; message_id: string; file_id: string; file_name?: string; mime_type?: string };

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

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

  static fromEnv(store: Store): MediaOffloader | null {
    const bucket = process.env.ARCHIVER_S3_BUCKET ?? "";
    const gwTemplate = process.env.ARCHIVER_GATEWAY_TEMPLATE ?? "";
    if (!bucket || !gwTemplate) return null; // медиа-оффлоад выключен
    return new MediaOffloader(store, {
      endpoint: process.env.ARCHIVER_S3_ENDPOINT ?? "",
      region: process.env.ARCHIVER_S3_REGION ?? "us-east-1",
      bucket,
      accessKeyId: process.env.ARCHIVER_S3_ACCESS_KEY_ID ?? "",
      secretAccessKey: process.env.ARCHIVER_S3_SECRET_ACCESS_KEY ?? "",
      prefix: (process.env.ARCHIVER_S3_PREFIX ?? "media/").replace(/^\/+/, ""),
      publicBase: (process.env.ARCHIVER_S3_PUBLIC_BASE ?? "").replace(/\/$/, ""),
      gwTemplate,
      gwToken: process.env.ARCHIVER_GATEWAY_TOKEN ?? "",
      maxBytes: Number(process.env.ARCHIVER_MEDIA_MAX_BYTES ?? String(100 * 1024 * 1024)),
    });
  }

  constructor(private store: Store, cfg: any) {
    this.s3 = new S3Client({
      endpoint: cfg.endpoint || undefined,
      region: cfg.region,
      forcePathStyle: true, // MinIO
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

  enqueue(job: MediaJob): void {
    const key = `${job.session_id}:${job.chat_id}:${job.message_id}`;
    if (this.seen.has(key)) return; // уже в работе/обработано в этом процессе
    this.seen.add(key);
    if (this.queue.length < 50000) this.queue.push(job);
  }

  stats() {
    return { pending: this.queue.length, uploaded: this.uploaded, failed: this.failed };
  }

  stop() {
    this.running = false;
  }

  private urlFor(key: string): string {
    if (this.publicBase) return `${this.publicBase}/${key}`;
    // Путь-стайл к S3-объекту (доступ по правам бакета/пресайну).
    return `s3://${this.bucket}/${key}`;
  }

  private async loop(): Promise<void> {
    while (this.running) {
      const job = this.queue.shift();
      if (!job) {
        await sleep(500);
        continue;
      }
      try {
        await this.process(job);
      } catch (e) {
        this.failed++;
      }
      await sleep(50); // мягкий троттлинг
    }
  }

  private async process(job: MediaJob): Promise<void> {
    const base = this.gwTemplate.replace("{sessionId}", job.session_id).replace(/\/$/, "");
    // Скачиваем файл из гейтвея; 202 = ещё качается TDLib — несколько ретраев.
    let bytes: Buffer | null = null;
    let contentType = job.mime_type || "application/octet-stream";
    for (let attempt = 0; attempt < 6; attempt++) {
      const resp = await fetch(`${base}/v1/files/${encodeURIComponent(job.file_id)}`, {
        headers: this.gwToken ? { Authorization: `Bearer ${this.gwToken}` } : {},
        signal: AbortSignal.timeout(60000), // не морозим воркер на зависшем скачивании
      });
      const ct = resp.headers.get("content-type") ?? "";
      if (resp.status === 202 || ct.includes("application/json")) {
        await sleep(2000 * (attempt + 1)); // ждём докачки
        continue;
      }
      if (!resp.ok) throw new Error(`download ${resp.status}`);
      const ab = await resp.arrayBuffer();
      if (ab.byteLength > this.maxBytes) return; // слишком большой — пропускаем
      bytes = Buffer.from(ab);
      if (ct) contentType = ct;
      break;
    }
    if (!bytes) return; // так и не докачался — оставим file_id, попробуем при следующем событии

    const safeName = (job.file_name || "file").replace(/[^\w.\-]+/g, "_");
    const key = `${this.prefix}${job.session_id}/${job.chat_id}/${job.message_id}/${safeName}`;
    await this.s3.send(new PutObjectCommand({ Bucket: this.bucket, Key: key, Body: bytes, ContentType: contentType }));
    await this.store.setMediaUrl(job.session_id, job.chat_id, job.message_id, this.urlFor(key));
    this.uploaded++;
  }
}
