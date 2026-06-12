#include "WebSocketConnectionManager.h"
#include <drogon/drogon.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#endif

namespace wepay {
namespace v3 {

WebSocketConnectionManager::WebSocketConnectionManager(std::shared_ptr<sw::redis::Redis> redis)
    : redis_(redis), serverId_(getServerId()) {
    LOG_INFO << "WebSocketConnectionManager initialized with serverId: " << serverId_;
}

std::string WebSocketConnectionManager::getServerId() {
    static std::string cachedServerId;

    if (!cachedServerId.empty()) {
        return cachedServerId;
    }

    // 生成唯一服务器ID: hostname-pid-timestamp-random
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::ostringstream oss;
    oss << hostname << "-" << getpid() << "-" << timestamp << "-" << dis(gen);

    cachedServerId = oss.str();
    return cachedServerId;
}

std::string WebSocketConnectionManager::buildRedisKey(const std::string& deviceId) const {
    return std::string(REDIS_KEY_PREFIX) + deviceId;
}

bool WebSocketConnectionManager::registerConnection(
    const std::string& deviceId,
    const std::string& merchantId,
    const drogon::WebSocketConnectionPtr& conn,
    const std::string& clientIp,
    const std::string& userAgent) {

    try {
        // 1. 保存到本地映射
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            localConnections_[deviceId] = conn;
        }

        // 2. 保存到Redis
        ConnectionInfo info;
        info.deviceId = deviceId;
        info.merchantId = merchantId;
        info.serverId = serverId_;
        info.connectTime = std::time(nullptr);
        info.lastHeartbeat = info.connectTime;
        info.clientIp = clientIp;
        info.userAgent = userAgent;

        nlohmann::json j;
        j["deviceId"] = info.deviceId;
        j["merchantId"] = info.merchantId;
        j["serverId"] = info.serverId;
        j["connectTime"] = info.connectTime;
        j["lastHeartbeat"] = info.lastHeartbeat;
        j["clientIp"] = info.clientIp;
        j["userAgent"] = info.userAgent;

        std::string key = buildRedisKey(deviceId);
        redis_->setex(key, 300, j.dump()); // 5分钟过期

        LOG_INFO << "WebSocket connection registered: deviceId=" << deviceId
                 << " serverId=" << serverId_;

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to register connection: " << e.what();
        return false;
    }
}

void WebSocketConnectionManager::unregisterConnection(const std::string& deviceId) {
    try {
        // 1. 从本地映射删除
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            localConnections_.erase(deviceId);
        }

        // 2. 从Redis删除
        std::string key = buildRedisKey(deviceId);
        redis_->del(key);

        LOG_INFO << "WebSocket connection unregistered: deviceId=" << deviceId;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to unregister connection: " << e.what();
    }
}

void WebSocketConnectionManager::updateHeartbeat(const std::string& deviceId) {
    try {
        std::string key = buildRedisKey(deviceId);
        auto value = redis_->get(key);

        if (!value) {
            return;
        }

        nlohmann::json j = nlohmann::json::parse(*value);
        j["lastHeartbeat"] = std::time(nullptr);

        redis_->setex(key, 300, j.dump());

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to update heartbeat: " << e.what();
    }
}

drogon::WebSocketConnectionPtr WebSocketConnectionManager::getLocalConnection(
    const std::string& deviceId) {

    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = localConnections_.find(deviceId);
    if (it != localConnections_.end()) {
        return it->second;
    }
    return nullptr;
}

bool WebSocketConnectionManager::isDeviceOnline(const std::string& deviceId) {
    try {
        std::string key = buildRedisKey(deviceId);
        return redis_->exists(key) > 0;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to check device online: " << e.what();
        return false;
    }
}

std::string WebSocketConnectionManager::getDeviceServerId(const std::string& deviceId) {
    try {
        std::string key = buildRedisKey(deviceId);
        auto value = redis_->get(key);

        if (!value) {
            return "";
        }

        nlohmann::json j = nlohmann::json::parse(*value);
        return j["serverId"].get<std::string>();

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get device serverId: " << e.what();
        return "";
    }
}

std::optional<WebSocketConnectionManager::ConnectionInfo>
WebSocketConnectionManager::getConnectionInfo(const std::string& deviceId) {
    try {
        std::string key = buildRedisKey(deviceId);
        auto value = redis_->get(key);

        if (!value) {
            return std::nullopt;
        }

        nlohmann::json j = nlohmann::json::parse(*value);

        ConnectionInfo info;
        info.deviceId = j["deviceId"].get<std::string>();
        info.merchantId = j["merchantId"].get<std::string>();
        info.serverId = j["serverId"].get<std::string>();
        info.connectTime = j["connectTime"].get<int64_t>();
        info.lastHeartbeat = j["lastHeartbeat"].get<int64_t>();
        info.clientIp = j["clientIp"].get<std::string>();
        info.userAgent = j["userAgent"].get<std::string>();

        return info;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get connection info: " << e.what();
        return std::nullopt;
    }
}

