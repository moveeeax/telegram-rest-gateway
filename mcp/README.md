# telegram-rest-gateway MCP

MCP-сервер поверх REST API гейтвея: любой MCP-клиент (Claude Code, Claude Desktop и др.)
получает 14 инструментов Telegram-аккаунта — чтение/отправка/правка сообщений, реакции,
пересылка, резолв @username, карточки чатов/юзеров, скачивание медиа (картинки агент видит).

Один MCP-сервер = один аккаунт (один инстанс гейтвея, модель `TGW_SESSION_ID`).

## Подключение к Claude Code

Через docker (ничего не устанавливая):

```bash
claude mcp add telegram -e TGW_BASE_URL=http://host.docker.internal:8080 \
  -e TGW_BEARER_TOKEN=tgw_ваш_токен \
  -- docker run -i --rm --add-host host.docker.internal:host-gateway \
     -e TGW_BASE_URL -e TGW_BEARER_TOKEN telegram-rest-gateway-mcp
```

Или нативно (нужен node ≥ 20):

```bash
cd mcp && npm ci && npm run build
claude mcp add telegram -e TGW_BASE_URL=http://127.0.0.1:8080 \
  -e TGW_BEARER_TOKEN=tgw_ваш_токен -- node /абсолютный/путь/mcp/dist/index.js
```

## Инструменты

`telegram_get_me` · `telegram_get_chats` · `telegram_get_history` · `telegram_send_message`
(markdown/html, reply, idempotency) · `telegram_edit_message` · `telegram_delete_messages` ·
`telegram_forward_messages` · `telegram_react` · `telegram_resolve_username` ·
`telegram_get_chat` · `telegram_get_user` · `telegram_get_contacts` ·
`telegram_download_file` · `telegram_mark_read`

## Безопасность

Выдавай агенту токен с минимальным скоупом (строка `BEARER_TOKENS` гейтвея):
`tgw_agent read` — только чтение; `tgw_agent read,write` — чтение и отправка, но без
`/v1/auth/*` (session export недоступен). Полный токен = полный захват аккаунта.

### HTTP-режим (`MCP_HTTP_PORT`)

При запуске со Streamable HTTP-транспортом (`MCP_HTTP_PORT=<порт>`) обязателен
`MCP_HTTP_TOKEN` — без него процесс не стартует (fail-closed): `/mcp` иначе проксирует
к гейтвею с его полным токеном для любого, кто достучится до порта. Явно принять этот
риск (доверенная сеть, отладка) можно через `MCP_HTTP_ALLOW_INSECURE=1`.

`MCP_SESSION_TTL_MS` — таймаут простоя HTTP-сессии до автозакрытия (по умолчанию 30 минут).
Клиенты, не присылающие `DELETE /mcp`, не копят сессии в памяти вечно.
