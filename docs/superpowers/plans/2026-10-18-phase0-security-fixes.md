# Фаза 0: срочные правки безопасности. План реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** закрыть найденные 2026-10-18 дыры (открытые `/ui` и `/metrics`, утечка архива между аккаунтами, окно до 300 с без бинлога в S3) до начала многоаккаунтной работы.

**Architecture:** чарты получают Traefik Middleware `ipAllowList` и явный список внешних путей вместо `/`. Guard понимает Traefik. MCP передает свой `session_id` в архиватор. Гейтвей пушит бинлог в S3 по событию Ready, а не только по таймеру. Выкатка в кластер идет отдельной задачей и только после явного согласия владельца.

**Tech Stack:** Helm 3/4 и Traefik 3.7 CRD `traefik.io/v1alpha1`, C++20 с Drogon и GoogleTest, TypeScript с `node:test` через `tsx`.

**Spec:** `docs/superpowers/specs/2026-10-18-multi-account-single-endpoint-design.md`, раздел «Срочное, до основной работы (фаза 0)».

Это первый из шести планов по спеку. Следующие пишутся после выкатки фазы 0, в таком порядке: изменения гейтвея (export-флаг, logout, режим мертвой сессии), control plane, MCP под реестр, архиватор, миграция двух аккаунтов.

## Global Constraints

1. В коммитах нет `Co-Authored-By`, `Generated with Claude Code` и любых других AI-трейлеров. Это касается и сабагентов.
2. Тексты (комментарии, DEPLOY.md, сообщения коммитов) по правилам `~/.claude/style-prompt.md`: без длинных тире, без канцелярита.
3. Состояние кластера `oci-k3s` не меняется без явного «да» владельца на конкретный шаг. Чтение (`get`, `logs`, `helm get values`) разрешено.
4. Образы только по immutable short-sha тегу, `image.tag` обязателен (как сейчас).
5. Инвариант «одна сессия = один под»: `replicas: 1`, strategy `Recreate`, двух подов одного `sessionId` одновременно не бывает.
6. Существующий стиль: комментарии в чартах и коде на русском, clang-format 18.1.8 проходит (`find src tests ... -exec clang-format --dry-run --Werror`).

## Review Focus

1. Путь в другом регистре (`/METRICS`, `/Ui`, `/V1/Auth/qr`) на внешнем хосте. Drogon матчит без учета регистра, Traefik `PathPrefix` с учетом. Ожидание: `/metrics` и `/ui` в любом регистре до пода не доходят. Проверка: живой curl в задаче 6, шаг 5.
2. Пользователь задал в `ingress.annotations` свой `traefik.ingress.kubernetes.io/router.middlewares` и одновременно `ipAllowList`. Ожидание: оба middleware в аннотации, свой не затерт. Тест: `render_test.sh`, кейс `middlewares merged`.
3. MCP запущен с `TGW_ARCHIVER_URL`, но без `TGW_SESSION_ID`. Ожидание: инструмент `telegram_search_history` не регистрируется, поиск по всем аккаунтам невозможен. Тест: `archive_search.test.ts`, кейс `no session id -> disabled`.
4. PUT в S3 на Ready упал (MinIO недоступен). Ожидание: хэш не запоминается, следующий тик таймера пушит заново. Тест: `S3SessionSync.FailedPutIsRetried`.
5. Ready приходит повторно (восстановленная из S3 сессия, реконнект). Ожидание: при неизменном бинлоге второго PUT нет. Тест: `S3SessionSync.UnchangedBinlogSkipped`.

---

### Task 1: Traefik allowlist и явные пути во внешнем Ingress гейтвея

**Files:**
- Modify: `deploy/helm/telegram-rest-gateway/templates/_helpers.tpl:42-46` (guard), добавить `tgw.ingressAnnotations`
- Modify: `deploy/helm/telegram-rest-gateway/templates/ingress.yaml`
- Create: `deploy/helm/telegram-rest-gateway/templates/middleware.yaml`
- Modify: `deploy/helm/telegram-rest-gateway/values.yaml:67-86`
- Modify: `deploy/helm/telegram-rest-gateway/Chart.yaml` (version `0.3.0`)
- Create: `deploy/helm/tests/render_test.sh`
- Modify: `.github/workflows/ci.yml` (job `helm-render`)