bool WebSocketConnectionManager::pushMessageToDevice(
    const std::string& deviceId,
    const nlohmann::json& message) {

    try {
        // 1. 检查是否是本地连接
        auto conn = getLocalConnection(deviceId);
        if (conn) {
            conn->send(message.dump());
            LOG_DEBUG << "Message sent to local connection: deviceId=" << deviceId;
            return true;
        }

        // 2. 检查设备是否在其他实例上
        std::string targetServerId = getDeviceServerId(deviceId);
        if (targetServerId.empty()) {
            LOG_WARN << "Device not online: deviceId=" << deviceId;
            return false;
        }

        // 3. 通过Redis Pub/Sub转发消息
        nlohmann::json pubsubMsg;
        pubsubMsg["targetDeviceId"] = deviceId;
        pubsubMsg["targetServerId"] = targetServerId;
        pubsubMsg["message"] = message;
        pubsubMsg["fromServerId"] = serverId_;

        publishMessage(deviceId, pubsubMsg);

        LOG_DEBUG << "Message forwarded via Pub/Sub: deviceId=" << deviceId
                  << " targetServerId=" << targetServerId;

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to push message: " << e.what();
        return false;
    }
}

void WebSocketConnectionManager::publishMessage(
    const std::string& deviceId,
    const nlohmann::json& message) {

    try {
        redis_->publish(REDIS_PUBSUB_CHANNEL, message.dump());
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to publish message: " << e.what();
    }
}

void WebSocketConnectionManager::broadcastMessage(const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    for (const auto& [deviceId, conn] : localConnections_) {
        try {
            conn->send(message.dump());
        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to broadcast to deviceId=" << deviceId
                      << " error=" << e.what();
        }
    }
}

void WebSocketConnectionManager::cleanupExpiredConnections(int timeoutSeconds) {
    try {
        std::vector<std::string> expiredDevices;

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);

            for (const auto& [deviceId, conn] : localConnections_) {
                auto info = getConnectionInfo(deviceId);
                if (!info) {
                    expiredDevices.push_back(deviceId);
                    continue;
                }

                int64_t now = std::time(nullptr);
                if (now - info->lastHeartbeat > timeoutSeconds) {
                    expiredDevices.push_back(deviceId);
                }
            }
        }

        for (const auto& deviceId : expiredDevices) {
            LOG_WARN << "Cleaning up expired connection: deviceId=" << deviceId;

            auto conn = getLocalConnection(deviceId);
            if (conn) {
                conn->forceClose();
            }

            unregisterConnection(deviceId);
        }

        if (!expiredDevices.empty()) {
            LOG_INFO << "Cleaned up " << expiredDevices.size() << " expired connections";
        }

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to cleanup expired connections: " << e.what();
    }
}

// WebSocketMessageSubscriber实现
WebSocketMessageSubscriber::WebSocketMessageSubscriber(
    std::shared_ptr<sw::redis::Redis> redis,
    WebSocketConnectionManager* manager)
    : redis_(redis), manager_(manager) {
}

WebSocketMessageSubscriber::~WebSocketMessageSubscriber() {
    stop();
}

void WebSocketMessageSubscriber::start() {
    if (running_) {
        return;
    }

    running_ = true;
    subscriberThread_ = std::thread(&WebSocketMessageSubscriber::subscribeLoop, this);

    LOG_INFO << "WebSocket message subscriber started";
}

void WebSocketMessageSubscriber::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (subscriberThread_.joinable()) {
        subscriberThread_.join();
    }

    LOG_INFO << "WebSocket message subscriber stopped";
}

void WebSocketMessageSubscriber::subscribeLoop() {
    if (!redis_) {
        LOG_WARN << "[V3-WS] No Redis configured, Pub/Sub disabled (single-instance mode)";
        return;
    }

    int retryDelaySec = 2;
    constexpr int maxDelaySec = 60;

    while (running_) {
        try {
            auto subscriber = redis_->subscriber();

            subscriber.on_message([this](std::string channel, std::string msg) {
                try {
                    nlohmann::json j = nlohmann::json::parse(msg);

                    std::string targetDeviceId = j["targetDeviceId"].get<std::string>();
                    std::string targetServerId = j["targetServerId"].get<std::string>();
                    std::string fromServerId = j["fromServerId"].get<std::string>();

                    if (targetServerId != WebSocketConnectionManager::getServerId()) {
                        return;
                    }

                    LOG_DEBUG << "Received Pub/Sub message: deviceId=" << targetDeviceId
                              << " from=" << fromServerId;

                    auto conn = manager_->getLocalConnection(targetDeviceId);
                    if (conn) {
                        nlohmann::json message = j["message"];
                        conn->send(message.dump());
                        LOG_DEBUG << "Message delivered to local connection: deviceId=" << targetDeviceId;
                    }
                } catch (const std::exception& e) {
                    LOG_WARN << "[V3-WS] Failed to process Pub/Sub message: " << e.what();
                }
            });

            subscriber.subscribe(WebSocketConnectionManager::REDIS_PUBSUB_CHANNEL);
            retryDelaySec = 2; // 连接成功，重置退避

            while (running_) {
                try {
                    subscriber.consume();
                } catch (const sw::redis::TimeoutError&) {
                    continue;
                } catch (const std::exception& e) {
                    LOG_WARN << "[V3-WS] Subscriber consume error: " << e.what();
                    break; // 退出内层循环，触发重连
                }
            }

        } catch (const std::exception& e) {
            if (running_) {
                LOG_WARN << "[V3-WS] Redis unavailable, WebSocket running in single-instance mode. "
                         << "Retry in " << retryDelaySec << "s. (" << e.what() << ")";
                for (int i = 0; i < retryDelaySec && running_; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                retryDelaySec = std::min(retryDelaySec * 2, maxDelaySec);
            }
        }
    }
}

} // namespace v3
} // namespace wepay
