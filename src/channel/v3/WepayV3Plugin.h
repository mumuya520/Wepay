#pragma once
#include <memory>
#include "WepayV3Config.h"
#include "EmailService.h"
#include "OcrService.h"
#include "OrderService.h"
#include "DeviceManager.h"
#ifdef ENABLE_ROCKETMQ
#include "RocketMQManager.h"
#endif

namespace wepay {
namespace v3 {

// 前向声明
class WebSocketConnectionManager;
class WebSocketMessageSubscriber;
class BloomFilter;

// WePay V3 插件主类
class WepayV3Plugin {
public:
    static WepayV3Plugin& getInstance() {
        static WepayV3Plugin instance;
        return instance;
    }

    // 初始化插件
    bool init(const std::string& configFile);

    // 关闭插件
    void shutdown();

    // 获取各个服务实例
    std::shared_ptr<EmailService> getEmailService() { return emailService_; }
    std::shared_ptr<OcrService> getOcrService() { return ocrService_; }
    std::shared_ptr<OrderService> getOrderService() { return orderService_; }
    std::shared_ptr<DeviceManager> getDeviceManager() { return deviceManager_; }
    std::shared_ptr<WebSocketConnectionManager> getWebSocketConnectionManager() { return wsConnectionManager_; }
    std::shared_ptr<BloomFilter> getDeviceBloomFilter() { return deviceBloomFilter_; }
    std::shared_ptr<BloomFilter> getOrderBloomFilter() { return orderBloomFilter_; }

    // 启动定时任务
    void startScheduledTasks();

private:
    WepayV3Plugin() = default;
    ~WepayV3Plugin() = default;

    bool initialized_ = false;

    // 服务实例
    std::shared_ptr<EmailService> emailService_;
    std::shared_ptr<OcrService> ocrService_;
    std::shared_ptr<OrderService> orderService_;
    std::shared_ptr<DeviceManager> deviceManager_;

    // WebSocket连接管理
    std::shared_ptr<WebSocketConnectionManager> wsConnectionManager_;
    std::shared_ptr<WebSocketMessageSubscriber> wsMessageSubscriber_;

    // 布隆过滤器
    std::shared_ptr<BloomFilter> deviceBloomFilter_;
    std::shared_ptr<BloomFilter> orderBloomFilter_;

#ifdef ENABLE_ROCKETMQ
    // RocketMQ消费者
    std::shared_ptr<OrderPushConsumer> orderPushConsumer_;
    std::shared_ptr<OcrTaskConsumer> ocrTaskConsumer_;
    std::shared_ptr<EmailNotifyConsumer> emailNotifyConsumer_;
    std::shared_ptr<OrderTimeoutConsumer> orderTimeoutConsumer_;
    std::shared_ptr<AlertNotifyConsumer> alertNotifyConsumer_;
#endif

    // 定时任务：设备离线检测
    void scheduleDeviceOfflineCheck();

    // 定时任务：每日汇总邮件
    void scheduleDailySummaryEmail();

    // 定时任务：WebSocket连接清理
    void scheduleWebSocketCleanup();
};

} // namespace v3
} // namespace wepay
