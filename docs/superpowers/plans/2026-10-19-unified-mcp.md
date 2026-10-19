# Единый MCP на mcp.tarassov.me. План реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** один MCP-процесс на всех: токен клиента определяет аккаунт, владелец держит один коннектор на все свои аккаунты.

**Architecture:** MCP в многоаккаунтном режиме резолвит предъявленный токен через реестр control plane (`GET /internal/registry`, кэш 30 с) в набор аккаунтов `{accountId, gatewayUrl, serviceToken}`. Сессия MCP привязывается к хэшу токена первого авторизованного запроса, инструменты получают контекст аккаунта из сессии, а не из модульных констант. Control plane выдает владельцу токен на несколько аккаунтов (`owner.<secret>`), только для MCP.

**Tech Stack:** TypeScript, Node 22, `@modelcontextprotocol/sdk` 1.29, express 4, zod, тесты `node:test` через `tsx`.

**Spec:** `docs/superpowers/specs/2026-10-18-multi-account-single-endpoint-design.md`, раздел «MCP» (пункты 1–9) и абзац про владельческий токен в разделе «Токены».

## Что входит и что нет

Входит: многоаккаунтный режим MCP, владельческий токен в control plane, новая форма реестра, Deployment единого MCP и правило `mcp.tarassov.me` в edge-Ingress, блок MCP на странице онбординга.

Не входит: удаление per-account подов `mcp-<id>`. Они живут, пока владелец не перенесет коннекторы claude.ai на `mcp.tarassov.me`, удаляются отдельным шагом выкатки с его подтверждения. Однопользовательский режим (stdio и `TGW_BASE_URL`+`TGW_BEARER_TOKEN`+`MCP_HTTP_TOKEN`) сохраняется без изменений поведения.

## Global Constraints

1. Токен из `Authorization: Bearer` или `X-API-Key`, как сейчас. Неизвестный токен 401.
2. `initialize`, `notifications/initialized`, `ping`, `tools/list` без токена пропускаются (проба claude.ai без пользовательских заголовков).
3. Кэш реестра 30 с. Отозванный или перевыпущенный токен перестает работать не позже чем через 30 с.
4. Параметр `account` у всех инструментов: необязателен и игнорируется при одном аккаунте у токена, обязателен при нескольких. Пишущие инструменты (`send_message`, `edit_message`, `delete_messages`, `forward_messages`, `react`, `mark_read`) при наборе больше одного аккаунта без `account` возвращают ошибку, дефолта нет. В ответе всегда `account`.
5. Новый инструмент `telegram_list_accounts`.
6. `mcp-session-id` привязан к хэшу токена первого авторизованного запроса. Запрос с другим токеном в этой сессии получает 404. Неизвестный session id получает 404 (сейчас 400).
7. `telegram_search_history` всегда передает в архиватор `session_id` аккаунта из контекста, модели этот параметр недоступен.
8. Проверка `Origin`: запрос с заголовком `Origin` вне `MCP_ALLOWED_ORIGINS` получает 403. Без заголовка `Origin` запрос проходит (серверные клиенты его не шлют). В SDK 1.29 опции `allowedOrigins` у транспорта нет, проверка своя, middleware express.
9. Владельческий токен `owner.<secret>` (32 байта CSPRNG, base64url), в базе только SHA-256. Через Traefik он никуда не маршрутизируется: префикс `owner` не является accountId.
10. MCP ходит в гейтвей сервисным токеном воркера, клиентский токен в гейтвей не передается (запрет token passthrough).
11. В репозиторий не попадают реальные accountId и Telegram id: в тестах и примерах только вымышленные.
12. Сборка только в GitHub CI. Изменения кластера только с подтверждения владельца.

## Review Focus

1. Токен с несколькими аккаунтами и пишущий инструмент без `account`: ошибка, ничего не отправлено ни в один аккаунт. Тест в Task 2.
2. `account`, которого нет в наборе токена, даже если такой аккаунт существует: ошибка, без подстановки. Тест в Task 2.
3. Реестр недоступен: уже резолвленные токены работают на последнем удачном снимке до 5 минут, неизвестный токен получает 503, а не 401 (клиент не должен решить, что ключ неверный). Тест в Task 3.
4. Сессия открыта токеном А, запрос с токеном Б или без токена на `tools/call`: 404 и 401 соответственно, контекст А не используется. Тест в Task 4.
5. Перевыпуск клиентского токена: старый перестает резолвиться после истечения кэша. Тест в Task 3.

---

### Task 1: control plane. Владельческий токен и новая форма реестра

