# Control plane: онбординг по инвайту и автозапуск воркеров. План реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** владелец создает инвайт, клиент открывает ссылку и сканирует QR, а control plane сам поднимает в k8s воркер этого клиента, логинит его и выдает токен для `tg.tarassov.me`.

**Architecture:** новый сервис `control-plane/` (TypeScript, Node 22) с Postgres как источником истины. Раз в 30 секунд и после каждого изменения он сверяет таблицу `accounts` с объектами в namespace `tgw-accounts`: Secret, StatefulSet, Service, NetworkPolicy, IngressRoute. Страницы онбординга ходят к воркеру по его admin-токену, браузер видит только control plane. В гейтвее два небольших изменения: флаг экспорта сессии и `POST /v1/auth/logout`.

**Tech Stack:** TypeScript 5, Node 22, express 4 (как в `mcp/`), `pg` (как в `archiver/`), `@kubernetes/client-node` 1.x, `@aws-sdk/client-s3` 3.x, `qrcode` для SVG, тесты `node:test` через `tsx --test`. Гейтвей: C++ (Drogon, TDLib), GoogleTest, сборка только в CI.

**Spec:** `docs/superpowers/specs/2026-10-18-multi-account-single-endpoint-design.md`

## Что входит и что нет

Входит: разделы спецификации «Идентификатор аккаунта», «Токены» (кроме владельческого MCP-токена), «Маршрутизация» (только `tg` и `join`), «Воркер аккаунта», «Онбординг нового клиента», «Control plane», пункты 2 и 3 «Изменений в гейтвее».

Не входит, идет отдельными планами:
1. Единый MCP на `mcp.tarassov.me` и его потребление реестра. Здесь реестр только отдается (`GET /internal/registry`), хост `mcp` лишь попадает в сертификат.
2. Архиватор под новую модель (purge, фильтр по `archive_enabled`, токены из реестра). До него архив тенантов выключается тем, что воркеру с `archive_enabled=false` передается пустой `TGW_KAFKA_BROKERS` (пустой список брокеров выключает Kafka, `src/config/config.cpp:232`).
3. Пункт 4 «Изменений в гейтвее» (`revoked_at`, `/v1/ready` 503 без выхода). Здесь `needs_relogin` определяется опросом `/v1/auth/state`.
4. Миграция двух существующих аккаунтов. Их Deployment из чарта `tgw` control plane не трогает: у них нет лейбла `tgw.tarassov.me/managed-by=control-plane`, и живут они в другом namespace.

## Отступления от спецификации (решения плана)

1. Воркеры живут в отдельном namespace `tgw-accounts`, Role control plane выдана только там. В `tgw` остаются сам control plane, edge-Ingress и Secret сертификата. Спецификация разрешает такой вариант для Secret сертификата («отдельный namespace»). Пункт 7 раздела «Control plane» говорит про Role в `tgw`, но тогда Secret сертификата был бы доступен control plane, и это противоречит той же спецификации. Цена ошибки: при миграции два старых аккаунта переезжают между namespace, их Secret надо переносить.
2. Хосты `tg`, `join`, `mcp` и сертификат заводит обычный Ingress `tgw-edge` с аннотацией cert-manager, а не объект Certificate. В кластере external-dns читает только `--source=service` и `--source=ingress` (проверено 2026-10-18, аргументы Deployment `external-dns`), IngressRoute он не видит, и DNS-записи без Ingress не появятся. Для `tg` в Ingress одно правило `Exact /__tgw-edge` на control plane, на все остальные пути без совпадающего IngressRoute Traefik сам отвечает 404.
3. Префикс `<accountId>.` есть только у клиентского токена. Админский и сервисные токены без префикса, поэтому Traefik не маршрутизирует их на общий хост ни при какой утечке.
4. После `POST /v1/auth/logout` процесс гейтвея не выходит: S3-синк отключен, процесс ждет SIGTERM. Спецификация просит «выйти с кодом 0», но StatefulSet перезапустил бы под до того, как control plane успеет поставить `replicas: 0`. Новый под восстановил бы из S3 бинлог, снятый до logout.
5. Rate limit на `join` глобальный: Traefik видит у всех клиентов адрес балансировщика (проверено в фазе 0). Плюс собственный лимит control plane: 5 попыток кода и 5 попыток пароля на сессию онбординга.
6. База `tgw_control` в CNPG `db/postgresql` с владельцем, существующей ролью `tgw` (той же, что у `tgw_archive`). В базе только хэши, новая роль заметно изоляции не добавляет.
7. Admin API наружу не выставляется. Владелец ходит к нему через `kubectl -n tgw port-forward svc/tgw-control-plane 8095:8095` с токеном `CP_ADMIN_TOKEN`. CLI-обертки нет.

## Global Constraints