**Interfaces:**
- Produces (values): `ingress.ipAllowList: []` (список CIDR), `ingress.exposeLoginUi: false`, `ingress.className: "traefik"` по умолчанию. Nginx-аннотации таймаутов из дефолтов убираются (у Traefik `websecure` уже 3600 с).
- Produces (helper): `tgw.ingressAnnotations` с контекстом `$` возвращает YAML-словарь аннотаций: `ingress.annotations` плюс, если `ipAllowList` не пуст, ключ `traefik.ingress.kubernetes.io/router.middlewares` со значением `<namespace>-<release>-ip-allowlist@kubernetescrd`. Если пользователь уже задал этот ключ, значения склеиваются через запятую, сначала allowlist.
- Produces (resource): Middleware `<release>-ip-allowlist`, `spec.ipAllowList.sourceRange` = `ingress.ipAllowList`. Рендерится только при `ingress.enabled` и непустом списке.
- Produces (script): `deploy/helm/tests/render_test.sh` с функциями `assert_contains <name> <haystack> <needle>`, `assert_not_contains`, `assert_not_matches <name> <haystack> <ere>`, `assert_fails <name> <helm args...>`. Задача 2 добавляет в него свои кейсы.

- [ ] **Step 1: Написать падающий render-тест**

`render_test.sh` (bash, `set -euo pipefail`) рендерит `helm template t deploy/helm/telegram-rest-gateway --namespace tgw --set image.tag=x --set ingress.enabled=true --set-string 'accounts[0].sessionId=111'` с разными флагами. Кейсы:

```
guard: no protection          -> assert_fails (без ipAllowList, без аннотаций, allowOpenIngress=false)
guard: nginx annotation       -> рендер проходит (обратная совместимость)
guard: traefik middlewares    -> рендер проходит с --set-string ingress.annotations.traefik\.ingress\.kubernetes\.io/router\.middlewares=x@kubernetescrd
allowlist renders middleware  -> --set 'ingress.ipAllowList={203.0.113.7/32}':
                                 assert_contains "kind: Middleware", "203.0.113.7/32",
                                 "traefik.ingress.kubernetes.io/router.middlewares: tgw-t-ip-allowlist@kubernetescrd"
middlewares merged            -> ipAllowList + своя аннотация other@kubernetescrd:
                                 assert_contains "tgw-t-ip-allowlist@kubernetescrd,other@kubernetescrd"
no root path                  -> assert_not_matches -E "path: /$" (корня нет, только /v1),
                                 assert_not_contains "path: /metrics"
ui hidden by default          -> assert_not_contains "path: /ui"
ui exposed on flag            -> --set ingress.exposeLoginUi=true: assert_contains "path: /ui"
class traefik                 -> assert_contains "ingressClassName: traefik"
```

Скрипт печатает `PASS <name>` или `FAIL <name>` и выходит с 1 при любом FAIL.

- [ ] **Step 2: Запустить, убедиться, что падает**

Run: `bash deploy/helm/tests/render_test.sh`
Expected: FAIL на `guard: traefik middlewares`, `allowlist renders middleware`, `no root path`, `class traefik`.

- [ ] **Step 3: Реализовать**

Guard в `_helpers.tpl` пропускает рендер, если выполнено любое из: `allowOpenIngress`, непустой `ipAllowList`, ключ `nginx.ingress.kubernetes.io/whitelist-source-range` или ключ `traefik.ingress.kubernetes.io/router.middlewares` в `ingress.annotations`. Текст ошибки называет все четыре варианта. В `ingress.yaml` вместо одного пути `/` два правила `Prefix`: `/v1` всегда и `/ui` только при `exposeLoginUi`. Путь `/` (редирект на `/ui`) наружу не выставляется. `/metrics` не выставляется никогда. Аннотации берутся из `tgw.ingressAnnotations`. Комментарии в `values.yaml` объясняют: Traefik nginx-аннотации не читает, allowlist работает только если Traefik видит реальный IP (в `oci-k3s` включен PROXY protocol от LB).

