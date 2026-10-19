# Деплой в Kubernetes

Гейтвей задеплоен в кластер `oci-k3s` (k3s, 6 нод), namespace `tgw`. Состояние на 2026-10-18:
релиз `tgw` (chart 0.2.0) и `tgw-tools` (chart 0.1.0), образы `aab9246`.

## Предпосылки

- Образ в реестре: `ghcr.io/moveeeax/telegram-rest-gateway:<short-sha>` (CI пушит `:<short-sha>`
  всегда и `:vX.Y.Z` на git-теге; **«последнего»/`latest` образа CI не делает** — пиньте sha,
  см. docs/CICD.md).
- Kafka в кластере: namespace `kafka`, сервис `kafka.kafka.svc.cluster.local:9092` (PLAINTEXT).
- S3 для сессий: MinIO внутри кластера, `http://minio.minio.svc.cluster.local:9000`, бакет
  `tgw-s3-bucket`. Бакет не должен отдавать объекты анонимно: в нем `td.binlog` всех аккаунтов
  и медиа архива.

## Секрет

Один Secret на namespace, общий для всех аккаунтов (envFrom). **Telegram-креды и
`DATABASE_ENCRYPTION_KEY` менять НЕЛЬЗЯ** — иначе сессии в S3 не расшифруются (ре-логин).
Ротировать можно `BEARER_TOKENS` (независим от Telegram-сессии).

```bash
kubectl create namespace tgw
kubectl -n tgw create secret generic telegram-rest-gateway \
  --from-literal=API_ID=... \
  --from-literal=API_HASH=... \
  --from-literal=DATABASE_ENCRYPTION_KEY=... \
  --from-literal=TGW_S3_ACCESS_KEY_ID=... \
  --from-literal=TGW_S3_SECRET_ACCESS_KEY=... \
  --from-file=BEARER_TOKENS=./bearer_tokens.txt   # по строке: "<token>[ read,write,admin]"
```

## Установка

```bash
helm upgrade --install tgw deploy/helm/telegram-rest-gateway \
  -n tgw -f deploy/helm/values-prod-example.yaml --wait
```

## ⚠️ Одна сессия = один живой процесс

Auth key Telegram нельзя использовать в двух местах одновременно (AUTH_KEY_DUPLICATED →
сессия отзывается). Поэтому:

- В чарте `replicas: 1` + `strategy: Recreate` (при апдейте старый под гасится ДО нового).
- **Перед деплоем погасите любой другой инстанс с тем же `session_id`** (локальный docker,
  старый под) — он на graceful-стопе запушит финальную сессию в S3, поды её подхватят.

## Первый логин аккаунта

Если сессии в S3 ещё нет — под стартует неавторизованным:

```bash
kubectl -n tgw port-forward svc/tgw-<sessionId> 8080:8080
# открой http://localhost:8080/ui — QR или телефон
```

## Проверка

```bash
kubectl -n tgw port-forward svc/tgw-<sessionId> 8080:8080 &
curl -s localhost:8080/v1/ready                                   # {"ready":true,...}
curl -s -H "Authorization: Bearer <admin-token>" localhost:8080/v1/me
curl -s localhost:8080/metrics | grep tgw_kafka                   # produced растёт, failed=0
```

## Мониторинг

`GET /metrics` (Prometheus). Алерты: `tgw_ready == 0` (слетела авторизация),
рост `tgw_kafka_failed_total`/`tgw_kafka_dropped_total`, `tgw_http_responses_5xx_total`.
Включить сбор: `serviceMonitor.enabled=true` (нужен Prometheus Operator).

## Каталоги данных (`TGW_DATABASE_DIR`/`TGW_FILES_DIR`) и volume

Чарт **без PVC**: `TGW_DATABASE_DIR` (по умолчанию `/data/session`) и `TGW_FILES_DIR`
(по умолчанию `/data/files`) живут на `emptyDir`, примонтированном в `/data`
(`templates/deployment.yaml`, `volumeMounts[].mountPath: /data`) — данные пода не переживают
рестарт сами по себе. Персистентность обеспечивает не volume, а S3-синк `td.binlog`
(см. `TGW_S3_*` выше): на старте бинлог тянется из S3, периодически и на graceful shutdown
заливается обратно. Если переопределяешь `TGW_DATABASE_DIR`/`TGW_FILES_DIR` через
`values.config` — держи их **внутри** `/data`, иначе каталог окажется вне смонтированного
volume и потеряется при пересоздании пода. `TGW_FILES_DIR` (скачанные/загруженные файлы)
в S3 не бэкапится — это кэш, не критичные данные.


