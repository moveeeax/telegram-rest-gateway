// Идентификаторы аккаунтов, токены и коды приглашений. Все случайное берется из CSPRNG.
import { createHash, randomBytes } from "node:crypto";

const ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789";
// 252 = 36 * 7: байты 252..255 отбрасываются, иначе первые 4 символа выпадали бы чаще.
const UNBIASED_LIMIT = 252;
const ACCOUNT_ID = /^(a[a-z0-9]{10}|[0-9]{1,20})$/;

// Новый accountId: "a" плюс 10 символов [a-z0-9].
export function newAccountId(): string {
  let out = "a";
  while (out.length < 11) {
    for (const b of randomBytes(16)) {
      if (b < UNBIASED_LIMIT && out.length < 11) out += ALPHABET[b % 36];
    }
  }
  return out;
}

// Новый slug или числовой id двух существующих аккаунтов. Годится как сегмент S3 и DNS.
export function isAccountId(s: string): boolean {
  return ACCOUNT_ID.test(s);
}

// 32 байта CSPRNG в base64url без паддинга (43 символа).
export function newSecret(): string {
  return randomBytes(32).toString("base64url");
}

// Клиентский токен "<accountId>.<secret>": по префиксу Traefik выбирает воркер.
export function clientToken(accountId: string): string {
  if (!isAccountId(accountId)) throw new Error(`invalid accountId: ${accountId}`);
  return `${accountId}.${newSecret()}`;
}

export function accountIdFromToken(token: string): string | null {
  const dot = token.indexOf(".");
  if (dot <= 0) return null;
  const id = token.slice(0, dot);
  return isAccountId(id) ? id : null;
}

// Код приглашения или ссылки на токен: 24 байта base64url (32 символа).
export function newCode(): string {
  return randomBytes(24).toString("base64url");
}

export function sha256hex(s: string): string {
  return createHash("sha256").update(s).digest("hex");
}
