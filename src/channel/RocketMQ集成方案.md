# WePay V3 RocketMQ 集成方案

## 一、RocketMQ 架构部署

### 1.1 集群架构
```
┌─────────────────────────────────────────────────────────────┐
│  NameServer 集群（服务发现与路由）                           │
│  NameServer1:9876  NameServer2:9876  NameServer3:9876       │
└────────────────────────┬────────────────────────────────────┘
                         │
         ┌───────────────┼───────────────┐
         ▼               ▼               ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│  Broker-a   │  │  Broker-b   │  │  Broker-c   │
│  Master     │  │  Master     │  │  Master     │
│  10911      │  │  10911      │  │  10911      │
└──────┬──────┘  └──────┬──────┘  └──────┬──────┘
       │                │                │
┌──────┴──────┐  ┌──────┴──────┐  ┌──────┴──────┐
│  Broker-a   │  │  Broker-b   │  │  Broker-c   │
│  Slave      │  │  Slave      │  │  Slave      │
│  10911      │  │  10911      │  │  10911      │
└─────────────┘  └─────────────┘  └─────────────┘
```

### 1.2 部署配置

**NameServer 配置（namesrv.conf）**
```properties
# 监听端口
listenPort=9876
# 删除Topic时是否删除对应的权限信息
deleteWhen=04
# 文件保留时间（小时）
fileReservedTime=48
```

**Broker Master 配置（broker-a.conf）**
```properties
# Broker 名称
brokerClusterName=WepayCluster
brokerName=broker-a
brokerId=0
# NameServer 地址
namesrvAddr=nameserver1:9876;nameserver2:9876;nameserver3:9876
# 监听端口
listenPort=10911
# 存储路径
storePathRootDir=/data/rocketmq/store-a
storePathCommitLog=/data/rocketmq/store-a/commitlog
# 同步刷盘（保证消息不丢失）
flushDiskType=SYNC_FLUSH
# 主从同步方式（同步双写）
brokerRole=SYNC_MASTER
# 自动创建Topic
autoCreateTopicEnable=false
# 消息删除时间（凌晨4点）
deleteWhen=04
# 文件保留时间（72小时）
fileReservedTime=72
```

**Broker Slave 配置（broker-a-s.conf）**
```properties
brokerClusterName=WepayCluster
brokerName=broker-a
brokerId=1
namesrvAddr=nameserver1:9876;nameserver2:9876;nameserver3:9876
listenPort=10911
storePathRootDir=/data/rocketmq/store-a-s
storePathCommitLog=/data/rocketmq/store-a-s/commitlog
flushDiskType=SYNC_FLUSH
brokerRole=SLAVE
autoCreateTopicEnable=false
```

## 二、Topic 设计与配置

### 2.1 Topic 列表

| Topic名称 | 用途 | 消息类型 | 队列数 | 重试次数 |
|----------|------|---------|--------|---------|
| wepay_order_push | 订单推送 | 事务消息 | 8 | 3 |
| wepay_ocr_task | OCR识别 | 普通消息 | 4 | 3 |
| wepay_image_compress | 截图压缩 | 普通消息 | 4 | 2 |
| wepay_alert_notify | 告警通知 | 延迟消息 | 2 | 5 |
| wepay_order_timeout | 订单超时 | 延迟消息 | 4 | 1 |
| wepay_device_status | 设备状态变更 | 顺序消息 | 8 | 3 |

### 2.2 创建 Topic 命令

```bash
# 订单推送Topic（事务消息）
sh mqadmin updateTopic -n localhost:9876 \
  -t wepay_order_push \
  -c WepayCluster \
  -r 8 -w 8 \
  -o true

# OCR识别Topic
sh mqadmin updateTopic -n localhost:9876 \
  -t wepay_ocr_task \
  -c WepayCluster \
  -r 4 -w 4

# 截图压缩Topic
sh mqadmin updateTopic -n localhost:9876 \
  -t wepay_image_compress \
  -c WepayCluster \
  -r 4 -w 4

# 告警通知Topic
sh mqadmin updateTopic -n localhost:9876 \
  -t wepay_alert_notify \
  -c WepayCluster \
  -r 2 -w 2

# 订单超时Topic
sh mqadmin updateTopic -n localhost:9876 \
  -t wepay_order_timeout \
  -c WepayCluster \
  -r 4 -w 4

# 设备状态变更Topic
sh mqadmin updateTopic -n localhost:9876 \
  -t wepay_device_status \
  -c WepayCluster \
  -r 8 -w 8
```

