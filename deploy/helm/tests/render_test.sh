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

# Защитой считается только ipAllowList (Middleware этого чарта) или явный allowOpenIngress.
# Имя класса не говорит, какой контроллер его читает (в oci-k3s класс nginx обслуживает Traefik),
# а чужая router.middlewares может быть redirect/headers и ничего не ограничивать.
NGINX_WL='ingress.annotations.nginx\.ingress\.kubernetes\.io/whitelist-source-range'
assert_fails "guard: nginx annotation does not count" "${GW_ARGS[@]}" --set ingress.className=nginx \
    --set-string "$NGINX_WL=1.2.3.4/32"
assert_fails "guard: foreign middleware does not count" "${GW_ARGS[@]}" \
    --set-string "$MW_KEY=kube-system-redirect@kubernetescrd"
assert_fails "guard: empty cidr" "${GW_ARGS[@]}" --set-json 'ingress.ipAllowList=[""]'
assert_fails "guard: open ipv4 cidr" "${GW_ARGS[@]}" --set 'ingress.ipAllowList={0.0.0.0/0}'
assert_fails "guard: open ipv6 cidr" "${GW_ARGS[@]}" --set-json 'ingress.ipAllowList=["::/0"]'
if render "guard: allowOpenIngress" "${GW_ARGS[@]}" --set ingress.allowOpenIngress=true >/dev/null; then
    pass "guard: allowOpenIngress"; else FAILED=1; fi

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

# --- tools chart ---
TOOLS="$ROOT/deploy/helm/telegram-rest-gateway-tools"
TOOLS_ARGS=(tt "$TOOLS" --namespace tgw --set archiver.image.tag=x --set mcp.image.tag=x
            --set ingress.enabled=true --set ingress.archiverHost=a.example
            --set-string 'mcp.accounts[0].sessionId=111' --set-string 'mcp.accounts[1].sessionId=222')

assert_fails "tools guard: no protection" "${TOOLS_ARGS[@]}"
assert_fails "tools guard: nginx annotation does not count" "${TOOLS_ARGS[@]}" --set-string "$NGINX_WL=1.2.3.4/32"
assert_fails "tools guard: foreign middleware does not count" "${TOOLS_ARGS[@]}" \
    --set-string "$MW_KEY=kube-system-redirect@kubernetescrd"
assert_fails "tools guard: open cidr" "${TOOLS_ARGS[@]}" --set 'ingress.ipAllowList={0.0.0.0/0}'

out="$(render "tools allowlist" "${TOOLS_ARGS[@]}" --set 'ingress.ipAllowList={203.0.113.7/32}')" || { out=""; FAILED=1; }
# Документы Ingress по одному: awk режет вывод по '---' и берет документ с нужным именем.
doc() { awk -v n="name: $2" 'BEGIN{RS="\n---\n"} index($0, "kind: Ingress") && index($0, n)' <<<"$1"; }
assert_contains "tools allowlist on mcp" "$(doc "$out" tt-mcp-111)" "tgw-tt-ip-allowlist@kubernetescrd"
assert_contains "tools allowlist on archiver" "$(doc "$out" tt-archiver)" "tgw-tt-ip-allowlist@kubernetescrd"
n_mw="$(grep -c '^kind: Middleware' <<<"$out" || true)"
if [[ "$n_mw" == 1 ]]; then pass "tools single middleware"; else fail "tools single middleware" "Middleware: $n_mw"; fi
assert_contains "tools class traefik" "$out" "ingressClassName: traefik"

# Значение берется из строки сразу после имени: grep -F с переводом строки в шаблоне ищет каждую
# строку отдельно и пропустил бы чужой value.
mcp_dep="$(awk -v n="name: tt-mcp-111" 'BEGIN{RS="\n---\n"} index($0, "kind: Deployment") && index($0, n)' <<<"$out")"
assert_contains "mcp session id env" "$(grep -A1 'name: TGW_SESSION_ID' <<<"$mcp_dep" | tail -1)" 'value: "111"'