1. `accountId` новых аккаунтов: `a` плюс 10 символов `[a-z0-9]`, CSPRNG.
2. Клиентский токен: `<accountId>.<secret>`, `secret` это 32 байта CSPRNG в base64url без паддинга. Админский и сервисные токены: тот же `secret` без префикса.
3. Инвайт одноразовый, живет 24 часа. Сессия онбординга живет 15 минут. Одновременно не больше трех онбордингов. Ссылка перевыпуска токена `join.tarassov.me/t/<code>` живет 24 часа.
4. Reconcile раз в 30 секунд и после каждого изменения в `accounts`.
5. Воркер: StatefulSet `replicas: 1` (0 для статуса `deleting`), requests 50m/64Mi, limits 500m/256Mi, лейблы `tgw.tarassov.me/account-id=<accountId>` и `tgw.tarassov.me/managed-by=control-plane`.
6. `TGW_SESSION_EXPORT_ENABLED=false` у всех воркеров, пока владелец не включит `export_enabled` в `accounts`. `archive_enabled` по умолчанию `false`.
7. В Postgres только хэши токенов и кодов (SHA-256 hex). Токены в открытом виде и `DATABASE_ENCRYPTION_KEY` живут только в Secret аккаунта.
8. На общем хосте пропускаются только `/v1/chats`, `/v1/me`, `/v1/users`, `/v1/contacts`, `/v1/resolve`, `/v1/search`, `/v1/files`, `/v1/updates`, `/v1/health` без учета регистра.
9. Страница онбординга не получает ни админский токен, ни session string. Cookie сессии онбординга: `tgw_onb`, `HttpOnly; Secure; SameSite=Strict; Path=/`.
10. Тексты страниц для клиента по-русски, копия из спецификации дословно: «запускаем…», «Telegram → Настройки → Устройства → Подключить устройство», «этот аккаунт уже подключен», «ссылка уже используется», «попробуйте через несколько минут».
11. Сборка C++ и docker только в GitHub CI, на Mac владельца не собирать (колима остановлена). TS-тесты без Postgres можно гонять локально.
12. Коммиты без AI-трейлеров (`Co-Authored-By`, `Generated with`).
13. Изменения в кластере только с явного разрешения владельца (Task 10).

## Review Focus

1. Reconcile, у которого не получилось прочитать базу, не удаляет ничего. Иначе одна сетевая ошибка снесет Secret, и с ним ключ шифрования всех тенантов. Тест в Task 5.
2. Повторное открытие `/i/<code>` в той же вкладке или после перезагрузки возвращает ту же сессию, а не второй аккаунт. Другой браузер получает «ссылка уже используется». Тест в Task 7.
3. Воркер не стал здоровым за 15 минут (медленный pull образа): ресурсы удаляются, `claimed_at` у инвайта сбрасывается, ссылка снова работает до истечения 24 часов. Тест в Task 7.
4. TDLib ротирует QR-ссылку: следующий опрос состояния отдает новый SVG. Тест в Task 7.
5. Control plane перезапустился посреди онбординга: состояние берется из базы, onboarding-объекты моложе 15 минут reconcile не трогает. Тест в Task 5.

---

### Task 1: гейтвей. Флаг экспорта сессии и logout

**Files:**
- Modify: `src/config/config.hpp`, `src/config/config.cpp` (поле `session_export_enabled`)
- Modify: `src/http/route_table.hpp` (новый `kAuthLogoutRoute{"/v1/auth/logout", drogon::Post, true}`, размер `kRoutesTable` 15)
- Modify: `src/http/routes.cpp` (регистрация экспорта под флагом, обработчик logout)
- Modify: `src/auth/s3_session.hpp`, `src/auth/s3_session.cpp` (`S3SyncHandle::disable`)
- Modify: `src/main.cpp` (пропуск финального push после logout)
- Test: `tests/config_test.cpp`, `tests/route_smoke_test.cpp`, `tests/scope_policy_test.cpp`, `tests/s3_session_sync_test.cpp`

**Interfaces:**
- Produces: `Config::session_export_enabled` (bool, env `TGW_SESSION_EXPORT_ENABLED`, по умолчанию `true`, принимает `true`/`false`/`1`/`0`). `POST /v1/auth/logout`, admin-скоуп (как весь `/v1/auth/*`), ответ 200 `{"logged_out":true}` после `Closed`, 409 `{"error":"not_ready"}` если состояние не `ready`. `S3SessionSync::disable()`: после вызова `syncOnce()` возвращает `true`, не вызывая `PutFn`. `S3SyncHandle` получает поля `std::function<void()> disable` и `std::shared_ptr<std::atomic_bool> disabled`, хвост `main` читает `disabled`.

- [ ] **Step 1: тесты.** `ConfigTest.SessionExportDefaultsTrue`, `ConfigTest.SessionExportFalse` (env `false` и `0` дают `false`). `RouteSmoke.ExportRouteAbsentWhenDisabled`: при `session_export_enabled=false` `GET /v1/auth/session/export` с admin-токеном дает 404. `ScopePolicy.LogoutRequiresAdmin`: `requiredScopeFor("/v1/auth/logout")` равен admin. `S3SessionSync.DisableStopsPuts`: после `disable()` два вызова `syncOnce()` при измененном бинлоге не вызывают `PutFn` (счетчик вызовов 0). `RouteSmoke` итерирует `kRoutesTable`: тест согласованности должен пропускать экспорт при выключенном флаге, иначе он упадет на новом поведении.
- [ ] **Step 2:** запушить ветку, CI `test-asan` падает на новых тестах. Expected: FAIL (компиляция или assertion по новым тестам).
- [ ] **Step 3:** реализовать. Logout: `auth.expectShutdown()` (после него `setOnUnexpectedTermination` не срабатывает, `src/auth/auth_state_manager.cpp:84-87`), затем `s3_sync.disable()`, затем `td_api::logOut`, ожидание `auth.waitForClosed(10s)`, ответ. В хвосте `main` финальный push выполняется, только если `disabled` ложно.
- [ ] **Step 4:** CI зеленый целиком (lint-format, tidy, asan, tsan). Expected: success во всех джобах.
- [ ] **Step 5:** коммит `feat(gateway): logout endpoint and session export flag`.

