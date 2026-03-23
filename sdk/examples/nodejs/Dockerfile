FROM ghcr.io/richarah/silex:slim

# Install Node.js (for TypeScript builds with native addons)
RUN apk add --no-cache nodejs npm

WORKDIR /workspace
COPY package.json package-lock.json ./

# npm ci is faster than npm install; node_modules cache avoids re-downloading
RUN --mount=type=cache,target=/root/.npm \
    npm ci

COPY . .

# TypeScript compilation; native addons use clang/mold automatically
RUN npm run build

CMD ["node", "dist/index.js"]
