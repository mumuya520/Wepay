#pragma once
#include <drogon/HttpController.h>
#include "SecurityValidator.h"
#include "DeviceManager.h"

namespace wepay {
namespace v3 {

// 设备注册/解绑控制器
class DeviceController : public drogon::HttpController<DeviceController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DeviceController::handleRegister,
                  "/api/wepay/v3/device/register", drogon::Post);
    ADD_METHOD_TO(DeviceController::handleUnbind,
                  "/api/wepay/v3/device/unbind", drogon::Post);
    ADD_METHOD_TO(DeviceController::handleDeviceInfo,
                  "/api/wepay/v3/device/info", drogon::Get);
    METHOD_LIST_END

    DeviceController();

    // 设备注册
    void handleRegister(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // 设备解绑
    void handleUnbind(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // 查询设备信息
    void handleDeviceInfo(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<SecurityValidator> validator_;
    std::shared_ptr<DeviceManager> deviceManager_;

    drogon::HttpResponsePtr buildResponse(int code, const std::string& message,
                                         const nlohmann::json& data = {});
};

// 健康检查控制器
class HealthController : public drogon::HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::handleHealth,
                  "/health", drogon::Get);
    ADD_METHOD_TO(HealthController::handleMetrics,
                  "/metrics", drogon::Get);
    METHOD_LIST_END

    // 健康检查
    void handleHealth(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // 系统指标
    void handleMetrics(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    // 检查Redis连接
    bool checkRedis();

    // 检查数据库连接
    bool checkDatabase();

    // 检查RocketMQ连接
    bool checkRocketMQ();

    // 获取系统指标
    nlohmann::json getSystemMetrics();
};

// 限流中间件
class RateLimiter {
public:
    static RateLimiter& getInstance() {
        static RateLimiter instance;
        return instance;
    }

    // 检查是否超过限流
    bool checkLimit(const std::string& key, int maxRequests, int windowSeconds);

    // 滑动窗口限流
    bool slidingWindowLimit(const std::string& key, int maxRequests, int windowSeconds);

private:
    RateLimiter() = default;
    std::shared_ptr<sw::redis::Redis> redis_;
};

// Sentinel限流熔断
class SentinelGuard {
public:
    SentinelGuard(const std::string& resource);
    ~SentinelGuard();

    bool isAllowed();

private:
    std::string resource_;
    bool allowed_ = false;
};

} // namespace v3
} // namespace wepay