### Task 2: каркас control-plane, конфиг, идентификаторы и токены

**Files:**
- Create: `control-plane/package.json`, `tsconfig.json` (копия настроек `mcp/tsconfig.json`), `Dockerfile` (по образцу `mcp/Dockerfile`, `node:22-alpine`, `USER node`, `EXPOSE 8095 8096`), `README.md`
- Create: `control-plane/src/config.ts`, `control-plane/src/ids.ts`
- Test: `control-plane/test/config.test.ts`, `control-plane/test/ids.test.ts`
- Modify: `.github/workflows/ci.yml`: `node-build.matrix.pkg` добавить `control-plane`; publish собирает `${GW_IMAGE}-control-plane`, digest пишется в `digests/control-plane`; цикл в `publish-merge` идет по `gateway archiver mcp control-plane`.

**Interfaces:**
- Produces (`ids.ts`): `newAccountId(): string`, `isAccountId(s: string): boolean` (новый slug или числовой id, проходит `^(a[a-z0-9]{10}|[0-9]{1,20})$`), `newSecret(): string`, `clientToken(accountId: string): string`, `accountIdFromToken(t: string): string | null`, `sha256hex(s: string): string`, `newCode(): string` (24 байта base64url).
- Produces (`config.ts`): `type Config = { publicPort: number; adminPort: number; pgUrl: string; adminToken: string; registryMcpToken: string; registryArchiverToken: string; workerNamespace: string; workerImage: string; sharedSecretName: string; workerConfigMap: string; kafkaBrokers: string; s3: { endpoint: string; region: string; bucket: string; accessKeyId: string; secretAccessKey: string; forcePathStyle: boolean }; joinBaseUrl: string; apiBaseUrl: string; mcpBaseUrl: string; reconcileIntervalMs: number }` и `loadConfig(env: NodeJS.ProcessEnv): Config`. Переменные: `CP_PUBLIC_PORT` (8095), `CP_ADMIN_PORT` (8096), `CP_PG_URL`, `CP_ADMIN_TOKEN`, `CP_REGISTRY_MCP_TOKEN`, `CP_REGISTRY_ARCHIVER_TOKEN`, `CP_WORKER_NAMESPACE` (`tgw-accounts`), `CP_WORKER_IMAGE`, `CP_SHARED_SECRET` (`telegram-rest-gateway`), `CP_WORKER_CONFIGMAP` (`tgw-worker-config`), `CP_KAFKA_BROKERS`, `CP_S3_*`, `CP_JOIN_BASE_URL` (`https://join.tarassov.me`), `CP_API_BASE_URL` (`https://tg.tarassov.me`), `CP_MCP_BASE_URL` (пусто: пока нет единого MCP из следующего плана, страница не показывает блок MCP), `CP_RECONCILE_INTERVAL_MS` (30000).

Порты разделены: публичный (страницы онбординга) идет в Ingress, admin и `/internal` слушают отдельный порт, который в Ingress не попадает.

- [ ] **Step 1: тесты.** `newAccountId` 1000 раз матчит `^a[a-z0-9]{10}$` и без повторов; `isAccountId("1000000001")` true, `isAccountId("A1234567890")` false, `isAccountId("a123")` false, `isAccountId("../x")` false; `accountIdFromToken(clientToken(id)) === id`; `accountIdFromToken("nodot")` null; `newSecret()` длиной 43 и матчит `^[A-Za-z0-9_-]+$`. `loadConfig({})` бросает ошибку с именем первой отсутствующей обязательной переменной (`CP_PG_URL`); пустой `CP_ADMIN_TOKEN` тоже ошибка.
- [ ] **Step 2:** `cd control-plane && npm test`. Expected: FAIL, модули не найдены.
- [ ] **Step 3:** реализовать. Slug: `randomBytes` с отбраковкой байтов ≥ 252 (`252 = 36·7`), без смещения распределения.
- [ ] **Step 4:** `npm test` и `npm run build`. Expected: PASS, tsc без ошибок.
- [ ] **Step 5:** коммит `feat(control-plane): scaffold, config, ids and tokens`.

### Task 3: хранилище

**Files:**
- Create: `control-plane/src/store.ts` (интерфейс и типы), `control-plane/src/store_pg.ts`, `control-plane/src/store_mem.ts`, `control-plane/src/schema.sql`
- Test: `control-plane/test/store.test.ts` (один набор тестов на обе реализации; Pg-вариант пропускается без `PG_TEST_URL`)
- Modify: `.github/workflows/ci.yml`: у `node-build` сервис `postgres:16` и `PG_TEST_URL` в env шага тестов (только для `control-plane`).