**Files:**
- Modify: `control-plane/src/store.ts`, `store_mem.ts`, `store_pg.ts` (таблица `owner_tokens`)
- Modify: `control-plane/src/admin_server.ts` (эндпоинты, реестр)
- Test: `control-plane/test/store.test.ts`, `control-plane/test/admin_server.test.ts`

**Interfaces:**
- Produces: `type OwnerToken = { tokenHash: string; label: string; accountIds: string[]; createdAt: Date }`; в `Store`: `insertOwnerToken(t: OwnerToken)`, `listOwnerTokens(): Promise<OwnerToken[]>`, `deleteOwnerToken(label: string): Promise<boolean>`. Таблица `owner_tokens(token_hash text primary key, label text unique not null, account_ids text[] not null, created_at timestamptz not null)`.
- Admin API: `POST /admin/owner-tokens {label, account_ids}` дает 201 `{label, token}` (токен один раз), `GET /admin/owner-tokens` дает `{tokens:[{label, account_ids, created_at}]}` без хэшей, `DELETE /admin/owner-tokens/:label` дает 204 или 404. `account_ids` непустой, каждый существует, иначе 400; повтор `label` 409.
- Реестр для MCP, новая форма: `{tokens: [{sha256, accounts: [{accountId, gatewayUrl, serviceToken}]}]}`. Клиентский токен аккаунта дает запись с одним аккаунтом; владельческий токен дает запись с аккаунтами из его набора, у которых `status = 'active'` (неактивные выпадают, токен с пустым набором в реестр не попадает). Форма для архиватора не меняется.

- [ ] **Step 1: тесты.** Store: вставка, список, удаление по label, повтор label бросает ошибку. Admin: создание дает токен `^owner\.[A-Za-z0-9_-]{43}$` и хэш в store; несуществующий аккаунт в `account_ids` дает 400; пустой список 400; без admin-токена 401. Реестр MCP: два активных аккаунта и владельческий токен на оба дают три записи, у владельческой `accounts.length === 2`; аккаунт в `needs_relogin` пропадает из владельческой записи; serviceToken взят из Secret воркера (`MCP_SERVICE_TOKEN`). Существующий тест реестра переписать на новую форму.
- [ ] **Step 2:** `npm --prefix control-plane test`. Expected: FAIL по новым тестам.
- [ ] **Step 3:** реализовать.
- [ ] **Step 4:** `npm --prefix control-plane test`. Expected: PASS.
- [ ] **Step 5:** коммит `feat(control-plane): owner tokens for MCP and account sets in the registry`.

### Task 2: MCP. Контекст аккаунта и инструменты

**Files:**
- Create: `mcp/src/gateway.ts`, `mcp/src/accounts.ts`, `mcp/src/tools.ts`
- Modify: `mcp/src/index.ts` (регистрация инструментов уезжает в `tools.ts`)
- Test: `mcp/test/accounts.test.ts`, `mcp/test/tools.test.ts`

**Interfaces:**
- Produces:
```ts
// gateway.ts
class GatewayClient {
  constructor(baseUrl: string, token: string);
  api(path: string, method?: string, body?: unknown, headers?: Record<string, string>): Promise<any>; // как текущий api()
  fetchFile(fileId: string, signal: AbortSignal): Promise<Response>;
}
// accounts.ts
type AccountRef = { accountId: string; gatewayUrl: string; serviceToken: string };
type AccountSet = { tokenHash: string; accounts: AccountRef[] };
const WRITE_TOOLS: ReadonlySet<string>; // 6 имен из Global Constraints 4
function pickAccount(set: AccountSet, requested: string | undefined, tool: string): { ok: true; account: AccountRef } | { ok: false; error: string };
// tools.ts
type ToolContext = { accounts: () => AccountSet | null; archiverUrl: string; archiverToken: string };
function buildServer(ctx: ToolContext): McpServer;
```
`pickAccount`: один аккаунт и `requested` пуст или равен ему, ok; один аккаунт и `requested` другой, ошибка; несколько и `requested` в наборе, ok; несколько и `requested` пуст: для чтения ошибка с перечнем допустимых значений, для пишущих та же ошибка (дефолта нет ни для кого: спецификация требует `account` при нескольких аккаунтах). Тексты ошибок: `account is required: one of <ids>`, `account <id> is not available for this token`.

Каждый инструмент получает необязательный zod-параметр `account: z.string().optional().describe(...)`, в результате JSON с полем `account`. `telegram_list_accounts` без параметров возвращает `[{account, id, username, first_name}]`, вызывая `/v1/me` каждого аккаунта набора (ошибка одного аккаунта дает у него поле `error`, остальные возвращаются). `telegram_search_history` строит параметры через `buildSearchParams(account.accountId, …)`.

