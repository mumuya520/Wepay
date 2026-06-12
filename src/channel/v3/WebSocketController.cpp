#include "WebSocketController.h"
#include "WepayV3Config.h"
#include <drogon/drogon.h>

namespace wepay {
namespace v3 {

// WebSocketManager实现
void WebSocketManager::addConnection(const std::string& deviceId,
                                    const drogon::WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_[deviceId] = conn;
}

void WebSocketManager::removeConnection(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(deviceId);
}

drogon::WebSocketConnectionPtr WebSocketManager::getConnection(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(deviceId);
    if (it != connections_.end()) {
        return it->second;
    }
    return nullptr;
}

bool WebSocketManager::pushOrderToDevice(const std::string& deviceId,
                                        const nlohmann::json& orderData) {
    auto conn = getConnection(deviceId);
    if (!conn) {
        return false;
    }

    nlohmann::json message;
    message["type"] = "ORDER_PUSH";
    message["data"] = orderData;
    message["timestamp"] = std::time(nullptr);

    conn->send(message.dump());
    return true;
}

void WebSocketManager::broadcastMessage(const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string messageStr = message.dump();

    for (const auto& [deviceId, conn] : connections_) {
        conn->send(messageStr);
    }
}

size_t WebSocketManager::getConnectionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

// WebSocketController实现
void WebSocketController::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& wsConnPtr) {

    std::string deviceId;
    std::string errorMsg;

    // 验证连接
    if (!validateConnection(req, deviceId, errorMsg)) {
        nlohmann::json errorResponse;
        errorResponse["type"] = "ERROR";
        errorResponse["message"] = errorMsg;
        wsConnPtr->send(errorResponse.dump());
        wsConnPtr->forceClose();
        return;
    }

    // 添加到连接管理器
    WebSocketManager::getInstance().addConnection(deviceId, wsConnPtr);

    // 存储deviceId到连接上下文
    wsConnPtr->setContext(std::make_shared<std::string>(deviceId));

    // 发送连接成功消息
    nlohmann::json welcomeMsg;
    welcomeMsg["type"] = "CONNECTED";
    welcomeMsg["deviceId"] = deviceId;
    welcomeMsg["serverTime"] = std::time(nullptr);
    wsConnPtr->send(welcomeMsg.dump());

    LOG_INFO << "WebSocket connected: " << deviceId;
}

void WebSocketController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& wsConnPtr,
    std::string&& message,
    const drogon::WebSocketMessageType& type) {

    if (type != drogon::WebSocketMessageType::Text) {
        return;
    }

    try {
        nlohmann::json msg = nlohmann::json::parse(message);
        std::string msgType = msg["type"];

        if (msgType == "PING") {
            handlePing(wsConnPtr, msg);
        } else if (msgType == "ORDER_ACK") {
            handleOrderAck(wsConnPtr, msg);
        } else {
            LOG_WARN << "Unknown message type: " << msgType;
        }

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to parse WebSocket message: " << e.what();
    }
}

void WebSocketController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& wsConnPtr) {

    auto deviceIdPtr = wsConnPtr->getContext<std::string>();
    if (deviceIdPtr) {
        std::string deviceId = *deviceIdPtr;
        WebSocketManager::getInstance().removeConnection(deviceId);
        LOG_INFO << "WebSocket disconnected: " << deviceId;
    }
}

bool WebSocketController::validateConnection(
    const drogon::HttpRequestPtr& req,
    std::string& deviceId,
    std::string& errorMsg) {

    // 从查询参数获取认证信息
    deviceId = req->getParameter("deviceId");
    std::string timestamp = req->getParameter("timestamp");
    std::string nonce = req->getParameter("nonce");
    std::string sign = req->getParameter("sign");

    if (deviceId.empty() || timestamp.empty() || nonce.empty() || sign.empty()) {
        errorMsg = "Missing authentication parameters";
        return false;
    }

    // 初始化validator
    auto& config = WepayV3Config::getInstance();
    validator_ = std::make_shared<SecurityValidator>(
        config.security.hmacSecret,
        config.security.rsaPublicKey
    );

    // 验证签名
    std::string clientIp = req->getPeerAddr().toIp();
    auto result = validator_->validateRequest(deviceId, timestamp, nonce, sign, clientIp);

    if (!result.success) {
        errorMsg = result.errorMsg;
        return false;
    }

    return true;
}

void WebSocketController::handlePing(
    const drogon::WebSocketConnectionPtr& wsConnPtr,
    const nlohmann::json& message) {

    // 回复PONG
    nlohmann::json pong;
    pong["type"] = "PONG";
    pong["timestamp"] = std::time(nullptr);

    wsConnPtr->send(pong.dump());
}

void WebSocketController::handleOrderAck(
    const drogon::WebSocketConnectionPtr& wsConnPtr,
    const nlohmann::json& message) {

    try {
        std::string orderId = message["orderId"];
        std::string status = message["status"]; // "RECEIVED" / "PAID" / "FAILED"

        LOG_INFO << "Order ACK received: " << orderId << " status: " << status;

        // 更新订单状态到数据库
        // TODO: 调用OrderService更新订单状态

        // 发送确认
        nlohmann::json ack;
        ack["type"] = "ACK_RECEIVED";
        ack["orderId"] = orderId;
        wsConnPtr->send(ack.dump());

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to handle order ACK: " << e.what();
    }
}

} // namespace v3
} // namespace wepay
