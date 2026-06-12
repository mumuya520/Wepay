#pragma once
#include <rocketmq/DefaultMQProducer.h>
#include <rocketmq/DefaultMQPushConsumer.h>
#include "EmailService.h"
#include "OrderService.h"
#include "AlertNotification.h"

namespace wepay {
namespace v3 {

// RocketMQ生产者管理器
class RocketMQProducerManager {
public:
    static RocketMQProducerManager& getInstance() {
        static RocketMQProducerManager instance;
        return instance;
    }

    void init(const std::string& namesrvAddr);
    void shutdown();

    // 发送订单推送消息（事务消息）
    bool sendOrderPushMessage(const std::string& orderId,
                             const std::string& deviceId,
                             const nlohmann::json& orderData);

    // 发送OCR识别任务
    bool sendOcrTask(const std::string& orderId, const std::string& imageUrl);

    // 发送邮件通知消息
    bool sendEmailNotify(const std::string& type,
                        const std::string& orderId,
                        const nlohmann::json& data);

    // 发送告警通知（延迟消息）
    bool sendAlertNotify(const std::string& alertType,
                        const nlohmann::json& data,
                        int delayLevel = 3);

    // 发送订单超时消息（延迟消息）
    bool sendOrderTimeout(const std::string& orderId, int timeoutMinutes);

    // 发送设备状态变更（顺序消息）
    bool sendDeviceStatusChange(const std::string& deviceId,
                               const nlohmann::json& statusData);

private:
    RocketMQProducerManager() = default;
    std::unique_ptr<rocketmq::DefaultMQProducer> producer_;
    std::string namesrvAddr_;
};

// 订单推送消费者
class OrderPushConsumer : public rocketmq::MessageListenerConcurrently {
public:
    OrderPushConsumer(std::shared_ptr<OrderService> orderService);

    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override;

    void start(const std::string& namesrvAddr);
    void shutdown();

private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;
    std::shared_ptr<OrderService> orderService_;
};

// OCR识别消费者
class OcrTaskConsumer : public rocketmq::MessageListenerConcurrently {
public:
    OcrTaskConsumer(std::shared_ptr<OcrService> ocrService);

    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override;

    void start(const std::string& namesrvAddr);
    void shutdown();

private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;
    std::shared_ptr<OcrService> ocrService_;
};

// 邮件通知消费者
class EmailNotifyConsumer : public rocketmq::MessageListenerConcurrently {
public:
    EmailNotifyConsumer(std::shared_ptr<EmailService> emailService);

    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override;

    void start(const std::string& namesrvAddr);
    void shutdown();

private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;
    std::shared_ptr<EmailService> emailService_;

    void handlePaySuccess(const nlohmann::json& body);
    void handlePayFail(const nlohmann::json& body);
    void handleDailySummary(const nlohmann::json& body);
};

// 订单超时消费者
class OrderTimeoutConsumer : public rocketmq::MessageListenerConcurrently {
public:
    OrderTimeoutConsumer(std::shared_ptr<OrderService> orderService);

    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override;

    void start(const std::string& namesrvAddr);
    void shutdown();

private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;
    std::shared_ptr<OrderService> orderService_;
};

// 告警通知消费者
class AlertNotifyConsumer : public rocketmq::MessageListenerConcurrently {
public:
    AlertNotifyConsumer();

    rocketmq::ConsumeStatus consumeMessage(
        const std::vector<rocketmq::MessageExt>& msgs) override;

    void start(const std::string& namesrvAddr);
    void shutdown();

private:
    std::unique_ptr<rocketmq::DefaultMQPushConsumer> consumer_;

    void handleDeviceOfflineAlert(const nlohmann::json& data, AlertMessage& alert);
    void handleOrderAbnormalAlert(const nlohmann::json& data, AlertMessage& alert);
    void handleSecurityAlert(const nlohmann::json& data, AlertMessage& alert);
    void handleChannelErrorAlert(const nlohmann::json& data, AlertMessage& alert);
};

} // namespace v3
} // namespace wepay