**Interfaces:**
- Produces:
```ts
type AccountStatus = "onboarding" | "active" | "needs_relogin" | "deleting";
type Account = { accountId: string; status: AccountStatus; telegramUserId: string | null; username: string | null;
  archiveEnabled: boolean; exportEnabled: boolean; clientTokenHash: string; invitedBy: string;
  createdAt: Date; activatedAt: Date | null; tokenShownAt: Date | null; deleteStep: string | null };
type InviteKind = "new" | "relogin" | "token";
type Invite = { codeHash: string; kind: InviteKind; accountId: string | null; expiresAt: Date;
  claimedAt: Date | null; sessionHash: string | null; sessionExpiresAt: Date | null; usedAt: Date | null;
  codeAttempts: number; passwordAttempts: number };
interface Store {
  createInvite(i: { codeHash: string; kind: InviteKind; accountId: string | null; ttlMs: number }): Promise<Invite>;
  // Атомарно: инвайт не истек, не использован, не занят; онбордингов в процессе < maxConcurrent.
  claimInvite(codeHash: string, sessionHash: string, sessionTtlMs: number, maxConcurrent: number, now: Date):
    Promise<{ ok: true; invite: Invite } | { ok: false; reason: "not_found" | "expired" | "in_use" | "busy" | "used" }>;
  inviteBySession(sessionHash: string, now: Date): Promise<Invite | null>;
  attachAccount(codeHash: string, accountId: string): Promise<void>;
  releaseInvite(codeHash: string): Promise<void>;          // сброс claimed_at/session для истекших сессий
  markInviteUsed(codeHash: string, now: Date): Promise<void>;
  bumpAttempt(codeHash: string, kind: "code" | "password"): Promise<number>;
  expiredSessions(now: Date): Promise<Invite[]>;
  insertAccount(a: Account): Promise<void>;
  getAccount(accountId: string): Promise<Account | null>;
  listAccounts(): Promise<Account[]>;
  updateAccount(accountId: string, patch: Partial<Account>): Promise<void>;
  deleteAccount(accountId: string): Promise<void>;
  activeByTelegramId(telegramUserId: string): Promise<Account | null>; // только status = 'active'
}
```
`schema.sql` создает таблицы `accounts` и `invites` (`CREATE TABLE IF NOT EXISTS`), применяется на старте. Уникальный индекс `accounts(telegram_user_id) WHERE status = 'active'` страхует проверку дубликата от гонки.

- [ ] **Step 1: тесты** (на `MemStore` и на `PgStore`): `claimInvite` дважды с разными `sessionHash` дает `ok`, затем `in_use`; после `ttl` дает `expired`; после `markInviteUsed` дает `used`; при трех занятых сессиях четвертый инвайт получает `busy`, и его `claimed_at` остается `null`; `releaseInvite` возвращает инвайт в оборот; `activeByTelegramId` не видит `needs_relogin`; два параллельных `claimInvite` (`Promise.all`) одного кода дают ровно один `ok`; `bumpAttempt` возвращает 1, 2, 3.
- [ ] **Step 2:** `npm test`. Expected: FAIL.
- [ ] **Step 3:** реализовать. В `PgStore.claimInvite` транзакция с `pg_advisory_xact_lock(727001)`, подсчет занятых сессий и `UPDATE ... WHERE claimed_at IS NULL ... RETURNING`.
- [ ] **Step 4:** `npm test` локально (Pg-тесты skip) и в CI с Postgres. Expected: PASS в обоих.
- [ ] **Step 5:** коммит `feat(control-plane): accounts and invites store`.

### Task 4: манифесты воркера

**Files:**
- Create: `control-plane/src/manifests.ts`
- Test: `control-plane/test/manifests.test.ts`

**Interfaces:**
- Consumes: `Account`, `Config`.
- Produces: `type WorkerSecrets = { clientToken: string; adminToken: string; mcpServiceToken: string; archiverServiceToken: string; databaseEncryptionKey: string }`, `type KubeObject = { apiVersion: string; kind: string; metadata: { name: string; namespace: string; labels: Record<string, string> }; [k: string]: unknown }`, `workerName(accountId): string` (`tgw-<accountId>`), `bearerTokensValue(s: WorkerSecrets): string`, `secretManifest(accountId, s: WorkerSecrets, cfg): KubeObject`, `desiredObjects(a: Account, cfg: Config): KubeObject[]` (без Secret: Secret создается один раз в онбординге и при перевыпуске токена, reconcile его не перезаписывает).

Состав `desiredObjects`:
1. StatefulSet `tgw-<id>`: шаблон пода как `deploy/helm/telegram-rest-gateway/templates/deployment.yaml` (probes на `/v1/health`, `securityContext`, `fsGroup: 65532`, `terminationGracePeriodSeconds: 120`, `emptyDir` на `/data`), `envFrom` ConfigMap `cfg.workerConfigMap` и Secret `cfg.sharedSecretName`. Поверх в `env`: `TGW_SESSION_ID`, `BEARER_TOKENS` и `DATABASE_ENCRYPTION_KEY` через `secretKeyRef` на Secret аккаунта (`env` перекрывает `envFrom`), `TGW_SESSION_EXPORT_ENABLED` из `exportEnabled`, `TGW_KAFKA_BROKERS` равен `cfg.kafkaBrokers` при `archiveEnabled`, иначе `""`. `replicas: 0` при `deleting`. Аннотация `tgw.tarassov.me/secret-rev` с ревизией токена, ее смена перезапускает под.
2. Service `tgw-<id>`, порт `http` 8080.
3. NetworkPolicy: ingress на 8080 только из namespace `kube-system` (под Traefik `app.kubernetes.io/name=traefik`) и из namespace `tgw` (control plane, MCP, архиватор).
4. IngressRoute `tgw-<id>`, только при `active`: entryPoint `websecure`, `tls: {}`, правило
   `` Host(`tg.tarassov.me`) && (HeaderRegexp(`Authorization`, `(?i)^bearer <id>\.`) || HeaderRegexp(`Sec-WebSocket-Protocol`, `(?i)^bearer\.<id>\.`)) && PathRegexp(`(?i)^/v1/(chats|me|users|contacts|resolve|search|files|updates|health)(/|$)`) ``,
   сервис `tgw-<id>:8080`.
