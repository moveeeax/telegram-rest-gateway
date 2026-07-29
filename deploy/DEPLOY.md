# Деплой в Kubernetes

Гейтвей задеплоен в кластер `talos-nbg1`, namespace `tgw`, релиз v1.2.0.

## Предпосылки

- Образ в реестре: `ghcr.io/moveeeax/telegram-rest-gateway:<short-sha>` (CI пушит `:<short-sha>`
  всегда и `:vX.Y.Z` на git-теге; **«последнего»/`latest` образа CI не делает** — пиньте sha,
  см. docs/CICD.md).
- Kafka в кластере: namespace `kafka`, сервис `kafka.kafka.svc.cluster.local:9092` (PLAINTEXT).
- S3/MinIO для сессий (`s3.tarassov.me`, бакет `tgw-s3-bucket`).

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

Сервисы отдают полный контроль над аккаунтом и всю историю — **в интернет напрямую не выставляем**.
Прод-вариант: nginx Ingress + Let's Encrypt (cert-manager) + external-dns, **жёстко ограниченный
IP-allowlist'ом**. Выставляются gateway (`/ui`+API), MCP и архиватор (только `/search` и `/health`,
хост `ingress.archiverHost` — один, не per-account; `/backfill` и `/stats` остаются ClusterIP).
Все за одним IP-allowlist'ом. Если архиватор наружу не нужен — оставь `archiverHost` пустым
(тогда только ClusterIP, MCP всё равно ходит в него внутри кластера).

Аннотации (см. `values-prod-example.yaml` обоих чартов):
- `cert-manager.io/cluster-issuer: letsencrypt-prod` — TLS-сертификат.
- `external-dns.alpha.kubernetes.io/target: <LB-IP>` — external-dns создаёт DNS A-запись (host
  берётся из rules).
- `nginx.ingress.kubernetes.io/whitelist-source-range: <ip>/32` — **обязательно**; пускает только
  доверенные IP (остальным 403).
- `nginx.ingress.kubernetes.io/ssl-redirect: "true"`.
- Для MCP: `proxy-buffering: "off"`, `proxy-read/send-timeout: "3600"` (Streamable HTTP/SSE).

cert-manager проходит ACME HTTP-01 через отдельный `cm-acme-http-solver` ingress (без whitelist),
поэтому allowlist выпуску сертификата не мешает. Хосты: `tg-<sessionId>`, `mcp-<sessionId>`.

Проверка: с доверенного IP — 200; с любого другого — 403 (whitelist). TLS — Let's Encrypt.

> Даже за allowlist'ом: `/v1/health`, `/v1/ready`, `/ui`, `/metrics` без Bearer — допустимо, т.к.
> доступ уже ограничен по IP. Не расширяй whitelist без необходимости.
