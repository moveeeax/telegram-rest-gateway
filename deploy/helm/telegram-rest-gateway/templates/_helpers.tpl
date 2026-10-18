{{/* Базовое имя релиза */}}
{{- define "tgw.fullname" -}}
{{- printf "%s" .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/* Имя ресурса аккаунта: <release>-<sessionId> */}}
{{- define "tgw.accountName" -}}
{{- printf "%s-%s" .root.Release.Name .account.sessionId | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/* Общие лейблы */}}
{{- define "tgw.labels" -}}
app.kubernetes.io/name: telegram-rest-gateway
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version }}
{{- end -}}

{{/* Селектор для конкретного аккаунта */}}
{{- define "tgw.accountSelector" -}}
app.kubernetes.io/name: telegram-rest-gateway
app.kubernetes.io/instance: {{ .root.Release.Name }}
tgw.tarassov.me/session-id: {{ .account.sessionId | quote }}
{{- end -}}

{{/* Тег образа: обязателен, дефолта нет — semver CI не публикует (см. docs/CICD.md) */}}
{{- define "tgw.imageTag" -}}
{{- required "image.tag обязателен: CI публикует immutable short-sha теги, см. docs/CICD.md" .Values.image.tag -}}
{{- end -}}

{{/* Host для Ingress аккаунта */}}
{{- define "tgw.accountHost" -}}
{{- replace "{sessionId}" .account.sessionId .root.Values.ingress.hostTemplate -}}
{{- end -}}

{{/* Guard: ingress без IP-allowlist выставляет в интернет то, что полагается только на него
     (форма логина /ui без Bearer-auth, /v1/auth/* под admin-токеном). Тот же fail-fast принцип,
     что у tgw.imageTag выше: явная ошибка на старте вместо тихой дыры в проде. Защитой считается
     любое из: ingress.ipAllowList (Traefik Middleware из этого чарта), своя Traefik-аннотация
     router.middlewares, nginx whitelist-source-range только при className=nginx (Traefik ее не читает).
     allowOpenIngress: осознанный опт-аут, если доступ ограничен иначе (mTLS, VPN, NetworkPolicy). */}}
{{- define "tgw.ingressGuard" -}}
{{- $ann := .Values.ingress.annotations | default dict -}}
{{- /* Засчитывается только механизм, который читает выбранный контроллер, и только непустое
       значение: nginx-аннотация на Traefik или ipAllowList на nginx не защищают ничего. */ -}}
{{- $nginx := eq (.Values.ingress.className | default "") "nginx" -}}
{{- $traefikOk := and (not $nginx) (or
      (gt (len (.Values.ingress.ipAllowList | default list)) 0)
      (ne (get $ann "traefik.ingress.kubernetes.io/router.middlewares" | default "" | trim) "")) -}}
{{- $nginxOk := and $nginx (ne (get $ann "nginx.ingress.kubernetes.io/whitelist-source-range" | default "" | trim) "") -}}
{{- $protected := or .Values.ingress.allowOpenIngress $traefikOk $nginxOk -}}
{{- if and .Values.ingress.enabled (not $protected) -}}
{{- fail "ingress.enabled=true без IP-allowlist: /ui и /v1/auth/* полагаются на ограничение по IP (см. DEPLOY.md). Задай ingress.ipAllowList, либо непустую аннотацию traefik.ingress.kubernetes.io/router.middlewares (Traefik) или при className=nginx nginx.ingress.kubernetes.io/whitelist-source-range, либо прими риск явно через ingress.allowOpenIngress=true." -}}
{{- end -}}
{{- end -}}

{{/* Имя Traefik Middleware с IP-allowlist (одно на релиз). */}}
{{- define "tgw.ipAllowListName" -}}
{{- printf "%s-ip-allowlist" .Release.Name -}}
{{- end -}}

{{/* Аннотации Ingress: ingress.annotations плюс ссылка на Middleware allowlist'а. Если
     router.middlewares уже задан пользователем, allowlist ставится первым через запятую,
     пользовательский список не затирается. */}}
{{- define "tgw.ingressAnnotations" -}}
{{- $ann := deepCopy (.Values.ingress.annotations | default dict) -}}
{{- if .Values.ingress.ipAllowList -}}
{{- $key := "traefik.ingress.kubernetes.io/router.middlewares" -}}
{{- $mw := printf "%s-%s@kubernetescrd" .Release.Namespace (include "tgw.ipAllowListName" .) -}}
{{- with (get $ann $key) -}}
{{- $mw = printf "%s,%s" $mw . -}}
{{- end -}}
{{- $_ := set $ann $key $mw -}}
{{- end -}}
{{- if $ann -}}
{{- toYaml $ann -}}
{{- end -}}
{{- end -}}
