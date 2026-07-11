# telegram-rest-gateway archiver

Консьюмер Kafka-событий гейтвея → архив переписки в SQLite с полнотекстовым поиском (FTS5).
Даёт то, чего нет в Telegram/TDLib API: поиск по **всей** сохранённой истории всех чатов
одним запросом, включая отредактированные (обновляются) и удалённые (soft-delete) сообщения.

```
gateway → Kafka(tgw.updates) → archiver → SQLite+FTS5 → HTTP /search → MCP-tool агента
```

## Запуск

```bash
docker build -t telegram-rest-gateway-archiver archiver/
docker run -d --name tgw-archiver --restart unless-stopped --network <сеть-с-брокером> \
  -p 8090:8090 -v tgw-archive:/data \
  -e ARCHIVER_KAFKA_BROKERS=redpanda:9092 \
  telegram-rest-gateway-archiver
```

| Переменная | Default | Назначение |
|---|---|---|
| `ARCHIVER_KAFKA_BROKERS` | `redpanda:9092` | Bootstrap |
| `ARCHIVER_KAFKA_TOPIC` | `tgw.updates` | Топик событий гейтвея |
| `ARCHIVER_KAFKA_GROUP` | `tgw-archiver` | Consumer group (offset'ы хранит Kafka) |
| `ARCHIVER_DB_PATH` | `/data/archive.db` | Файл SQLite (нужен volume) |
| `ARCHIVER_HTTP_PORT` | `8090` | Порт HTTP |
| `ARCHIVER_TOKEN` | — | Bearer для /search и /stats (пусто = без auth) |

## HTTP

- `GET /search?q=слова&chat_id=&session_id=&limit=` — FTS-поиск (AND), сниппеты с `<<подсветкой>>`,
  сортировка по дате. Мультиаккаунт: события всех инстансов в одном топике различаются `session_id`.
- `GET /stats` — счётчики сообщений/чатов/обработанных событий.
- `GET /health` — живость (без auth).

## Семантика

- Kafka at-least-once → записи идемпотентны (PK `session_id+chat_id+message_id`, upsert).
- `updateMessageSendSucceeded` заменяет временный id финальным; правки обновляют текст;
  удаления — soft-delete (`deleted=1`, из поиска не скрываются — архив помнит).
- Дыры в `seq` логируются (потери канала видны и по `tgw_kafka_dropped_total` гейтвея).

## MCP

MCP-сервер (`mcp/`) при заданном `TGW_ARCHIVER_URL` (+опц. `TGW_ARCHIVER_TOKEN`) регистрирует
инструмент `telegram_search_history` — агент ищет по архиву и переходит к контексту через
`telegram_get_history`.


## Бэкенды и медиа

- **Хранилище:** SQLite (дефолт) или PostgreSQL — задай `ARCHIVER_PG_URL` (`postgres://...`).
  PG использует `tsvector`+GIN и `websearch_to_tsquery('simple', q)` со сниппетами `ts_headline`.
- **Медиа в S3:** если заданы `ARCHIVER_S3_BUCKET` + `ARCHIVER_GATEWAY_TEMPLATE`, при каждом
  сообщении с `file_id` файл качается из гейтвея (`GET /v1/files/{id}`) и кладётся в S3
  (`ARCHIVER_S3_PREFIX<session>/<chat>/<message>/<file>`), в строку пишется `media_url`.
  Выделенный воркер с очередью — не блокирует consumer. Env: `ARCHIVER_S3_ENDPOINT/_REGION/
  _BUCKET/_ACCESS_KEY_ID/_SECRET_ACCESS_KEY/_PREFIX/_PUBLIC_BASE`, `ARCHIVER_S3_FORCE_PATH_STYLE`
  (default: true; `false` → virtual-hosted AWS), `ARCHIVER_GATEWAY_TOKEN`, `ARCHIVER_MEDIA_MAX_BYTES`
  (default 100 MiB). `media_url` = `<PUBLIC_BASE>/<key>` или `s3://bucket/key`.
  Ретраи media: до **5 attempts** на job (202 pending, 5xx/network/timeout/S3 —
  requeue в конец очереди). 401/403/404 — permanent fail (ключ в `seen`, без цикла).
  После исчерпания attempts — `failed++`, ключ снимается (`seen`), повтор при следующем
  событии. Oversized — постоянный skip до рестарта. Kafka: storage-ошибки ретраятся
  с бэкоффом (5 попыток + heartbeat), затем drop+commit (`dropped_events` в `/stats`);
  **N подряд drop'ов** (`ARCHIVER_DROP_CIRCUIT`, default 20) — fail-closed (process exit,
  lag растёт, k8s рестартит; не «молча» проглатываем топик при outage PG). Битый JSON /
  non-object — poison pill, коммитится.
- **Бэкфилл** пишет в текущий store и триггерит медиа-оффлоад — так наполняется свежая PG-база.
  `gateway_url` ограничен: должен совпадать с `ARCHIVER_GATEWAY_TEMPLATE` (если задан) либо
  быть localhost / cluster DNS / private IP. Через внешний Ingress `/backfill` не публикуется.
