#include "HeartbeatController.h"
#include "WepayV3Config.h"
#include "../../common/PayDb.h"

namespace wepay {
namespace v3 {

HeartbeatController::HeartbeatController() {
    auto& config = WepayV3Config::getInstance();

    validator_ = std::make_shared<SecurityValidator>(
        config.security.hmacSecret,
        config.security.rsaPublicKey
    );

    auto redis = std::make_shared<sw::redis::Redis>("tcp://" + config.redis.host + ":" +
                                                     std::to_string(config.redis.port));
    deviceManager_ = std::make_shared<DeviceManager>(redis);
}

void HeartbeatController::handleHeartbeat(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        // 1. 解析请求体
        auto jsonBody = req->getJsonObject();
        if (!jsonBody) {
            callback(buildResponse(400, "Invalid JSON body"));
            return;
        }

        std::string deviceId, timestamp, nonce, sign;
        DeviceInfo deviceInfo;

        if (!parseHeartbeatRequest(*jsonBody, deviceId, timestamp, nonce, sign, deviceInfo)) {
            callback(buildResponse(400, "Invalid request parameters"));
            return;
        }

        // 2. 获取客户端IP
        std::string clientIp = req->getPeerAddr().toIp();
        deviceInfo.ip = clientIp;

        // 3. 动态从 DB 读取 hmac_secret
        auto dynValidator = std::make_shared<SecurityValidator>(
            SecurityValidator::resolveHmacSecret(deviceId), "");

        // 安全校验
        auto validationResult = dynValidator->validateRequest(
            deviceId, timestamp, nonce, sign, clientIp
        );

        if (!validationResult.success) {
            callback(buildResponse(403, validationResult.errorMsg));
            return;
        }

        // 4. IP白名单校验
        auto& config = WepayV3Config::getInstance();
        if (!config.security.ipWhitelist.empty()) {
            if (!validator_->validateIpWhitelist(clientIp, config.security.ipWhitelist)) {
                callback(buildResponse(403, "IP not in whitelist"));
                return;
            }
        }

        // 5. 更新设备心跳（Redis）
        deviceManager_->updateHeartbeat(deviceId, deviceInfo);

        // 5b. 同步写入 v3_device DB（Admin 列表数据源）
        try {
            auto& db = PayDb::instance();
            long long now = std::time(nullptr);
            auto existing = db.queryOne(
                "SELECT id,online FROM v3_device WHERE device_id=?", {deviceId});
            if (existing.empty()) {
                db.exec(
                    "INSERT INTO v3_device"
                    "(device_id,ip,online,battery,network,app_version,last_heartbeat,created_at,updated_at)"
                    " VALUES(?,?,1,?,?,?,?,?,?)",
                    {deviceId, deviceInfo.ip,
                     std::to_string(deviceInfo.battery), deviceInfo.network,
                     deviceInfo.appVersion,
                     std::to_string(now), std::to_string(now), std::to_string(now)});
            } else {
                db.exec(
                    "UPDATE v3_device SET last_heartbeat=?,ip=?,online=1,battery=?,network=?,app_version=?,updated_at=?"
                    " WHERE device_id=?",
                    {std::to_string(now), deviceInfo.ip,
                     std::to_string(deviceInfo.battery), deviceInfo.network,
                     deviceInfo.appVersion, std::to_string(now), deviceId});
            }
            // 5c. 绑定商户（pid → v3_device_merchant）
            if (!jsonBody->get("pid", "").asString().empty()) {
                std::string pid = (*jsonBody)["pid"].asString();
                auto m = db.queryOne(
                    "SELECT id FROM merchant WHERE mch_no=? AND state=1", {pid});
                if (!m.empty()) {
                    db.exec(
                        "INSERT INTO v3_device_merchant(device_id,merchant_id,bind_time,status,created_at,updated_at)"
                        " VALUES(?,?,?,1,?,?)"
                        " ON CONFLICT(device_id,merchant_id) DO UPDATE SET updated_at=EXCLUDED.updated_at",
                        {deviceId, m["id"],
                         std::to_string(now), std::to_string(now), std::to_string(now)});
                }
            }
            // 5d. 更新全局在线状态（供 WepayV3Plugin::createOrder 的 require_online 检测）
            db.setSetting("wepay_v3_lastheart", std::to_string(now));
            db.setSetting("wepay_v3_jkstate",   "1");
        } catch (...) {}

        // 6. 返回成功响应
        nlohmann::json responseData;
        responseData["deviceId"] = deviceId;
        responseData["online"] = true;
        responseData["serverTime"] = std::time(nullptr);

        callback(buildResponse(200, "success", responseData));

    } catch (const std::exception& e) {
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

bool HeartbeatController::parseHeartbeatRequest(
    const Json::Value& body,
    std::string& deviceId,
    std::string& timestamp,
    std::string& nonce,
    std::string& sign,
    DeviceInfo& deviceInfo) {

    try {
        // 必填字段（jsoncpp API）
        deviceId = body["deviceId"].asString();
        timestamp = body["timestamp"].asString();
        nonce = body["nonce"].asString();
        sign = body["sign"].asString();

        // 设备信息（可选）
        if (body.isMember("battery")) {
            deviceInfo.battery = body["battery"].asInt();
        }
        if (body.isMember("network")) {
            deviceInfo.network = body["network"].asString();
        }
        if (body.isMember("appVersion")) {
            deviceInfo.appVersion = body["appVersion"].asString();
        }
        if (body.isMember("screenResolution")) {
            deviceInfo.screenResolution = body["screenResolution"].asString();
        }
        if (body.isMember("pid")) {
            deviceInfo.pid = body["pid"].asString();
        }

        return true;
    } catch (...) {
        return false;
    }
}

drogon::HttpResponsePtr HeartbeatController::buildResponse(
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
    } else {
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    return resp;
}

} // namespace v3
} // namespace wepay
