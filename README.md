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
| [`docs/openapi.yaml`](docs/openapi.yaml) | Единый источник истины HTTP-контракта |

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
docs/openapi.yaml       — единый источник истины HTTP-контракта
Dockerfile.builder      — образ тулчейн+TDLib+Drogon (пины TDLIB_REF/DROGON_REF)
Dockerfile              — образ сервиса (FROM builder) → distroless
```

CI/CD — GitHub Actions ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): lint (clang-format), suppressions-guard, кэшируемый через GHCR builder-образ (TDLib+Drogon), тесты под ASan/TSan, clang-tidy и Release-сборка сервиса. Историческая заметка о переменных прежнего пайплайна — [`docs/CICD.md`](docs/CICD.md).

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
| `BEARER_TOKENS` | — | По строке на токен: `<token>[ <scopes>]`; scopes: `read`,`write`,`admin` (без scopes = все) |
| `TGW_DATABASE_DIR` | `/data/session` | Каталог БД TDLib (`td.binlog`); должен быть на persistent-томе (или восстанавливаться из S3, см. ниже) |
| `TGW_FILES_DIR` | `/data/files` | Каталог файлов TDLib (скачанные/аплоады); тоже должен переживать рестарт пода |
| `TGW_USE_TEST_DC` | `false` (`0`) | `1`/`true` — подключаться к тестовому дата-центру Telegram вместо прод (`use_test_dc` TDLib) |
| `TGW_KEEP_ONLINE` | `false` | `1`/`true` — держать аккаунт online: `setOption("online", true)` после авторизации + переустановка при каждом реконнекте (`connectionStateReady`). Делает last-seen аккаунта видимым 24/7 |
| `TGW_TDLIB_LOG_VERBOSITY` | `1` | Уровень логирования TDLib (0 — тихо, выше — подробнее); см. `Td::TdStatic` |
| `TGW_MAX_UPLOAD_BYTES` | `67108864` (64 MiB) | Лимит тела `POST /v1/chats/{chatId}/files` (`setClientMaxBodySize`); больше — `413` |
| `TGW_WS_MAX_PENDING_BYTES` | `8388608` | WS back-pressure: лимит байт с последнего pong; 0 — выкл |
| `TGW_MAX_MEMORY_BODY_BYTES` | `1048576` | Порог spool-на-диск для тел запросов (RSS при аплоадах) |
| `TGW_KAFKA_BROKERS` | — | Kafka/Redpanda bootstrap; пусто — события в Kafka выключены |
| `TGW_KAFKA_TOPIC` | `tgw.updates` | Топик событий |
| `TGW_KAFKA_CLIENT_ID` | `tgw-<session_id>` | client.id продюсера |
| `TGW_HUMANIZE_TYPING` | `false` | `1`/`true` — перед отправкой текстового `POST /v1/chats/{chatId}/messages` показать "печатает…" и выдержать паузу, пропорциональную длине текста |
| `TGW_HUMANIZE_CHARS_PER_MINUTE` | `200` | Скорость "печати" для расчёта базовой паузы: `base_ms = chars / (chars_per_minute / 60000)` |
| `TGW_HUMANIZE_JITTER_PERCENT` | `20` | Разброс паузы в процентах вокруг базового значения (`uniform(1 − jitter, 1 + jitter)`) |
| `TGW_HUMANIZE_MIN_DELAY_MS` | `1000` | Нижняя граница паузы после применения jitter (`clamp()`) |
| `TGW_HUMANIZE_MAX_DELAY_MS` | `10000` | Верхняя граница паузы после применения jitter (`clamp()`) |
| `TGW_HUMANIZE_ID_WAIT_MS` | `4000` | Сколько ждать подтверждение (`updateMessageSendSucceeded`/`Failed`) реального id перед тем, как отдать `202 Accepted` без ожидания |
| `TGW_IDLE_CONNECTION_TIMEOUT_SECONDS` | `90` | Idle-таймаут Drogon-соединения; должен покрывать худший случай humanize-паузы (`max_delay_ms + id_wait_ms` + запас 2000мс) — иначе старт упадёт с ошибкой конфигурации |

**Онлайн-статус (`TGW_KEEP_ONLINE`).** По умолчанию TDLib держит аккаунт offline. При включении
gateway после авторизации шлёт `setOption("online", true)` и переустанавливает его при каждом
восстановлении соединения (`connectionStateReady`), чтобы статус пережил реконнекты. Учти: аккаунт
будет виден как online (и last-seen обновляется) круглосуточно, пока процесс жив. При штатном
завершении спец-действий не нужно — TDLib на `close` сам выставит offline.

**Имитация человеческой печати (`TGW_HUMANIZE_TYPING`).** По умолчанию выключено — поведение
`POST /v1/chats/{chatId}/messages` не меняется (`202 Accepted` + `temporary_message_id` +
`sending_state: pending`, финал по WS). При включении гейтвей перед реальной отправкой:

1. Считает базовую паузу по длине текста (в UTF-8 кодпоинтах, не байтах — иначе кириллица/эмодзи
   завышают паузу): `base_ms = длина / (TGW_HUMANIZE_CHARS_PER_MINUTE / 60000)`.
2. Применяет случайный разброс `± TGW_HUMANIZE_JITTER_PERCENT %` и приводит результат к границам
   `[TGW_HUMANIZE_MIN_DELAY_MS, TGW_HUMANIZE_MAX_DELAY_MS]`.
3. На всё время паузы шлёт `sendChatAction(chatActionTyping)` (аккаунт виден собеседнику как
   "печатает…"), обновляя действие каждые 4с — TDLib гасит статус печати сам примерно через 5с.
4. После паузы вызывает `sendMessage` как обычно и до `TGW_HUMANIZE_ID_WAIT_MS` мс ждёт
   подтверждение реального id (`updateMessageSendSucceeded`/`Failed`) от TDLib.

Контракт ответа в результате двойной: если подтверждение успело прийти в окно ожидания —
`200 OK` с полным объектом сообщения (`Message`, реальный `id`) и `sending_state: "sent"`; если
не успело (таймаут) или флаг выключен — как и раньше, `202 Accepted` с `temporary_message_id` и
`sending_state: "pending"` (финал по WS). Клиенту следует обрабатывать оба кода на этом эндпоинте.

При старте гейтвей проверяет, что худший случай паузы влезает в `TGW_IDLE_CONNECTION_TIMEOUT_SECONDS`
(`TGW_HUMANIZE_MAX_DELAY_MS + TGW_HUMANIZE_ID_WAIT_MS` + запас 2000мс) — иначе падает с понятной
ошибкой конфигурации при загрузке, не давая заведомо рвущимся соединениям уйти в прод.

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

**Scoped-токены.** Строка `BEARER_TOKENS` вида `tgw_xxx read` выдаёт токен только на чтение
(GET + WS), `read,write` — плюс мутации, но без `/v1/auth/*` (логин и session export — только
`admin`/полный токен). Недостаточный скоуп → `403 INSUFFICIENT_SCOPE`. Агентам (MCP) выдавайте
минимально необходимый скоуп. Маршруты Drogon матчатся без учёта регистра, поэтому и проверка
префикса `/v1/auth/` регистронезависима: `GET /V1/Auth/session/export` тоже требует `admin`.

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

[`docs/openapi.yaml`](docs/openapi.yaml) — единый источник истины HTTP-контракта (все эндпоинты,
конверты, коды ошибок).

## Лицензия

[MIT](LICENSE).