В `ci.yml` новый job `helm-render`: `actions/checkout@v4`, `azure/setup-helm@v4`, `bash deploy/helm/tests/render_test.sh`.

- [ ] **Step 4: Запустить, убедиться, что проходит**

Run: `bash deploy/helm/tests/render_test.sh`
Expected: все строки `PASS`, код выхода 0.

- [ ] **Step 5: Commit**

```bash
git add deploy/helm/telegram-rest-gateway deploy/helm/tests .github/workflows/ci.yml
git commit -m "fix(helm): Traefik ipAllowList and explicit external paths for gateway ingress"
```

### Task 2: то же для чарта tools (MCP и архиватор)

**Files:**
- Create: `deploy/helm/telegram-rest-gateway-tools/templates/_helpers.tpl` (`tools.ingressGuard`, `tools.ingressAnnotations`)
- Create: `deploy/helm/telegram-rest-gateway-tools/templates/middleware.yaml`
- Modify: `deploy/helm/telegram-rest-gateway-tools/templates/mcp.yaml:86-119`, `archiver.yaml:144-187`
- Modify: `deploy/helm/telegram-rest-gateway-tools/values.yaml` (секция `ingress`), `values-prod-example.yaml`
- Modify: `deploy/helm/telegram-rest-gateway-tools/Chart.yaml` (version `0.2.0`)
- Modify: `deploy/helm/tests/render_test.sh`

**Interfaces:**
- Consumes: функции `assert_*` из задачи 1. Логика `tools.ingressAnnotations` та же, что `tgw.ingressAnnotations`, имя Middleware `<release>-ip-allowlist`.
- Produces (values): `ingress.ipAllowList: []`, `ingress.allowOpenIngress: false`, `ingress.className: "traefik"`.

- [ ] **Step 1: Добавить кейсы в render-тест**

Рендер `helm template tt deploy/helm/telegram-rest-gateway-tools --namespace tgw --set archiver.image.tag=x --set mcp.image.tag=x --set ingress.enabled=true --set ingress.archiverHost=a.example --set-string 'mcp.accounts[0].sessionId=111'`:

```
tools guard: no protection    -> assert_fails
tools allowlist on mcp        -> ipAllowList={203.0.113.7/32}: в Ingress tt-mcp-111 есть
                                 "tgw-tt-ip-allowlist@kubernetescrd"
tools allowlist on archiver   -> то же для Ingress tt-archiver
tools single middleware       -> ровно одно вхождение "kind: Middleware" при двух MCP-аккаунтах
```

- [ ] **Step 2: Запустить, убедиться, что новые кейсы падают**

Run: `bash deploy/helm/tests/render_test.sh`
Expected: FAIL на четырех `tools *` кейсах, кейсы задачи 1 по-прежнему PASS.

- [ ] **Step 3: Реализовать**

Guard вызывается в начале `mcp.yaml` и `archiver.yaml` внутри `if .Values.ingress.enabled`. Nginx-аннотации буферизации и таймаутов из примера prod-values убираются, вместо `whitelist-source-range` в примере `ipAllowList`.

- [ ] **Step 4: Запустить, убедиться, что проходит**

Run: `bash deploy/helm/tests/render_test.sh`
Expected: все `PASS`.

- [ ] **Step 5: Commit**

```bash
git add deploy/helm/telegram-rest-gateway-tools deploy/helm/tests/render_test.sh
git commit -m "fix(helm): ingress guard and Traefik ipAllowList for tools chart"
```

### Task 3: MCP передает session_id своего аккаунта в поиск по архиву

**Files:**
- Create: `mcp/src/archive_search.ts`
- Create: `mcp/test/archive_search.test.ts`
- Modify: `mcp/src/index.ts:336-361`
- Modify: `mcp/package.json` (script `test`, devDependency `tsx`)
- Modify: `deploy/helm/telegram-rest-gateway-tools/templates/mcp.yaml` (env `TGW_SESSION_ID: {{ $account.sessionId | quote }}`)
- Modify: `.github/workflows/ci.yml` (шаг `npm test` в матрице для `mcp`)
- Modify: `deploy/helm/tests/render_test.sh`

