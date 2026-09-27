FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
ARG LIBPQXX_VERSION=7.8.1
ARG LIBPQXX_SHA256=0f4c0762de45a415c9fd7357ce508666fa88b9a4a463f5fb76c235bc80dd6a84
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config ca-certificates curl \
    protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev \
    libboost-system-dev libpq-dev libhiredis-dev libcurl4-openssl-dev \
    nlohmann-json3-dev \
  && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04's libpqxx 7.8.1 binary is built as C++17, while its public
# headers expose a different ABI when consumed as C++20. Build the same pinned
# release as C++20 so the library and MiniCloud agree on that ABI. The archive
# checksum makes this network build input reproducible and tamper-evident.
RUN curl --fail --location --silent --show-error \
      "https://github.com/jtv/libpqxx/archive/refs/tags/${LIBPQXX_VERSION}.tar.gz" \
      --output /tmp/libpqxx.tar.gz \
  && echo "${LIBPQXX_SHA256}  /tmp/libpqxx.tar.gz" | sha256sum --check --strict \
  && mkdir /tmp/libpqxx-source \
  && tar --extract --gzip --file /tmp/libpqxx.tar.gz \
      --directory /tmp/libpqxx-source --strip-components=1 \
  && cmake -S /tmp/libpqxx-source -B /tmp/libpqxx-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_STANDARD=20 \
      -DCMAKE_INSTALL_PREFIX=/opt/libpqxx \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_DOC=OFF \
      -DBUILD_TEST=OFF \
      -DSKIP_BUILD_TEST=ON \
  && cmake --build /tmp/libpqxx-build --parallel 2 \
  && cmake --install /tmp/libpqxx-build \
  && rm -rf /tmp/libpqxx.tar.gz /tmp/libpqxx-source /tmp/libpqxx-build
WORKDIR /src
COPY CMakeLists.txt ./
COPY proto ./proto
COPY cpp ./cpp
COPY tests ./tests
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/opt/libpqxx -DMINICLOUD_BUILD_TESTS=ON \
 && cmake --build /build --parallel 2 \
 && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl libprotobuf-dev libgrpc++-dev libboost-system-dev \
    libpq5 libhiredis-dev libcurl4-openssl-dev \
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
