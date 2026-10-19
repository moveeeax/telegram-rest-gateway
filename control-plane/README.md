# control-plane

Сервис онбординга по инвайту. Владелец создает приглашение, клиент открывает ссылку и сканирует
QR, control plane сам поднимает воркер гейтвея аккаунта в namespace `tgw-accounts` и выдает
клиенту токен для `tg.tarassov.me`.

Дизайн: `docs/superpowers/specs/2026-10-18-multi-account-single-endpoint-design.md`.
План: `docs/superpowers/plans/2026-10-18-control-plane.md`.

Порты: `8095` страницы онбординга, `8096` admin API и `/internal/registry` (наружу не выставляется).

```bash
npm ci && npm test        # без Postgres тесты PgStore пропускаются
PG_TEST_URL=postgres://... npm test
```
