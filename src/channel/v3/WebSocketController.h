#pragma once
#include <drogon/HttpController.h>
#include <drogon/WebSocketController.h>
#include "SecurityValidator.h"
#include "DeviceManager.h"

namespace wepay {
namespace v3 {

// WebSocket连接管理器
class WebSocketManager {
public:
    static WebSocketManager& getInstance() {
        static WebSocketManager instance;
        return instance;
    }

    // 添加连接
    void addConnection(const std::string& deviceId,
                      const drogon::WebSocketConnectionPtr& conn);

    // 移除连接
    void removeConnection(const std::string& deviceId);

    // 获取连接
    drogon::WebSocketConnectionPtr getConnection(const std::string& deviceId);

    // 推送订单到设备
    bool pushOrderToDevice(const std::string& deviceId, const nlohmann::json& orderData);

    // 广播消息到所有设备
    void broadcastMessage(const nlohmann::json& message);

    // 获取在线连接数
    size_t getConnectionCount() const;

private:
    WebSocketManager() = default;
    std::map<std::string, drogon::WebSocketConnectionPtr> connections_;
    mutable std::mutex mutex_;
};

// V3 WebSocket控制器
class WebSocketController : public drogon::WebSocketController<WebSocketController> {
public:
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/api/wepay/v3/ws", drogon::Get);
    WS_PATH_LIST_END

    void handleNewMessage(const drogon::WebSocketConnectionPtr& wsConnPtr,
                         std::string&& message,
                         const drogon::WebSocketMessageType& type) override;

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                            const drogon::WebSocketConnectionPtr& wsConnPtr) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& wsConnPtr) override;

private:
    std::shared_ptr<SecurityValidator> validator_;
    std::shared_ptr<DeviceManager> deviceManager_;

    // 验证WebSocket连接
    bool validateConnection(const drogon::HttpRequestPtr& req,
                           std::string& deviceId,
                           std::string& errorMsg);

    // 处理心跳消息
    void handlePing(const drogon::WebSocketConnectionPtr& wsConnPtr,
                   const nlohmann::json& message);

    // 处理订单确认消息
    void handleOrderAck(const drogon::WebSocketConnectionPtr& wsConnPtr,
                       const nlohmann::json& message);
};

} // namespace v3
} // namespace wepay
