# WePay-Cpp 第三方支付系统 — 多阶段构建镜像
# 目标: Debian-based, 使用系统包安装 Drogon 依赖

# ═══════════════════════════════════════════════════════════════
# Stage 1: 构建
# ═══════════════════════════════════════════════════════════════
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Shanghai

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config \
    libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
    libsqlite3-dev libpq-dev libhiredis-dev \
    libcurl4-openssl-dev librabbitmq-dev \
    libbrotli-dev \
    ca-certificates tzdata \
 && rm -rf /var/lib/apt/lists/*

# 构建 Drogon(若没有系统包)
RUN git clone --depth 1 --recurse-submodules https://github.com/drogonframework/drogon /tmp/drogon \
 && cd /tmp/drogon && mkdir build && cd build \
 && cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
 && make -j$(nproc) && make install && ldconfig \
 && rm -rf /tmp/drogon

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY config.json ./

RUN mkdir -p build && cd build \
 && cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DDROGON_INSTALL_PREFIX=/usr/local \
    -DENABLE_REDIS=ON -DENABLE_RABBITMQ=ON \
 && make -j$(nproc)

# ═══════════════════════════════════════════════════════════════
# Stage 2: 运行
# ═══════════════════════════════════════════════════════════════
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Shanghai

RUN apt-get update && apt-get install -y --no-install-recommends \
    libjsoncpp25 zlib1g libssl3 libsqlite3-0 libpq5 libhiredis0.14 \
    libcurl4 librabbitmq4 libbrotli1 libuuid1 \
    ca-certificates tzdata \
 && rm -rf /var/lib/apt/lists/* \
 && ln -snf /usr/share/zoneinfo/$TZ /etc/localtime

# 从 builder 复制 Drogon 运行时
COPY --from=builder /usr/local/lib/libdrogon.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libtrantor.so* /usr/local/lib/
RUN ldconfig

WORKDIR /app
COPY --from=builder /src/build/wepay-cpp /app/wepay-cpp
COPY config.json /app/config.json

# 数据卷(持久化 SQLite 数据库、日志、备份)
VOLUME ["/app/data", "/app/logs", "/app/upload"]
RUN mkdir -p /app/data /app/logs /app/upload /app/data/backup \
 && ln -sf /app/data/wepay.db /app/wepay.db

# OPS 监控标签
LABEL maintainer="WePay-Cpp" \
      version="1.0" \
      ops.monitor="enabled" \
      ops.backup.dir="/app/data/backup" \
      ops.log.dir="/app/logs" \
      ops.config="/app/config.json" \
      ops.healthcheck.path="/swagger/openapi.json"

EXPOSE 8088

HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD wget -q -O- http://127.0.0.1:8088/swagger/openapi.json >/dev/null || exit 1

CMD ["/app/wepay-cpp", "--config", "/app/config.json"]
