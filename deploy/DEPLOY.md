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
- **archiver** — Deployment (replicas 1, Recreate — SQLite single-writer) + PVC (longhorn) +
  Service :8090. Консьюмит кластерную Kafka, отдаёт `/search`.
- **mcp** — по Deployment+Service на аккаунт (Streamable HTTP, stateful session), env
  `TGW_BASE_URL` → gateway-Service, `TGW_ARCHIVER_URL` → archiver-Service.

Секрет `tgw-tools`: `TGW_BEARER_TOKEN` (MCP→gateway, скоуп read,write), `MCP_HTTP_TOKEN`
(клиент→MCP), `ARCHIVER_TOKEN` (доступ к /search).

```bash
kubectl -n tgw create secret generic tgw-tools \
  --from-literal=TGW_BEARER_TOKEN=<agent-token> \
  --from-literal=MCP_HTTP_TOKEN=<random> \
  --from-literal=ARCHIVER_TOKEN=<random>
helm upgrade --install tgw-tools deploy/helm/telegram-rest-gateway-tools -n tgw --wait
```

### Подключить Claude к MCP в кластере

MCP отдаёт Streamable HTTP на `/mcp` (Bearer = `MCP_HTTP_TOKEN`). Наружу — через Ingress
(`ingress.enabled=true`, `mcpHostTemplate`) или локально:

```bash
kubectl -n tgw port-forward svc/tgw-tools-mcp-<sessionId> 8080:8080
claude mcp add --transport http telegram http://localhost:8080/mcp \
  --header "Authorization: Bearer <MCP_HTTP_TOKEN>"
```


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

Allowlist работает, только если Traefik видит реальный IP клиента. В `oci-k3s` на entrypoint'ах
`web` и `websecure` включен PROXY protocol с `trustedIPs` балансировщика (`130.61.178.81/32`), так
что адрес клиента должен доходить. Проверять это после каждой смены LB или Traefik (см. ниже).

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
Хосты: `tg-<sessionId>`, `mcp-<sessionId>`, `tgw-archive`.

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
