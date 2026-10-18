{{/* Guard: Ingress без IP-allowlist выставляет в интернет то, что держится только на нем.
     Защитой считается ровно два варианта: ingress.ipAllowList (Middleware из этого чарта) без
     открытых сетей 0.0.0.0/0 и ::/0, либо осознанный опт-аут allowOpenIngress (доступ ограничен
     иначе: mTLS, VPN, NetworkPolicy). Аннотации не засчитываются: по имени класса не понять, какой
     контроллер его читает (в oci-k3s класс nginx обслуживает Traefik, nginx-аннотации молча
     игнорируются), а своя router.middlewares может быть redirect или headers. */}}
{{- define "tools.ingressGuard" -}}
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
{{- define "tools.ipAllowListName" -}}
{{- printf "%s-ip-allowlist" .Release.Name -}}
{{- end -}}

{{/* Аннотации Ingress: ingress.annotations плюс ссылка на Middleware allowlist'а первой в
     router.middlewares; пользовательский список не затирается. */}}
{{- define "tools.ingressAnnotations" -}}
{{- $ann := deepCopy (.Values.ingress.annotations | default dict) -}}
{{- if .Values.ingress.ipAllowList -}}
{{- $key := "traefik.ingress.kubernetes.io/router.middlewares" -}}
{{- $mw := printf "%s-%s@kubernetescrd" .Release.Namespace (include "tools.ipAllowListName" .) -}}
{{- with (get $ann $key) -}}
{{- $mw = printf "%s,%s" $mw . -}}
{{- end -}}
{{- $_ := set $ann $key $mw -}}
{{- end -}}
{{- if $ann -}}
{{- toYaml $ann -}}
{{- end -}}
{{- end -}}
