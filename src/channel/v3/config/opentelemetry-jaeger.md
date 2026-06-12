# OpenTelemetry + Jaeger 分布式链路追踪配置指南

## 架构说明

### 组件
- **OpenTelemetry**: 统一的可观测性框架，采集 Trace、Metrics、Logs
- **Jaeger**: 分布式追踪系统，存储和展示 Trace 数据
- **OpenTelemetry Collector**: 数据收集和处理中间件

### 追踪链路示例
```
HTTP请求 -> Nginx -> WePay V3 -> Redis -> PostgreSQL -> RocketMQ
   |          |          |          |          |             |
   +----------+----------+----------+----------+-------------+
                         Trace ID: abc123
```

---

## 一、Jaeger 安装与配置

### 1. 使用 Docker 快速部署（推荐）

```bash
# 拉取 Jaeger All-in-One 镜像
docker pull jaegertracing/all-in-one:latest

# 启动 Jaeger
docker run -d --name jaeger \
  -e COLLECTOR_ZIPKIN_HOST_PORT=:9411 \
  -p 5775:5775/udp \
  -p 6831:6831/udp \
  -p 6832:6832/udp \
  -p 5778:5778 \
  -p 16686:16686 \
  -p 14250:14250 \
  -p 14268:14268 \
  -p 14269:14269 \
  -p 9411:9411 \
  jaegertracing/all-in-one:latest

# 访问 Jaeger UI: http://localhost:16686
```

### 2. 生产环境部署（使用 Elasticsearch 存储）

#### docker-compose.yml

```yaml
version: '3.8'

services:
  # Elasticsearch
  elasticsearch:
    image: docker.elastic.co/elasticsearch/elasticsearch:8.10.0
    environment:
      - discovery.type=single-node
      - xpack.security.enabled=false
      - "ES_JAVA_OPTS=-Xms2g -Xmx2g"
    ports:
      - "9200:9200"
    volumes:
      - es-data:/usr/share/elasticsearch/data

  # Jaeger Collector
  jaeger-collector:
    image: jaegertracing/jaeger-collector:latest
    environment:
      - SPAN_STORAGE_TYPE=elasticsearch
      - ES_SERVER_URLS=http://elasticsearch:9200
      - ES_TAGS_AS_FIELDS_ALL=true
    ports:
      - "14250:14250"
      - "14268:14268"
      - "14269:14269"
    depends_on:
      - elasticsearch

  # Jaeger Query
  jaeger-query:
    image: jaegertracing/jaeger-query:latest
    environment:
      - SPAN_STORAGE_TYPE=elasticsearch
      - ES_SERVER_URLS=http://elasticsearch:9200
    ports:
      - "16686:16686"
      - "16687:16687"
    depends_on:
      - elasticsearch

  # Jaeger Agent
  jaeger-agent:
    image: jaegertracing/jaeger-agent:latest
    command: ["--reporter.grpc.host-port=jaeger-collector:14250"]
    ports:
      - "5775:5775/udp"
      - "6831:6831/udp"
      - "6832:6832/udp"
      - "5778:5778"
    depends_on:
      - jaeger-collector

volumes:
  es-data:
```

```bash
# 启动服务
docker-compose up -d

# 查看日志
docker-compose logs -f
```

---

## 二、OpenTelemetry C++ SDK 集成

### 1. 安装 OpenTelemetry C++ SDK

```bash
# 克隆仓库
git clone --recurse-submodules https://github.com/open-telemetry/opentelemetry-cpp.git
cd opentelemetry-cpp

# 编译安装
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_JAEGER=ON \
  -DWITH_OTLP=ON \
  -DWITH_ZIPKIN=OFF \
  -DBUILD_TESTING=OFF

make -j$(nproc)
sudo make install
```

### 2. CMakeLists.txt 配置

```cmake
# 添加 OpenTelemetry 依赖
find_package(opentelemetry-cpp CONFIG REQUIRED)

target_link_libraries(wepay_v3_plugin PUBLIC
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::ext
    opentelemetry-cpp::exporters_jaeger
    opentelemetry-cpp::exporters_otlp_http
)
```

---