`BEARER_TOKENS`: строки `<client> read,write`, `<admin>`, `<mcpService> read,write`, `<archiverService> read`.

- [ ] **Step 1: тесты.** У каждого объекта оба лейбла и `namespace: tgw-accounts`; `onboarding` дает 3 объекта без IngressRoute, `active` дает 4; `deleting` дает `replicas: 0`; при `archiveEnabled=false` `TGW_KAFKA_BROKERS` равен `""`; `TGW_SESSION_EXPORT_ENABLED` равен `"false"` по умолчанию; правило IngressRoute содержит `bearer a1b2c3d4e5f\.`, не содержит `auth`, `webhooks`, `ui`, `metrics`; `bearerTokensValue` не дает клиентскому токену admin-скоуп (строка клиента кончается на ` read,write`); `desiredObjects` бросает ошибку на `accountId` `"../x"`.
- [ ] **Step 2:** `npm test`. Expected: FAIL.
- [ ] **Step 3:** реализовать.
- [ ] **Step 4:** `npm test`. Expected: PASS.
- [ ] **Step 5:** коммит `feat(control-plane): worker manifests`.

### Task 5: доступ к k8s и reconcile

**Files:**
- Create: `control-plane/src/kube.ts` (интерфейс и `K8sKube`), `control-plane/test/fake_kube.ts`, `control-plane/src/reconcile.ts`
- Test: `control-plane/test/reconcile.test.ts`

**Interfaces:**
- Produces:
```ts
interface Kube {
  apply(obj: KubeObject): Promise<void>;                 // server-side apply, fieldManager "tgw-control-plane", force
  delete(kind: string, name: string): Promise<void>;     // 404 не ошибка
  listManaged(kind: string): Promise<KubeObject[]>;      // по лейблу managed-by=control-plane в workerNamespace
  getSecret(name: string): Promise<Record<string, string> | null>;
  podGone(accountId: string): Promise<boolean>;          // нет подов с лейблом account-id
}
const MANAGED_KINDS = ["StatefulSet", "Service", "NetworkPolicy", "IngressRoute", "Secret"];
function reconcileOnce(store: Store, kube: Kube, cfg: Config, now: Date): Promise<{ applied: number; deleted: number }>;
function startReconciler(store, kube, cfg): { kick(): void; stop(): void };
```
`K8sKube` строится на `KubernetesObjectApi` из `@kubernetes/client-node` с patch-стратегией server-side apply. Точную сигнатуру `patch` сверить с установленной версией.

Поведение `reconcileOnce`: читает `listAccounts()`; если чтение бросило ошибку, функция бросает ее дальше, ничего не применив и не удалив. Для каждого аккаунта применяет `desiredObjects`. Объекты с лейблом, чей `account-id` не встречается в базе, удаляет; IngressRoute аккаунта не в статусе `active` удаляет. Secret удаляется только при отсутствии записи в базе. `kick()` склеивает частые вызовы в один проход, параллельных проходов нет.

- [ ] **Step 1: тесты на `FakeKube`.** Новый `onboarding`-аккаунт дает 3 apply. Объект с лейблом без записи в базе удаляется. Объект без лейбла `managed-by` не трогается, даже с похожим именем `tgw-1000000001`. Store, у которого `listAccounts` бросает ошибку, дает reject и ноль вызовов `delete` (Review Focus 1). `onboarding`-аккаунт моложе 15 минут после «перезапуска» (новый экземпляр reconciler на том же store и FakeKube) сохраняет свои объекты (Review Focus 5). Переход `active` в `needs_relogin` удаляет IngressRoute и оставляет StatefulSet. Пять `kick()` подряд дают один проход.
- [ ] **Step 2:** `npm test`. Expected: FAIL.
- [ ] **Step 3:** реализовать.
- [ ] **Step 4:** `npm test`. Expected: PASS.
- [ ] **Step 5:** коммит `feat(control-plane): kube client and reconcile loop`.

### Task 6: клиент воркера

**Files:**
- Create: `control-plane/src/worker_client.ts`
- Test: `control-plane/test/worker_client.test.ts` (локальный `http.createServer` как заглушка гейтвея)

**Interfaces:**
- Produces:
```ts
type AuthState = { authorization_state: string; ready: boolean; qr_link?: string;
  code_info?: { type: string; next_type: string; length: number; resend_timeout: number } };
type WorkerResult<T> = { ok: true; value: T } | { ok: false; status: number; error: string };
class WorkerClient {
  constructor(baseUrl: string, adminToken: string, timeoutMs?: number); // по умолчанию 10000
  health(): Promise<boolean>;
  state(): Promise<WorkerResult<AuthState>>;
  startQr(): Promise<WorkerResult<AuthState>>;
  phone(phone_number: string): Promise<WorkerResult<AuthState>>;
  code(code: string): Promise<WorkerResult<AuthState>>;
  password(password: string): Promise<WorkerResult<AuthState>>;
  me(): Promise<WorkerResult<{ id: string; username?: string; first_name: string; last_name: string }>>;
  logout(): Promise<WorkerResult<{ logged_out: boolean }>>;
}
function workerBaseUrl(accountId: string, cfg: Config): string; // http://tgw-<id>.<ns>.svc:8080
```

