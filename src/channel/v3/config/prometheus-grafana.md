# Prometheus + Grafana 监控配置指南

## 架构说明

### 监控组件
- **Prometheus**: 时序数据库，采集和存储监控指标
- **Grafana**: 可视化面板，展示监控数据
- **Node Exporter**: 采集服务器指标（CPU、内存、磁盘、网络）
- **Redis Exporter**: 采集 Redis 指标
- **PostgreSQL Exporter**: 采集 PostgreSQL 指标
- **Custom Exporter**: 采集 WePay V3 业务指标

---

## 一、Prometheus 安装与配置

### 1. 安装 Prometheus

```bash
# 下载 Prometheus
cd /opt
wget https://github.com/prometheus/prometheus/releases/download/v2.45.0/prometheus-2.45.0.linux-amd64.tar.gz
tar -xzf prometheus-2.45.0.linux-amd64.tar.gz
mv prometheus-2.45.0.linux-amd64 prometheus

# 创建用户
sudo useradd --no-create-home --shell /bin/false prometheus

# 创建目录
sudo mkdir -p /var/lib/prometheus
sudo chown prometheus:prometheus /var/lib/prometheus

# 创建配置目录
sudo mkdir -p /etc/prometheus
sudo chown prometheus:prometheus /etc/prometheus
```

### 2. Prometheus 配置文件

#### /etc/prometheus/prometheus.yml

```yaml
# 全局配置
global:
  scrape_interval: 15s       # 采集间隔
  evaluation_interval: 15s   # 规则评估间隔
  external_labels:
    cluster: 'wepay-v3'
    environment: 'production'

# 告警管理器配置
alerting:
  alertmanagers:
    - static_configs:
        - targets:
            - localhost:9093

# 规则文件
rule_files:
  - "/etc/prometheus/rules/*.yml"

# 采集配置
scrape_configs:
  # Prometheus 自身监控
  - job_name: 'prometheus'
    static_configs:
      - targets: ['localhost:9090']

  # WePay V3 应用监控
  - job_name: 'wepay-v3'
    static_configs:
      - targets:
          - '192.168.1.10:8080'
          - '192.168.1.11:8080'
          - '192.168.1.12:8080'
    metrics_path: '/metrics'
    scrape_interval: 10s

  # Node Exporter（服务器监控）
  - job_name: 'node'
    static_configs:
      - targets:
          - '192.168.1.10:9100'
          - '192.168.1.11:9100'
          - '192.168.1.12:9100'
          - '192.168.1.20:9100'  # Redis
          - '192.168.1.30:9100'  # PostgreSQL

  # Redis 监控
  - job_name: 'redis'
    static_configs:
      - targets:
          - '192.168.1.20:9121'
          - '192.168.1.21:9121'
          - '192.168.1.22:9121'

  # PostgreSQL 监控
  - job_name: 'postgresql'
    static_configs:
      - targets:
          - '192.168.1.10:9187'
          - '192.168.1.11:9187'

  # RocketMQ 监控
  - job_name: 'rocketmq'
    static_configs:
      - targets:
          - '192.168.1.33:5557'
          - '192.168.1.35:5557'

  # Nginx 监控
  - job_name: 'nginx'
    static_configs:
      - targets:
          - '192.168.1.10:9113'
```

### 3. 告警规则配置

#### /etc/prometheus/rules/wepay-alerts.yml