**Interfaces:**
- Produces: `export function buildSearchParams(sessionId: string, args: { query: string; chat_id?: string; limit: number }): URLSearchParams` в `mcp/src/archive_search.ts`. Всегда ставит `q`, `limit`, `session_id`, при наличии `chat_id`.
- Produces: `export function archiveSearchEnabled(archiverUrl: string, sessionId: string): boolean`, true только если обе строки непустые.

- [ ] **Step 1: Написать падающий тест**

```ts
import { test } from "node:test";
import assert from "node:assert/strict";
import { buildSearchParams, archiveSearchEnabled } from "../src/archive_search.js";

test("session_id always set", () => {
  const p = buildSearchParams("111", { query: "hi", limit: 20 });
  assert.equal(p.get("session_id"), "111");
  assert.equal(p.get("q"), "hi");
  assert.equal(p.get("limit"), "20");
  assert.equal(p.has("chat_id"), false);
});

test("chat_id passed through", () => {
  assert.equal(buildSearchParams("111", { query: "x", chat_id: "-100", limit: 5 }).get("chat_id"), "-100");
});

test("no session id -> disabled", () => {
  assert.equal(archiveSearchEnabled("http://a:8090", ""), false);
  assert.equal(archiveSearchEnabled("", "111"), false);
  assert.equal(archiveSearchEnabled("http://a:8090", "111"), true);
});
```

Render-кейс `mcp session id env`: в Deployment `tt-mcp-111` есть `name: TGW_SESSION_ID` и `value: "111"`.

- [ ] **Step 2: Запустить, убедиться, что падает**

Run: `cd mcp && npm install -D tsx && npm test`
Expected: FAIL, модуль `../src/archive_search.js` не найден. `render_test.sh` падает на `mcp session id env`.

- [ ] **Step 3: Реализовать**

`"test": "tsx --test test/*.test.ts"`. В `index.ts` константа `SESSION_ID = process.env.TGW_SESSION_ID ?? ""`. Инструмент регистрируется под `archiveSearchEnabled(ARCHIVER_URL, SESSION_ID)`. Если `ARCHIVER_URL` задан, а `SESSION_ID` пуст, в stderr пишется предупреждение, что поиск по архиву выключен. Параметр `session_id` модели не выставляется. `tsconfig.json` не меняется, `test/` вне `rootDir` и в `dist` не попадает.

- [ ] **Step 4: Запустить, убедиться, что проходит**

Run: `cd mcp && npm test && npm run build && cd .. && bash deploy/helm/tests/render_test.sh`
Expected: 3 теста pass, `tsc` без ошибок, все render-кейсы `PASS`.

- [ ] **Step 5: Commit**

```bash
git add mcp deploy/helm/telegram-rest-gateway-tools/templates/mcp.yaml deploy/helm/tests/render_test.sh .github/workflows/ci.yml
git commit -m "fix(mcp): scope archive search to the connector's own session_id"
```

### Task 4: гейтвей пушит бинлог в S3 сразу после Ready

**Files:**
- Modify: `src/auth/s3_session.hpp`, `src/auth/s3_session.cpp:122-153`
- Modify: `src/main.cpp:296-330` (onReady) и `:424-427` (создание синка переносится выше `auth.setOnReady`)
- Create: `tests/s3_session_sync_test.cpp`
- Modify: `tests/CMakeLists.txt` (добавить файл в `tgw_unit_tests`)

**Interfaces:**
- Produces в `tgw::auth`:

```cpp
class S3SessionSync {
public:
    using PutFn = std::function<bool(const std::string& data)>;
    S3SessionSync(std::string database_dir, PutFn put);
    // Читает td.binlog; если sha256 отличается от последнего успешно залитого, вызывает put.
    // Хэш запоминается только при put()==true. Возвращает true, если был успешный PUT.
    bool syncOnce();
private:
    std::string database_dir_;
    PutFn put_;
    std::string last_hash_;
};

struct S3SyncHandle {
    std::shared_ptr<std::atomic_bool> in_flight;
    // Потокобезопасно; ставит внеочередной syncOnce на loop Drogon (тот же путь, что тик
    // таймера, с тем же in_flight). no-op при выключенном S3.
    std::function<void()> push_now;
};

S3SyncHandle startS3Sync(tgw::util::S3Config s3, std::string database_dir, int interval_seconds);
```

