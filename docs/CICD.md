# CI/CD

Пайплайн — **GitHub Actions** (`.github/workflows/ci.yml`). Мигрировал с GitLab CI.
Порядок: lint → сборка тулчейна (builder) → тесты (ASan/TSan/tidy/exe) + сборка TS-сайдкаров
→ публикация образов. Деплой пока не автоматизирован (решение: только сборка+пуш).

## Реестр

Все образы публикуются в **GHCR** (`ghcr.io`) под неймспейсом репозитория
(`ghcr.io/moveeeax/telegram-rest-gateway`). Аутентификация — штатным `GITHUB_TOKEN`
(`permissions: packages: write`), отдельные секреты не нужны.

> **Смена реестра.** Раньше (на GitLab) образы уходили во внешний реестр
> `docker.io/resert` через секреты `REGISTRY_*`. Он **больше не используется**:
> последний образ там старше обоих секьюрити-фиксов. Секретов Docker Hub в GitHub-репо
> нет и не планируется — публикуем только в GHCR под `GITHUB_TOKEN`.

## Обзор пайплайна

| Джоб | Раннер | Когда | Что делает |
|---|---|---|---|
| `lint-format` | ubuntu-latest | каждый push/PR | `clang-format --dry-run --Werror` |
| `suppressions-guard` | ubuntu-latest | каждый push/PR | наш код не должен попадать в sanitizer-suppressions |
| `node-build` (matrix: archiver, mcp) | ubuntu-latest | каждый push/PR | `npm ci && npm run build` (node 22) + `docker build` образа сайдкара **без пуша** |
| `builder` | ubuntu-latest | каждый push/PR | собирает/переиспользует тулчейн-образ (см. ниже) |
| `test-asan` | ubuntu-latest (в builder-образе) | каждый push/PR | ASan+LSan юнит-тесты |
| `test-tsan` | ubuntu-latest (в builder-образе) | каждый push/PR | TSan юнит-тесты |
| `tidy` | ubuntu-latest (в builder-образе) | каждый push/PR | `clang-tidy` (bugprone-*, concurrency-*) |
| `build-app` | ubuntu-latest (в builder-образе) | каждый push/PR | полный Release-exe сервиса |
| `publish` | ubuntu-latest | push в `main` / git-тег `v*` | сборка+пуш трёх образов (см. ниже) |

### Тулчейн-образ (`builder`) — content-addressed тег

Дорогой образ TDLib+Drogon (`Dockerfile.builder`). Раньше пушился как мутабельный
`:latest` с каждой ветки → кросс-веточное отравление и гонки. Теперь тег
**контентно-адресуемый**:

```
ghcr.io/<repo>/builder:<hash>,   hash = sha256(Dockerfile.builder + TDLIB_REF + DROGON_REF)[:16]
```

- `TDLIB_REF`/`DROGON_REF`/`Dockerfile.builder` **не менялись** → тот же тег → джоб
  `builder` видит образ в GHCR и **ничего не собирает**; downstream-джобы тянут готовый.
- что-то из трёх **изменилось** → новый тег → образ пересобирается и пушится.
- `:latest` дополнительно ставится **только с `main`** (удобный указатель на «последний
  тулчейн», сам пайплайн им не пользуется — потребители ссылаются на content-тег).

Пины `TDLIB_REF`/`DROGON_REF` — файлы в корне репозитория (единый источник истины для
CMake и Docker). `TDLIB_REF` **обязан быть полным git-SHA** (не веткой), иначе сборки
невоспроизводимы. Ручная сборка `Dockerfile.builder` без `--build-arg` **падает на guard**
(дефолтов у ARG нет) — так исключён неверный тулчейн:

```sh
docker build -f Dockerfile.builder \
  --build-arg TDLIB_REF="$(grep -E '^TDLIB_REF=' TDLIB_REF | cut -d= -f2)" \
  --build-arg DROGON_REF="$(grep -E '^DROGON_REF=' DROGON_REF | cut -d= -f2)" .
```

> **Видимость пакета — обязательно публичный.** Пакет `builder` в GHCR должен быть
> **публичным**; секретов в образе нет. Тогда любой PR, включая форки, тянет тулчейн без
> кредов. Пока пакет **приватный**, токен форк-PR не имеет доступа к пакетам base-репо, и
> `docker buildx imagetools inspect` падает для **любого** content-тега (не только для PR,
> правящих пины) → `available=false` → **все** C++-джобы форк-PR (`test-*`/`tidy`/`build-app`)
> скипаются с зелёным `skipped`-статусом. Для внешнего контрибьютора это ложная уверенность:
> тесты выглядят «прошли», хотя не запускались. Публичность пакета — обязательное условие
> работоспособности форк-PR (см. «Форки» и чеклист «Первого запуска», п.4).

### Публикация образов (`publish`)

