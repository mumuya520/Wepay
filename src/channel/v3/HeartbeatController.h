#pragma once
#include <drogon/HttpController.h>
#include "SecurityValidator.h"
#include "DeviceManager.h"

namespace wepay {
namespace v3 {

// V3心跳接口控制器
class HeartbeatController : public drogon::HttpController<HeartbeatController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HeartbeatController::handleHeartbeat,
                  "/api/wepay/v3/heart", drogon::Post);
    ADD_METHOD_TO(HeartbeatController::handleHeartbeat,
                  "/device/heartbeat",   drogon::Post);
    METHOD_LIST_END

    HeartbeatController();

    void handleHeartbeat(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<SecurityValidator> validator_;
    std::shared_ptr<DeviceManager> deviceManager_;

    // 解析心跳请求
    bool parseHeartbeatRequest(const Json::Value& body,
                              std::string& deviceId,
                              std::string& timestamp,
                              std::string& nonce,
                              std::string& sign,
                              DeviceInfo& deviceInfo);

    // 构建响应
    drogon::HttpResponsePtr buildResponse(int code, const std::string& message,
                                         const nlohmann::json& data = {});
};

} // namespace v3
} // namespace wepay