- [ ] **Step 1: Написать падающий тест**

Тесты пишут файл `td.binlog` во временный каталог (как `uniqueDir` в `session_io_test.cpp`) и считают вызовы фейкового `put`:

```
S3SessionSync.FirstSyncPushes          -> binlog "a": syncOnce()==true, put вызван 1 раз с "a"
S3SessionSync.UnchangedBinlogSkipped   -> второй syncOnce() без изменения файла: false, вызовов 1
S3SessionSync.ChangedBinlogPushed      -> файл "b": syncOnce()==true, вызовов 2, последний аргумент "b"
S3SessionSync.FailedPutIsRetried       -> put возвращает false: syncOnce()==false;
                                          put возвращает true: syncOnce()==true на том же "a"
S3SessionSync.MissingBinlogNoPut       -> файла нет: syncOnce()==false, put не вызван
```

- [ ] **Step 2: Запустить, убедиться, что падает**

Run: `cmake --build --preset dev-debug --target tgw_unit_tests && ctest --preset dev-debug -R S3SessionSync --output-on-failure`
Expected: ошибка компиляции, `S3SessionSync` не объявлен.

- [ ] **Step 3: Реализовать**

`startS3Sync` создает `std::shared_ptr<S3SessionSync>` с `put`, который делает `S3Client(s3).put(data).ok()`. Тело тика (проверка `in_flight`, отдельный поток, `syncOnce`, сброс `in_flight`) выносится в лямбду, которую зовут и `runEvery`, и `push_now`. `push_now` делает `drogon::app().getLoop()->queueInLoop(tick)`. В `main.cpp` вызов `startS3Sync` переносится выше `auth.setOnReady`. Handle захватывается в колбэк по значению, `push_now()` вызывается первой строкой колбэка с логом `s3 sync: push on ready`. Финальный push на shutdown и ожидание `in_flight` не меняются.

- [ ] **Step 4: Запустить, убедиться, что проходит**

Run: `cmake --build --preset dev-debug --target tgw_unit_tests && ctest --preset dev-debug --output-on-failure`
Expected: 5 новых тестов pass, остальные без регрессий. Затем `find src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) -exec clang-format --dry-run --Werror {} +` без вывода.

- [ ] **Step 5: Commit**

```bash
git add src/auth/s3_session.hpp src/auth/s3_session.cpp src/main.cpp tests/s3_session_sync_test.cpp tests/CMakeLists.txt
git commit -m "fix(auth): push session binlog to S3 right after authorization Ready"
```

### Task 5: DEPLOY.md под реальный кластер

**Files:**
- Modify: `deploy/DEPLOY.md:104-138`

**Interfaces:**
- Consumes: имена values из задач 1 и 2 (`ingress.ipAllowList`, `ingress.exposeLoginUi`, `ingress.className: traefik`).

- [ ] **Step 1: Переписать раздел «Внешний доступ»**

Что должно быть в тексте, по фактам из кластера на 2026-10-18 (`kubectl -n kube-system get deploy traefik -o yaml`, `helm get values`):

1. Ingress-контроллер Traefik 3.7.13 в `kube-system`, провайдеры `kubernetescrd` и `kubernetesingress`. Аннотации `nginx.ingress.kubernetes.io/*` игнорируются.
2. PROXY protocol на `web` и `websecure` с `trustedIPs` LB `130.61.178.81/32`. От этого зависит, работает ли `ipAllowList`.
3. Сертификаты: cert-manager, ClusterIssuer `letsencrypt-prod`, DNS-01 через Cloudflare. HTTP-01 solver и абзац про него удалить.
4. S3 сессий: MinIO внутри кластера, `http://minio.minio.svc.cluster.local:9000`, бакет `tgw-s3-bucket`.
5. Наружу выставляется только `/v1` и, при `exposeLoginUi=true`, `/ui`. `/metrics` только изнутри кластера.
6. Проверка после выкатки: команды из задачи 6, шаг 5.

- [ ] **Step 2: Проверить**

Run: `grep -n "nginx\|HTTP-01\|whitelist" deploy/DEPLOY.md`
Expected: совпадения только в фразе о том, что nginx-аннотации игнорируются.

