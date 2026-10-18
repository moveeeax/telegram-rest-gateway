# Control plane: онбординг по инвайту и воркеры аккаунтов в k8s.
FROM node:22-alpine AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY src ./src
RUN npm run build && npm prune --omit=dev

FROM node:22-alpine
WORKDIR /app
ENV NODE_ENV=production
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
USER node
# 8095: страницы онбординга (идут в Ingress). 8096: admin API и /internal (только внутри кластера).
EXPOSE 8095 8096
ENTRYPOINT ["node", "dist/index.js"]
