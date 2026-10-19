# Postman

Коллекции и окружения для API гейтвея и архиватора.

## Гейтвей

### Импорт
1. Postman → Import → `telegram-rest-gateway.postman_collection.json`.
2. Импортируй окружение:
   - `tgw-prod.postman_environment.json` — `baseUrl=https://tg.tarassov.me`, общий хост всех аккаунтов.
     В `token` клиентский токен вида `<accountId>.<secret>`: по префиксу Traefik выбирает воркер.
   - `tgw-local.postman_environment.json` — `baseUrl=http://localhost:8080` через port-forward к воркеру:
     `kubectl -n tgw-accounts port-forward svc/tgw-<accountId> 8080:8080`. Нужен для папки Auth,
     `/v1/ready` и `/metrics`, которых на общем хосте нет. Токен — `ADMIN_TOKEN` воркера:
     `kubectl -n tgw-accounts get secret tgw-<accountId> -o jsonpath='{.data.ADMIN_TOKEN}' | base64 -d`.
3. По ходу работы заполняй `chatId`, `messageId`, `fileId`, `userId`.

### Заметки
- Авторизация — на уровне коллекции (Bearer `{{token}}`); health/ready/metrics переопределены на No Auth.
- `Send text` содержит опциональный заголовок `Idempotency-Key` (по умолчанию выключен; значение `{{$guid}}`).
- `Upload` — вкладка Body → binary → выбери файл; query `type=document|photo|video|voice|audio`.
- `Download` — опциональный заголовок `Range: bytes=0-1023` (206).
- WebSocket `/v1/updates` — создай в Postman отдельный **WebSocket Request** к `wss://tg.tarassov.me/v1/updates`
  с заголовком `Sec-WebSocket-Protocol: bearer.<token>` или `Authorization: Bearer <token>`; элемент в коллекции —
  только документация. Первый кадр `{"type":"hello","session_id":"<accountId>"}`.
- На общем хосте любой запрос, включая `/v1/health`, нужен с токеном: без него Traefik не знает, к какому воркеру вести, и отвечает 404.
- На общем хосте открыты только `/v1/chats`, `/v1/me`, `/v1/users`, `/v1/contacts`, `/v1/resolve`, `/v1/search`,
  `/v1/files`, `/v1/updates`, `/v1/health`. Остальное отдает 404 в любом регистре.
- Коллекция сгенерирована из `docs/openapi.yaml`; при изменениях API обновляй оба.

## Архиватор

Внутренний сервис (HTTP :8090), наружу не выставлен. Доступ только через port-forward:
`kubectl -n tgw port-forward svc/tgw-tools-archiver 8090:8090`.

### Импорт
1. Postman → Import → `archiver.postman_collection.json` и `archiver-local.postman_environment.json`.
2. `token` — `ARCHIVER_TOKEN` (токен MCP, только поиск):
   `kubectl -n tgw get secret tgw-tools -o jsonpath='{.data.ARCHIVER_TOKEN}' | base64 -d`.
3. `adminToken` — `ARCHIVER_ADMIN_TOKEN` (статистика, бэкфилл, повтор медиа, purge):
   `kubectl -n tgw get secret tgw-tools -o jsonpath='{.data.ARCHIVER_ADMIN_TOKEN}' | base64 -d`.
4. `sessionId` — `accountId` воркера control plane.

### Запросы
- **System → Health** — живость (No Auth).
- **System → Stats** — счетчики и медиа (`uploaded`, `failed`, `pending`), `skipped_not_archived`. Admin.
- **Search → Search history** — `?q=…&session_id=…`, без `session_id` 400.
- **Backfill → Start** — тело `{"session_id": "..."}`: адрес гейтвея и токен из реестра. Admin.
- **Backfill → Status** — Admin.
- **Admin → Media retry** — повтор оффлоада для строк без `media_url`.
- **Admin → Purge account** — удаление архива аккаунта (обычно его вызывает control plane при удалении).

Архиватор пишет только аккаунты с `archive_enabled` (флаг в control plane, по умолчанию включен).

## Control plane

Инвайты и аккаунты (admin API, наружу не выставлен).

### Импорт
1. Postman → Import → `control-plane.postman_collection.json` и `control-plane-local.postman_environment.json`.
2. `kubectl -n tgw port-forward svc/tgw-tools-control-plane 18096:8096`.
3. `adminToken`: `kubectl -n tgw get secret tgw-control-plane -o jsonpath='{.data.CP_ADMIN_TOKEN}' | base64 -d`.

### Запросы
- **Invites → Create invite** — ссылка `https://join.tarassov.me/i/<code>` на 24 ч.
- **Accounts → List** — аккаунты, статусы и `tgw_ready`; `accountId` отсюда подставляй в остальные запросы.
- **Accounts → Set flags** — `archive_enabled`, `export_enabled`.
- **Accounts → Reissue client token** — ссылка `https://join.tarassov.me/t/<code>`, новый токен показывается один раз.
- **Accounts → Relogin invite** — для `needs_relogin`.
- **Accounts → Delete** — logout, остановка воркера, зачистка S3 и Secret.

## Проверка через curl

```bash
TOKEN='<accountId>.<secret>'   # клиентский токен со страницы онбординга
curl -s -H "Authorization: Bearer $TOKEN" https://tg.tarassov.me/v1/health   # без токена 404: маршрут выбирается по префиксу токена
curl -s -H "Authorization: Bearer $TOKEN" https://tg.tarassov.me/v1/me
curl -s -H "Authorization: Bearer $TOKEN" 'https://tg.tarassov.me/v1/chats?limit=5'
curl -s -H "Authorization: Bearer $TOKEN" 'https://tg.tarassov.me/v1/search?q=привет&limit=5'
# Изоляция: чужой или неизвестный префикс и служебные пути дают 404
curl -s -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer azzzzzzzzzz.${TOKEN#*.}" https://tg.tarassov.me/v1/me
curl -s -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer $TOKEN" https://tg.tarassov.me/v1/auth/state
# WebSocket: первый кадр hello с session_id аккаунта
curl -s -N --http1.1 -m 5 -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
  -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' -H "Sec-WebSocket-Protocol: bearer.$TOKEN" \
  https://tg.tarassov.me/v1/updates | head -c 200; echo

# Admin API control plane
kubectl -n tgw port-forward svc/tgw-tools-control-plane 18096:8096 &
ADM=$(kubectl -n tgw get secret tgw-control-plane -o jsonpath='{.data.CP_ADMIN_TOKEN}' | base64 -d)
curl -s -H "Authorization: Bearer $ADM" localhost:18096/admin/accounts
curl -s -X POST -H "Authorization: Bearer $ADM" localhost:18096/admin/invites
```

## MCP

Единый MCP: `https://mcp.tarassov.me/mcp`, ключ в `X-API-Key` (клиентский токен аккаунта или токен
владельца `owner.<secret>`). Postman умеет MCP-запросы (New → MCP Request, транспорт HTTP);
коллекции для MCP в репозитории нет. Проверка через curl:

```bash
U=https://mcp.tarassov.me/mcp; KEY='<token>'
H=(-H 'content-type: application/json' -H 'accept: application/json, text/event-stream' -H "x-api-key: $KEY")
SID=$(curl -s -D - -o /dev/null -X POST $U "${H[@]}" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}' \
  | grep -i '^mcp-session-id' | awk '{print $2}' | tr -d '\r')
curl -s -o /dev/null -X POST $U "${H[@]}" -H "mcp-session-id: $SID" -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
curl -s -X POST $U "${H[@]}" -H "mcp-session-id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"telegram_list_accounts","arguments":{}}}'
```