## 三、WePay V3 追踪集成

### 1. 追踪初始化

#### TracingManager.h

```cpp
#pragma once
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/exporters/jaeger/jaeger_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#include <opentelemetry/sdk/trace/batch_span_processor.h>
#include <opentelemetry/trace/provider.h>

namespace wepay {
namespace v3 {

class TracingManager {
public:
    static TracingManager& getInstance();
    
    void init(const std::string& serviceName, const std::string& jaegerEndpoint);
    void shutdown();
    
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> getTracer();

private:
    TracingManager() = default;
    
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> provider_;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer_;
};

} // namespace v3
} // namespace wepay
```

#### TracingManager.cpp

```cpp
#include "TracingManager.h"
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/resource/semantic_conventions.h>

namespace wepay {
namespace v3 {

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace jaeger = opentelemetry::exporter::jaeger;
namespace resource = opentelemetry::sdk::resource;

TracingManager& TracingManager::getInstance() {
    static TracingManager instance;
    return instance;
}

void TracingManager::init(const std::string& serviceName, const std::string& jaegerEndpoint) {
    // 创建 Jaeger Exporter
    jaeger::JaegerExporterOptions opts;
    opts.endpoint = jaegerEndpoint;  // "localhost:14250"
    opts.transport_format = jaeger::TransportFormat::kGrpc;
    
    auto exporter = std::unique_ptr<trace_sdk::SpanExporter>(
        new jaeger::JaegerExporter(opts)
    );
    
    // 创建 Batch Span Processor（批量处理，提高性能）
    trace_sdk::BatchSpanProcessorOptions processor_opts;
    processor_opts.max_queue_size = 2048;
    processor_opts.schedule_delay_millis = std::chrono::milliseconds(5000);
    processor_opts.max_export_batch_size = 512;
    
    auto processor = std::unique_ptr<trace_sdk::SpanProcessor>(
        new trace_sdk::BatchSpanProcessor(std::move(exporter), processor_opts)
    );
    
    // 创建 Resource（服务信息）
    auto resource_attributes = resource::ResourceAttributes{
        {resource::SemanticConventions::kServiceName, serviceName},
        {resource::SemanticConventions::kServiceVersion, "3.0.0"},
        {resource::SemanticConventions::kDeploymentEnvironment, "production"}
    };
    auto resource = resource::Resource::Create(resource_attributes);
    
    // 创建 Tracer Provider
    provider_ = opentelemetry::nostd::shared_ptr<trace_api::TracerProvider>(
        new trace_sdk::TracerProvider(std::move(processor), resource)
    );
    
    // 设置全局 Tracer Provider
    trace_api::Provider::SetTracerProvider(provider_);
    
    // 获取 Tracer
    tracer_ = provider_->GetTracer(serviceName, "3.0.0");
    
    LOG_INFO << "OpenTelemetry tracing initialized, service: " << serviceName;
}

void TracingManager::shutdown() {
    if (provider_) {
        // 刷新并关闭
        std::static_pointer_cast<trace_sdk::TracerProvider>(provider_)->Shutdown();
    }
}

opentelemetry::nostd::shared_ptr<trace_api::Tracer> TracingManager::getTracer() {
    return tracer_;
}

} // namespace v3
} // namespace wepay
```

### 2. HTTP 请求追踪

#### TracingMiddleware.h

```cpp
#pragma once
#include <drogon/HttpFilter.h>
#include <opentelemetry/trace/span.h>

namespace wepay {
namespace v3 {

class TracingMiddleware : public drogon::HttpFilter<TracingMiddleware> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                 drogon::FilterCallback&& fcb,
                 drogon::FilterChainCallback&& fccb) override;
};

} // namespace v3
} // namespace wepay
```

#### TracingMiddleware.cpp