## Архиватор и MCP в кластере (chart telegram-rest-gateway-tools)

Отдельный чарт `deploy/helm/telegram-rest-gateway-tools`:
- **archiver** — Deployment (replicas 1) + Service :8090, Postgres `tgw_archive` в CNPG.
  Консьюмит Kafka `tgw.updates` и пишет только аккаунты с `archive_enabled` (флаг control plane,
  по умолчанию включен). Адрес гейтвея и токен каждого аккаунта для медиа и бэкфилла берет из
  реестра control plane (`ARCHIVER_REGISTRY_URL`). Наружу не выставлен.
- **mcp** — единый MCP за `mcp.tarassov.me`, см. раздел «Единый MCP».

Секрет `tgw-tools`: `ARCHIVER_TOKEN` (MCP → `/search`), `ARCHIVER_ADMIN_TOKEN` (`/stats`,
`/backfill`, `/media/retry`, `DELETE /accounts/:id`; его же использует control plane для purge),
`ARCHIVER_PG_URL`.

```bash
kubectl -n tgw port-forward svc/tgw-tools-archiver 8090:8090 &
ADM=$(kubectl -n tgw get secret tgw-tools -o jsonpath='{.data.ARCHIVER_ADMIN_TOKEN}' | base64 -d)
curl -s -H "Authorization: Bearer $ADM" localhost:8090/stats
curl -s -X POST -H "Authorization: Bearer $ADM" -H 'content-type: application/json' -d '{}' localhost:8090/media/retry
curl -s -X POST -H "Authorization: Bearer $ADM" -H 'content-type: application/json' \
  -d '{"session_id":"<accountId>"}' localhost:8090/backfill
```

`/search` требует `session_id`; токен MCP на админских путях получает 403. При удалении
аккаунта control plane вызывает `DELETE /accounts/<accountId>`: удаляются сообщения, чаты, seq и
медиа `media/<accountId>/` в S3.

## Внешний доступ (Ingress + TLS + IP-allowlist)

Сервисы отдают полный контроль над аккаунтом и всю историю, поэтому наружу они выставляются только
за IP-allowlist'ом. Выставляются gateway, MCP и архиватор (только `/search` и `/health`, один хост
`ingress.archiverHost`; `/backfill` и `/stats` остаются ClusterIP). Если архиватор снаружи не нужен,
оставь `archiverHost` пустым: MCP ходит в него внутри кластера.

### Ingress-контроллер: Traefik

В `oci-k3s` стоит Traefik 3.7.13 (`kube-system/traefik`) с провайдерами `kubernetescrd` и
`kubernetesingress`. Провайдера `kubernetesIngressNGINX` нет, поэтому аннотации
`nginx.ingress.kubernetes.io/*` (whitelist, таймауты, буферизация) Traefik не читает. В обоих
чартах `ingress.className: traefik`.

Allowlist задается списком CIDR в `ingress.ipAllowList`. Чарт создает Traefik Middleware
`<release>-ip-allowlist` (`ipAllowList.sourceRange`) и вешает его на свои Ingress аннотацией
`traefik.ingress.kubernetes.io/router.middlewares`. Свои middleware в этой аннотации можно задать
в `ingress.annotations`, чарт допишет allowlist первым. Без `ipAllowList` (или с `0.0.0.0/0`,
`::/0`) `helm template` падает на guard'е, аннотации он не засчитывает: в `oci-k3s` класс `nginx`
тоже обслуживает Traefik, так что имя класса ничего не гарантирует. Осознанный опт-аут
`ingress.allowOpenIngress=true`.

Allowlist работает, только если Traefik видит реальный IP клиента. В `oci-k3s` он его НЕ видит
(проверено 2026-10-18): на entrypoint'ах `web` и `websecure` прописан PROXY protocol с
`trustedIPs` балансировщика, но все внешние запросы доходят до Traefik с адресом LB
`130.61.178.81`. Allowlist с внешним адресом клиента режет всех, allowlist с адресом LB пускает
всех. Поэтому прод сейчас открыт (`allowOpenIngress=true`): `/v1` под Bearer-токеном, MCP под
своим токеном, `/ui` и `/metrics` наружу не выставлены. Чтобы вернуть allowlist, сначала нужно
доставить IP клиента до Traefik (PROXY protocol на стороне OCI LB или `externalTrafficPolicy: Local`)
и повторить проверку ниже.