```yaml
groups:
  # 应用层告警
  - name: wepay_application
    interval: 30s
    rules:
      # HTTP 错误率告警
      - alert: HighHttpErrorRate
        expr: rate(http_requests_total{status=~"5.."}[5m]) > 0.05
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High HTTP error rate on {{ $labels.instance }}"
          description: "HTTP 5xx error rate is {{ $value }} requests/sec"

      # 订单处理延迟告警
      - alert: HighOrderProcessingLatency
        expr: histogram_quantile(0.95, rate(order_processing_duration_seconds_bucket[5m])) > 5
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High order processing latency on {{ $labels.instance }}"
          description: "P95 latency is {{ $value }} seconds"

      # WebSocket 连接数告警
      - alert: HighWebSocketConnections
        expr: websocket_connections_total > 1000
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High WebSocket connections on {{ $labels.instance }}"
          description: "WebSocket connections: {{ $value }}"

      # 设备离线率告警
      - alert: HighDeviceOfflineRate
        expr: (device_offline_total / device_total) > 0.3
        for: 10m
        labels:
          severity: critical
        annotations:
          summary: "High device offline rate"
          description: "Offline rate: {{ $value | humanizePercentage }}"

  # 系统层告警
  - name: system_alerts
    interval: 30s
    rules:
      # CPU 使用率告警
      - alert: HighCpuUsage
        expr: 100 - (avg by(instance) (irate(node_cpu_seconds_total{mode="idle"}[5m])) * 100) > 80
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High CPU usage on {{ $labels.instance }}"
          description: "CPU usage is {{ $value }}%"

      # 内存使用率告警
      - alert: HighMemoryUsage
        expr: (1 - (node_memory_MemAvailable_bytes / node_memory_MemTotal_bytes)) * 100 > 85
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High memory usage on {{ $labels.instance }}"
          description: "Memory usage is {{ $value }}%"

      # 磁盘使用率告警
      - alert: HighDiskUsage
        expr: (1 - (node_filesystem_avail_bytes{fstype!~"tmpfs|fuse.lxcfs"} / node_filesystem_size_bytes)) * 100 > 85
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High disk usage on {{ $labels.instance }}"
          description: "Disk usage is {{ $value }}%"

  # Redis 告警
  - name: redis_alerts
    interval: 30s
    rules:
      # Redis 内存使用率告警
      - alert: RedisHighMemoryUsage
        expr: (redis_memory_used_bytes / redis_memory_max_bytes) * 100 > 90
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Redis high memory usage on {{ $labels.instance }}"
          description: "Memory usage is {{ $value }}%"

      # Redis 连接数告警
      - alert: RedisHighConnections
        expr: redis_connected_clients > 1000
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Redis high connections on {{ $labels.instance }}"
          description: "Connected clients: {{ $value }}"

      # Redis 主从复制延迟告警
      - alert: RedisReplicationLag
        expr: redis_replication_lag_seconds > 10
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "Redis replication lag on {{ $labels.instance }}"
          description: "Replication lag: {{ $value }} seconds"

  # PostgreSQL 告警
  - name: postgresql_alerts
    interval: 30s
    rules:
      # PostgreSQL 连接数告警
      - alert: PostgreSQLHighConnections
        expr: pg_stat_database_numbackends > 80
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "PostgreSQL high connections on {{ $labels.instance }}"
          description: "Active connections: {{ $value }}"

      # PostgreSQL 慢查询告警
      - alert: PostgreSQLSlowQueries
        expr: rate(pg_stat_statements_mean_time_seconds[5m]) > 1
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "PostgreSQL slow queries on {{ $labels.instance }}"
          description: "Mean query time: {{ $value }} seconds"

      # PostgreSQL 复制延迟告警
      - alert: PostgreSQLReplicationLag
        expr: pg_replication_lag_seconds > 30
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "PostgreSQL replication lag on {{ $labels.instance }}"
          description: "Replication lag: {{ $value }} seconds"
```

### 4. 启动 Prometheus

#### systemd 服务文件

```bash
# 创建服务文件
sudo vim /etc/systemd/system/prometheus.service
```

```ini
[Unit]
Description=Prometheus
Wants=network-online.target
After=network-online.target

[Service]
User=prometheus
Group=prometheus
Type=simple
ExecStart=/opt/prometheus/prometheus \
    --config.file=/etc/prometheus/prometheus.yml \
    --storage.tsdb.path=/var/lib/prometheus/ \
    --web.console.templates=/opt/prometheus/consoles \
    --web.console.libraries=/opt/prometheus/console_libraries \
    --web.listen-address=0.0.0.0:9090 \
    --storage.tsdb.retention.time=30d

Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
# 启动服务
sudo systemctl daemon-reload
sudo systemctl start prometheus
sudo systemctl enable prometheus

# 验证
sudo systemctl status prometheus
curl http://localhost:9090/metrics
```

---

## 二、Grafana 安装与配置

### 1. 安装 Grafana

```bash
# 添加 Grafana 仓库
sudo apt-get install -y software-properties-common
sudo add-apt-repository "deb https://packages.grafana.com/oss/deb stable main"
wget -q -O - https://packages.grafana.com/gpg.key | sudo apt-key add -

# 安装 Grafana
sudo apt-get update
sudo apt-get install grafana

# 启动服务
sudo systemctl start grafana-server
sudo systemctl enable grafana-server

# 访问 http://localhost:3000
# 默认用户名/密码: admin/admin
```

### 2. 配置 Prometheus 数据源

```bash
# 登录 Grafana
# Configuration -> Data Sources -> Add data source -> Prometheus

# 配置参数
Name: Prometheus
URL: http://localhost:9090
Access: Server (default)
```

### 3. 导入 Dashboard

#### WePay V3 业务监控面板

