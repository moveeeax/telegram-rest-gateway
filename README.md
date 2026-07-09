# telegram-rest-gateway

Self-hosted REST/WebSocket-обёртка над пользовательским Telegram-аккаунтом на базе
**TDLib** (нативный C++-интерфейс, `td::ClientManager`) и **Drogon**. Даёт языконезависимый
HTTP-доступ к аккаунту, скрывая асинхронную природу TDLib за моделью «запрос → ответ»
и доставляя апдейты через WebSocket.

> Статус: **Stage 0 (каркас)**. Функциональность моста/авторизации/сообщений — в разработке
> по этапам (см. `docs/SPEC_ELABORATION.md` §12). Сборка только в Docker/CI.

## Документация

| Файл | Назначение |
|---|---|
| [`TZ_TDLib_Drogon_REST.md`](TZ_TDLib_Drogon_REST.md) | Основное ТЗ (v2.0) |
| [`docs/SPEC_ELABORATION.md`](docs/SPEC_ELABORATION.md) | Детальная проработка уровня реализации (v3.0) |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | Журнал принятых решений |
| [`docs/GITFLOW.md`](docs/GITFLOW.md) | Модель ветвления |
| [`openapi/openapi.yaml`](openapi/openapi.yaml) | Единый источник истины HTTP-контракта |

## Стек и ключевые решения

- **C++20** (корутины Drogon), TDLib собирается своим C++17.
- **TDLib линкуется статически** через `Td::TdStatic` — нативный C++-API не экспортируется
  `libtdjson.so`.
- Единый **OpenSSL 3.0** для TDLib, Drogon и нашего кода.
- Single-account, только WebSocket для апдейтов, шифрование БД TDLib, id в JSON — строками.
- Деплой: multi-arch (amd64 + arm64) Docker, финал — distroless nonroot.

## Структура

```
src/
  main.cpp              — точка входа, /v1/health, --healthcheck
  bridge/
    transport.hpp       — ITdTransport (seam над td::ClientManager)
    real_transport.*    — RealTdTransport (единственная точка реального TDLib)
    clock.hpp           — инъекция времени (SteadyClock / FakeClock в тестах)
tests/
  fake_transport.hpp    — FakeTdTransport (scripted, потокобезопасный)
  transport_smoke_test.cpp
openapi/openapi.yaml
Dockerfile.builder      — образ тулчейн+TDLib+Drogon (пины TDLIB_REF/DROGON_REF)
Dockerfile              — образ сервиса (FROM builder) → distroless
```

CI/CD и список переменных GitLab — [`docs/CICD.md`](docs/CICD.md).

## Сборка (в контейнере)

```bash
# 1. Зафиксировать TDLIB_REF (полный git-SHA!) и DROGON_REF.
# 2. Собрать builder-образ (долго; только при смене пинов):
docker build -f Dockerfile.builder \
  --build-arg TDLIB_REF="$(grep -oP '(?<=^TDLIB_REF=).*' TDLIB_REF)" \
  --build-arg DROGON_REF="$(grep -oP '(?<=^DROGON_REF=).*' DROGON_REF)" \
  -t tgw-builder:local .
# 3. Собрать сервис:
docker build --build-arg BUILDER_IMAGE=tgw-builder:local -t telegram-rest-gateway .
```

Локальная сборка вне Docker требует установленных TDLib (`Td::TdStatic`), Drogon и
OpenSSL 3.0; конфигурация — через `CMakePresets.json` (`cmake --preset dev-debug`).

## Конфигурация (env)

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `TGW_LISTEN_ADDRESS` | `127.0.0.1` | Адрес прослушивания (в Docker — `0.0.0.0`) |
| `TGW_LISTEN_PORT` | `8080` | Порт HTTP |
| `TGW_SESSION` / `TGW_SESSION_FILE` | — | Session string (base64 от `td.binlog`) для stateless-запуска |
| `TGW_WS_MAX_PENDING_BYTES` | `8388608` | WS back-pressure: лимит байт с последнего pong; 0 — выкл |
| `TGW_MAX_MEMORY_BODY_BYTES` | `1048576` | Порог spool-на-диск для тел запросов (RSS при аплоадах) |
| `TGW_KAFKA_BROKERS` | — | Kafka/Redpanda bootstrap; пусто — события в Kafka выключены |
| `TGW_KAFKA_TOPIC` | `tgw.updates` | Топик событий |
| `TGW_KAFKA_CLIENT_ID` | `tgw-<session_id>` | client.id продюсера |

### Хранение сессии в S3/MinIO (опционально)