# Prod-примеры из репо должны проходить guard и давать allowlist (по ним ставят, см. DEPLOY.md).
out="$(render "gw prod example" t "$GW" --namespace tgw -f "$ROOT/deploy/helm/values-prod-example.yaml")" || { out=""; FAILED=1; }
assert_contains "gw prod example: allowlist" "$out" "tgw-t-ip-allowlist@kubernetescrd"
assert_not_contains "gw prod example: no nginx" "$out" "nginx.ingress.kubernetes.io"
out="$(render "tools prod example" tt "$TOOLS" --namespace tgw --set archiver.image.tag=x --set mcp.image.tag=x \
    -f "$TOOLS/values-prod-example.yaml")" || { out=""; FAILED=1; }
assert_contains "tools prod example: allowlist" "$out" "tgw-tt-ip-allowlist@kubernetescrd"

# Архиватор на Postgres: SQLite-том не нужен, PVC не создается (иначе local-path привязал бы под к ноде).
out="$(render "archiver pg" tt "$TOOLS" --namespace tgw --set archiver.image.tag=x --set mcp.enabled=false \
    --set archiver.postgres.enabled=true)" || { out=""; FAILED=1; }
assert_not_contains "archiver pg: no pvc" "$out" "kind: PersistentVolumeClaim"
assert_contains "archiver pg: emptyDir" "$out" "emptyDir: {}"
out="$(render "archiver sqlite" tt "$TOOLS" --namespace tgw --set archiver.image.tag=x --set mcp.enabled=false)" || { out=""; FAILED=1; }
assert_contains "archiver sqlite: pvc" "$out" "kind: PersistentVolumeClaim"

# --- control plane и edge ---
CP_ARGS=(tt "$TOOLS" --namespace tgw --set archiver.enabled=false --set mcp.enabled=false
         --set controlPlane.enabled=true --set controlPlane.image.tag=x --set controlPlane.workerImage=gw:x
         --set edge.enabled=true --set ingress.allowOpenIngress=true)
out="$(render "cp" "${CP_ARGS[@]}")" || { out=""; FAILED=1; }
role="$(awk 'BEGIN{RS="\n---\n"} index($0, "kind: Role\n")' <<<"$out")"
assert_contains "cp: role only in worker ns" "$role" "namespace: tgw-accounts"
assert_not_contains "cp: no cluster role" "$out" "kind: ClusterRole"
edge="$(awk 'BEGIN{RS="\n---\n"} index($0, "kind: Ingress") && index($0, "name: tt-edge")' <<<"$out")"
assert_not_contains "cp: admin port not in ingress" "$edge" "admin"
assert_not_contains "cp: admin port number not in ingress" "$edge" "8096"
assert_contains "edge: join invite path" "$edge" "path: /i/"
assert_contains "edge: join onboard path" "$edge" "path: /onboard"
assert_not_contains "edge: no internal path" "$edge" "/internal"
assert_contains "edge: tls tg" "$edge" "tg.tarassov.me"
assert_contains "edge: tls join" "$edge" "join.tarassov.me"
assert_contains "edge: tls mcp" "$edge" "mcp.tarassov.me"
assert_contains "edge: cert-manager issuer" "$edge" "cert-manager.io/cluster-issuer: letsencrypt-prod"
assert_contains "edge: ratelimit middleware" "$edge" "tgw-tt-join-ratelimit@kubernetescrd"
assert_contains "edge: ratelimit object" "$out" "rateLimit:"
assert_contains "cp: worker config map in worker ns" "$out" "name: tgw-worker-config"
assert_fails "cp: image tag required" tt "$TOOLS" --namespace tgw --set archiver.enabled=false --set mcp.enabled=false \
    --set controlPlane.enabled=true --set controlPlane.workerImage=gw:x
assert_fails "edge: guard applies" tt "$TOOLS" --namespace tgw --set archiver.enabled=false --set mcp.enabled=false \
    --set edge.enabled=true
