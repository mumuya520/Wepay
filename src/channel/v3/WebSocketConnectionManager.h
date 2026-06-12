#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <sw/redis++/redis++.h>
#include <drogon/WebSocketConnection.h>
#include <nlohmann/json.hpp>

namespace wepay {
namespace v3 {

/**
 * WebSocket连接管理器
 * 支持多实例部署，连接信息存储在Redis中
 */
class WebSocketConnectionManager {
public:
    static constexpr const char* REDIS_PUBSUB_CHANNEL = "ws:message";

    struct ConnectionInfo {
        std::string deviceId;
        std::string merchantId;
        std::string serverId;      // 当前服务器实例ID
        int64_t connectTime;
        int64_t lastHeartbeat;
        std::string clientIp;
        std::string userAgent;
    };

    explicit WebSocketConnectionManager(std::shared_ptr<sw::redis::Redis> redis);
    ~WebSocketConnectionManager() = default;

    // 注册连接（本地+Redis）
    bool registerConnection(const std::string& deviceId,
                           const std::string& merchantId,
                           const drogon::WebSocketConnectionPtr& conn,
                           const std::string& clientIp,
                           const std::string& userAgent);

    // 注销连接
    void unregisterConnection(const std::string& deviceId);

    // 更新心跳时间
    void updateHeartbeat(const std::string& deviceId);

    // 获取本地连接
    drogon::WebSocketConnectionPtr getLocalConnection(const std::string& deviceId);

    // 检查设备是否在线（查询Redis）
    bool isDeviceOnline(const std::string& deviceId);

    // 获取设备所在服务器ID
    std::string getDeviceServerId(const std::string& deviceId);

    // 获取连接信息
    std::optional<ConnectionInfo> getConnectionInfo(const std::string& deviceId);

    // 推送消息到设备（支持跨实例）
    bool pushMessageToDevice(const std::string& deviceId,
                            const nlohmann::json& message);

    // 广播消息到所有连接
    void broadcastMessage(const nlohmann::json& message);

    // 清理过期连接（定时任务调用）
    void cleanupExpiredConnections(int timeoutSeconds);

    // 获取当前服务器ID
    static std::string getServerId();

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    std::string serverId_;

    // 本地连接映射
    std::unordered_map<std::string, drogon::WebSocketConnectionPtr> localConnections_;
    std::mutex connectionsMutex_;

    // Redis键前缀
    static constexpr const char* REDIS_KEY_PREFIX = "ws:conn:";

    // 构建Redis键
    std::string buildRedisKey(const std::string& deviceId) const;

    // 发布消息到Redis Pub/Sub
    void publishMessage(const std::string& deviceId, const nlohmann::json& message);

    // 订阅Redis Pub/Sub消息
    void subscribeMessages();
};

/**
 * Redis Pub/Sub订阅器
 * 用于接收其他实例发送的WebSocket消息
 */
class WebSocketMessageSubscriber {
public:
    WebSocketMessageSubscriber(std::shared_ptr<sw::redis::Redis> redis,
                               WebSocketConnectionManager* manager);
    ~WebSocketMessageSubscriber();

    void start();
    void stop();

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    WebSocketConnectionManager* manager_;
    std::atomic<bool> running_{false};
    std::thread subscriberThread_;

    void subscribeLoop();
};

} // namespace v3
} // namespace wepay