- [ ] **Step 1: тесты.** Каждый метод шлет `Authorization: Bearer <admin>` на правильный путь и метод (`/v1/auth/qr` POST, `/v1/auth/state` GET, `/v1/auth/phone` с телом `{"phone_number":...}` и так далее); 400 от гейтвея дает `{ok:false,status:400}` с текстом ошибки; таймаут дает `{ok:false,status:0}`; `health()` на закрытом порту возвращает `false`, а не бросает исключение.
- [ ] **Step 2:** `npm test`. Expected: FAIL.
- [ ] **Step 3:** реализовать на `fetch` с `AbortSignal.timeout`.
- [ ] **Step 4:** `npm test`. Expected: PASS.
- [ ] **Step 5:** коммит `feat(control-plane): worker auth client`.

### Task 7: онбординг

**Files:**
- Create: `control-plane/src/onboarding.ts` (логика), `control-plane/src/public_server.ts` (express на `publicPort`), `control-plane/src/pages.ts` (HTML страниц, без внешних скриптов, опрос `fetch` раз в 2 секунды), `control-plane/src/s3.ts` (`putMeta`, `deletePrefix`)
- Test: `control-plane/test/onboarding.test.ts`, `control-plane/test/public_server.test.ts`

**Interfaces:**
- Consumes: `Store`, `Kube`, `WorkerClient`, `desiredObjects`, `secretManifest`, `startReconciler().kick`.
- Produces:
```ts
type Phase = "starting" | "qr" | "code" | "password" | "ready" | "duplicate" | "expired" | "error";
type OnboardView = { phase: Phase; qr_svg?: string; code_info?: AuthState["code_info"]; error?: string;
  token?: string; api_url?: string; mcp_url?: string; claude_mcp_add?: string };
class Onboarding {
  constructor(deps: { store: Store; kube: Kube; cfg: Config; worker: (accountId: string, adminToken: string) => WorkerClient;
    s3: S3Ops; kick: () => void; now?: () => Date });
  open(code: string, sessionCookie: string | null): Promise<{ kind: "redirect"; setCookie?: string } | { kind: "page"; message: string; status: number }>;
  view(sessionCookie: string): Promise<OnboardView>;
  submitPhone(sessionCookie: string, phone: string): Promise<OnboardView>;
  submitCode(sessionCookie: string, code: string): Promise<OnboardView>;
  submitPassword(sessionCookie: string, password: string): Promise<OnboardView>;
  expire(now: Date): Promise<void>;        // вызывается reconciler'ом каждый проход
}
interface S3Ops { putMeta(accountId: string, meta: object): Promise<void>; deletePrefix(prefix: string): Promise<number> }
```
Маршруты `public_server.ts`: `GET /i/:code`, `GET /onboard` (страница), `GET /onboard/state`, `POST /onboard/phone`, `POST /onboard/code`, `POST /onboard/password`, `GET /t/:code`, `GET /healthz`. Других путей нет, на прочее 404.

Поток `view()`:
1. Новый аккаунт при `open`: `newAccountId`, пять секретов, `insertAccount(status onboarding)`, `kube.apply(secretManifest)`, `attachAccount`, `kick()`.
2. Пока `health()` ложно, фаза `starting`. Первый успешный health вызывает `startQr()`.
3. `wait_other_device_confirmation` дает `qr` с SVG из `qr_link` (`qrcode.toString(link, {type: "svg"})`), `wait_code` дает `code`, `wait_password` дает `password`.
4. `ready`: `me()`, затем `activeByTelegramId`. Совпадение: `logout()`, статус `deleting` (удаление по пайплайну Task 8), фаза `duplicate`. Иначе `putMeta`, `updateAccount(status active, telegramUserId, username, activatedAt)`, `markInviteUsed`, `kick()`, фаза `ready` с токеном. Токен отдается, пока `tokenShownAt` пуст, и в том же запросе `tokenShownAt` проставляется.
5. Relogin-инвайт идет по тем же шагам на существующем `accountId` без создания объектов. На `ready` `me().id` сверяется с `telegramUserId`: совпало, статус `active`; не совпало, `logout()`, статус остается `needs_relogin`, ошибка «вошли другим аккаунтом».
6. Попытки кода и пароля сверх 5 дают фазу `error` и завершают сессию.
7. `expire`: для сессий старше 15 минут без `ready` аккаунт `onboarding` уходит в `deleting`, инвайт `releaseInvite`.

- [ ] **Step 1: тесты** (MemStore, FakeKube, заглушка воркера с программируемыми состояниями). Полный QR-путь до `ready`: токен виден ровно в одном ответе `view`, во втором его нет. Перезагрузка страницы с тем же cookie дает ту же сессию и один аккаунт в store; второй cookie по той же ссылке дает 409 «ссылка уже используется» (Review Focus 2). Четвертый одновременный инвайт дает 503 «попробуйте через несколько минут». Сессия без health 16 минут после `expire()` дает аккаунт `deleting` и снова рабочую ссылку для нового cookie (Review Focus 3). Смена `qr_link` в заглушке между двумя `view` дает разные `qr_svg` (Review Focus 4). Дубликат: `me().id` совпадает с активным аккаунтом, существующий аккаунт не меняется, новый в `deleting`, фаза `duplicate`. 2FA: состояние `wait_password` отправляет пароль в `/v1/auth/password`. Шестая попытка кода дает `error`. В HTML и JSON ответах нет admin-токена (поиск подстроки по всем ответам теста). Cookie ставится с `HttpOnly`, `Secure`, `SameSite=Strict`. При пустом `mcpBaseUrl` в ответе `ready` нет `mcp_url` и `claude_mcp_add`.
- [ ] **Step 2:** `npm test`. Expected: FAIL.
- [ ] **Step 3:** реализовать. Состояние сессии берется из базы и опроса воркера при каждом `view`; памяти процесса не нужно, кроме кэша SVG по `qr_link`.
- [ ] **Step 4:** `npm test`. Expected: PASS.
- [ ] **Step 5:** коммит `feat(control-plane): invite onboarding flow and pages`.

