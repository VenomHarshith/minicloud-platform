FROM node:22-alpine AS build
WORKDIR /app
COPY package.json ./
RUN npm install --no-audit --no-fund
COPY tsconfig*.json vite.config.ts index.html ./
COPY src ./src
ARG VITE_PUBLIC_DEMO=false
ENV VITE_PUBLIC_DEMO=${VITE_PUBLIC_DEMO}
RUN npm run build

FROM nginx:1.31-alpine
ARG NGINX_CONFIG=nginx.conf
COPY ${NGINX_CONFIG} /etc/nginx/templates/default.conf.template
COPY --from=build /app/dist /usr/share/nginx/html
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=2s --retries=3 CMD wget -qO- http://127.0.0.1:8080/healthz || exit 1
