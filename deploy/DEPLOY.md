# Деплой в Kubernetes

Гейтвей задеплоен в кластер `talos-nbg1`, namespace `tgw`, релиз v1.2.0.

## Предпосылки

- Образ в реестре: `docker.io/resert/telegram-rest-gateway:<short-sha>` (CI собирает `:<sha>`
  и `:latest`; **semver-тег образа CI не делает** — пиньте sha).
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
