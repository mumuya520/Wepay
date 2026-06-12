#include "WepayV3Plugin.h"
#include "WebSocketConnectionManager.h"
#include "OcrResultCache.h"
#include "CacheOptimizer.h"
#include "SentinelFlowControl.h"
#include "GrayscaleManager.h"
#include "AlertNotification.h"
#include <drogon/drogon.h>
#include "common/PayDb.h"

namespace wepay {
namespace v3 {

bool WepayV3Plugin::init(const std::string& configFile) {
    if (initialized_) {
        LOG_WARN << "WePay V3 Plugin already initialized";
        return true;
    }

    LOG_INFO << "Initializing WePay V3 Plugin...";

    // 1. 加载配置文件
    auto& config = WepayV3Config::getInstance();
    if (!config.loadFromFile(configFile)) {
        LOG_ERROR << "Failed to load config file: " << configFile;
        return false;
    }

    // 2. 初始化Redis连接（可选，失败时降级运行）
    std::shared_ptr<sw::redis::Redis> redis;
    bool redisAvailable = false;
    try {
        redis = std::make_shared<sw::redis::Redis>("tcp://" + config.redis.host + ":" +
                                                    std::to_string(config.redis.port));
        // 发一条 PING 探测连通性
        redis->ping();
        redisAvailable = true;
        LOG_INFO << "[V3] Redis connected: " << config.redis.host << ":" << config.redis.port;
    } catch (const std::exception& e) {
        LOG_WARN << "[V3] Redis unavailable (" << e.what() << "), running in degraded mode "
                 << "(no Bloom filter / no Pub/Sub cross-instance WS / no grayscale from Redis)";
    }

    // 3. 初始化缓存优化组件（仅 Redis 可用时）
    std::shared_ptr<BloomFilter> deviceBloomFilter;
    std::shared_ptr<BloomFilter> orderBloomFilter;
    if (redisAvailable) {
        try {
            auto cacheWarmer = std::make_shared<CacheWarmer>(redis);
            deviceBloomFilter = std::make_shared<BloomFilter>(redis, "device", 1000000, 0.01);
            orderBloomFilter  = std::make_shared<BloomFilter>(redis, "order", 10000000, 0.01);
            cacheWarmer->warmupAll();
            cacheWarmer->warmupDeviceBloomFilter(deviceBloomFilter);
            cacheWarmer->warmupOrderBloomFilter(orderBloomFilter);
            auto progress = cacheWarmer->getProgress();
            LOG_INFO << "[V3] Cache warmup: " << progress.completedTasks << "/" << progress.totalTasks
                     << " tasks, " << progress.failedTasks << " failed";
        } catch (const std::exception& e) {
            LOG_WARN << "[V3] Cache warmup failed: " << e.what();
        }
    }

    // 4. 初始化WebSocket连接管理器（无 Redis 时退化为单实例本地映射）
    wsConnectionManager_ = std::make_shared<WebSocketConnectionManager>(redis);
    wsMessageSubscriber_ = std::make_shared<WebSocketMessageSubscriber>(redis, wsConnectionManager_.get());
    wsMessageSubscriber_->start();

    // 5. 初始化OCR缓存
    auto ocrCache = std::make_shared<OcrResultCache>(redis);

    // 6. 初始化Sentinel流量控制
    SentinelFlowManager::getInstance().init(redis);

    // 注册熔断规则
    CircuitBreakerRule cbRule;
    cbRule.resourceName = "order_push";
    cbRule.threshold = 50;
    cbRule.timeWindow = 10;
    cbRule.minRequestAmount = 10;
    cbRule.slowRatioThreshold = 0.5;
    cbRule.maxSlowRequestRt = 1000;
    cbRule.errorRatioThreshold = 0.5;
    cbRule.retryTimeoutMs = 5000;
    SentinelFlowManager::getInstance().registerCircuitBreakerRule(cbRule);

    // 注册限流规则
    RateLimitRule rlRule;
    rlRule.resourceName = "order_push";
    rlRule.threshold = 100;
    rlRule.timeWindow = 1;
    rlRule.warmUp = true;
    rlRule.warmUpPeriodSec = 10;
    SentinelFlowManager::getInstance().registerRateLimitRule(rlRule);

    // 7. 初始化灰度发布管理器
    GrayscaleManager::getInstance().init(redis);

    // 8. 初始化设备分组管理器
    DeviceGroupManager::getInstance().init(redis);

    // 9. 初始化告警通知管理器
    if (config.alert.enabled) {
        nlohmann::json alertConfig;
        alertConfig["dingtalk"]["webhook"] = config.alert.dingtalkWebhook;
        alertConfig["wecom"]["webhook"]    = config.alert.wecomWebhook;
        AlertNotificationManager::getInstance().init(alertConfig);
    }

#ifdef ENABLE_ROCKETMQ
    // 10. 初始化RocketMQ生产者
    RocketMQProducerManager::getInstance().init(config.rocketmq.namesrvAddr);
    LOG_INFO << "RocketMQ producer initialized";
#else
    LOG_INFO << "[V3] RocketMQ 未启用，异步消息使用同步模式";
#endif

    // 11. 初始化邮件服务（多账号从 v3_email_account 表加载，支持轮询）
    {
        EmailService::EmailConfig empty;  // 空默认，loadAccountsFromDb() 后填充
        emailService_ = std::make_shared<EmailService>(empty);
        emailService_->loadAccountsFromDb();  // 加载数据库中全部启用账号
    }

    ocrService_ = std::make_shared<OcrService>(ocrCache);
    orderService_ = std::make_shared<OrderService>();
    orderService_->setEmailService(emailService_);  // 注入邮件服务，每笔订单自动发通知
    EmailService::setGlobalInstance(emailService_); // 注册全局单例供 AdminV3Ctrl 热更新
    deviceManager_ = std::make_shared<DeviceManager>(redis);

    // 保存布隆过滤器引用
    deviceBloomFilter_ = deviceBloomFilter;
    orderBloomFilter_ = orderBloomFilter;

    LOG_INFO << "All services initialized";

#ifdef ENABLE_ROCKETMQ
    // 12. 启动RocketMQ消费者
    orderPushConsumer_ = std::make_shared<OrderPushConsumer>(orderService_);
    orderPushConsumer_->start(config.rocketmq.namesrvAddr);

    ocrTaskConsumer_ = std::make_shared<OcrTaskConsumer>(ocrService_);
    ocrTaskConsumer_->start(config.rocketmq.namesrvAddr);

    emailNotifyConsumer_ = std::make_shared<EmailNotifyConsumer>(emailService_);
    emailNotifyConsumer_->start(config.rocketmq.namesrvAddr);

    orderTimeoutConsumer_ = std::make_shared<OrderTimeoutConsumer>(orderService_);
    orderTimeoutConsumer_->start(config.rocketmq.namesrvAddr);

    alertNotifyConsumer_ = std::make_shared<AlertNotifyConsumer>();
    alertNotifyConsumer_->start(config.rocketmq.namesrvAddr);

    LOG_INFO << "All RocketMQ consumers started";
#endif

    // 13. 启动定时任务
    startScheduledTasks();

    initialized_ = true;
    LOG_INFO << "WePay V3 Plugin initialized successfully";

    return true;
}

void WepayV3Plugin::shutdown() {
    if (!initialized_) {
        return;
    }

    LOG_INFO << "Shutting down WePay V3 Plugin...";

    // 关闭WebSocket消息订阅器
    if (wsMessageSubscriber_) {
        wsMessageSubscriber_->stop();
    }

#ifdef ENABLE_ROCKETMQ
    // 关闭RocketMQ消费者
    if (orderPushConsumer_) orderPushConsumer_->shutdown();
    if (ocrTaskConsumer_) ocrTaskConsumer_->shutdown();
    if (emailNotifyConsumer_) emailNotifyConsumer_->shutdown();
    if (orderTimeoutConsumer_) orderTimeoutConsumer_->shutdown();
    if (alertNotifyConsumer_) alertNotifyConsumer_->shutdown();

    // 关闭RocketMQ生产者
    RocketMQProducerManager::getInstance().shutdown();
#endif

    initialized_ = false;
    LOG_INFO << "WePay V3 Plugin shutdown complete";
}

void WepayV3Plugin::startScheduledTasks() {
    // 启动设备离线检测
    scheduleDeviceOfflineCheck();

    // 启动每日汇总邮件
    scheduleDailySummaryEmail();

    // 启动WebSocket连接清理
    scheduleWebSocketCleanup();

    LOG_INFO << "Scheduled tasks started";
}

void WepayV3Plugin::scheduleWebSocketCleanup() {
    // 每60秒清理一次过期的WebSocket连接
    drogon::app().getLoop()->runEvery(60.0, [this]() {
        if (wsConnectionManager_) {
            auto& config = WepayV3Config::getInstance();
            wsConnectionManager_->cleanupExpiredConnections(config.heartbeatTimeout);
        }
    });
}

void WepayV3Plugin::scheduleDeviceOfflineCheck() {
    // 每30秒检测一次设备离线
    drogon::app().getLoop()->runEvery(30.0, [this]() {
        auto& config = WepayV3Config::getInstance();
        auto offlineDevices = deviceManager_->checkOfflineDevices(config.heartbeatTimeout);

        for (const auto& deviceId : offlineDevices) {
            LOG_WARN << "Device offline detected: " << deviceId;
#ifdef ENABLE_ROCKETMQ
            nlohmann::json alertData;
            alertData["deviceId"]  = deviceId;
            alertData["timestamp"] = (int64_t)std::time(nullptr);
            RocketMQProducerManager::getInstance().sendAlertNotify(
                "DEVICE_OFFLINE", alertData, 3);
#endif
        }
    });
}

void WepayV3Plugin::scheduleDailySummaryEmail() {
    drogon::app().getLoop()->runEvery(60.0, [this]() {
        time_t now = time(nullptr);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif

        if (t.tm_hour != 10 || t.tm_min > 5) {
            return;
        }

        char todayKey[48];
        std::strftime(todayKey, sizeof(todayKey), "v3_daily_email_%Y%m%d", &t);
        const std::string idempotentKey(todayKey);
        auto& db = PayDb::instance();

        if (!db.getSetting(idempotentKey).empty()) {
            LOG_INFO << "[V3] Daily summary skipped: idempotent key exists, key=" << idempotentKey;
            return;
        }

        if (!emailService_) {
            LOG_WARN << "[V3] Daily summary skipped: email service unavailable";
            return;
        }

        char nowText[32];
        std::strftime(nowText, sizeof(nowText), "%Y-%m-%d %H:%M:%S", &t);
        LOG_INFO << "[V3] Daily summary task triggered at local time " << nowText;

        try {
            time_t y = now - 86400;
            struct tm yt;
#ifdef _WIN32
            localtime_s(&yt, &y);
#else
            localtime_r(&y, &yt);
#endif
            char yesterday[16];
            std::strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", &yt);

            const time_t dayStart = y - (yt.tm_hour * 3600 + yt.tm_min * 60 + yt.tm_sec);
            const time_t dayEnd   = dayStart + 86400;

            auto rows = db.query(
                "SELECT merchant_id, notify_email FROM v3_merchant_config "
                "WHERE email_notify_enabled=1 AND daily_summary_enabled=1 "
                "AND notify_email IS NOT NULL AND notify_email!='' AND status=1");

            if (rows.empty()) {
                LOG_INFO << "[V3] Daily summary skipped: no eligible merchants";
                return;
            }

            int successCount = 0;
            int failCount = 0;
            int skippedCount = 0;

            for (auto& row : rows) {
                const std::string mchId = row.count("merchant_id") ? row.at("merchant_id") : "";
                const std::string email = row.count("notify_email") ? row.at("notify_email") : "";
                if (mchId.empty() || email.empty()) {
                    ++skippedCount;
                    continue;
                }

                auto stats = db.query(
                    "SELECT COUNT(*) AS total_orders,"
                    "SUM(CASE WHEN status='PAID' THEN 1 ELSE 0 END) AS success_orders,"
                    "COALESCE(SUM(CASE WHEN status='PAID' THEN CAST(amount AS REAL) ELSE 0 END), 0) AS total_amount,"
                    "SUM(CASE WHEN status IN ('FAILED','TIMEOUT') THEN 1 ELSE 0 END) AS failed_orders "
                    "FROM v3_order WHERE merchant_id='" + mchId + "' "
                    "AND created_at>=" + std::to_string(dayStart) +
                    " AND created_at<"  + std::to_string(dayEnd));

                nlohmann::json summary;
                summary["merchantId"] = mchId;
                summary["date"] = yesterday;
                summary["paidAmount"] = "0.00";
                if (!stats.empty()) {
                    auto& s = stats[0];
                    summary["totalOrders"]   = s.count("total_orders")   ? std::stoi(s.at("total_orders"))   : 0;
                    summary["successOrders"] = s.count("success_orders") ? std::stoi(s.at("success_orders")) : 0;
                    double totalAmount = s.count("total_amount") && !s.at("total_amount").empty()
                                            ? std::stod(s.at("total_amount")) : 0.0;
                    summary["totalAmount"]   = totalAmount;
                    summary["paidAmount"]    = std::to_string(totalAmount);
                    summary["failedOrders"]  = s.count("failed_orders")  ? std::stoi(s.at("failed_orders"))  : 0;
                } else {
                    summary["totalOrders"] = 0;
                    summary["successOrders"] = 0;
                    summary["totalAmount"] = 0.0;
                    summary["failedOrders"] = 0;
                }

                if (emailService_->sendDailySummaryEmail(mchId, email, summary)) {
                    ++successCount;
                } else {
                    ++failCount;
                    LOG_WARN << "[V3] Daily summary send failed, merchant=" << mchId
                             << ", email=" << email;
                }
            }

            if (successCount > 0 && failCount == 0) {
                db.setSetting(idempotentKey, std::to_string(now));
                LOG_INFO << "[V3] Daily summary completed, success=" << successCount
                         << ", skipped=" << skippedCount
                         << ", key=" << idempotentKey;
            } else {
                LOG_WARN << "[V3] Daily summary not marked complete, success=" << successCount
                         << ", failed=" << failCount
                         << ", skipped=" << skippedCount
                         << ", key=" << idempotentKey;
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "[V3] Daily summary email error: " << e.what();
        }
    });
}

} // namespace v3
} // namespace wepay