## 三、消息生产者实现

### 3.1 订单推送（事务消息）

```cpp
#include <rocketmq/DefaultMQProducer.h>
#include <rocketmq/TransactionMQProducer.h>

class OrderPushTransactionListener : public rocketmq::TransactionListener {
public:
    // 执行本地事务
    rocketmq::LocalTransactionState executeLocalTransaction(
        const rocketmq::Message& msg,
        void* arg) override {
        
        std::string orderId = msg.getProperty("orderId");
        
        try {
            // 1. 更新订单状态为"推送中"
            bool success = updateOrderStatus(orderId, "PUSHING");
            
            if (success) {
                return rocketmq::LocalTransactionState::COMMIT_MESSAGE;
            } else {
                return rocketmq::LocalTransactionState::ROLLBACK_MESSAGE;
            }
        } catch (const std::exception& e) {
            // 异常时返回UNKNOW，等待回查
            return rocketmq::LocalTransactionState::UNKNOW;
        }
    }
    
    // 回查本地事务状态
    rocketmq::LocalTransactionState checkLocalTransaction(
        const rocketmq::MessageExt& msg) override {
        
        std::string orderId = msg.getProperty("orderId");
        
        // 查询订单状态
        std::string status = getOrderStatus(orderId);
        
        if (status == "PUSHING" || status == "PUSHED") {
            return rocketmq::LocalTransactionState::COMMIT_MESSAGE;
        } else if (status == "FAILED") {
            return rocketmq::LocalTransactionState::ROLLBACK_MESSAGE;
        } else {
            return rocketmq::LocalTransactionState::UNKNOW;
        }
    }
};

class OrderPushProducer {
private:
    std::unique_ptr<rocketmq::TransactionMQProducer> producer_;
    
public:
    OrderPushProducer(const std::string& namesrvAddr) {
        producer_ = std::make_unique<rocketmq::TransactionMQProducer>("wepay_order_push_group");
        producer_->setNamesrvAddr(namesrvAddr);
        producer_->setInstanceName("order_push_producer");
        
        // 设置事务监听器
        auto listener = std::make_shared<OrderPushTransactionListener>();
        producer_->setTransactionListener(listener.get());
        
        producer_->start();
    }
    
    bool sendOrderPushMessage(const std::string& orderId, 
                              const std::string& deviceId,
                              double amount) {
        try {
            rocketmq::Message msg("wepay_order_push",
                                 "*",  // tag
                                 orderId,  // keys
                                 createOrderPushBody(orderId, deviceId, amount));
            
            // 设置消息属性
            msg.setProperty("orderId", orderId);
            msg.setProperty("deviceId", deviceId);
            msg.setProperty("amount", std::to_string(amount));
            
            // 发送事务消息
            auto result = producer_->sendMessageInTransaction(msg, nullptr);
            
            return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
        } catch (const std::exception& e) {
            // 记录日志
            return false;
        }
    }
};
```

### 3.2 OCR识别任务（普通消息）

```cpp
class OcrTaskProducer {
private:
    std::unique_ptr<rocketmq::DefaultMQProducer> producer_;
    
public:
    OcrTaskProducer(const std::string& namesrvAddr) {
        producer_ = std::make_unique<rocketmq::DefaultMQProducer>("wepay_ocr_group");
        producer_->setNamesrvAddr(namesrvAddr);
        producer_->setInstanceName("ocr_task_producer");
        producer_->start();
    }
    
    bool sendOcrTask(const std::string& orderId,
                     const std::string& imageUrl) {
        try {
            nlohmann::json body;
            body["orderId"] = orderId;
            body["imageUrl"] = imageUrl;
            body["timestamp"] = std::time(nullptr);
            
            rocketmq::Message msg("wepay_ocr_task",
                                 "OCR",
                                 orderId,
                                 body.dump());
            
            auto result = producer_->send(msg);
            return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
        } catch (const std::exception& e) {
            return false;
        }
    }
};
```

