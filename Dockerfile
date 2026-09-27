FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config ca-certificates \
    protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev \
    libboost-system-dev libpqxx-dev libhiredis-dev libcurl4-openssl-dev \
    nlohmann-json3-dev \
  && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY proto ./proto
COPY cpp ./cpp
COPY tests ./tests
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMINICLOUD_BUILD_TESTS=ON \
 && cmake --build /build --parallel 2 \
 && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl libprotobuf-dev libgrpc++-dev libboost-system-dev \
    libpqxx-dev libhiredis-dev libcurl4-openssl-dev \
  && rm -rf /var/lib/apt/lists/* \
  && groupadd --gid 10001 minicloud \
  && useradd --uid 10001 --gid minicloud --no-create-home --shell /usr/sbin/nologin minicloud

FROM runtime AS controller
COPY --from=build /build/bin/minicloud-controller /usr/local/bin/minicloud-controller
USER 10001:10001
EXPOSE 8090 50051 9091
ENTRYPOINT ["/usr/local/bin/minicloud-controller"]

FROM runtime AS gateway
COPY --from=build /build/bin/minicloud-gateway /usr/local/bin/minicloud-gateway
USER 10001:10001
EXPOSE 8080 9092
ENTRYPOINT ["/usr/local/bin/minicloud-gateway"]

FROM runtime AS worker
COPY --from=build /build/bin/minicloud-worker /usr/local/bin/minicloud-worker
# The worker needs access to the trusted local Docker Engine socket. The Compose
# file scopes that capability to workers only. See docs/THREAT_MODEL.md.
USER 0:0
EXPOSE 9101
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=3 \
  CMD curl --fail --silent --show-error http://127.0.0.1:9101/metrics >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/minicloud-worker"]

FROM runtime AS cli
COPY --from=build /build/bin/minicloudctl /usr/local/bin/minicloudctl
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/minicloudctl"]