- [ ] **Step 3: Commit**

```bash
git add deploy/DEPLOY.md
git commit -m "docs(deploy): describe the actual Traefik, DNS-01 and MinIO setup"
```

### Task 6: выкатка в `oci-k3s` (только с согласия владельца)

Каждый шаг, меняющий кластер, исполнитель показывает владельцу командой и ждет «да». Шаг 1 только читает.

**Files:**
- Нет файлов в репо. Prod-values живут вне репо, текущие берутся `helm get values`.

**Interfaces:**
- Consumes: чарты из задач 1–3, образ гейтвея и MCP с short-sha коммита задачи 4 (публикует CI после push в `main`).

- [ ] **Step 1: Снять текущие values (чтение)**

Run: `helm --kube-context oci-k3s -n tgw get values tgw -o yaml > $SCRATCH/tgw.yaml && helm --kube-context oci-k3s -n tgw get values tgw-tools -o yaml > $SCRATCH/tgw-tools.yaml`
Expected: два файла, в обоих `className: nginx`.

- [ ] **Step 2: Проверить MinIO-бакет на анонимное чтение (временный под, спросить)**

Run: `kubectl --context oci-k3s -n tgw run s3check --rm -i --restart=Never --image=curlimages/curl -- -s -o /dev/null -w '%{http_code}\n' http://minio.minio.svc.cluster.local:9000/tgw-s3-bucket/`
Expected: `403`. Если `200`, остановиться и сообщить владельцу: бакет с сессиями открыт. Под создается и удаляется, это тоже изменение кластера, спросить перед запуском.

- [ ] **Step 3: Подготовить новые values**

В копиях из шага 1: `ingress.className: traefik`, nginx-аннотации удалить (оставить `cert-manager.io/cluster-issuer`), `ingress.ipAllowList: [<IP владельца>/32]`. IP владелец называет сам или подтверждает вывод `curl -s https://ifconfig.me`. `ingress.exposeLoginUi: true` для `tgw`. `image.tag` нового short-sha. Показать `helm diff` или `helm template | diff` против текущего рендера.

- [ ] **Step 4: Выкатить (с согласия)**

Run: `helm --kube-context oci-k3s -n tgw upgrade tgw deploy/helm/telegram-rest-gateway -f $SCRATCH/tgw.new.yaml`, затем то же для `tgw-tools`.
Expected: `STATUS: deployed`. Поды гейтвея пересоздаются по одному (Recreate), `kubectl get pods` показывает по одному поду на `sessionId`, `/v1/ready` 200.

- [ ] **Step 5: Проверка снаружи**

С IP владельца:

```
curl -s -o /dev/null -w '%{http_code}' https://tg-1214732998.tarassov.me/v1/health        -> 200
curl -s -o /dev/null -w '%{http_code}' https://tg-1214732998.tarassov.me/metrics          -> 404
curl -s -o /dev/null -w '%{http_code}' https://tg-1214732998.tarassov.me/METRICS          -> 404
curl -s -o /dev/null -w '%{http_code}' https://tg-1214732998.tarassov.me/Ui               -> 404
curl -s -o /dev/null -w '%{http_code}' https://tg-1214732998.tarassov.me/ui               -> 200
```

С другого IP (мобильная сеть владельца или любой внешний хост, который он укажет) все пять адресов дают `403`. Если с IP владельца тоже `403`, Traefik видит адрес LB, а не клиента. Тогда `ipAllowList` убрать, `exposeLoginUi: false`, `allowOpenIngress: true` с комментарием, повторить шаг 4 и сообщить владельцу.

- [ ] **Step 6: Проверка утечки архива**

Через коннектор аккаунта `1214732998` вызвать `telegram_search_history` со словом, которое есть только в переписке `1389231240`.
Expected: пустой результат. В логах архиватора запрос `/search` содержит `session_id=1214732998`.

- [ ] **Step 7: Проверка push на Ready**

Run: `kubectl --context oci-k3s -n tgw logs deploy/tgw-1214732998 | grep "push on ready"`
Expected: строка `s3 sync: push on ready` после последнего рестарта и следом `s3 sync: session pushed`.
