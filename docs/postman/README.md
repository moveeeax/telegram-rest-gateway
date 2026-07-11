# Postman

Коллекции и окружения для API гейтвея и архиватора.

## Гейтвей

### Импорт
1. Postman → Import → `telegram-rest-gateway.postman_collection.json`.
2. Импортируй окружение:
   - `tgw-local.postman_environment.json` — `baseUrl=http://localhost:8080` (через `kubectl port-forward`).
   - `tgw-prod.postman_environment.json` — `baseUrl=https://tg-<sessionId>.tarassov.me` (Ingress; доступ только с whitelisted IP).
3. В окружении задай `token` (Bearer). По ходу работы заполняй `chatId`, `messageId`, `fileId`, `userId`.

### Заметки
- Авторизация — на уровне коллекции (Bearer `{{token}}`); health/ready/metrics переопределены на No Auth.
- `Send text` содержит опциональный заголовок `Idempotency-Key` (по умолчанию выключен; значение `{{$guid}}`).
- `Upload` — вкладка Body → binary → выбери файл; query `type=document|photo|video|voice|audio`.
- `Download` — опциональный заголовок `Range: bytes=0-1023` (206).
- WebSocket `/v1/updates` — создай в Postman отдельный **WebSocket Request** к `wss://<host>/v1/updates`
  с заголовком `Authorization: Bearer <token>`; элемент в коллекции — только документация.
- Коллекция сгенерирована из `docs/openapi.yaml`; при изменениях API обновляй оба.

## Архиватор

Отдельный сервис (HTTP :8090): статистика, полнотекстовый поиск по истории и бэкфилл.

### Импорт
1. Postman → Import → `archiver.postman_collection.json`.
2. Импортируй окружение:
   - `archiver-local.postman_environment.json` — `baseUrl=http://localhost:8090` (через `kubectl -n tgw port-forward svc/tgw-tools-archiver 8090:8090`).
   - `archiver-prod.postman_environment.json` — `baseUrl=https://tgw-archive.tarassov.me` (Ingress; доступ только с whitelisted IP).
3. В окружении задай `token` — это `ARCHIVER_TOKEN` (`kubectl -n tgw get secret tgw-tools -o jsonpath='{.data.ARCHIVER_TOKEN}' | base64 -d`).

### Запросы
- **System → Health** — живость (No Auth).
- **System → Stats** — `messages`/`chats`/`processed_events` + статус медиа-оффлоада.
- **Search → Search history** — `?q=…`; опциональные `chat_id`/`session_id` выключены по умолчанию — включи во вкладке Params.
- **Backfill → Start** — POST; в теле задай `gateway_url`/`token`/`session_id` (переменные `gatewayUrl`/`gatewayToken`/`sessionId`). `token` в теле — это Bearer **гейтвея** (read), не архиватора.
- **Backfill → Status** — GET; стейт in-memory, обнуляется при рестарте пода.
