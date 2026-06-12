#include "DeviceController.h"
#include "WepayV3Config.h"
#include "WebSocketController.h"
#ifdef ENABLE_ROCKETMQ
#include "RocketMQManager.h"
#endif
#ifndef _WIN32
#include <sys/sysinfo.h>
#endif

namespace wepay {
namespace v3 {

// DeviceController实现
DeviceController::DeviceController() {
    auto& config = WepayV3Config::getInstance();

    validator_ = std::make_shared<SecurityValidator>(
        config.security.hmacSecret,
        config.security.rsaPublicKey
    );

    auto redis = std::make_shared<sw::redis::Redis>("tcp://" + config.redis.host + ":" +
                                                     std::to_string(config.redis.port));
    deviceManager_ = std::make_shared<DeviceManager>(redis);
}

void DeviceController::handleRegister(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        auto jsonBody = req->getJsonObject();
        if (!jsonBody) {
            callback(buildResponse(400, "Invalid JSON body"));
            return;
        }

        std::string deviceId = (*jsonBody)["deviceId"].asString();
        std::string merchantId = (*jsonBody)["merchantId"].asString();
        std::string timestamp = (*jsonBody)["timestamp"].asString();
        std::string nonce = (*jsonBody)["nonce"].asString();
        std::string sign = (*jsonBody)["sign"].asString();

        // 安全校验
        std::string clientIp = req->getPeerAddr().toIp();
        auto validationResult = validator_->validateRequest(
            deviceId, timestamp, nonce, sign, clientIp
        );

        if (!validationResult.success) {
            callback(buildResponse(403, validationResult.errorMsg));
            return;
        }

        // 检查设备数量限制
        auto& config = WepayV3Config::getInstance();
        int deviceCount = deviceManager_->getDeviceCount(merchantId);
        if (deviceCount >= config.security.maxDevices) {
            callback(buildResponse(400, "Device limit exceeded"));
            return;
        }

        // 注册设备
        bool success = deviceManager_->registerDevice(deviceId, merchantId);
        if (!success) {
            callback(buildResponse(400, "Device already registered"));
            return;
        }

        nlohmann::json responseData;
        responseData["deviceId"] = deviceId;
        responseData["merchantId"] = merchantId;
        responseData["registerTime"] = std::time(nullptr);

        callback(buildResponse(200, "success", responseData));

    } catch (const std::exception& e) {
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

void DeviceController::handleUnbind(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        auto jsonBody = req->getJsonObject();
        if (!jsonBody) {
            callback(buildResponse(400, "Invalid JSON body"));
            return;
        }

        std::string deviceId = (*jsonBody)["deviceId"].asString();
        std::string merchantId = (*jsonBody)["merchantId"].asString();
        std::string timestamp = (*jsonBody)["timestamp"].asString();
        std::string nonce = (*jsonBody)["nonce"].asString();
        std::string sign = (*jsonBody)["sign"].asString();

        // 安全校验
        std::string clientIp = req->getPeerAddr().toIp();
        auto validationResult = validator_->validateRequest(
            deviceId, timestamp, nonce, sign, clientIp
        );

        if (!validationResult.success) {
            callback(buildResponse(403, validationResult.errorMsg));
            return;
        }

        // 解绑设备
        bool success = deviceManager_->unbindDevice(deviceId, merchantId);
        if (!success) {
            callback(buildResponse(400, "Failed to unbind device"));
            return;
        }

        callback(buildResponse(200, "success"));

    } catch (const std::exception& e) {
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

void DeviceController::handleDeviceInfo(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        std::string deviceId = req->getParameter("deviceId");

        if (deviceId.empty()) {
            callback(buildResponse(400, "Missing deviceId parameter"));
            return;
        }

        auto deviceInfo = deviceManager_->getDeviceInfo(deviceId);
        if (!deviceInfo) {
            callback(buildResponse(404, "Device not found"));
            return;
        }

        nlohmann::json responseData;
        responseData["deviceId"] = deviceInfo->deviceId;
        responseData["ip"] = deviceInfo->ip;
        responseData["online"] = deviceInfo->online;
        responseData["battery"] = deviceInfo->battery;
        responseData["network"] = deviceInfo->network;
        responseData["appVersion"] = deviceInfo->appVersion;
        responseData["lastHeartbeat"] = deviceInfo->lastHeartbeat;
        responseData["merchantIds"] = deviceInfo->merchantIds;

        callback(buildResponse(200, "success", responseData));

    } catch (const std::exception& e) {
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

drogon::HttpResponsePtr DeviceController::buildResponse(
    int code,
    const std::string& message,
    const nlohmann::json& data) {

    nlohmann::json response;
    response["code"] = code;
    response["message"] = message;

    if (!data.empty()) {
        response["data"] = data;
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeString("application/json");
    resp->setBody(response.dump());

    if (code == 200) {
        resp->setStatusCode(drogon::k200OK);
    } else if (code == 400) {
        resp->setStatusCode(drogon::k400BadRequest);
    } else if (code == 403) {
        resp->setStatusCode(drogon::k403Forbidden);
    } else if (code == 404) {
        resp->setStatusCode(drogon::k404NotFound);
    } else {
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    return resp;
}

// HealthController实现
void HealthController::handleHealth(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    nlohmann::json health;
    health["status"] = "UP";
    health["timestamp"] = std::time(nullptr);

    // 检查各个组件
    nlohmann::json components;
    components["redis"] = checkRedis() ? "UP" : "DOWN";
    components["database"] = checkDatabase() ? "UP" : "DOWN";
#ifdef ENABLE_ROCKETMQ
    components["rocketmq"] = checkRocketMQ() ? "UP" : "DOWN";
#else
    components["rocketmq"] = "DISABLED";
#endif

    health["components"] = components;

    // 如果任何组件DOWN，整体状态为DOWN
    bool allUp = true;
    for (auto& [key, value] : components.items()) {
        if (value == "DOWN") {
            allUp = false;
            break;
        }
    }

    if (!allUp) {
        health["status"] = "DOWN";
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeString("application/json");
    resp->setBody(health.dump());
    resp->setStatusCode(allUp ? drogon::k200OK : drogon::k503ServiceUnavailable);
    callback(resp);
}

void HealthController::handleMetrics(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    nlohmann::json metrics = getSystemMetrics();

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeString("application/json");
    resp->setBody(metrics.dump());
    callback(resp);
}

bool HealthController::checkRedis() {
    try {
        auto& config = WepayV3Config::getInstance();
        auto redis = sw::redis::Redis("tcp://" + config.redis.host + ":" +
                                     std::to_string(config.redis.port));
        redis.ping();
        return true;
    } catch (...) {
        return false;
    }
}

bool HealthController::checkDatabase() {
    try {
        // TODO: 检查PostgreSQL连接
        return true;
    } catch (...) {
        return false;
    }
}

bool HealthController::checkRocketMQ() {
    try {
        // TODO: 检查RocketMQ连接
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json HealthController::getSystemMetrics() {
    nlohmann::json metrics;

#ifndef _WIN32
    // 系统信息（Linux only）
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        metrics["system"]["uptime"] = si.uptime;
        metrics["system"]["totalRam"] = si.totalram;
        metrics["system"]["freeRam"] = si.freeram;
        metrics["system"]["procs"] = si.procs;
    }
#endif

    // WebSocket连接数
    metrics["websocket"]["connections"] = (int64_t)WebSocketManager::getInstance().getConnectionCount();

    // TODO: 添加更多指标
    // - 订单统计
    // - 设备统计
    // - 请求QPS
    // - 响应时间

    return metrics;
}

// RateLimiter实现
bool RateLimiter::checkLimit(const std::string& key, int maxRequests, int windowSeconds) {
    return slidingWindowLimit(key, maxRequests, windowSeconds);
}

bool RateLimiter::slidingWindowLimit(const std::string& key,
                                     int maxRequests,
                                     int windowSeconds) {
    try {
        if (!redis_) {
            auto& config = WepayV3Config::getInstance();
            redis_ = std::make_shared<sw::redis::Redis>("tcp://" + config.redis.host + ":" +
                                                        std::to_string(config.redis.port));
        }

        std::string limitKey = "rate_limit:" + key;
        int64_t now = std::time(nullptr);
        int64_t windowStart = now - windowSeconds;

        // 使用Redis ZSET实现滑动窗口
        // 1. 删除窗口外的记录
        // redis++ 1.3.x zremrangebyscore/zcount 接受 Interval 对象，用 command 接口更通用
        redis_->command<long long>("ZREMRANGEBYSCORE", limitKey, "0", std::to_string(windowStart));

        // 2. 统计当前窗口内的请求数
        int64_t count = redis_->command<long long>("ZCOUNT", limitKey,
                                                   std::to_string(windowStart),
                                                   std::to_string(now));

        if (count >= maxRequests) {
            return false; // 超过限流
        }

        // 3. 添加当前请求
        redis_->zadd(limitKey, std::to_string(now), now);

        // 4. 设置过期时间
        redis_->expire(limitKey, windowSeconds);

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Rate limit check failed: " << e.what();
        return true; // 限流失败时放行
    }
}

// SentinelGuard实现
SentinelGuard::SentinelGuard(const std::string& resource)
    : resource_(resource) {

    // TODO: 集成Sentinel C++ SDK
    // 这里简化实现，实际应该调用Sentinel API
    allowed_ = true;
}

SentinelGuard::~SentinelGuard() {
    if (allowed_) {
        // 记录成功
    }
}

bool SentinelGuard::isAllowed() {
    return allowed_;
}

} // namespace v3
} // namespace wepay
