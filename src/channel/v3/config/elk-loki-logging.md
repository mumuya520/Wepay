# ELK/Loki 日志聚合配置指南

## 方案选择

### 方案一：ELK Stack（Elasticsearch + Logstash + Kibana）
- **优点**: 功能强大，查询灵活，生态成熟
- **缺点**: 资源消耗大，配置复杂
- **适用**: 大规模日志分析，复杂查询需求

### 方案二：Loki + Promtail + Grafana（推荐）
- **优点**: 轻量级，与 Prometheus/Grafana 集成好，成本低
- **缺点**: 查询功能相对简单
- **适用**: 中小规模，日志查看和简单分析

**推荐**: WePay V3 使用 **Loki + Promtail + Grafana**

---

## 一、Loki 日志聚合方案（推荐）

### 1. Loki 安装与配置

#### 下载安装

```bash
# 下载 Loki
cd /opt
wget https://github.com/grafana/loki/releases/download/v2.9.0/loki-linux-amd64.zip
unzip loki-linux-amd64.zip
chmod +x loki-linux-amd64
mv loki-linux-amd64 loki

# 创建目录
sudo mkdir -p /var/lib/loki
sudo mkdir -p /etc/loki
```

#### 配置文件

```yaml
# /etc/loki/loki-config.yml

auth_enabled: false

server:
  http_listen_port: 3100
  grpc_listen_port: 9096

common:
  path_prefix: /var/lib/loki
  storage:
    filesystem:
      chunks_directory: /var/lib/loki/chunks
      rules_directory: /var/lib/loki/rules
  replication_factor: 1
  ring:
    instance_addr: 127.0.0.1
    kvstore:
      store: inmemory

schema_config:
  configs:
    - from: 2023-01-01
      store: boltdb-shipper
      object_store: filesystem
      schema: v11
      index:
        prefix: index_
        period: 24h

storage_config:
  boltdb_shipper:
    active_index_directory: /var/lib/loki/boltdb-shipper-active
    cache_location: /var/lib/loki/boltdb-shipper-cache
    cache_ttl: 24h
    shared_store: filesystem
  filesystem:
    directory: /var/lib/loki/chunks

compactor:
  working_directory: /var/lib/loki/boltdb-shipper-compactor
  shared_store: filesystem

limits_config:
  reject_old_samples: true
  reject_old_samples_max_age: 168h
  ingestion_rate_mb: 16
  ingestion_burst_size_mb: 32
  max_query_length: 721h
  max_query_parallelism: 32

chunk_store_config:
  max_look_back_period: 0s

table_manager:
  retention_deletes_enabled: true
  retention_period: 720h

ruler:
  storage:
    type: local
    local:
      directory: /var/lib/loki/rules
  rule_path: /var/lib/loki/rules-temp
  alertmanager_url: http://localhost:9093
  ring:
    kvstore:
      store: inmemory
  enable_api: true
```

#### 启动服务

```bash
# 创建 systemd 服务
sudo vim /etc/systemd/system/loki.service
```

```ini
[Unit]
Description=Loki
After=network.target

[Service]
Type=simple
User=root
ExecStart=/opt/loki -config.file=/etc/loki/loki-config.yml
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
# 启动服务
sudo systemctl daemon-reload
sudo systemctl start loki
sudo systemctl enable loki

# 验证
curl http://localhost:3100/ready
```

---

### 2. Promtail 安装与配置

#### 下载安装

```bash
# 下载 Promtail
cd /opt
wget https://github.com/grafana/loki/releases/download/v2.9.0/promtail-linux-amd64.zip
unzip promtail-linux-amd64.zip
chmod +x promtail-linux-amd64
mv promtail-linux-amd64 promtail

# 创建配置目录
sudo mkdir -p /etc/promtail
```

#### 配置文件

