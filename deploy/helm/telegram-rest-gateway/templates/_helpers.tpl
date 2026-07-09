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

{{/* Тег образа: values.image.tag или appVersion */}}
{{- define "tgw.imageTag" -}}
{{- .Values.image.tag | default (printf "v%s" .Chart.AppVersion) -}}
{{- end -}}

{{/* Host для Ingress аккаунта */}}
{{- define "tgw.accountHost" -}}
{{- replace "{sessionId}" .account.sessionId .root.Values.ingress.hostTemplate -}}
{{- end -}}