```json
{
  "dashboard": {
    "title": "WePay V3 Business Metrics",
    "panels": [
      {
        "title": "订单处理速率",
        "targets": [
          {
            "expr": "rate(order_processed_total[5m])"
          }
        ]
      },
      {
        "title": "订单成功率",
        "targets": [
          {
            "expr": "rate(order_success_total[5m]) / rate(order_processed_total[5m])"
          }
        ]
      },
      {
        "title": "设备在线数量",
        "targets": [
          {
            "expr": "device_online_total"
          }
        ]
      },
      {
        "title": "WebSocket 连接数",
        "targets": [
          {
            "expr": "websocket_connections_total"
          }
        ]
      },
      {
        "title": "OCR 识别速率",
        "targets": [
          {
            "expr": "rate(ocr_processed_total[5m])"
          }
        ]
      },
      {
        "title": "缓存命中率",
        "targets": [
          {
            "expr": "rate(cache_hits_total[5m]) / (rate(cache_hits_total[5m]) + rate(cache_misses_total[5m]))"
          }
        ]
      }
    ]
  }
}
```

---

## 三、Exporter 安装

### 1. Node Exporter（服务器监控）

```bash
# 下载安装
cd /opt
wget https://github.com/prometheus/node_exporter/releases/download/v1.6.0/node_exporter-1.6.0.linux-amd64.tar.gz
tar -xzf node_exporter-1.6.0.linux-amd64.tar.gz
mv node_exporter-1.6.0.linux-amd64 node_exporter

# 创建服务
sudo vim /etc/systemd/system/node_exporter.service
```

```ini
[Unit]
Description=Node Exporter
After=network.target

[Service]
Type=simple
ExecStart=/opt/node_exporter/node_exporter

[Install]
WantedBy=multi-user.target
```

```bash
# 启动服务
sudo systemctl start node_exporter
sudo systemctl enable node_exporter
```

### 2. Redis Exporter

```bash
# 下载安装
cd /opt
wget https://github.com/oliver006/redis_exporter/releases/download/v1.52.0/redis_exporter-v1.52.0.linux-amd64.tar.gz
tar -xzf redis_exporter-v1.52.0.linux-amd64.tar.gz
mv redis_exporter-v1.52.0.linux-amd64 redis_exporter

# 创建服务
sudo vim /etc/systemd/system/redis_exporter.service
```

```ini
[Unit]
Description=Redis Exporter
After=network.target

[Service]
Type=simple
ExecStart=/opt/redis_exporter/redis_exporter \
    --redis.addr=redis://localhost:6379 \
    --redis.password=wepay_redis_password_123

[Install]
WantedBy=multi-user.target
```

```bash
# 启动服务
sudo systemctl start redis_exporter
sudo systemctl enable redis_exporter
```

### 3. PostgreSQL Exporter

```bash
# 下载安装
cd /opt
wget https://github.com/prometheus-community/postgres_exporter/releases/download/v0.13.2/postgres_exporter-0.13.2.linux-amd64.tar.gz
tar -xzf postgres_exporter-0.13.2.linux-amd64.tar.gz
mv postgres_exporter-0.13.2.linux-amd64 postgres_exporter

# 创建监控用户
psql -U postgres -c "CREATE USER exporter WITH PASSWORD 'exporter_password';"
psql -U postgres -c "GRANT pg_monitor TO exporter;"

# 创建服务
sudo vim /etc/systemd/system/postgres_exporter.service
```

```ini
[Unit]
Description=PostgreSQL Exporter
After=network.target

[Service]
Type=simple
Environment="DATA_SOURCE_NAME=postgresql://exporter:exporter_password@localhost:5432/wepay_v3?sslmode=disable"
ExecStart=/opt/postgres_exporter/postgres_exporter

[Install]
WantedBy=multi-user.target
```

```bash
# 启动服务
sudo systemctl start postgres_exporter
sudo systemctl enable postgres_exporter
```

---

## 四、WePay V3 应用指标暴露

### 1. 在 Drogon 中暴露 Prometheus 指标

#### MetricsController.h

```cpp
#pragma once
#include <drogon/HttpController.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/exposer.h>

namespace wepay {
namespace v3 {

class MetricsController : public drogon::HttpController<MetricsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MetricsController::getMetrics, "/metrics", drogon::Get);
    METHOD_LIST_END

    void getMetrics(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // 单例
    static MetricsController& getInstance();

    // 指标记录方法
    void recordOrderProcessed(const std::string& status);
    void recordOrderLatency(double seconds);
    void recordDeviceOnline(int count);
    void recordWebSocketConnections(int count);
    void recordOcrProcessed(const std::string& result);
    void recordCacheHit(bool hit);

private:
    MetricsController();
    
    std::shared_ptr<prometheus::Registry> registry_;
    
    // 计数器
    prometheus::Family<prometheus::Counter>* orderCounter_;
    prometheus::Family<prometheus::Counter>* ocrCounter_;
    prometheus::Family<prometheus::Counter>* cacheCounter_;
    
    // 仪表盘
    prometheus::Family<prometheus::Gauge>* deviceGauge_;
    prometheus::Family<prometheus::Gauge>* websocketGauge_;
    
    // 直方图
    prometheus::Family<prometheus::Histogram>* orderLatencyHistogram_;
};

} // namespace v3
} // namespace wepay
```