### 3.3 告警通知（延迟消息）

```cpp
class AlertNotifyProducer {
private:
    std::unique_ptr<rocketmq::DefaultMQProducer> producer_;
    
public:
    AlertNotifyProducer(const std::string& namesrvAddr) {
        producer_ = std::make_unique<rocketmq::DefaultMQProducer>("wepay_alert_group");
        producer_->setNamesrvAddr(namesrvAddr);
        producer_->start();
    }
    
    // RocketMQ延迟级别：1s 5s 10s 30s 1m 2m 3m 4m 5m 6m 7m 8m 9m 10m 20m 30m 1h 2h
    bool sendDeviceOfflineAlert(const std::string& deviceId, int delayLevel = 3) {
        try {
            nlohmann::json body;
            body["type"] = "DEVICE_OFFLINE";
            body["deviceId"] = deviceId;
            body["timestamp"] = std::time(nullptr);
            
            rocketmq::Message msg("wepay_alert_notify",
                                 "DEVICE_OFFLINE",
                                 deviceId,
                                 body.dump());
            
            // 设置延迟级别（3=10秒，防止抖动）
            msg.setDelayTimeLevel(delayLevel);
            
            auto result = producer_->send(msg);
            return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
        } catch (const std::exception& e) {
            return false;
        }
    }
};
```

### 3.4 订单超时（延迟消息）

```cpp
class OrderTimeoutProducer {
private:
    std::unique_ptr<rocketmq::DefaultMQProducer> producer_;
    
public:
    bool sendOrderTimeoutMessage(const std::string& orderId, int timeoutMinutes) {
        try {
            nlohmann::json body;
            body["orderId"] = orderId;
            body["createTime"] = std::time(nullptr);
            
            rocketmq::Message msg("wepay_order_timeout",
                                 "TIMEOUT",
                                 orderId,
                                 body.dump());
            
            // 根据超时时间选择延迟级别
            // 5分钟=level 9, 10分钟=level 10, 30分钟=level 16
            int delayLevel = getDelayLevel(timeoutMinutes);
            msg.setDelayTimeLevel(delayLevel);
            
            auto result = producer_->send(msg);
            return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
        } catch (const std::exception& e) {
            return false;
        }
    }
    
private:
    int getDelayLevel(int minutes) {
        if (minutes <= 5) return 9;   // 5分钟
        if (minutes <= 10) return 10; // 10分钟
        if (minutes <= 30) return 16; // 30分钟
        return 17; // 1小时
    }
};
```

## 四、消息消费者实现

### 4.1 订单推送消费者

```cpp
class OrderPushConsumer : public rocketmq::MessageListenerConcurrently {
public:
    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override {
        
        for (const auto& msg : msgs) {
            try {
                std::string orderId = msg.getProperty("orderId");
                std::string deviceId = msg.getProperty("deviceId");
                
                // 解析消息体
                nlohmann::json body = nlohmann::json::parse(msg.getBody());
                
                // 推送订单到设备（通过WebSocket）
                bool success = pushOrderToDevice(deviceId, body);
                
                if (success) {
                    // 更新订单状态
                    updateOrderStatus(orderId, "PUSHED");
                } else {
                    // 推送失败，返回RECONSUME_LATER触发重试
                    return rocketmq::ConsumeStatus::RECONSUME_LATER;
                }
            } catch (const std::exception& e) {
                // 异常时重试
                return rocketmq::ConsumeStatus::RECONSUME_LATER;
            }
        }
        
        return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
    }
    
    void start(const std::string& namesrvAddr) {
        consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_order_push_consumer");
        consumer_->setNamesrvAddr(namesrvAddr);
        consumer_->subscribe("wepay_order_push", "*");
        consumer_->registerMessageListener(this);
        consumer_->setConsumeThreadCount(4);
        consumer_->setMaxReconsumeTimes(3);  // 最多重试3次
        consumer_->start();
    }
    
private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;
};
```

### 4.2 OCR识别消费者

