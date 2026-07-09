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

Токен даёт агенту **полный доступ к аккаунту** (в объёме API гейтвея). Храни его как секрет
и осознанно решай, какому агенту его выдаёшь. Scoped-токены (read-only и т.п.) — в бэклоге.
