{{/* Guard: Ingress MCP и архиватора без IP-allowlist выставляет в интернет эндпоинты, которые
     держатся только на одном общем токене. Та же логика, что tgw.ingressGuard в чарте гейтвея:
     защитой считается ingress.ipAllowList, Traefik-аннотация router.middlewares или nginx
     whitelist-source-range (Traefik ее не читает). allowOpenIngress: осознанный опт-аут. */}}
{{- define "tools.ingressGuard" -}}
{{- $ann := .Values.ingress.annotations | default dict -}}
{{- $protected := or .Values.ingress.allowOpenIngress
      (gt (len (.Values.ingress.ipAllowList | default list)) 0)
      (hasKey $ann "traefik.ingress.kubernetes.io/router.middlewares")
      (hasKey $ann "nginx.ingress.kubernetes.io/whitelist-source-range") -}}
{{- if and .Values.ingress.enabled (not $protected) -}}
{{- fail "ingress.enabled=true без IP-allowlist (см. DEPLOY.md). Задай ingress.ipAllowList, либо аннотацию traefik.ingress.kubernetes.io/router.middlewares (Traefik) или nginx.ingress.kubernetes.io/whitelist-source-range (nginx), либо прими риск явно через ingress.allowOpenIngress=true." -}}
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
