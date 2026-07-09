# Архиватор: Kafka -> SQLite(FTS5) + HTTP /search. node:22-slim (glibc — prebuilt better-sqlite3).
FROM node:22-slim AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY src ./src
RUN npm run build && npm prune --omit=dev

FROM node:22-slim
WORKDIR /app
ENV NODE_ENV=production ARCHIVER_DB_PATH=/data/archive.db
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
RUN mkdir -p /data && chown node:node /data
USER node
VOLUME /data
EXPOSE 8090
ENTRYPOINT ["node", "dist/index.js"]