```cpp
#include "TracingMiddleware.h"
#include "TracingManager.h"
#include <opentelemetry/trace/propagation/http_trace_context.h>

namespace wepay {
namespace v3 {

namespace trace_api = opentelemetry::trace;
namespace context = opentelemetry::context;

void TracingMiddleware::doFilter(
    const drogon::HttpRequestPtr& req,
    drogon::FilterCallback&& fcb,
    drogon::FilterChainCallback&& fccb) {
    
    auto tracer = TracingManager::getInstance().getTracer();
    
    // 从 HTTP Header 提取 Trace Context
    context::propagation::HttpTraceContext propagator;
    auto carrier = req->getHeaders();
    auto ctx = propagator.Extract(carrier, context::RuntimeContext::GetCurrent());
    
    // 创建 Span
    trace_api::StartSpanOptions options;
    options.kind = trace_api::SpanKind::kServer;
    options.parent = trace_api::GetSpan(ctx)->GetContext();
    
    auto span = tracer->StartSpan(
        req->getMethod() + " " + req->getPath(),
        options
    );
    
    // 设置 Span 属性
    span->SetAttribute("http.method", req->getMethodString());
    span->SetAttribute("http.url", req->getPath());
    span->SetAttribute("http.target", req->getPath());
    span->SetAttribute("http.host", req->getHeader("Host"));
    span->SetAttribute("http.scheme", "https");
    span->SetAttribute("http.user_agent", req->getHeader("User-Agent"));
    span->SetAttribute("http.client_ip", req->getPeerAddr().toIp());
    
    // 将 Span 存储到请求属性中
    req->attributes()->insert("otel_span", span);
    
    // 继续处理请求
    fccb();
    
    // 请求完成后设置响应状态
    auto resp = req->attributes()->get<drogon::HttpResponsePtr>("response");
    if (resp) {
        span->SetAttribute("http.status_code", resp->getStatusCode());
        
        if (resp->getStatusCode() >= 400) {
            span->SetStatus(trace_api::StatusCode::kError);
        } else {
            span->SetStatus(trace_api::StatusCode::kOk);
        }
    }
    
    // 结束 Span
    span->End();
}

} // namespace v3
} // namespace wepay
```

### 3. 业务逻辑追踪

#### OrderService.cpp

```cpp
#include "TracingManager.h"

namespace trace_api = opentelemetry::trace;

void OrderService::processOrder(const Order& order) {
    auto tracer = TracingManager::getInstance().getTracer();
    
    // 创建子 Span
    auto span = tracer->StartSpan("OrderService.processOrder");
    
    // 设置 Span 属性
    span->SetAttribute("order.id", order.orderId);
    span->SetAttribute("order.merchant_id", order.merchantId);
    span->SetAttribute("order.device_id", order.deviceId);
    span->SetAttribute("order.amount", order.amount);
    span->SetAttribute("order.pay_type", order.payType);
    
    try {
        // 1. 验证订单
        {
            auto validateSpan = tracer->StartSpan(
                "validateOrder",
                {{"parent", span->GetContext()}}
            );
            validateOrder(order);
            validateSpan->End();
        }
        
        // 2. 查询 Redis 缓存
        {
            auto redisSpan = tracer->StartSpan(
                "redis.get",
                {{"parent", span->GetContext()}}
            );
            redisSpan->SetAttribute("db.system", "redis");
            redisSpan->SetAttribute("db.operation", "get");
            redisSpan->SetAttribute("db.key", "order:" + order.orderId);
            
            auto cached = redis_->get("order:" + order.orderId);
            
            redisSpan->End();
        }
        
        // 3. 写入 PostgreSQL
        {
            auto dbSpan = tracer->StartSpan(
                "postgresql.insert",
                {{"parent", span->GetContext()}}
            );
            dbSpan->SetAttribute("db.system", "postgresql");
            dbSpan->SetAttribute("db.operation", "insert");
            dbSpan->SetAttribute("db.table", "orders");
            
            insertOrderToDb(order);
            
            dbSpan->End();
        }
        
        // 4. 发送 RocketMQ 消息
        {
            auto mqSpan = tracer->StartSpan(
                "rocketmq.send",
                {{"parent", span->GetContext()}}
            );
            mqSpan->SetAttribute("messaging.system", "rocketmq");
            mqSpan->SetAttribute("messaging.destination", "order-push-topic");
            mqSpan->SetAttribute("messaging.operation", "send");
            
            sendToRocketMQ(order);
            
            mqSpan->End();
        }
        
        span->SetStatus(trace_api::StatusCode::kOk);
        
    } catch (const std::exception& e) {
        // 记录异常
        span->SetStatus(trace_api::StatusCode::kError, e.what());
        span->AddEvent("exception", {
            {"exception.type", typeid(e).name()},
            {"exception.message", e.what()}
        });
        
        throw;
    }
    
    // 结束 Span
    span->End();
}
```

