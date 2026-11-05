# Архиватор: Kafka -> SQLite(FTS5) + HTTP /search. node:26-slim; better-sqlite3 has no prebuilt for Node 26, so it builds from source here.
FROM node:26-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends python3 make g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY src ./src
RUN npm run build && npm prune --omit=dev

FROM node:26-slim
WORKDIR /app
ENV NODE_ENV=production ARCHIVER_DB_PATH=/data/archive.db
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
RUN mkdir -p /data && chown node:node /data
USER node
VOLUME /data
EXPOSE 8090
ENTRYPOINT ["node", "dist/index.js"]
