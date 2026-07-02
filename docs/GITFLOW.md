# Git-flow проекта

Проект использует классическую модель **git-flow** (Vincent Driessen), реализованную
на «голом» `git` — CLI `git-flow` ставить не обязательно.

## Постоянные ветки

| Ветка     | Назначение                                                        | Куда мёржится |
|-----------|-------------------------------------------------------------------|---------------|
| `main`    | Только production-релизы. Каждый коммит — тегированный релиз.      | —             |
| `develop` | Интеграционная ветка. Сюда стекается вся завершённая работа.       | → `release/*` |

`main` защищена: прямые пуши запрещены, только merge из `release/*` и `hotfix/*`.

## Временные ветки

| Префикс     | От чего   | Куда мёржится        | Назначение                                  |
|-------------|-----------|----------------------|---------------------------------------------|
| `feature/*` | `develop` | `develop`            | Новая функциональность, рефакторинг, доки.  |
| `release/*` | `develop` | `main` **и** `develop` | Стабилизация перед релизом (bump, фиксы).  |
| `hotfix/*`  | `main`    | `main` **и** `develop` | Срочные правки прод-бага.                    |

Именование: `feature/<краткое-описание>`, `release/<версия>`, `hotfix/<версия>`.

## Типовые команды

### Feature
```bash
git switch develop && git pull
git switch -c feature/my-thing develop
# ...работа, коммиты...
git switch develop
git merge --no-ff feature/my-thing      # сохраняем узел ветки в истории
git branch -d feature/my-thing
git push origin develop
```

### Release
```bash
git switch -c release/1.0.0 develop
# только багфиксы, bump версии, changelog
git switch main && git merge --no-ff release/1.0.0
git tag -a v1.0.0 -m "Release 1.0.0"
git switch develop && git merge --no-ff release/1.0.0
git branch -d release/1.0.0
git push origin main develop --tags
```

### Hotfix
```bash
git switch -c hotfix/1.0.1 main
# правка + bump
git switch main && git merge --no-ff hotfix/1.0.1
git tag -a v1.0.1 -m "Hotfix 1.0.1"
git switch develop && git merge --no-ff hotfix/1.0.1
git branch -d hotfix/1.0.1
git push origin main develop --tags
```

## Merge Requests (GitLab)

Рабочий процесс — через MR, а не прямой merge в консоли, когда есть ревью:
- `feature/*` → `develop`: MR с ревью, squash по желанию.
- `release/*` / `hotfix/*` → `main`: MR обязателен, после — тег.

## Коммиты

- Заголовок в повелительном наклонении, ≤ 72 символов (`Add correlation map`, `Fix WS fan-out race`).
- Тело — через пустую строку, «почему», а не «что».
- Атомарность: один коммит — одно логическое изменение.
- Версионирование релизов — [SemVer](https://semver.org): `vMAJOR.MINOR.PATCH`.

## Соответствие этапам разработки

Каждый этап из раздела 12 ТЗ = одна или несколько `feature/*` веток, финализируется
`release/*` при достижении вехи (milestone).