### 4. Redis 操作追踪

```cpp
class TracedRedisClient {
public:
    TracedRedisClient(std::shared_ptr<sw::redis::Redis> redis)
        : redis_(redis) {}
    
    std::optional<std::string> get(const std::string& key) {
        auto tracer = TracingManager::getInstance().getTracer();
        auto span = tracer->StartSpan("redis.get");
        
        span->SetAttribute("db.system", "redis");
        span->SetAttribute("db.operation", "get");
        span->SetAttribute("db.key", key);
        
        try {
            auto result = redis_->get(key);
            span->SetStatus(trace_api::StatusCode::kOk);
            span->End();
            return result;
        } catch (const std::exception& e) {
            span->SetStatus(trace_api::StatusCode::kError, e.what());
            span->End();
            throw;
        }
    }
    
    void set(const std::string& key, const std::string& value, int ttl = 0) {
        auto tracer = TracingManager::getInstance().getTracer();
        auto span = tracer->StartSpan("redis.set");
        
        span->SetAttribute("db.system", "redis");
        span->SetAttribute("db.operation", "set");
        span->SetAttribute("db.key", key);
        if (ttl > 0) {
            span->SetAttribute("db.ttl", ttl);
        }
        
        try {
            if (ttl > 0) {
                redis_->setex(key, ttl, value);
            } else {
                redis_->set(key, value);
            }
            span->SetStatus(trace_api::StatusCode::kOk);
            span->End();
        } catch (const std::exception& e) {
            span->SetStatus(trace_api::StatusCode::kError, e.what());
            span->End();
            throw;
        }
    }

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};
```

### 5. PostgreSQL 操作追踪

```cpp
void OrderService::insertOrderToDb(const Order& order) {
    auto tracer = TracingManager::getInstance().getTracer();
    auto span = tracer->StartSpan("postgresql.insert");
    
    span->SetAttribute("db.system", "postgresql");
    span->SetAttribute("db.operation", "insert");
    span->SetAttribute("db.table", "orders");
    span->SetAttribute("db.statement", "INSERT INTO orders (order_id, merchant_id, ...) VALUES (?, ?, ...)");
    
    try {
        auto db = drogon::app().getDbClient();
        db->execSqlSync(
            "INSERT INTO orders (order_id, merchant_id, device_id, amount, pay_type, status, create_time) "
            "VALUES ($1, $2, $3, $4, $5, $6, NOW())",
            order.orderId,
            order.merchantId,
            order.deviceId,
            order.amount,
            order.payType,
            "pending"
        );
        
        span->SetStatus(trace_api::StatusCode::kOk);
    } catch (const std::exception& e) {
        span->SetStatus(trace_api::StatusCode::kError, e.what());
        throw;
    }
    
    span->End();
}
```

---

## 四、配置文件

### config.yaml

```yaml
tracing:
  enabled: true
  serviceName: "wepay-v3"
  jaegerEndpoint: "localhost:14250"
  samplingRate: 1.0  # 采样率（1.0 = 100%）
```

### WepayV3Plugin.cpp 初始化

```cpp
void WepayV3Plugin::init(const std::string& configFile) {
    // ... 其他初始化
    
    // 初始化追踪
    if (config.tracing.enabled) {
        TracingManager::getInstance().init(
            config.tracing.serviceName,
            config.tracing.jaegerEndpoint
        );
        LOG_INFO << "Tracing initialized";
    }
}

void WepayV3Plugin::shutdown() {
    // ... 其他清理
    
    // 关闭追踪
    TracingManager::getInstance().shutdown();
}
```

---

## 五、Jaeger UI 使用

### 1. 访问 Jaeger UI

```
http://localhost:16686
```

### 2. 查询 Trace