```yaml
# /etc/promtail/promtail-config.yml

server:
  http_listen_port: 9080
  grpc_listen_port: 0

positions:
  filename: /var/lib/promtail/positions.yaml

clients:
  - url: http://localhost:3100/loki/api/v1/push

scrape_configs:
  # WePay V3 应用日志
  - job_name: wepay-v3
    static_configs:
      - targets:
          - localhost
        labels:
          job: wepay-v3
          __path__: /var/log/wepay/*.log
    pipeline_stages:
      # 解析 JSON 日志
      - json:
          expressions:
            timestamp: timestamp
            level: level
            message: message
            module: module
            device_id: device_id
            order_id: order_id
      # 提取时间戳
      - timestamp:
          source: timestamp
          format: RFC3339
      # 提取日志级别
      - labels:
          level:
          module:
      # 输出格式
      - output:
          source: message

  # Nginx 访问日志
  - job_name: nginx-access
    static_configs:
      - targets:
          - localhost
        labels:
          job: nginx
          log_type: access
          __path__: /var/log/nginx/wepay_access.log
    pipeline_stages:
      # 解析 Nginx 日志格式
      - regex:
          expression: '^(?P<remote_addr>[\w\.]+) - (?P<remote_user>[\w-]+) \[(?P<time_local>.*?)\] "(?P<method>\w+) (?P<request>.*?) (?P<protocol>.*?)" (?P<status>\d+) (?P<body_bytes_sent>\d+) "(?P<http_referer>.*?)" "(?P<http_user_agent>.*?)"'
      - labels:
          method:
          status:
      - timestamp:
          source: time_local
          format: 02/Jan/2006:15:04:05 -0700

  # Nginx 错误日志
  - job_name: nginx-error
    static_configs:
      - targets:
          - localhost
        labels:
          job: nginx
          log_type: error
          __path__: /var/log/nginx/wepay_error.log

  # PostgreSQL 日志
  - job_name: postgresql
    static_configs:
      - targets:
          - localhost
        labels:
          job: postgresql
          __path__: /var/log/postgresql/postgresql-*.log

  # Redis 日志
  - job_name: redis
    static_configs:
      - targets:
          - localhost
        labels:
          job: redis
          __path__: /var/log/redis/redis-server.log

  # RocketMQ 日志
  - job_name: rocketmq
    static_configs:
      - targets:
          - localhost
        labels:
          job: rocketmq
          __path__: /opt/rocketmq/logs/*.log

  # 系统日志
  - job_name: syslog
    static_configs:
      - targets:
          - localhost
        labels:
          job: syslog
          __path__: /var/log/syslog
```

#### 启动服务

```bash
# 创建 positions 目录
sudo mkdir -p /var/lib/promtail

# 创建 systemd 服务
sudo vim /etc/systemd/system/promtail.service
```

```ini
[Unit]
Description=Promtail
After=network.target

[Service]
Type=simple
User=root
ExecStart=/opt/promtail -config.file=/etc/promtail/promtail-config.yml
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
# 启动服务
sudo systemctl daemon-reload
sudo systemctl start promtail
sudo systemctl enable promtail

# 验证
sudo systemctl status promtail
```

---

### 3. Grafana 配置 Loki 数据源

```bash
# 登录 Grafana (http://localhost:3000)
# Configuration -> Data Sources -> Add data source -> Loki

# 配置参数
Name: Loki
URL: http://localhost:3100
Access: Server (default)
```

### 4. Grafana 日志查询示例

```logql
# 查询所有 WePay V3 日志
{job="wepay-v3"}

# 查询错误日志
{job="wepay-v3", level="ERROR"}

# 查询特定模块的日志
{job="wepay-v3", module="OrderService"}

# 查询包含特定关键字的日志
{job="wepay-v3"} |= "payment failed"

# 查询特定设备的日志
{job="wepay-v3"} | json | device_id="device123"

# 统计错误日志数量
sum(rate({job="wepay-v3", level="ERROR"}[5m]))

# 按模块统计日志数量
sum by (module) (rate({job="wepay-v3"}[5m]))
```

---

## 二、ELK Stack 方案（可选）

### 1. Elasticsearch 安装

```bash
# 添加 Elasticsearch 仓库
wget -qO - https://artifacts.elastic.co/GPG-KEY-elasticsearch | sudo apt-key add -
echo "deb https://artifacts.elastic.co/packages/8.x/apt stable main" | sudo tee /etc/apt/sources.list.d/elastic-8.x.list

# 安装 Elasticsearch
sudo apt-get update
sudo apt-get install elasticsearch

# 配置 Elasticsearch
sudo vim /etc/elasticsearch/elasticsearch.yml
```

```yaml
cluster.name: wepay-elk
node.name: node-1
path.data: /var/lib/elasticsearch
path.logs: /var/log/elasticsearch
network.host: 0.0.0.0
http.port: 9200
discovery.type: single-node
xpack.security.enabled: false
```