Однопользовательский режим: `ToolContext.accounts` возвращает набор из одного аккаунта, собранный из `TGW_BASE_URL`, `TGW_BEARER_TOKEN`, `TGW_SESSION_ID`.

- [ ] **Step 1: тесты.** `accounts.test.ts`: все ветки `pickAccount`, включая аккаунт вне набора при одном аккаунте (Review Focus 2) и пишущий инструмент без `account` при двух (Review Focus 1). `tools.test.ts` на заглушке гейтвея (`http.createServer`, два «воркера» на разных портах, каждый отвечает своим `/v1/me`): `telegram_get_me` с `account=b` уходит во второй воркер с его serviceToken; `telegram_send_message` без `account` при двух аккаунтах возвращает ошибку и ни один воркер не получил POST; `telegram_list_accounts` возвращает оба; `telegram_search_history` шлет в архиватор `session_id` выбранного аккаунта. Вызов инструментов через `Client` и `InMemoryTransport` из SDK.
- [ ] **Step 2:** `npm --prefix mcp test`. Expected: FAIL.
- [ ] **Step 3:** реализовать, перенести тела инструментов из `index.ts` без изменения их поведения.
- [ ] **Step 4:** `npm --prefix mcp test` и `npm --prefix mcp run build`. Expected: PASS, tsc без ошибок.
- [ ] **Step 5:** коммит `feat(mcp): per-request account context and account parameter on tools`.

### Task 3: MCP. Клиент реестра

**Files:**
- Create: `mcp/src/registry.ts`
- Test: `mcp/test/registry.test.ts`

**Interfaces:**
- Produces: `class RegistryClient { constructor(url: string, token: string, opts?: { ttlMs?: number; staleMs?: number; now?: () => number }); resolve(token: string): Promise<AccountSet | null | "unavailable"> }`. `ttlMs` 30 000, `staleMs` 300 000. Поиск по SHA-256 hex токена. Параллельные `resolve` во время обновления ждут одного запроса к реестру.

- [ ] **Step 1: тесты** на заглушке реестра: известный токен дает набор; неизвестный `null`; в пределах ttl второй `resolve` не ходит в реестр; после ttl токен, исчезнувший из реестра, дает `null` (Review Focus 5); реестр отвечает 500 после удачного снимка: известный токен резолвится до `staleMs`, неизвестный дает `"unavailable"` (Review Focus 3); после `staleMs` известный тоже `"unavailable"`; пять параллельных `resolve` дают один запрос к реестру.
- [ ] **Step 2:** `npm --prefix mcp test`. Expected: FAIL.
- [ ] **Step 3:** реализовать.
- [ ] **Step 4:** `npm --prefix mcp test`. Expected: PASS.
- [ ] **Step 5:** коммит `feat(mcp): control plane registry client with cache`.

### Task 4: MCP. HTTP-слой многоаккаунтного режима

**Files:**
- Create: `mcp/src/http.ts` (express-приложение обоих режимов)
- Modify: `mcp/src/index.ts` (выбор режима, запуск)
- Test: `mcp/test/http.test.ts`

**Interfaces:**
- Consumes: `buildServer`, `RegistryClient`, `AccountSet`.
- Produces: `function mcpApp(opts: { resolve: (token: string) => Promise<AccountSet | null | "unavailable">; allowedOrigins: string[]; archiverUrl: string; archiverToken: string; sessionTtlMs?: number }): express.Express`. Многоаккаунтный режим включает `MCP_REGISTRY_URL` + `MCP_REGISTRY_TOKEN`; однопользовательский режим строит `resolve` из `MCP_HTTP_TOKEN` и env гейтвея. `MCP_ALLOWED_ORIGINS` через запятую, по умолчанию `https://claude.ai,https://claude.com`.

Поведение: рукопожатие без токена проходит и создает сессию без привязки; первый запрос с токеном, который резолвится, привязывает сессию к его хэшу и набору; запрос с другим токеном в привязанной сессии дает 404 с JSON-RPC ошибкой `session belongs to another token`; `tools/call` без токена дает 401; `"unavailable"` дает 503; неизвестный `mcp-session-id` на POST, GET и DELETE дает 404. Набор аккаунтов сессии обновляется из `resolve` на каждом запросе, чтобы отзыв доходил до открытых сессий.