Таймауты: у `websecure` `readTimeout`, `writeTimeout` и `idleTimeout` по 3600 с. Это покрывает
Streamable HTTP MCP и паузы humanize-typing гейтвея (до 90 с), отдельные аннотации не нужны.

### Какие пути выставляются

Гейтвей: только префикс `/v1` и, при `ingress.exposeLoginUi=true`, `/ui`. Корень `/` и `/metrics`
наружу не выставляются, `/metrics` скрейпится только изнутри кластера. Traefik сравнивает префикс с
учетом регистра, поэтому `/METRICS` и `/UI` до пода тоже не доходят (Drogon внутри сравнивает пути
без учета регистра, полагаться на его сторону нельзя).

MCP: весь хост `mcp-<sessionId>` (Streamable HTTP на `/mcp`). Архиватор: `/search` и `/health`.

### TLS и DNS

cert-manager, ClusterIssuer `letsencrypt-prod`, ACME DNS-01 через Cloudflare (Secret
`cloudflare-api-key`). HTTP-01 не используется, allowlist выпуску сертификатов не мешает.
DNS-записи создает external-dns по аннотации `external-dns.alpha.kubernetes.io/target: <LB-IP>`.
Хосты: `tg.tarassov.me`, `join.tarassov.me` (Ingress `tgw-tools-edge`), `mcp.tarassov.me` (Ingress `tgw-tools-mcp-shared`).

### Проверка после выкатки

С IP из allowlist'а:

```bash
H=https://tg-<sessionId>.tarassov.me
curl -s -o /dev/null -w '%{http_code}\n' $H/v1/health   # 200
curl -s -o /dev/null -w '%{http_code}\n' $H/metrics     # 404
curl -s -o /dev/null -w '%{http_code}\n' $H/METRICS     # 404
curl -s -o /dev/null -w '%{http_code}\n' $H/Ui          # 404
curl -s -o /dev/null -w '%{http_code}\n' $H/ui          # 200 при exposeLoginUi=true, иначе 404
```

С любого другого IP те же запросы дают 403. Если 403 приходит и с IP из allowlist'а, Traefik видит
адрес балансировщика, а не клиента: allowlist в таком виде бесполезен. Тогда выключи
`exposeLoginUi`, убери `ipAllowList`, поставь `allowOpenIngress=true` и чини доставку IP клиента.

> `/v1/health`, `/v1/ready` и `/ui` отвечают без Bearer. Это допустимо только потому, что доступ
> ограничен по IP. Не расширяй allowlist без необходимости.

## Новые клиенты: control plane (онбординг по инвайту)

Новые аккаунты больше не заводятся через values и `helm upgrade`. Control plane
(`control-plane/`, включается в чарте tools: `controlPlane.enabled`, `edge.enabled`) сам
поднимает воркер аккаунта в namespace `tgw-accounts`: Secret, StatefulSet, Service,
NetworkPolicy и, после логина, IngressRoute на `tg.tarassov.me`. Дизайн:
`docs/superpowers/specs/2026-10-18-multi-account-single-endpoint-design.md`.

Хосты: `join.tarassov.me` (страницы онбординга), `tg.tarassov.me` (REST и WebSocket всех
новых аккаунтов, аккаунт выбирается по префиксу токена `<accountId>.`), `mcp.tarassov.me`
(пока только в сертификате, единый MCP отдельным планом). DNS и сертификат заводит Ingress
`tgw-tools-edge`.

Однократная подготовка кластера (сделана 2026-10-19):

```bash
kubectl create ns tgw-accounts
# копия общего Secret гейтвея без BEARER_TOKENS, DATABASE_ENCRYPTION_KEY и TGW_SESSION владельца
# CNPG Database tgw_control (owner tgw) и Secret tgw/tgw-control-plane:
#   CP_PG_URL, CP_ADMIN_TOKEN, CP_REGISTRY_MCP_TOKEN, CP_REGISTRY_ARCHIVER_TOKEN
helm upgrade tgw-tools deploy/helm/telegram-rest-gateway-tools -n tgw --reuse-values \
  --set controlPlane.enabled=true --set controlPlane.image.tag=<sha> \
  --set controlPlane.workerImage=ghcr.io/moveeeax/telegram-rest-gateway:<sha> --set edge.enabled=true
```