```bash
# 启动服务
sudo systemctl start elasticsearch
sudo systemctl enable elasticsearch

# 验证
curl http://localhost:9200
```

### 2. Logstash 安装

```bash
# 安装 Logstash
sudo apt-get install logstash

# 配置 Logstash
sudo vim /etc/logstash/conf.d/wepay.conf
```

```ruby
input {
  # 从文件读取日志
  file {
    path => "/var/log/wepay/*.log"
    type => "wepay-v3"
    codec => json
  }
  
  file {
    path => "/var/log/nginx/wepay_access.log"
    type => "nginx-access"
  }
  
  file {
    path => "/var/log/nginx/wepay_error.log"
    type => "nginx-error"
  }
}

filter {
  # WePay V3 日志过滤
  if [type] == "wepay-v3" {
    json {
      source => "message"
    }
    date {
      match => ["timestamp", "ISO8601"]
      target => "@timestamp"
    }
  }
  
  # Nginx 访问日志过滤
  if [type] == "nginx-access" {
    grok {
      match => { "message" => "%{COMBINEDAPACHELOG}" }
    }
    date {
      match => ["timestamp", "dd/MMM/yyyy:HH:mm:ss Z"]
      target => "@timestamp"
    }
  }
}

output {
  elasticsearch {
    hosts => ["localhost:9200"]
    index => "wepay-%{type}-%{+YYYY.MM.dd}"
  }
  
  # 调试输出（可选）
  # stdout { codec => rubydebug }
}
```

```bash
# 启动服务
sudo systemctl start logstash
sudo systemctl enable logstash
```

### 3. Kibana 安装

```bash
# 安装 Kibana
sudo apt-get install kibana

# 配置 Kibana
sudo vim /etc/kibana/kibana.yml
```

```yaml
server.port: 5601
server.host: "0.0.0.0"
elasticsearch.hosts: ["http://localhost:9200"]
```

```bash
# 启动服务
sudo systemctl start kibana
sudo systemctl enable kibana

# 访问 http://localhost:5601
```

---

## 三、WePay V3 日志格式规范

### 1. 结构化日志格式（JSON）

```cpp
// 日志工具类
class Logger {
public:
    static void log(const std::string& level,
                   const std::string& module,
                   const std::string& message,
                   const nlohmann::json& context = ) {
        nlohmann::json logEntry;
        logEntry["timestamp"] = getCurrentTimestamp();
        logEntry["level"] = level;
        logEntry["module"] = module;
        logEntry["message"] = message;
        logEntry["hostname"] = getHostname();
        logEntry["pid"] = getpid();
        
        // 合并上下文
        for (auto& [key, value] : context.items()) {
            logEntry[key] = value;
        }
        
        // 输出到文件
        std::ofstream logFile("/var/log/wepay/app.log", std::ios::app);
        logFile << logEntry.dump() << std::endl;
    }
    
    static void info(const std::string& module, const std::string& message, const nlohmann::json& context = {}) {
        log("INFO", module, message, context);
    }
    
    static void warn(const std::string& module, const std::string& message, const nlohmann::json& context = {}) {
        log("WARN", module, message, context);
    }
    
    static void error(const std::string& module, const std::string& message, const nlohmann::json& context = {}) {
        log("ERROR", module, message, context);
    }
};
```

### 2. 使用示例

```cpp
// 订单处理日志
void OrderService::processOrder(const Order& order) {
    nlohmann::json context;
    context["order_id"] = order.orderId;
    context["merchant_id"] = order.merchantId;
    context["device_id"] = order.deviceId;
    context["amount"] = order.amount;
    
    Logger::info("OrderService", "Processing order", context);
    
    try {
        // 处理订单
        // ...
        
        Logger::info("OrderService", "Order processed successfully", context);
    } catch (const std::exception& e) {
        context["error"] = e.what();
        Logger::error("OrderService", "Order processing failed", context);
        throw;
    }
}

// 设备离线日志
void DeviceManager::onDeviceOffline(const std::string& deviceId) {
    nlohmann::json context;
    context["device_id"] = deviceId;
    context["last_heartbeat"] = getLastHeartbeat(deviceId);
    
    Logger::warn("DeviceManager", "Device offline detected", context);
}

// WebSocket 连接日志
void WebSocketController::onConnect(const std::string& deviceId) {
    nlohmann::json context;
    context["device_id"] = deviceId;
    context["remote_addr"] = getRemoteAddr();
    
    Logger::info("WebSocketController", "WebSocket connected", context);
}
```