out="$(render "cp off" tt "$TOOLS" --namespace tgw --set archiver.image.tag=x --set mcp.enabled=false)" || { out=""; FAILED=1; }
assert_not_contains "cp: disabled renders nothing" "$out" "control-plane"

# MCP-под старого аккаунта на новом воркере: имя и хост по sessionId, адрес, id архива и токен по workerId.
out="$(render "mcp worker" tt "$TOOLS" --namespace tgw --set archiver.enabled=false --set mcp.image.tag=x \
    --set gatewayServiceTemplate='tgw-{sessionId}.tgw-accounts.svc.cluster.local' \
    --set-string 'mcp.accounts[0].sessionId=111' --set-string 'mcp.accounts[0].workerId=aaaaaaaaaa1' \
    --set-string 'mcp.accounts[0].gatewayTokenKey=GATEWAY_TOKEN_aaaaaaaaaa1')" || { out=""; FAILED=1; }
dep="$(awk -v n="name: tt-mcp-111" 'BEGIN{RS="\n---\n"} index($0, "kind: Deployment") && index($0, n)' <<<"$out")"
assert_contains "mcp worker: base url" "$dep" "http://tgw-aaaaaaaaaa1.tgw-accounts.svc.cluster.local:8080"
assert_contains "mcp worker: archive session id" "$(grep -A1 'name: TGW_SESSION_ID' <<<"$dep" | tail -1)" 'value: "aaaaaaaaaa1"'
assert_contains "mcp worker: token key" "$(grep -A4 'name: TGW_BEARER_TOKEN' <<<"$dep")" "key: GATEWAY_TOKEN_aaaaaaaaaa1"
assert_contains "mcp worker: name stays" "$out" "name: tt-mcp-111"

# Единый MCP за mcp.tarassov.me: реестр control plane вместо токена гейтвея, правило в edge.
SH_ARGS=(tt "$TOOLS" --namespace tgw --set archiver.enabled=false --set mcp.image.tag=x --set mcp.shared.enabled=true
         --set controlPlane.enabled=true --set controlPlane.image.tag=x --set controlPlane.workerImage=gw:x
         --set edge.enabled=true --set ingress.allowOpenIngress=true)
out="$(render "mcp shared" "${SH_ARGS[@]}")" || { out=""; FAILED=1; }
shdep="$(awk 'BEGIN{RS="\n---\n"} index($0, "kind: Deployment") && index($0, "name: tt-mcp\n")' <<<"$out")"
assert_contains "mcp shared: deployment" "$shdep" "name: MCP_REGISTRY_URL"
assert_contains "mcp shared: registry url" "$shdep" "http://tt-control-plane:8096/internal/registry"
assert_contains "mcp shared: registry token" "$(grep -A4 'name: MCP_REGISTRY_TOKEN' <<<"$shdep")" "key: CP_REGISTRY_MCP_TOKEN"
assert_not_contains "mcp shared: no gateway token" "$shdep" "TGW_BEARER_TOKEN"
assert_contains "mcp shared: origins" "$shdep" "https://claude.ai,https://claude.com"
edge="$(awk 'BEGIN{RS="\n---\n"} index($0, "kind: Ingress") && index($0, "name: tt-edge")' <<<"$out")"
assert_contains "mcp shared: edge host" "$(grep -A12 'host: "mcp.tarassov.me"' <<<"$edge")" "name: tt-mcp"
out="$(render "mcp shared off" tt "$TOOLS" --namespace tgw --set archiver.enabled=false --set mcp.image.tag=x \
    --set controlPlane.enabled=true --set controlPlane.image.tag=x --set controlPlane.workerImage=gw:x \
    --set edge.enabled=true --set ingress.allowOpenIngress=true)" || { out=""; FAILED=1; }
assert_not_contains "mcp shared: disabled" "$out" "MCP_REGISTRY_URL"
assert_not_contains "mcp shared: no edge rule when disabled" "$out" 'host: "mcp.tarassov.me"'

exit "$FAILED"
