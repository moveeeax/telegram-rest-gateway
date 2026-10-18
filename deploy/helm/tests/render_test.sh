#!/usr/bin/env bash
# Render-тесты Helm-чартов: helm template с разными values и проверки по выводу.
# Запуск из корня репо: bash deploy/helm/tests/render_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
GW="$ROOT/deploy/helm/telegram-rest-gateway"
FAILED=0

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1${2:+: $2}"; FAILED=1; }

assert_contains() {
    if grep -qF -- "$3" <<<"$2"; then pass "$1"; else fail "$1" "нет '$3'"; fi
}
# Отрицательные проверки на пустом рендере проходили бы ложно, поэтому пустой вывод = FAIL.
assert_not_contains() {
    [[ -n "$2" ]] || { fail "$1" "пустой рендер"; return; }
    if grep -qF -- "$3" <<<"$2"; then fail "$1" "есть '$3'"; else pass "$1"; fi
}
assert_not_matches() {
    [[ -n "$2" ]] || { fail "$1" "пустой рендер"; return; }
    if grep -qE -- "$3" <<<"$2"; then fail "$1" "совпало '$3'"; else pass "$1"; fi
}
assert_fails() {
    local name="$1"; shift
    if helm template "$@" >/dev/null 2>&1; then fail "$name" "рендер прошел"; else pass "$name"; fi
}
# Рендер, при ошибке пустая строка и FAIL кейса.
render() {
    local name="$1"; shift
    local out
    if out="$(helm template "$@" 2>&1)"; then echo "$out"; else echo "FAIL $name: $out" >&2; return 1; fi
}

GW_ARGS=(t "$GW" --namespace tgw --set image.tag=x --set ingress.enabled=true
         --set-string 'accounts[0].sessionId=111')
MW_KEY='ingress.annotations.traefik\.ingress\.kubernetes\.io/router\.middlewares'

# --- gateway chart ---
assert_fails "guard: no protection" "${GW_ARGS[@]}" --set ingress.allowOpenIngress=false

if render "guard: nginx annotation" "${GW_ARGS[@]}" \
    --set-string 'ingress.annotations.nginx\.ingress\.kubernetes\.io/whitelist-source-range=1.2.3.4/32' >/dev/null; then
    pass "guard: nginx annotation"; else FAILED=1; fi

if render "guard: traefik middlewares" "${GW_ARGS[@]}" --set-string "$MW_KEY=x@kubernetescrd" >/dev/null; then
    pass "guard: traefik middlewares"; else FAILED=1; fi

out="$(render "allowlist" "${GW_ARGS[@]}" --set 'ingress.ipAllowList={203.0.113.7/32}')" || { out=""; FAILED=1; }
assert_contains "allowlist renders middleware: kind" "$out" "kind: Middleware"
assert_contains "allowlist renders middleware: cidr" "$out" "203.0.113.7/32"
assert_contains "allowlist renders middleware: annotation" "$out" \
    "traefik.ingress.kubernetes.io/router.middlewares: tgw-t-ip-allowlist@kubernetescrd"
assert_not_matches "no root path" "$out" "path: /$"
assert_not_contains "no metrics path" "$out" "path: /metrics"
assert_contains "v1 path" "$out" "path: /v1"
assert_not_contains "ui hidden by default" "$out" "path: /ui"
assert_contains "class traefik" "$out" "ingressClassName: traefik"

out="$(render "merged" "${GW_ARGS[@]}" --set 'ingress.ipAllowList={203.0.113.7/32}' \
    --set-string "$MW_KEY=other@kubernetescrd")" || { out=""; FAILED=1; }
assert_contains "middlewares merged" "$out" "tgw-t-ip-allowlist@kubernetescrd,other@kubernetescrd"

out="$(render "ui" "${GW_ARGS[@]}" --set 'ingress.ipAllowList={203.0.113.7/32}' \
    --set ingress.exposeLoginUi=true)" || { out=""; FAILED=1; }
assert_contains "ui exposed on flag" "$out" "path: /ui"

exit "$FAILED"
