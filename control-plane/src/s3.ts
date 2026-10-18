// S3 (MinIO): meta.json рядом с бинлогом и зачистка префикса аккаунта при удалении.
import { DeleteObjectsCommand, ListObjectsV2Command, PutObjectCommand, S3Client } from "@aws-sdk/client-s3";
import type { S3Settings } from "./config.js";
import { isAccountId } from "./ids.js";

export interface S3Ops {
  putMeta(accountId: string, meta: object): Promise<void>;
  // Удаляет все объекты под префиксом, возвращает их число.
  deletePrefix(prefix: string): Promise<number>;
}

export const SESSION_PREFIX = "telegram-sessions/";

export function sessionPrefix(accountId: string): string {
  if (!isAccountId(accountId)) throw new Error(`invalid accountId: ${accountId}`);
  return `${SESSION_PREFIX}${accountId}/`;
}

export class S3Store implements S3Ops {
  private client: S3Client;

  constructor(private s: S3Settings) {
    this.client = new S3Client({
      endpoint: s.endpoint,
      region: s.region,
      forcePathStyle: s.forcePathStyle,
      credentials: { accessKeyId: s.accessKeyId, secretAccessKey: s.secretAccessKey },
    });
  }

  async putMeta(accountId: string, meta: object): Promise<void> {
    await this.client.send(new PutObjectCommand({
      Bucket: this.s.bucket, Key: `${sessionPrefix(accountId)}meta.json`,
      Body: JSON.stringify(meta), ContentType: "application/json",
    }));
  }

  async deletePrefix(prefix: string): Promise<number> {
    // Защита от удаления всего бакета: только префикс конкретного аккаунта.
    if (!/^telegram-sessions\/(a[a-z0-9]{10}|[0-9]{1,20})\/$/.test(prefix)) throw new Error(`refusing prefix ${prefix}`);
    let n = 0;
    let token: string | undefined;
    do {
      const page = await this.client.send(new ListObjectsV2Command({ Bucket: this.s.bucket, Prefix: prefix, ContinuationToken: token }));
      const keys = (page.Contents ?? []).map((o) => ({ Key: o.Key! }));
      if (keys.length) {
        await this.client.send(new DeleteObjectsCommand({ Bucket: this.s.bucket, Delete: { Objects: keys } }));
        n += keys.length;
      }
      token = page.IsTruncated ? page.NextContinuationToken : undefined;
    } while (token);
    return n;
  }
}