### 3. 日志输出示例

```json
{
  "timestamp": "2024-01-23T10:30:45.123Z",
  "level": "INFO",
  "module": "OrderService",
  "message": "Processing order",
  "hostname": "wepay-server-1",
  "pid": 12345,
  "order_id": "ORD20240123001",
  "merchant_id": "M001",
  "device_id": "device123",
  "amount": 100.50
}
```

---

## 四、日志轮转配置

### logrotate 配置

```bash
# 创建 logrotate 配置
sudo vim /etc/logrotate.d/wepay
```

```conf
/var/log/wepay/*.log {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    create 0644 root root
    sharedscripts
    postrotate
        # 重新打开日志文件（如果需要）
        # systemctl reload wepay-v3
    endscript
}

/var/log/nginx/wepay_*.log {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    create 0644 www-data www-data
    sharedscripts
    postrotate
        systemctl reload nginx
    endscript
}
```

---

## 五、日志查询与分析

### Loki 查询示例

```bash
# 使用 logcli 查询日志
logcli query '{job="wepay-v3", level="ERROR"}' --limit=100 --since=1h

# 统计错误日志
logcli stats '{job="wepay-v3", level="ERROR"}' --since=24h

# 实时查看日志
logcli query '{job="wepay-v3"}' --tail --follow
```

### Grafana Dashboard 配置

```json
{
  "dashboard": {
    "title": "WePay V3 Logs",
    "panels": [
      {
        "title": "日志级别分布",
        "targets": [
          {
            "expr": "sum by (level) (rate({job=\"wepay-v3\"}[5m]))"
          }
        ]
      },
      {
        "title": "错误日志趋势",
        "targets": [
          {
            "expr": "sum(rate({job=\"wepay-v3\", level=\"ERROR\"}[5m]))"
          }
        ]
      },
      {
        "title": "最近错误日志",
        "targets": [
          {
            "expr": "{job=\"wepay-v3\", level=\"ERROR\"}"
          }
        ]
      }
    ]
  }
}
```

---

## 六、日志告警规则

### Loki 告警规则

```yaml
# /var/lib/loki/rules/wepay-alerts.yml

groups:
  - name: wepay_log_alerts
    interval: 1m
    rules:
      # 错误日志告警
      - alert: HighErrorLogRate
        expr: |
          sum(rate({job="wepay-v3", level="ERROR"}[5m])) > 10
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High error log rate"
          description: "Error log rate is {{ $value }} logs/sec"
      
      # 支付失败告警
      - alert: PaymentFailureDetected
        expr: |
          sum(rate({job="wepay-v3"} |= "payment failed" [5m])) > 5
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "Payment failures detected"
          description: "Payment failure rate is {{ $value }} failures/sec"
      
      # 设备离线告警
      - alert: DeviceOfflineDetected
        expr: |
          sum(rate({job="wepay-v3"} |= "device offline" [5m])) > 3
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Device offline events detected"
          description: "Device offline rate is {{ $value }} events/sec"
```

---

## 七、性能优化

### Loki 性能优化

```yaml
# 增加查询并发
limits_config:
  max_query_parallelism: 64

# 增加缓存
chunk_store_config:
  chunk_cache_config:
    enable_fifocache: true
    fifocache:
      max_size_bytes: 1GB
      ttl: 1h

# 压缩日志
storage_config:
  boltdb_shipper:
    compressor: snappy
```

### Promtail 性能优化

```yaml
# 批量发送
clients:
  - url: http://localhost:3100/loki/api/v1/push
    batchwait: 1s
    batchsize: 1048576  # 1MB

# 限制日志大小
limits_config:
  readline_rate: 10000
  readline_burst: 20000
```

---

## 配置文件位置

- Loki 配置: `/etc/loki/loki-config.yml`
- Promtail 配置: `/etc/promtail/promtail-config.yml`
- Loki 数据: `/var/lib/loki`
- Promtail positions: `/var/lib/promtail/positions.yaml`
- 日志文件: `/var/log/wepay/*.log`
- 日志轮转: `/etc/logrotate.d/wepay`