Пример полного набора values: `deploy/helm/telegram-rest-gateway-tools/values-prod-example.yaml`.

Admin API наружу не выставлен, только через port-forward:

```bash
kubectl -n tgw port-forward svc/tgw-tools-control-plane 18096:8096 &
ADM=$(kubectl -n tgw get secret tgw-control-plane -o jsonpath='{.data.CP_ADMIN_TOKEN}' | base64 -d)
curl -s -X POST -H "Authorization: Bearer $ADM" localhost:18096/admin/invites            # ссылка на 24 ч
curl -s -H "Authorization: Bearer $ADM" localhost:18096/admin/accounts                   # список и tgw_ready
curl -s -X PATCH -H "Authorization: Bearer $ADM" -H 'content-type: application/json' \
  -d '{"archive_enabled":true}' localhost:18096/admin/accounts/<id>                      # архив, экспорт
curl -s -X POST -H "Authorization: Bearer $ADM" localhost:18096/admin/accounts/<id>/token    # новый токен
curl -s -X POST -H "Authorization: Bearer $ADM" localhost:18096/admin/accounts/<id>/relogin  # needs_relogin
curl -s -X DELETE -H "Authorization: Bearer $ADM" localhost:18096/admin/accounts/<id>       # logout + зачистка
```

Клиент открывает ссылку, нажимает «Подключить», сканирует QR (или входит по телефону) и один
раз видит токен. Ссылка одноразовая, сессия онбординга 15 минут, одновременно не больше трех.
Удаление: logout в Telegram, остановка пода, удаление `telegram-sessions/<id>/` в S3, затем
Secret и запись в базе.

Проверено на тестовом аккаунте 2026-10-19: онбординг по QR, маршрутизация и изоляция на
`tg.tarassov.me` (чужой и неизвестный префикс 404, служебные пути 404 в любом регистре и с
`..`), WebSocket hello, перезапуск пода после логина, push бинлога на Ready, NetworkPolicy
(из `default` до воркера не доходит), удаление и повторный онбординг того же человека.

Ограничения сейчас: архив тенантов выключен (`archive_enabled=false`, Kafka у воркера пустой),
единого MCP еще нет, два старых аккаунта живут по-старому в чарте `tgw` до миграции.

## Единый MCP (mcp.tarassov.me)

Один MCP-сервер на все аккаунты: `https://mcp.tarassov.me/mcp` (Streamable HTTP), чарт tools
с `mcp.shared.enabled=true`. Ключ (заголовок `X-API-Key` или `Authorization: Bearer`)
резолвится через реестр control plane в набор аккаунтов, кэш 30 с. В гейтвей MCP ходит
сервисным токеном воркера, ключ клиента туда не передается.

- Клиент подключает коннектор со своим клиентским токеном `<accountId>.<secret>` и видит только
  свой аккаунт.
- Владелец держит один коннектор на несколько аккаунтов с токеном `owner.<secret>`:

```bash
kubectl -n tgw port-forward svc/tgw-tools-control-plane 18096:8096 &
ADM=$(kubectl -n tgw get secret tgw-control-plane -o jsonpath='{.data.CP_ADMIN_TOKEN}' | base64 -d)
curl -s -X POST -H "Authorization: Bearer $ADM" -H 'content-type: application/json' \
  -d '{"label":"owner","account_ids":["<accountId>","<accountId>"]}' localhost:18096/admin/owner-tokens
curl -s -H "Authorization: Bearer $ADM" localhost:18096/admin/owner-tokens          # список без токенов
curl -s -X DELETE -H "Authorization: Bearer $ADM" localhost:18096/admin/owner-tokens/owner
```

Токен показывается один раз, в базе только хэш. У каждого инструмента есть параметр `account`:
при одном аккаунте у ключа его можно не указывать, при нескольких он обязателен для всех
инструментов (дефолта нет). `telegram_list_accounts` возвращает доступные аккаунты.

Коннектор claude.ai: URL `https://mcp.tarassov.me/mcp`, заголовок `X-API-Key`. Запросы с
заголовком `Origin` вне `mcp.shared.allowedOrigins` получают 403; коннектор claude.ai проходит
(проверено 2026-10-19). У MCP свой Ingress, rate limit онбординга на него не действует.

Per-account MCP (`mcp-<id>`) убраны 2026-10-19 (`mcp.accounts: []`); их DNS-записи в
Cloudflare external-dns с `--policy=upsert-only` не удаляет.
