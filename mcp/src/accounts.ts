// Набор аккаунтов, доступных предъявленному токену, и выбор аккаунта для вызова инструмента.
export type AccountRef = { accountId: string; gatewayUrl: string; serviceToken: string };
export type AccountSet = { tokenHash: string; accounts: AccountRef[] };

export const WRITE_TOOLS: ReadonlySet<string> = new Set([
  "telegram_send_message",
  "telegram_edit_message",
  "telegram_delete_messages",
  "telegram_forward_messages",
  "telegram_react",
  "telegram_mark_read",
]);

// Один аккаунт у токена: параметр account можно не указывать. Несколько: account обязателен для
// всех инструментов, дефолта нет, чтобы модель не прочитала и не написала не в тот аккаунт.
export function pickAccount(
  set: AccountSet, requested: string | undefined, _tool: string,
): { ok: true; account: AccountRef } | { ok: false; error: string } {
  if (requested) {
    const hit = set.accounts.find((a) => a.accountId === requested);
    return hit ? { ok: true, account: hit } : { ok: false, error: `account ${requested} is not available for this token` };
  }
  if (set.accounts.length === 1) return { ok: true, account: set.accounts[0] };
  return { ok: false, error: `account is required: one of ${set.accounts.map((a) => a.accountId).join(", ")}` };
}