### Task 8: admin API, удаление, relogin, реестр

**Files:**
- Create: `control-plane/src/admin_server.ts` (express на `adminPort`), `control-plane/src/lifecycle.ts`, `control-plane/src/registry.ts`, `control-plane/src/index.ts` (сборка всего вместе, применение `schema.sql`, graceful SIGTERM)
- Test: `control-plane/test/admin_server.test.ts`, `control-plane/test/lifecycle.test.ts`, `control-plane/test/registry.test.ts`

**Interfaces:**
- Produces, admin API (`Authorization: Bearer <CP_ADMIN_TOKEN>`, иначе 401; сравнение `timingSafeEqual` хэшей):
  1. `POST /admin/invites` дает `201 {url, expires_at}`.
  2. `GET /admin/accounts` дает список без хэшей: `accountId`, `status`, `telegramUserId`, `username`, `archiveEnabled`, `exportEnabled`, `createdAt`, `activatedAt` и `tgw_ready` (из `state()` воркера, `null` при ошибке).
  3. `PATCH /admin/accounts/:id` принимает `{archive_enabled?, export_enabled?}`.
  4. `POST /admin/accounts/:id/relogin` дает `{url}`, только для `needs_relogin`.
  5. `POST /admin/accounts/:id/token` дает `{url}` вида `/t/<code>`: новый клиентский токен пишется в Secret, `secret-rev` растет, под перезапускается.
  6. `DELETE /admin/accounts/:id` дает 202, статус `deleting`.
- `GET /internal/registry` на том же порту. С `CP_REGISTRY_MCP_TOKEN` отдает `{tokens: [{sha256, accountIds: [id], gatewayUrl, serviceToken}]}` по аккаунтам `active`. С `CP_REGISTRY_ARCHIVER_TOKEN` отдает `{accounts: {[id]: {gatewayUrl, archiverGatewayToken, archive_enabled}}}`. С admin-токеном и любым другим 401.
- `lifecycle.ts`: `advanceDeletion(a: Account, deps): Promise<void>`, шаги по `deleteStep`: `logout` (ошибка воркера не блокирует, если под не отвечает на health 3 прохода подряд), `scaled` (replicas 0 через reconcile), `pod_gone` (`kube.podGone`), `s3_deleted` (`deletePrefix("telegram-sessions/<id>/")`), затем `deleteAccount` и `kick()`; reconcile убирает Secret и прочие объекты. Шаг S3 строго после `podGone`. Purge архива в этом плане не вызывается, архиватор плана 4 добавит шаг `archive_purged`.
- `detectRelogin(a: Account, state: AuthState | null): AccountStatus`: для `active` состояния `wait_phone_number`, `closed`, `logging_out` дают `needs_relogin`; `null` (воркер не ответил) статус не меняет.

- [ ] **Step 1: тесты.** Admin без токена или с клиентским токеном дает 401. Реестр с MCP-токеном не содержит `archiverGatewayToken`, с архиваторским не содержит `serviceToken`, с admin-токеном 401. `advanceDeletion`: `deletePrefix` вызывается только после `podGone() === true` (FakeKube возвращает `false` два прохода, на них `deletePrefix` не вызывается). Удаление при недоступном воркере проходит до конца. `detectRelogin` по таблице состояний. Перевыпуск токена: старый хэш в `accounts` заменен, `secret-rev` у StatefulSet вырос, ссылка `/t/<code>` показывает токен один раз, второе открытие дает «ссылка уже используется». Relogin-инвайт для `active` аккаунта дает 409.
- [ ] **Step 2:** `npm test`. Expected: FAIL.
- [ ] **Step 3:** реализовать. Опрос `state()` активных аккаунтов и `advanceDeletion` идут внутри прохода reconciler'а.
- [ ] **Step 4:** `npm test`, `npm run build`, CI зеленый, образ `control-plane` собирается в `node-build`. Expected: PASS и success.
- [ ] **Step 5:** коммит `feat(control-plane): admin api, deletion pipeline, registry`.

### Task 9: Helm

**Files:**
- Create: `deploy/helm/telegram-rest-gateway-tools/templates/control-plane.yaml` (Deployment `tgw-control-plane` в `tgw`, ServiceAccount, Service с портами `public` 8095 и `admin` 8096, ConfigMap `tgw-worker-config` в `tgw-accounts` из `controlPlane.workerConfig`, Role и RoleBinding в `tgw-accounts`)
- Create: `deploy/helm/telegram-rest-gateway-tools/templates/edge.yaml` (Ingress `tgw-edge` в `tgw`, Middleware `tgw-join-ratelimit`)
- Modify: `deploy/helm/telegram-rest-gateway-tools/values.yaml`, `values-prod-example.yaml`, `Chart.yaml` (0.3.0)
- Modify: `deploy/helm/tests/render_test.sh`