```cpp
class OcrTaskConsumer : public rocketmq::MessageListenerConcurrently {
public:
    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override {
        
        for (const auto& msg : msgs) {
            try {
                nlohmann::json body = nlohmann::json::parse(msg.getBody());
                std::string orderId = body["orderId"];
                std::string imageUrl = body["imageUrl"];
                
                // 执行OCR识别
                auto ocrResult = performOcr(imageUrl);
                
                if (ocrResult.success) {
                    // 保存识别结果
                    saveOcrResult(orderId, ocrResult);
                    
                    // 匹配订单金额
                    matchOrderAmount(orderId, ocrResult.amount);
                } else {
                    // OCR失败，重试
                    return rocketmq::ConsumeStatus::RECONSUME_LATER;
                }
            } catch (const std::exception& e) {
                return rocketmq::ConsumeStatus::RECONSUME_LATER;
            }
        }
        
        return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
    }
    
    void start(const std::string& namesrvAddr) {
        consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_ocr_consumer");
        consumer_->setNamesrvAddr(namesrvAddr);
        consumer_->subscribe("wepay_ocr_task", "*");
        consumer_->registerMessageListener(this);
        consumer_->setConsumeThreadCount(2);
        consumer_->setMaxReconsumeTimes(3);
        consumer_->start();
    }
    
private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;
};
```

### 4.3 订单超时消费者

```cpp
class OrderTimeoutConsumer : public rocketmq::MessageListenerConcurrently {
public:
    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override {
        
        for (const auto& msg : msgs) {
            try {
                nlohmann::json body = nlohmann::json::parse(msg.getBody());
                std::string orderId = body["orderId"];
                
                // 查询订单当前状态
                std::string status = getOrderStatus(orderId);
                
                // 如果订单仍未支付，则取消订单
                if (status == "PENDING" || status == "PUSHING") {
                    cancelOrder(orderId, "TIMEOUT");
                    
                    // 发送超时告警
                    sendTimeoutAlert(orderId);
                }
                // 如果已支付，则忽略
                
            } catch (const std::exception& e) {
                // 超时处理失败不重试，记录日志即可
            }
        }
        
        return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
    }
    
    void start(const std::string& namesrvAddr) {
        consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_timeout_consumer");
        consumer_->setNamesrvAddr(namesrvAddr);
        consumer_->subscribe("wepay_order_timeout", "*");
        consumer_->registerMessageListener(this);
        consumer_->setConsumeThreadCount(2);
        consumer_->start();
    }
    
private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;
};
```

## 五、监控与运维

### 5.1 监控指标

```cpp
class RocketMQMonitor {
public:
    struct Metrics {
        // 生产者指标
        int64_t totalSent;
        int64_t sendSuccess;
        int64_t sendFailed;
        double avgSendLatency;
        
        // 消费者指标
        int64_t totalConsumed;
        int64_t consumeSuccess;
        int64_t consumeFailed;
        int64_t consumeRetry;
        double avgConsumeLatency;
        
        // 消息积压
        int64_t messageBacklog;
    };
    
    Metrics getTopicMetrics(const std::string& topic) {
        // 通过RocketMQ Admin API获取指标
        // ...
    }
    
    void checkMessageBacklog() {
        auto metrics = getTopicMetrics("wepay_order_push");
        
        if (metrics.messageBacklog > 1000) {
            // 告警：消息积压超过1000条
            sendAlert("MESSAGE_BACKLOG", "订单推送消息积压: " + 
                     std::to_string(metrics.messageBacklog));
        }
    }
};
```

### 5.2 死信队列处理

```cpp
class DeadLetterQueueHandler {
public:
    void processDLQ() {
        // 消费死信队列
        consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_dlq_consumer");
        consumer_->subscribe("%DLQ%wepay_order_push_consumer", "*");
        
        // 记录到数据库，人工处理
        consumer_->registerMessageListener([](const std::vector<rocketmq::MessageExt>& msgs) {
            for (const auto& msg : msgs) {
                saveToDLQTable(msg);
                sendDLQAlert(msg);
            }
            return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
        });
        
        consumer_->start();
    }
};
```

## 六、配置管理

### 6.1 配置文件（rocketmq.yaml）

