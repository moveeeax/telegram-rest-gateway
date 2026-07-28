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