Срабатывает **только** на `push` в `main` или на git-тег `v*` (из PR, в т.ч. форков, — нет,
поэтому read-only токен форка пайплайн не красит). Гейт: только после зелёных
`test-asan`/`test-tsan`/`tidy`/`build-app` — непроверенное не публикуем. Три образа:

| Dockerfile | Образ |
|---|---|
| `Dockerfile` (корневой сервис) | `ghcr.io/<repo>` |
| `archiver/Dockerfile` | `ghcr.io/<repo>-archiver` |
| `mcp/Dockerfile` | `ghcr.io/<repo>-mcp` |

Теги: `:<short-sha>` — всегда; дополнительно `:vX.Y.Z` — на git-теге.

> **⚠️ Пока только amd64 (arm64 — TODO).** Старый GitLab-пайплайн собирал multi-arch
> (amd64+arm64) нативно на per-arch раннерах и склеивал манифест. Корневой `Dockerfile`
> делает `FROM ${BUILDER_IMAGE}`, а тулчейн-образ **нативен per-arch** (сборка TDLib под
> QEMU-эмуляцию нереалистична по времени и памяти — `td_api.cpp` требует ≥8 ГБ RAM).
> Значит, arm64-образ сервиса потребовал бы отдельного **arm64-builder'а**. Публичные
> arm64-раннеры (`ubuntu-24.04-arm`) в GitHub есть, но бутстрап arm64-тулчейна внутри
> publish-джоба (который нельзя прогнать до мержа) — заметный риск и время. Приоритет
> задачи — **вернуть публикацию как таковую** (образов не было вовсе, оба секьюрити-фикса
> не выпущены), поэтому publish сейчас **amd64-only**.
>
> **TODO (arm64):** завести джоб `builder-arm64` на `ubuntu-24.04-arm` (тот же
> content-addressed тег с суффиксом `-arm64`, собирается при отсутствии), publish-джоб
> для arm64 на arm64-раннере с `--build-arg BUILDER_IMAGE=<builder>:<hash>-arm64`, и
> `docker buildx imagetools create` для склейки `:<short-sha>` из per-arch образов —
> как `image:manifest` в старом `.gitlab-ci.yml`.

### Форки

Всё нижеследующее для форк-PR предполагает, что пакет `builder` **публичный** (см. врезку
«Видимость пакета»). Пока он приватный, токен форк-PR не читает пакеты base-репо: `imagetools
inspect` падает для любого тега → `available=false` → **все** C++-джобы форк-PR скипаются
(зелёный `skipped`), даже если тулчейн давно собран. Это и есть причина, по которой
публичность обязательна.

При публичном пакете:

- **PR из своего репо / push в ветку**: токен пишущий — builder при необходимости
  собирается и пушится, все джобы идут полностью.
- **Форк-PR, builder-тег уже есть** (пины не менялись — обычный случай): downstream тянет
  готовый публичный тулчейн, C++-джобы идут полностью.
- **Форк-PR, builder-тег отсутствует** (PR правит пины/`Dockerfile.builder`): пушить нельзя
  → джоб `builder` собирает образ локально как валидацию рецепта (без пуша), а C++-джобы
  (`test-*`/`tidy`/`build-app`) **скипаются** с честным статусом. Тулчейн-образ должен
  собрать и запушить мейнтейнер (мержем PR или вручную).

## Секреты и переменные

Сейчас пайплайну достаточно штатного `GITHUB_TOKEN` (никаких `REGISTRY_*`/Docker Hub).

### Понадобятся для CD (рантайм-секреты; заводить, когда добавим деплой)

Читаются в контейнере по конвенции `*_FILE` (§11.10). Сейчас пайплайном НЕ используются.

| Ключ | Назначение |
|---|---|
| `API_ID` | Telegram `api_id` (my.telegram.org). |
| `API_HASH` | Telegram `api_hash`. |
| `DATABASE_ENCRYPTION_KEY` | 32-байтный ключ шифрования БД TDLib. **Стабильный, не терять** — утеря = полный ре-логин. |
| `BEARER_TOKENS` | Список Bearer-токенов API. |

## Первый запуск

1. Проставить `TDLIB_REF` = конкретный git-SHA TDLib, при необходимости обновить `DROGON_REF`.
2. Убедиться, что у workflow есть `packages: write` (по умолчанию в `ci.yml` уже задано).
3. Первый push прогонит `builder` (соберёт content-addressed тулчейн-образ), затем тесты.
4. **Обязательно** сделать пакет `builder` публичным (Packages → builder → Package settings
   → Change visibility → Public) сразу после первой сборки образа. Без этого форк-PR не
   могут читать пакет и **все** их C++-джобы молча скипаются (ложно-зелёный статус).
5. Push в `main` → соберутся и запушатся образы сервиса/archiver/mcp с тегом `:<short-sha>`;
   git-тег `vX.Y.Z` добавит версионный тег.
