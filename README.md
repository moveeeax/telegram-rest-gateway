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
Dockerfile.tdlib        — кэш-образ со статической TDLib (пин TDLIB_REF)
Dockerfile              — основной multi-stage → distroless
```

## Сборка (в контейнере)

```bash
# 1. Зафиксировать TDLIB_REF (полный git-SHA!) и DROGON_REF.
# 2. Собрать кэш-образ TDLib (долго; только при смене пина):
docker build -f Dockerfile.tdlib --build-arg TDLIB_REF="$(grep -oP '(?<=^TDLIB_REF=).*' TDLIB_REF)" -t tdlib-base:pinned .
# 3. Собрать сервис:
docker build --build-arg TDLIB_REF=pinned -t telegram-rest-gateway .
```

Локальная сборка вне Docker требует установленных TDLib (`Td::TdStatic`), Drogon и
OpenSSL 3.0; конфигурация — через `CMakePresets.json` (`cmake --preset dev-debug`).

## Конфигурация (env)

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `TGW_LISTEN_ADDRESS` | `127.0.0.1` | Адрес прослушивания (в Docker — `0.0.0.0`) |
| `TGW_LISTEN_PORT` | `8080` | Порт HTTP |

Секреты (`api_id`, `api_hash`, `database_encryption_key`, Bearer-токены) — только через
`*_FILE` / secret manager, никогда в образ/env напрямую (появятся на этапе 2).

## Лицензия

TODO.
