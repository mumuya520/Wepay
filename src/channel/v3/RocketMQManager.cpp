#include "RocketMQManager.h"
#include "WepayV3Config.h"
#include "AlertNotification.h"

namespace wepay {
namespace v3 {

// RocketMQProducerManager实现
void RocketMQProducerManager::init(const std::string& namesrvAddr) {
    namesrvAddr_ = namesrvAddr;
    producer_ = std::make_unique<rocketmq::DefaultMQProducer>("wepay_v3_producer");
    producer_->setNamesrvAddr(namesrvAddr_);
    producer_->setInstanceName("wepay_v3_producer_instance");
    producer_->start();
}

void RocketMQProducerManager::shutdown() {
    if (producer_) {
        producer_->shutdown();
    }
}

bool RocketMQProducerManager::sendOrderPushMessage(const std::string& orderId,
                                                   const std::string& deviceId,
                                                   const nlohmann::json& orderData) {
    try {
        nlohmann::json body;
        body["orderId"] = orderId;
        body["deviceId"] = deviceId;
        body["orderData"] = orderData;
        body["timestamp"] = std::time(nullptr);

        rocketmq::Message msg("wepay_order_push", "ORDER_PUSH", orderId, body.dump());
        msg.setProperty("orderId", orderId);
        msg.setProperty("deviceId", deviceId);

        auto result = producer_->send(msg);
        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send order push message: " << e.what();
        return false;
    }
}

bool RocketMQProducerManager::sendOcrTask(const std::string& orderId,
                                         const std::string& imageUrl) {
    try {
        nlohmann::json body;
        body["orderId"] = orderId;
        body["imageUrl"] = imageUrl;
        body["timestamp"] = std::time(nullptr);

        rocketmq::Message msg("wepay_ocr_task", "OCR", orderId, body.dump());
        auto result = producer_->send(msg);
        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send OCR task: " << e.what();
        return false;
    }
}

bool RocketMQProducerManager::sendEmailNotify(const std::string& type,
                                              const std::string& orderId,
                                              const nlohmann::json& data) {
    try {
        nlohmann::json body;
        body["type"] = type;
        body["orderId"] = orderId;
        body["data"] = data;
        body["timestamp"] = std::time(nullptr);

        rocketmq::Message msg("wepay_email_notify", type, orderId, body.dump());
        auto result = producer_->send(msg);
        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send email notify: " << e.what();
        return false;
    }
}

bool RocketMQProducerManager::sendAlertNotify(const std::string& alertType,
                                              const nlohmann::json& data,
                                              int delayLevel) {
    try {
        nlohmann::json body;
        body["alertType"] = alertType;
        body["data"] = data;
        body["timestamp"] = std::time(nullptr);

        rocketmq::Message msg("wepay_alert_notify", alertType, body.dump());
        msg.setDelayTimeLevel(delayLevel); // 延迟消息

        auto result = producer_->send(msg);
        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send alert notify: " << e.what();
        return false;
    }
}

bool RocketMQProducerManager::sendOrderTimeout(const std::string& orderId,
                                               int timeoutMinutes) {
    try {
        nlohmann::json body;
        body["orderId"] = orderId;
        body["createTime"] = std::time(nullptr);

        rocketmq::Message msg("wepay_order_timeout", "TIMEOUT", orderId, body.dump());

        // 根据超时时间选择延迟级别
        int delayLevel = 9; // 默认5分钟
        if (timeoutMinutes <= 5) delayLevel = 9;
        else if (timeoutMinutes <= 10) delayLevel = 10;
        else if (timeoutMinutes <= 30) delayLevel = 16;
        else delayLevel = 17;

        msg.setDelayTimeLevel(delayLevel);

        auto result = producer_->send(msg);
        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send order timeout: " << e.what();
        return false;
    }
}

bool RocketMQProducerManager::sendDeviceStatusChange(const std::string& deviceId,
                                                     const nlohmann::json& statusData) {
    try {
        nlohmann::json body;
        body["deviceId"] = deviceId;
        body["statusData"] = statusData;
        body["timestamp"] = std::time(nullptr);

        rocketmq::Message msg("wepay_device_status", "STATUS_CHANGE", deviceId, body.dump());

        // 顺序消息：使用deviceId作为sharding key
        auto selector = [](const std::vector<rocketmq::MessageQueue>& mqs,
                          const rocketmq::Message& msg,
                          void* arg) -> rocketmq::MessageQueue {
            std::string deviceId = *static_cast<std::string*>(arg);
            int index = std::hash<std::string>{}(deviceId) % mqs.size();
            return mqs[index];
        };

        auto result = producer_->send(msg, selector, (void*)&deviceId);
        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send device status change: " << e.what();
        return false;
    }
}

// OrderPushConsumer实现
OrderPushConsumer::OrderPushConsumer(std::shared_ptr<OrderService> orderService)
    : orderService_(orderService) {}

rocketmq::ConsumeStatus OrderPushConsumer::consumeMessage(
    const std::vector<rocketmq::MessageExt>& msgs) {

    for (const auto& msg : msgs) {
        try {
            nlohmann::json body = nlohmann::json::parse(msg.getBody());
            std::string orderId = body["orderId"];
            std::string deviceId = body["deviceId"];

            // 推送订单到设备
            bool success = orderService_->pushOrderToDevice(orderId);

            if (!success) {
                LOG_WARN << "Failed to push order to device, will retry: " << orderId;
                return rocketmq::ConsumeStatus::RECONSUME_LATER;
            }

        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to consume order push message: " << e.what();
            return rocketmq::ConsumeStatus::RECONSUME_LATER;
        }
    }

    return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
}

void OrderPushConsumer::start(const std::string& namesrvAddr) {
    consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_order_push_consumer");
    consumer_->setNamesrvAddr(namesrvAddr);
    consumer_->subscribe("wepay_order_push", "*");
    consumer_->registerMessageListener(this);
    consumer_->setConsumeThreadCount(4);
    consumer_->setMaxReconsumeTimes(3);
    consumer_->start();
}

void OrderPushConsumer::shutdown() {
    if (consumer_) {
        consumer_->shutdown();
    }
}

// OcrTaskConsumer实现
OcrTaskConsumer::OcrTaskConsumer(std::shared_ptr<OcrService> ocrService)
    : ocrService_(ocrService) {}

rocketmq::ConsumeStatus OcrTaskConsumer::consumeMessage(
    const std::vector<rocketmq::MessageExt>& msgs) {

    for (const auto& msg : msgs) {
        try {
            nlohmann::json body = nlohmann::json::parse(msg.getBody());
            std::string orderId = body["orderId"];
            std::string imageUrl = body["imageUrl"];

            // 执行OCR识别
            auto ocrResult = ocrService_->recognizePaymentScreenshot(imageUrl);

            if (!ocrResult.success) {
                LOG_WARN << "OCR recognition failed, will retry: " << orderId;
                return rocketmq::ConsumeStatus::RECONSUME_LATER;
            }

            // 保存OCR结果到数据库
            // TODO: 更新订单表的OCR识别结果

        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to consume OCR task: " << e.what();
            return rocketmq::ConsumeStatus::RECONSUME_LATER;
        }
    }

    return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
}

void OcrTaskConsumer::start(const std::string& namesrvAddr) {
    consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_ocr_consumer");
    consumer_->setNamesrvAddr(namesrvAddr);
    consumer_->subscribe("wepay_ocr_task", "*");
    consumer_->registerMessageListener(this);
    consumer_->setConsumeThreadCount(2);
    consumer_->setMaxReconsumeTimes(3);
    consumer_->start();
}

void OcrTaskConsumer::shutdown() {
    if (consumer_) {
        consumer_->shutdown();
    }
}

// EmailNotifyConsumer实现
EmailNotifyConsumer::EmailNotifyConsumer(std::shared_ptr<EmailService> emailService)
    : emailService_(emailService) {}

rocketmq::ConsumeStatus EmailNotifyConsumer::consumeMessage(
    const std::vector<rocketmq::MessageExt>& msgs) {

    for (const auto& msg : msgs) {
        try {
            nlohmann::json body = nlohmann::json::parse(msg.getBody());
            std::string type = body["type"];

            if (type == "PAY_SUCCESS") {
                handlePaySuccess(body);
            } else if (type == "PAY_FAIL") {
                handlePayFail(body);
            } else if (type == "DAILY_SUMMARY") {
                handleDailySummary(body);
            }

        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to consume email notify: " << e.what();
            return rocketmq::ConsumeStatus::RECONSUME_LATER;
        }
    }

    return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
}

void EmailNotifyConsumer::handlePaySuccess(const nlohmann::json& body) {
    EmailService::EmailData data;
    auto orderData = body["data"];

    data.orderId = body["orderId"];
    data.merchantId = orderData["merchantId"];
    data.toEmail = orderData["toEmail"];
    data.money = orderData["money"];
    data.payType = orderData["payType"];
    data.payTime = orderData["payTime"];
    data.deviceId = orderData["deviceId"];

    emailService_->sendPaySuccessEmail(data);
}

void EmailNotifyConsumer::handlePayFail(const nlohmann::json& body) {
    EmailService::EmailData data;
    auto orderData = body["data"];

    data.orderId = body["orderId"];
    data.merchantId = orderData["merchantId"];
    data.toEmail = orderData["toEmail"];
    data.money = orderData["money"];
    data.failReason = orderData["failReason"];
    data.createTime = orderData["createTime"];
    data.callbackUrl = orderData["callbackUrl"];

    emailService_->sendPayFailEmail(data);
}

void EmailNotifyConsumer::handleDailySummary(const nlohmann::json& body) {
    std::string toEmail = body["data"]["toEmail"];
    nlohmann::json summaryData = body["data"]["summaryData"];

    emailService_->sendDailySummaryEmail(summaryData.value("merchantId", ""), toEmail, summaryData);
}

void EmailNotifyConsumer::start(const std::string& namesrvAddr) {
    consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_email_consumer");
    consumer_->setNamesrvAddr(namesrvAddr);
    consumer_->subscribe("wepay_email_notify", "*");
    consumer_->registerMessageListener(this);
    consumer_->setConsumeThreadCount(2);
    consumer_->setMaxReconsumeTimes(3);
    consumer_->start();
}

void EmailNotifyConsumer::shutdown() {
    if (consumer_) {
        consumer_->shutdown();
    }
}

// OrderTimeoutConsumer实现
OrderTimeoutConsumer::OrderTimeoutConsumer(std::shared_ptr<OrderService> orderService)
    : orderService_(orderService) {}

rocketmq::ConsumeStatus OrderTimeoutConsumer::consumeMessage(
    const std::vector<rocketmq::MessageExt>& msgs) {

    for (const auto& msg : msgs) {
        try {
            nlohmann::json body = nlohmann::json::parse(msg.getBody());
            std::string orderId = body["orderId"];

            // 处理订单超时
            orderService_->handleOrderTimeout(orderId);

        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to consume order timeout: " << e.what();
        }
    }

    return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
}

void OrderTimeoutConsumer::start(const std::string& namesrvAddr) {
    consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_timeout_consumer");
    consumer_->setNamesrvAddr(namesrvAddr);
    consumer_->subscribe("wepay_order_timeout", "*");
    consumer_->registerMessageListener(this);
    consumer_->setConsumeThreadCount(2);
    consumer_->start();
}

void OrderTimeoutConsumer::shutdown() {
    if (consumer_) {
        consumer_->shutdown();
    }
}

// AlertNotifyConsumer实现
AlertNotifyConsumer::AlertNotifyConsumer()

rocketmq::ConsumeStatus AlertNotifyConsumer::consumeMessage(
    const std::vector<rocketmq::MessageExt>& msgs) {

    for (const auto& msg : msgs) {
        try {
            nlohmann::json body = nlohmann::json::parse(msg.getBody());
            std::string alertType = body["alertType"];
            auto& data = body["data"];

            // 构造告警消息
            AlertMessage alert;
            alert.timestamp = body.value("timestamp", std::time(nullptr));
            alert.data = data;

            if (alertType == "DEVICE_OFFLINE") {
                handleDeviceOfflineAlert(data, alert);
            } else if (alertType == "ORDER_ABNORMAL") {
                handleOrderAbnormalAlert(data, alert);
            } else if (alertType == "SECURITY_EVENT") {
                handleSecurityAlert(data, alert);
            } else if (alertType == "CHANNEL_ERROR") {
                handleChannelErrorAlert(data, alert);
            }

            // 通过 AlertNotificationManager 发送通知（钉钉/企微/短信/邮件）
            bool sent = AlertNotificationManager::getInstance().sendAlert(alert);
            LOG_INFO << "[AlertNotifyConsumer] alertType=" << alertType
                     << " sent=" << sent << " channels=" << alert.channels.size();

        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to consume alert notify: " << e.what();
        }
    }

    return rocketmq::ConsumeStatus::CONSUME_SUCCESS;
}

void AlertNotifyConsumer::handleDeviceOfflineAlert(const nlohmann::json& data, AlertMessage& alert) {
    alert.type = AlertType::DEVICE_OFFLINE;
    alert.level = AlertLevel::WARNING;
    alert.title = "设备离线告警";
    std::string deviceId = data.value("deviceId", "unknown");
    std::string deviceName = data.value("deviceName", deviceId);
    alert.content = "设备「" + deviceName + "」已离线，请检查网络或设备状态";
    alert.channels = {AlertChannel::DINGTALK, AlertChannel::WECOM, AlertChannel::EMAIL};
    LOG_WARN << "Device offline alert: " << deviceId;
}

void AlertNotifyConsumer::handleOrderAbnormalAlert(const nlohmann::json& data, AlertMessage& alert) {
    alert.type = AlertType::ORDER_TIMEOUT;
    alert.level = AlertLevel::ALERT_ERROR;
    std::string orderId = data.value("orderId", "unknown");
    std::string reason = data.value("reason", "未知异常");
    alert.title = "订单异常告警";
    alert.content = "订单「" + orderId + "」发生异常: " + reason;
    alert.channels = {AlertChannel::DINGTALK, AlertChannel::WECOM};
    LOG_WARN << "Order abnormal alert: " << orderId << " reason: " << reason;
}

void AlertNotifyConsumer::handleSecurityAlert(const nlohmann::json& data, AlertMessage& alert) {
    alert.type = AlertType::SECURITY_EVENT;
    alert.level = AlertLevel::CRITICAL;
    std::string ip = data.value("ip", "unknown");
    std::string reason = data.value("reason", "未知原因");
    alert.title = "安全告警";
    alert.content = "检测到安全事件 - IP: " + ip + " 原因: " + reason;
    alert.channels = {AlertChannel::DINGTALK, AlertChannel::WECOM, AlertChannel::SMS, AlertChannel::EMAIL};
    LOG_ERROR << "Security alert: IP=" << ip << " reason: " << reason;
}

void AlertNotifyConsumer::handleChannelErrorAlert(const nlohmann::json& data, AlertMessage& alert) {
    alert.type = AlertType::SYSTEM_ERROR;
    alert.level = AlertLevel::ALERT_ERROR;
    std::string channel = data.value("channel", "unknown");
    std::string errMsg = data.value("error", "未知错误");
    alert.title = "通道异常告警";
    alert.content = "支付通道「" + channel + "」发生错误: " + errMsg;
    alert.channels = {AlertChannel::DINGTALK, AlertChannel::WECOM};
    LOG_ERROR << "Channel error alert: channel=" << channel << " error=" << errMsg;
}

void AlertNotifyConsumer::start(const std::string& namesrvAddr) {
    consumer_ = std::make_unique<rocketmq::DefaultMQPushConsumer>("wepay_alert_consumer");
    consumer_->setNamesrvAddr(namesrvAddr);
    consumer_->subscribe("wepay_alert_notify", "*");
    consumer_->registerMessageListener(this);
    consumer_->setConsumeThreadCount(2);
    consumer_->start();
}

void AlertNotifyConsumer::shutdown() {
    if (consumer_) {
        consumer_->shutdown();
    }
}

} // namespace v3
} // namespace wepay
