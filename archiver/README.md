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
