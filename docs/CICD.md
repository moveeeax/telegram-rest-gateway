# CI/CD

Пайплайн GitLab CI: линт → тесты (ASan/TSan) → сборка+пуш multi-arch образа.
Деплой пока не автоматизирован (решение: только сборка+пуш). Реестр — внешний.

## Обзор пайплайна

| Стадия | Джоба | Раннер | Когда |
|---|---|---|---|
| `builder` | `builder:amd64`, `builder:arm64` | amd64 / arm64 | **вручную** (при смене `TDLIB_REF`/`DROGON_REF`) |
| `lint` | `clang-format` | amd64 | каждый push |
| `lint` | `suppressions-guard` | arm64 | каждый push |
| `test` | `tidy` (clang-tidy) | amd64 | каждый push |
| `test` | `test:asan` (amd64+arm64) | оба | каждый push |
| `test` | `test:tsan` (amd64+arm64) | оба | каждый push |
| `image` | `image:amd64`, `image:arm64`, `image:manifest` | оба / amd64 | ветки `develop`, `main` |

**Образы в реестре** (`$REGISTRY_IMAGE`):
- `:builder-amd64`, `:builder-arm64` — тулчейн + статические TDLib/Drogon (из `Dockerfile.builder`); пересобираются вручную. Их тянут test/lint-джобы и основной `Dockerfile`.
- `:<short-sha>-amd64`, `:<short-sha>-arm64` + мультиарх-манифест `:<short-sha>`; на `main` ещё `:latest`.

> Порядок первого запуска: сначала прогнать `builder:amd64` и `builder:arm64` (создают
> builder-образы), потом обычный пайплайн сможет их использовать.

## Переменные CI/CD (Settings → CI/CD → Variables)

### Нужны сейчас (сборка + пуш образа)

| Ключ | Тип | Флаги | Пример | Назначение |
|---|---|---|---|---|
| `REGISTRY_HOST` | Variable | protected | `ghcr.io` | Хост реестра для `docker login`. |
| `REGISTRY_IMAGE` | Variable | protected | `ghcr.io/tarassov/telegram-rest-gateway` | Полный путь образа (host/namespace/name). |
| `REGISTRY_USER` | Variable | protected, **masked** | `tarassov` / robot-акк | Логин в реестр. |
| `REGISTRY_TOKEN` | Variable | protected, **masked** | PAT с `write:packages` | Пароль/токен для пуша. |
| `DOCKER_AUTH_CONFIG` | Variable | **НЕ protected** | `{"auths":{"ghcr.io":{"auth":"<base64 user:token>"}}}` | Даёт раннеру тянуть **приватный** builder-образ в `image:`. См. предупреждение ниже. |

> **`DOCKER_AUTH_CONFIG` и защищённость.** Джобы `test:*`/`clang-format`/`tidy` тянут
> приватный `:builder-*` через `image:` и запускаются на ЛЮБЫХ ветках (в т.ч. feature).
> Protected-переменные доступны только на protected-ветках → если пометить `DOCKER_AUTH_CONFIG`
> как protected, пайплайны feature-веток не смогут спуллить builder и упадут на `image:`.
> Варианты: (а) оставить `DOCKER_AUTH_CONFIG` **непротектед**; либо (б) сделать builder-образ
> публичным (тогда переменная вообще не нужна). `REGISTRY_TOKEN` для пуша при этом остаётся
> protected — им пользуются только `builder`/`image`-джобы на protected-ветках.

### Пины (в репозитории, не в GitLab)

`TDLIB_REF`, `DROGON_REF` — файлы в корне. **`TDLIB_REF` обязан быть полным git-SHA**
(не `master`), иначе сборки невоспроизводимы.

### Понадобятся для CD (рантайм-секреты; заводить, когда добавим деплой)

Тип **File**, `protected`, по возможности `masked`. В контейнере читаются по конвенции
`*_FILE` (§11.10). Сейчас пайплайном НЕ используются — резерв.

| Ключ | Тип | Флаги | Назначение |
|---|---|---|---|
| `API_ID` | Variable | protected, masked | Telegram `api_id` (my.telegram.org). |
| `API_HASH` | File/Variable | protected, masked | Telegram `api_hash`. |
| `DATABASE_ENCRYPTION_KEY` | File | protected, masked | 32-байтный ключ шифрования БД TDLib (base64/hex). **Стабильный, не терять** — утеря = полный ре-логин. |
| `BEARER_TOKENS` | File | protected | Список Bearer-токенов API (по строке / JSON). |

### Предопределённые GitLab (заводить НЕ нужно)

`CI_COMMIT_SHORT_SHA`, `CI_COMMIT_BRANCH`, `CI_PROJECT_DIR` — подставляются автоматически.

## Чеклист первого запуска

1. Завести переменные из блока «Нужны сейчас».
2. Проставить `TDLIB_REF` = конкретный git-SHA TDLib, при необходимости обновить `DROGON_REF`.
3. Убедиться, что ветки `develop`/`main` — protected (Settings → Repository → Protected branches).
4. Запустить вручную `builder:amd64` и `builder:arm64`.
5. Обычный push → отработают lint + test; push в `develop`/`main` → соберётся и запушится образ.
