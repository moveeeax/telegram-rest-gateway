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

{{/* Guard: Ingress без IP-allowlist выставляет в интернет то, что держится только на нем.
     Защитой считается ровно два варианта: ingress.ipAllowList (Middleware из этого чарта) без
     открытых сетей 0.0.0.0/0 и ::/0, либо осознанный опт-аут allowOpenIngress (доступ ограничен
     иначе: mTLS, VPN, NetworkPolicy). Аннотации не засчитываются: по имени класса не понять, какой
     контроллер его читает (в oci-k3s класс nginx обслуживает Traefik, nginx-аннотации молча
     игнорируются), а своя router.middlewares может быть redirect или headers. */}}
{{- define "tgw.ingressGuard" -}}
{{- $cidrs := .Values.ingress.ipAllowList | default list -}}
{{- $valid := gt (len $cidrs) 0 -}}
{{- range $cidrs -}}
{{- $c := toString . | trim -}}
{{- if or (eq $c "") (eq $c "0.0.0.0/0") (eq $c "::/0") -}}
{{- $valid = false -}}
{{- end -}}
{{- end -}}
{{- if and .Values.ingress.enabled (not .Values.ingress.allowOpenIngress) (not $valid) -}}
{{- fail "ingress.enabled=true без IP-allowlist (см. DEPLOY.md). Задай ingress.ipAllowList (CIDR доверенных адресов, без 0.0.0.0/0 и ::/0) либо прими риск явно через ingress.allowOpenIngress=true. Аннотации nginx whitelist-source-range и traefik router.middlewares guard не засчитывает." -}}
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