#### MetricsController.cpp

```cpp
#include "MetricsController.h"
#include <prometheus/text_serializer.h>

namespace wepay {
namespace v3 {

MetricsController& MetricsController::getInstance() {
    static MetricsController instance;
    return instance;
}

MetricsController::MetricsController() {
    registry_ = std::make_shared<prometheus::Registry>();
    
    // 初始化计数器
    orderCounter_ = &prometheus::BuildCounter()
        .Name("order_processed_total")
        .Help("Total number of orders processed")
        .Register(*registry_);
    
    ocrCounter_ = &prometheus::BuildCounter()
        .Name("ocr_processed_total")
        .Help("Total number of OCR processed")
        .Register(*registry_);
    
    cacheCounter_ = &prometheus::BuildCounter()
        .Name("cache_operations_total")
        .Help("Total number of cache operations")
        .Register(*registry_);
    
    // 初始化仪表盘
    deviceGauge_ = &prometheus::BuildGauge()
        .Name("device_online_total")
        .Help("Number of online devices")
        .Register(*registry_);
    
    websocketGauge_ = &prometheus::BuildGauge()
        .Name("websocket_connections_total")
        .Help("Number of WebSocket connections")
        .Register(*registry_);
    
    // 初始化直方图
    orderLatencyHistogram_ = &prometheus::BuildHistogram()
        .Name("order_processing_duration_seconds")
        .Help("Order processing latency in seconds")
        .Register(*registry_);
}

void MetricsController::getMetrics(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    
    // 序列化指标
    prometheus::TextSerializer serializer;
    auto metrics = serializer.Serialize(registry_->Collect());
    
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody(metrics);
    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    callback(resp);
}

void MetricsController::recordOrderProcessed(const std::string& status) {
    orderCounter_->Add({{"status", status}}).Increment();
}

void MetricsController::recordOrderLatency(double seconds) {
    orderLatencyHistogram_->Add({}, prometheus::Histogram::BucketBoundaries{
        0.1, 0.5, 1.0, 2.0, 5.0, 10.0
    }).Observe(seconds);
}

void MetricsController::recordDeviceOnline(int count) {
    deviceGauge_->Add({}).Set(count);
}

void MetricsController::recordWebSocketConnections(int count) {
    websocketGauge_->Add({}).Set(count);
}

void MetricsController::recordOcrProcessed(const std::string& result) {
    ocrCounter_->Add({{"result", result}}).Increment();
}

void MetricsController::recordCacheHit(bool hit) {
    cacheCounter_->Add({{"type", hit ? "hit" : "miss"}}).Increment();
}

} // namespace v3
} // namespace wepay
```

### 2. 在业务代码中记录指标

```cpp
// 订单处理
void OrderService::processOrder(const Order& order) {
    auto start = std::chrono::steady_clock::now();
    
    try {
        // 处理订单逻辑
        // ...
        
        // 记录成功
        MetricsController::getInstance().recordOrderProcessed("success");
    } catch (...) {
        // 记录失败
        MetricsController::getInstance().recordOrderProcessed("failed");
        throw;
    }
    
    // 记录延迟
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration<double>(end - start).count();
    MetricsController::getInstance().recordOrderLatency(duration);
}

// 设备在线统计
void DeviceManager::updateOnlineCount() {
    int count = getOnlineDeviceCount();
    MetricsController::getInstance().recordDeviceOnline(count);
}

// WebSocket 连接统计
void WebSocketConnectionManager::onConnect() {
    int count = getConnectionCount();
    MetricsController::getInstance().recordWebSocketConnections(count);
}

// 缓存命中统计
void CacheManager::get(const std::string& key) {
    bool hit = redis_->exists(key);
    MetricsController::getInstance().recordCacheHit(hit);
    // ...
}
```

---

## 五、告警通知配置

### Alertmanager 配置

```yaml
# /etc/alertmanager/alertmanager.yml

global:
  resolve_timeout: 5m

route:
  group_by: ['alertname', 'cluster', 'service']
  group_wait: 10s
  group_interval: 10s
  repeat_interval: 12h
  receiver: 'wepay-alerts'

receivers:
  - name: 'wepay-alerts'
    webhook_configs:
      - url: 'http://localhost:8080/api/wepay/v3/alert/webhook'
        send_resolved: true
```

---

## 配置文件位置

- Prometheus 配置: `/etc/prometheus/prometheus.yml`
- 告警规则: `/etc/prometheus/rules/*.yml`
- Grafana 配置: `/etc/grafana/grafana.ini`
- Alertmanager 配置: `/etc/alertmanager/alertmanager.yml`