Если заданы, `td.binlog` на старте тянется из S3 (при отсутствии локального), периодически и на
graceful shutdown заливается обратно. Позволяет запускать сервис полностью stateless без монтирования
volume. Включается только при заполненных `bucket` + credentials + `endpoint`.

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `TGW_S3_ENDPOINT` | — | `http(s)://host[:port]` (AWS: `https://s3.<region>.amazonaws.com`) |
| `TGW_S3_REGION` | `us-east-1` | Регион для подписи SigV4 |
| `TGW_S3_BUCKET` | — | Имя бакета |
| `TGW_SESSION_ID` | `default` | Метка инстанса → путь в S3 (сегмент `[A-Za-z0-9._-]`) |
| `TGW_S3_PREFIX` | `telegram-sessions` | Префикс ключа |
| `TGW_S3_KEY` | *(derived)* | Явный ключ-override; иначе `<prefix>/<session_id>/td.binlog` |
| `TGW_S3_ACCESS_KEY_ID` / `_FILE` | — | Access key |
| `TGW_S3_SECRET_ACCESS_KEY` / `_FILE` | — | Secret key |
| `TGW_S3_PATH_STYLE` | `true` | `true` — path-style (MinIO); `false` — virtual-host (AWS) |
| `TGW_S3_SYNC_INTERVAL_SECONDS` | `300` | Период фонового бэкапа сессии в S3 |

**Несколько аккаунтов на одном бакете.** Каждый инстанс получает свой путь по `TGW_SESSION_ID`:
`telegram-sessions/<session_id>/td.binlog`. Задавай разный `TGW_SESSION_ID` на каждый аккаунт
(Telegram account_id недоступен до логина, поэтому метку назначает оператор). Один и тот же
`session_id` нельзя гонять в двух инстансах одновременно — Telegram убьёт сессию (`AUTH_KEY_DUPLICATED`).

Секреты (`api_id`, `api_hash`, `database_encryption_key`, Bearer-токены, S3 credentials) — только
через `*_FILE` / secret manager, никогда в образ/env напрямую.

## События в Kafka

Если задан `TGW_KAFKA_BROKERS`, каждый апдейт из allowlist (тот же набор, что в WS) публикуется
в топик `TGW_KAFKA_TOPIC`. Формат тела — как WS-фрейм (`type/update_type/seq/session_id/data`);
**ключ сообщения — `<session_id>:<chat_id>`** (префикс id аккаунта; порядок в рамках чата
гарантирован партиционированием). Доставка at-least-once: дедупликация у консьюмера по
`(session_id, seq)`; дыра в `seq` = потеря (см. `tgw_kafka_dropped_total`). Продюсер никогда
не блокирует приём апдейтов Telegram: при переполнении очереди события дропаются с метрикой.

## Архиватор: поиск по переписке

[`archiver/`](archiver/README.md) — консьюмер Kafka-событий → SQLite+FTS5: полнотекстовый
поиск по всей сохранённой истории (`GET :8090/search?q=`), включая правки и удалённые.
Агенту доступен как MCP-tool `telegram_search_history`.

## MCP: агентский коннектор

[`mcp/`](mcp/README.md) — MCP-сервер (TypeScript, stdio): Claude Code/Desktop и любой
MCP-клиент получают 14 инструментов аккаунта (сообщения, реакции, resolve, медиа).
Один сервер = один аккаунт; токен = полный доступ, храни как секрет.

## Деплой в Kubernetes (Helm)

Чарт: [`deploy/helm/telegram-rest-gateway`](deploy/helm/telegram-rest-gateway). Особенности:
одна сессия = один под (**strategy Recreate, реплики не масштабировать** — иначе
`AUTH_KEY_DUPLICATED`), без PVC (сессии в S3), readiness на `/v1/health` (неавторизованный под
должен принимать трафик для логина через `/ui`). Секреты — через `existingSecret`.

```bash
kubectl create secret generic telegram-rest-gateway \
  --from-literal=API_ID=... --from-literal=API_HASH=... \
  --from-literal=DATABASE_ENCRYPTION_KEY=... --from-literal=BEARER_TOKENS=... \
  --from-literal=TGW_S3_ACCESS_KEY_ID=... --from-literal=TGW_S3_SECRET_ACCESS_KEY=...
helm install tgw deploy/helm/telegram-rest-gateway \
  --set 'accounts[0].sessionId=<account_id>'
```

## Метрики

`GET /metrics` — Prometheus text format (без Bearer): auth-состояние, WS-подписчики,
inflight моста, счётчики HTTP/апдейтов.

## Веб-интерфейс входа

`GET /ui` — самодостаточная страница логина (без Bearer-фильтра; токен вводится в форме):
выбор метода **QR-код** (авто-обновление при ротации ссылки TDLib) или **телефон**
(номер → код → 2FA). После авторизации показывает аккаунт.

## OpenAPI

Спецификация всех эндпоинтов: [`docs/openapi.yaml`](docs/openapi.yaml).

## Лицензия

[MIT](LICENSE).