- [ ] **Step 1: тесты** (supertest не нужен, `app.listen(0)` и `fetch`): рукопожатие без токена 200 и `mcp-session-id`; `tools/call` без токена 401; с токеном А 200, затем с токеном Б в той же сессии 404 (Review Focus 4); неизвестный session id 404; `resolve` вернул `"unavailable"` дает 503; `Origin: https://evil.example` дает 403, `Origin: https://claude.ai` проходит, без `Origin` проходит; токен в `X-API-Key` работает так же, как Bearer; после отзыва токена (заглушка `resolve` начинает отдавать `null`) следующий вызов в открытой сессии дает 401.
- [ ] **Step 2:** `npm --prefix mcp test`. Expected: FAIL.
- [ ] **Step 3:** реализовать. Сессия хранит `{transport, boundHash, set}`; `ToolContext.accounts` читает `set` своей сессии. Реапер сессий как сейчас.
- [ ] **Step 4:** `npm --prefix mcp test`, `npm --prefix mcp run build`, CI зеленый. Expected: PASS и success.
- [ ] **Step 5:** коммит `feat(mcp): multi-account streamable HTTP mode with session binding`.

### Task 5: Helm

**Files:**
- Modify: `deploy/helm/telegram-rest-gateway-tools/templates/mcp.yaml` (Deployment и Service `tgw-tools-mcp` для `mcp.shared.enabled`)
- Modify: `deploy/helm/telegram-rest-gateway-tools/templates/edge.yaml` (правило хоста `edge.hosts.mcp` на Service единого MCP при `mcp.shared.enabled`)
- Modify: `deploy/helm/telegram-rest-gateway-tools/values.yaml`, `values-prod-example.yaml`
- Modify: `deploy/helm/tests/render_test.sh`

**Interfaces:**
- Values: `mcp.shared.enabled` (false), `mcp.shared.registryUrl` (по умолчанию `http://<release>-control-plane:8096/internal/registry`), `mcp.shared.registryTokenSecret` (`{name: tgw-control-plane, key: CP_REGISTRY_MCP_TOKEN}`), `mcp.shared.allowedOrigins` (`https://claude.ai,https://claude.com`). Образ тот же, что у per-account MCP. Env архиватора как у per-account подов. `controlPlane.mcpBaseUrl` в prod-примере `https://mcp.tarassov.me`.

- [ ] **Step 1: render-тесты:** `mcp shared: deployment` (есть `name: tt-mcp`, env `MCP_REGISTRY_URL` и `MCP_REGISTRY_TOKEN` через secretKeyRef, нет `TGW_BEARER_TOKEN`); `mcp shared: edge rule` (в Ingress `tt-edge` хост `mcp.tarassov.me` ведет на `tt-mcp`); `mcp shared: disabled` (без флага нет `name: tt-mcp\n` и правила); per-account поды рендерятся как раньше.
- [ ] **Step 2:** `bash deploy/helm/tests/render_test.sh`. Expected: FAIL по новым кейсам.
- [ ] **Step 3:** реализовать.
- [ ] **Step 4:** `bash deploy/helm/tests/render_test.sh`. Expected: все PASS.
- [ ] **Step 5:** коммит `feat(helm): shared MCP behind mcp.tarassov.me`.

### Task 6: выкатка (только с подтверждения владельца)

- [ ] **Step 1:** образы из CI на `main`; `helm upgrade tgw-tools` с образами control plane и MCP нового тега, `mcp.shared.enabled=true`, `controlPlane.mcpBaseUrl=https://mcp.tarassov.me`. Per-account поды `mcp-<id>` остаются.
- [ ] **Step 2:** проверки без коннектора: рукопожатие на `https://mcp.tarassov.me/mcp` без ключа 200; `tools/call` без ключа 401; клиентский токен тестового аккаунта видит только его (`telegram_list_accounts` из одного аккаунта, `get_me` его id); `account` чужого аккаунта дает ошибку; `search_history` возвращает строки только этого аккаунта.
- [ ] **Step 3:** владельческий токен на три аккаунта владельца через `POST /admin/owner-tokens`. `telegram_list_accounts` дает три; `send_message` без `account` дает ошибку; `get_me` с каждым `account` возвращает свой аккаунт.
- [ ] **Step 4:** владелец добавляет коннектор в claude.ai: URL `https://mcp.tarassov.me/mcp`, заголовок `X-API-Key` с владельческим токеном, и подтверждает, что инструменты работают. Если claude.ai шлет `Origin` вне списка, расширить `MCP_ALLOWED_ORIGINS` по факту из логов.
- [ ] **Step 5:** после подтверждения владельца удалить per-account MCP (`mcp.accounts=[]`), их Ingress и сертификаты, ключи `GATEWAY_TOKEN_*` из Secret `tgw-tools`. Обновить `deploy/DEPLOY.md` и `docs/postman/README.md` (раздел MCP). Коммит `docs: shared MCP`.