**Interfaces:**
- Values: `controlPlane.enabled` (false), `controlPlane.image.repository` (`ghcr.io/moveeeax/telegram-rest-gateway-control-plane`), `controlPlane.image.tag` (обязателен при enabled), `controlPlane.workerNamespace` (`tgw-accounts`), `controlPlane.workerImage` (обязателен), `controlPlane.workerConfig` (map), `controlPlane.existingSecret` (`tgw-control-plane`: `CP_PG_URL`, `CP_ADMIN_TOKEN`, `CP_REGISTRY_MCP_TOKEN`, `CP_REGISTRY_ARCHIVER_TOKEN`), `edge.enabled` (false), `edge.hosts` (`{api: tg.tarassov.me, join: join.tarassov.me, mcp: mcp.tarassov.me}`), `edge.rateLimit` (`{average: 20, burst: 40}`).
- Role в `tgw-accounts`: `secrets`, `services`, `apps/statefulsets`, `networking.k8s.io/networkpolicies`, `traefik.io/ingressroutes` с глаголами `get list watch create update patch delete`; `pods` только `get list`. ClusterRole нет.
- Ingress `tgw-edge`: `ingressClassName: traefik`, аннотация `cert-manager.io/cluster-issuer: letsencrypt-prod`, TLS `tgw-edge-tls` на три хоста; `join`: Prefix `/i/`, `/t/`, `/onboard`, Exact `/healthz` на порт `public`, middleware rate limit; `tg`: Exact `/__tgw-edge` на порт `public`; `mcp` в этом плане только в TLS. Проходит `tools.ingressGuard`.

- [ ] **Step 1: render-тесты.** `cp: role only in worker ns` (Role с `namespace: tgw-accounts`, в рендере нет `kind: ClusterRole`); `cp: admin port not in ingress` (в Ingress `tgw-edge` нет `8096` и `admin`); `edge: join paths only` (есть `/i/`, `/onboard`, нет `/admin`, `/internal`); `edge: tls three hosts`; `edge: ratelimit middleware`; `cp: image tag required` (`assert_fails` без тега); `cp: disabled renders nothing` (без `controlPlane.enabled` нет `tgw-control-plane`).
- [ ] **Step 2:** `bash deploy/helm/tests/render_test.sh`. Expected: FAIL по новым кейсам.
- [ ] **Step 3:** реализовать.
- [ ] **Step 4:** `bash deploy/helm/tests/render_test.sh` и CI `helm-render`. Expected: все PASS.
- [ ] **Step 5:** коммит `feat(helm): control plane and edge ingress`.

### Task 10: выкатка (только с разрешения владельца)

Каждый пункт меняет кластер. Перед началом показать владельцу список и получить «да».

- [ ] **Step 1:** `kubectl create ns tgw-accounts`. Скопировать Secret `telegram-rest-gateway` из `tgw` в `tgw-accounts` без вывода значений, ключ `BEARER_TOKENS` при копировании удалить (у воркеров он свой). В `workerConfig` перенести данные ConfigMap `tgw/tgw-config`, кроме `TGW_SESSION_ID`.
- [ ] **Step 2:** CNPG Database `tgw-control` (`name: tgw_control`, `owner: tgw`, `cluster: postgresql`, `databaseReclaimPolicy: retain`). Secret `tgw/tgw-control-plane` с `CP_PG_URL` на `postgresql-rw.db.svc` и тремя новыми токенами из `openssl rand -base64 32`, значения не печатать.
- [ ] **Step 3:** `helm upgrade tgw-tools` с `controlPlane.enabled=true`, `edge.enabled=true`, тегами образов из main. Expected: под `tgw-control-plane` Running, в логах `reconcile: applied 0 deleted 0`.
- [ ] **Step 4: проверки до тестового аккаунта.** `dig +short join.tarassov.me tg.tarassov.me` дает `130.61.178.81`. `curl -sv https://tg.tarassov.me/v1/me` показывает сертификат Let's Encrypt на `tg.tarassov.me` и 404. Если Traefik отдает дефолтный сертификат на `tls: {}`, остановиться и спросить владельца, решение 1 под вопросом. `https://join.tarassov.me/admin/accounts` и `/internal/registry` дают 404.
- [ ] **Step 5: онбординг тестового аккаунта Telegram** (аккаунт дает владелец). Через QR до `ready`. Убить под через 1 секунду после `ready`: новый под авторизован (это заодно закрывает непроверенный push на Ready из фазы 0). Повторить с телефоном и 2FA. Повторный онбординг того же аккаунта дает «этот аккаунт уже подключен», живой под один (`kubectl -n tgw-accounts get pods -l tgw.tarassov.me/account-id=...`).
- [ ] **Step 6: изоляция** (раздел «Проверка» спецификации, пункты 1 и 2). Токен тестового аккаунта с подмененным префиксом дает 404 или 401. Без токена 404. `/V1/Auth/session/export`, `/v1/AUTH/qr`, `/ui`, `/metrics`, `/v1/webhooks`, `/v1/me/../auth/qr` в любом регистре до пода не доходят. WebSocket `/v1/updates` с `Sec-WebSocket-Protocol: bearer.<token>` получает hello с `session_id` тестового аккаунта. NetworkPolicy: `curl` до `tgw-<id>.tgw-accounts.svc:8080` из временного пода в `default` не проходит.
- [ ] **Step 7: удаление.** `DELETE /admin/accounts/<id>`: в S3 нет `telegram-sessions/<id>/`, в `tgw-accounts` нет объектов с этим лейблом, в Telegram у тестового аккаунта нет сессии гейтвея (Настройки → Устройства).
- [ ] **Step 8:** обновить `deploy/DEPLOY.md`: раздел «Новый клиент» (инвайт через port-forward и curl), раздел про `tgw-accounts`. Коммит `docs(deploy): control plane onboarding`.