- **Service**: 选择 `wepay-v3`
- **Operation**: 选择具体操作（如 `POST /api/wepay/v3/push`）
- **Tags**: 添加过滤条件（如 `order.id=ORD123`）
- **Lookback**: 选择时间范围

### 3. Trace 详情

每个 Trace 包含：
- **Trace ID**: 唯一标识
- **Spans**: 操作列表（按时间顺序）
- **Duration**: 总耗时
- **Services**: 涉及的服务

每个 Span 包含：
- **Operation Name**: 操作名称
- **Duration**: 耗时
- **Tags**: 属性（如 order_id, status）
- **Logs**: 事件（如异常）

---

## 六、性能优化

### 1. 采样策略

```cpp
// 基于概率采样（生产环境推荐 10%）
trace_sdk::AlwaysOnSampler sampler;  // 100% 采样
// trace_sdk::TraceIdRatioBasedSampler sampler(0.1);  // 10% 采样

auto provider = trace_sdk::TracerProvider::Create(
    std::move(processor),
    resource,
    std::make_unique<trace_sdk::AlwaysOnSampler>()
);
```

### 2. 批量导出

```cpp
// 批量处理配置
trace_sdk::BatchSpanProcessorOptions opts;
opts.max_queue_size = 2048;           // 队列大小
opts.schedule_delay_millis = std::chrono::milliseconds(5000);  // 5秒导出一次
opts.max_export_batch_size = 512;     // 每批最多512个 Span
```

### 3. 异步导出

使用 `BatchSpanProcessor` 而不是 `SimpleSpanProcessor`，避免阻塞主线程。

---

## 七、监控指标

### Jaeger 监控指标

```bash
# Jaeger Collector 指标
curl http://localhost:14269/metrics

# 关键指标
jaeger_collector_spans_received_total
jaeger_collector_spans_saved_total
jaeger_collector_spans_dropped_total
jaeger_collector_queue_length
```

### Grafana Dashboard

导入 Jaeger 官方 Dashboard：
- Dashboard ID: 10001
- URL: https://grafana.com/grafana/dashboards/10001

---

## 八、故障排查

### 1. Trace 未显示

```bash
# 检查 Jaeger Agent 是否运行
docker ps | grep jaeger-agent

# 检查应用日志
tail -f /var/log/wepay/app.log | grep -i tracing

# 检查 Jaeger Collector 日志
docker logs jaeger-collector
```

### 2. 性能问题

```cpp
// 降低采样率
trace_sdk::TraceIdRatioBasedSampler sampler(0.01);  // 1% 采样

// 增加批量大小
opts.max_export_batch_size = 1024;
opts.schedule_delay_millis = std::chrono::milliseconds(10000);
```

---

## 九、最佳实践

### 1. Span 命名规范

- HTTP: `GET /api/wepay/v3/push`
- 数据库: `postgresql.insert.orders`
- 缓存: `redis.get.order:123`
- 消息队列: `rocketmq.send.order-push-topic`

### 2. 属性设置

```cpp
// 必须设置的属性
span->SetAttribute("service.name", "wepay-v3");
span->SetAttribute("service.version", "3.0.0");

// 业务属性
span->SetAttribute("order.id", orderId);
span->SetAttribute("merchant.id", merchantId);
span->SetAttribute("device.id", deviceId);

// 技术属性
span->SetAttribute("db.system", "postgresql");
span->SetAttribute("db.operation", "insert");
span->SetAttribute("http.status_code", 200);
```

### 3. 异常处理

```cpp
try {
    // 业务逻辑
} catch (const std::exception& e) {
    span->SetStatus(trace_api::StatusCode::kError, e.what());
    span->AddEvent("exception", {
        {"exception.type", typeid(e).name()},
        {"exception.message", e.what()},
        {"exception.stacktrace", getStackTrace()}
    });
    throw;
}
```

---

## 配置文件位置

- Jaeger 配置: `docker-compose.yml`
- OpenTelemetry 配置: `config.yaml`
- 追踪代码: `src/channel/v3/TracingManager.cpp`
- 中间件: `src/channel/v3/TracingMiddleware.cpp`