```yaml
rocketmq:
  namesrv_addr: "nameserver1:9876;nameserver2:9876;nameserver3:9876"
  
  producers:
    order_push:
      group: "wepay_order_push_group"
      send_timeout: 3000
      retry_times: 2
      
    ocr_task:
      group: "wepay_ocr_group"
      send_timeout: 3000
      retry_times: 2
      
  consumers:
    order_push:
      group: "wepay_order_push_consumer"
      thread_count: 4
      max_reconsume_times: 3
      consume_timeout: 15
      
    ocr_task:
      group: "wepay_ocr_consumer"
      thread_count: 2
      max_reconsume_times: 3
      consume_timeout: 30
```

### 6.2 配置加载

```cpp
class RocketMQConfig {
public:
    static RocketMQConfig& getInstance() {
        static RocketMQConfig instance;
        return instance;
    }
    
    void loadConfig(const std::string& configFile) {
        YAML::Node config = YAML::LoadFile(configFile);
        
        namesrvAddr_ = config["rocketmq"]["namesrv_addr"].as<std::string>();
        
        // 加载生产者配置
        auto producers = config["rocketmq"]["producers"];
        orderPushGroup_ = producers["order_push"]["group"].as<std::string>();
        
        // 加载消费者配置
        auto consumers = config["rocketmq"]["consumers"];
        orderPushConsumerGroup_ = consumers["order_push"]["group"].as<std::string>();
        orderPushThreadCount_ = consumers["order_push"]["thread_count"].as<int>();
    }
    
    std::string getNamesrvAddr() const { return namesrvAddr_; }
    std::string getOrderPushGroup() const { return orderPushGroup_; }
    
private:
    std::string namesrvAddr_;
    std::string orderPushGroup_;
    std::string orderPushConsumerGroup_;
    int orderPushThreadCount_;
};
```

## 七、最佳实践

### 7.1 消息幂等性保证

```cpp
class MessageIdempotentHandler {
public:
    bool isDuplicate(const std::string& msgId) {
        // 使用Redis SET结构去重
        std::string key = "mq:consumed:" + msgId;
        
        // SETNX：如果key不存在则设置，返回1；存在则返回0
        bool isNew = redis_->setnx(key, "1");
        
        if (isNew) {
            // 设置过期时间（24小时）
            redis_->expire(key, 86400);
            return false;  // 不是重复消息
        }
        
        return true;  // 重复消息
    }
};
```

### 7.2 消息顺序性保证

```cpp
class OrderedMessageProducer {
public:
    bool sendOrderedMessage(const std::string& deviceId, const std::string& body) {
        rocketmq::Message msg("wepay_device_status", body);
        
        // 使用deviceId作为sharding key，保证同一设备的消息进入同一队列
        auto selector = [](const std::vector<rocketmq::MessageQueue>& mqs,
                          const rocketmq::Message& msg,
                          void* arg) -> rocketmq::MessageQueue {
            std::string deviceId = *static_cast<std::string*>(arg);
            int index = std::hash<std::string>{}(deviceId) % mqs.size();
            return mqs[index];
        };
        
        auto result = producer_->send(msg, selector, (void*)&deviceId);
        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    }
};
```

### 7.3 消息过滤

```cpp
// 生产者发送时设置Tag
rocketmq::Message msg("wepay_alert_notify", "DEVICE_OFFLINE", body);

// 消费者订阅时过滤Tag
consumer_->subscribe("wepay_alert_notify", "DEVICE_OFFLINE || ORDER_TIMEOUT");
```

## 八、故障处理

### 8.1 常见问题

1. **消息发送失败**
   - 检查NameServer连接
   - 检查Broker状态
   - 检查网络连通性

2. **消息积压**
   - 增加消费者线程数
   - 增加消费者实例数
   - 优化消费逻辑

3. **消息丢失**
   - 确认使用同步刷盘
   - 确认主从同步
   - 检查消费者ACK机制

### 8.2 应急预案

```cpp
class RocketMQEmergency {
public:
    // 紧急降级：关闭消息队列，改为同步处理
    void degradeToSync() {
        // 停止生产者
        stopAllProducers();
        
        // 停止消费者
        stopAllConsumers();
        
        // 启用同步处理模式
        enableSyncMode();
    }
    
    // 恢复正常
    void recoverToAsync() {
        // 启动生产者
        startAllProducers();
        
        // 启动消费者
        startAllConsumers();
        
        // 关闭同步模式
        disableSyncMode();
    }
};
```
