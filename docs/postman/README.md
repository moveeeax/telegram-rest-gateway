# Postman

Коллекция и окружения для API гейтвея.

## Импорт
1. Postman → Import → `telegram-rest-gateway.postman_collection.json`.
2. Импортируй окружение:
   - `tgw-local.postman_environment.json` — `baseUrl=http://localhost:8080` (через `kubectl port-forward`).
   - `tgw-prod.postman_environment.json` — `baseUrl=https://tg-<sessionId>.tarassov.me` (Ingress; доступ только с whitelisted IP).
3. В окружении задай `token` (Bearer). По ходу работы заполняй `chatId`, `messageId`, `fileId`, `userId`.

## Заметки
- Авторизация — на уровне коллекции (Bearer `{{token}}`); health/ready/metrics переопределены на No Auth.
- `Send text` содержит опциональный заголовок `Idempotency-Key` (по умолчанию выключен; значение `{{$guid}}`).
- `Upload` — вкладка Body → binary → выбери файл; query `type=document|photo|video|voice|audio`.
- `Download` — опциональный заголовок `Range: bytes=0-1023` (206).
- WebSocket `/v1/updates` — создай в Postman отдельный **WebSocket Request** к `wss://<host>/v1/updates`
  с заголовком `Authorization: Bearer <token>`; элемент в коллекции — только документация.
- Коллекция сгенерирована из `docs/openapi.yaml`; при изменениях API обновляй оба.
