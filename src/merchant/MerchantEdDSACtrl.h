// WePay-Cpp — 商户端 EdDSA (Ed25519) 设备密钥登录
// POST /merchant/api/auth/challenge          获取登录挑战值
// POST /merchant/api/auth/login-eddsa        EdDSA 签名登录
// POST /merchant/api/auth/register-device    注册设备公钥
// GET  /merchant/api/auth/devices            查看已注册设备列表
// DELETE /merchant/api/auth/device/:id       撤销设备
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <sstream> // 字符串流
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/SimpleJwt.h" // JWT 令牌生成
#include "../common/EdDSAUtils.h" // EdDSA 签名工具
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户 EdDSA 设备密钥登录控制器类
class MerchantEdDSACtrl : public drogon::HttpController<MerchantEdDSACtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantEdDSACtrl::getChallenge,    "/merchant/api/auth/challenge",       drogon::Post); // 获取挑战值
        ADD_METHOD_TO(MerchantEdDSACtrl::loginEdDSA,      "/merchant/api/auth/login-eddsa",     drogon::Post); // EdDSA 登录
        ADD_METHOD_TO(MerchantEdDSACtrl::registerDevice,  "/merchant/api/auth/register-device", drogon::Post, "MerchantAuthFilter"); // 注册设备
        ADD_METHOD_TO(MerchantEdDSACtrl::listDevices,     "/merchant/api/auth/devices",         drogon::Get,  "MerchantAuthFilter"); // 设备列表
        ADD_METHOD_TO(MerchantEdDSACtrl::revokeDevice,    "/merchant/api/auth/device/{1}",      drogon::Delete, "MerchantAuthFilter"); // 撤销设备
    METHOD_LIST_END // 路由列表结束

    // 获取登录挑战值方法（公开接口，无需认证）
    void getChallenge(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string publicKey = (*body).get("public_key", "").asString();
        if (publicKey.empty()) { RESP_ERR(cb, "public_key 必填"); return; }

        // 验证公钥是否已注册
        auto &db = PayDb::instance();
        auto row = db.queryOne(
            "SELECT user_id,username,state FROM device_keys WHERE public_key=? AND user_type=2",
            {publicKey});
        if (row.empty()) { RESP_ERR(cb, "设备未注册"); return; }
        if (row["state"] != "1") { RESP_ERR(cb, "设备已被禁用"); return; }

        // 生成挑战值
        std::string challenge = EdDSAUtils::generateChallenge(32);
        long long timestamp = std::time(nullptr);

        // 挑战值存入缓存（5分钟有效）
        std::string cacheKey = "eddsa_challenge:" + publicKey;
        std::ostringstream oss;
        oss << challenge << ":" << timestamp;
        db.setSetting(cacheKey, oss.str());

        Json::Value data;
        data["challenge"] = challenge;
        data["timestamp"] = (Json::Int64)timestamp;
        data["expires_in"] = 300;  // 5分钟
        RESP_OK(cb, data);
    }

    // EdDSA 签名登录方法
    void loginEdDSA(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string publicKey = (*body).get("public_key", "").asString();
        std::string signature = (*body).get("signature", "").asString();
        std::string challenge = (*body).get("challenge", "").asString();

        if (publicKey.empty() || signature.empty() || challenge.empty()) {
            RESP_ERR(cb, "public_key, signature, challenge 必填"); return;
        }

        auto &db = PayDb::instance();

        // 验证挑战值是否有效
        std::string cacheKey = "eddsa_challenge:" + publicKey;
        std::string cached = db.getSetting(cacheKey, "");
        if (cached.empty()) { RESP_ERR(cb, "挑战值已过期或不存在"); return; }

        // 解析缓存的挑战值和时间戳
        size_t pos = cached.find(':');
        if (pos == std::string::npos) { RESP_ERR(cb, "挑战值格式错误"); return; }
        std::string cachedChallenge = cached.substr(0, pos);
        long long cachedTimestamp = std::stoll(cached.substr(pos + 1));

        if (cachedChallenge != challenge) { RESP_ERR(cb, "挑战值不匹配"); return; }

        // 检查时间戳（5分钟内有效）
        long long now = std::time(nullptr);
        if (now - cachedTimestamp > 300) {
            db.setSetting(cacheKey, "");  // 清除过期挑战值
            RESP_ERR(cb, "挑战值已过期"); return;
        }

        // 查询设备信息
        auto device = db.queryOne(
            "SELECT id,user_id,username,state FROM device_keys WHERE public_key=? AND user_type=2",
            {publicKey});
        if (device.empty()) { RESP_ERR(cb, "设备未注册"); return; }
        if (device["state"] != "1") { RESP_ERR(cb, "设备已被禁用"); return; }

        // 验证签名
        std::ostringstream msgStream;
        msgStream << challenge << ":" << cachedTimestamp;
        std::string message = msgStream.str();

        if (!EdDSAUtils::verify(message, signature, publicKey)) {
            RESP_ERR(cb, "签名验证失败"); return;
        }

        // 签名验证通过，清除挑战值
        db.setSetting(cacheKey, "");

        // 查询商户信息
        std::string userId = device["user_id"];
        std::string username = device["username"];
        auto mch = db.queryOne(
            "SELECT id,mch_no,mch_name,state FROM merchant WHERE id=?",
            {userId});
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; }
        if (mch["state"] != "1") { RESP_ERR(cb, "账号已被禁用"); return; }

        // 更新设备最后使用时间
        db.exec("UPDATE device_keys SET last_used_at=?,updated_at=? WHERE id=?",
                {std::to_string(now), std::to_string(now), device["id"]});

        // 签发 JWT token
        std::string sub = "mch:" + userId + ":" + username;
        std::string token = SimpleJwt::sign(sub);

        Json::Value data;
        data["token"]    = token;
        data["mch_no"]   = mch["mch_no"];
        data["mch_name"] = mch["mch_name"];
        data["username"] = username;
        data["login_method"] = "eddsa";
        RESP_JSON(cb, AjaxResult::success("登录成功", data));
    }

    // 注册设备公钥方法（需要先用账号密码登录）
    void registerDevice(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id");
        std::string username = req->getHeader("X-Mch-Username");

        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string publicKey = (*body).get("public_key", "").asString();
        std::string deviceName = (*body).get("device_name", "未命名设备").asString();
        std::string deviceInfo = (*body).get("device_info", "{}").asString();

        if (publicKey.empty()) { RESP_ERR(cb, "public_key 必填"); return; }

        // 验证公钥格式（Ed25519 公钥应该是 64 个十六进制字符）
        if (publicKey.size() != 64) {
            RESP_ERR(cb, "公钥格式错误（应为64位十六进制字符串）"); return;
        }
        for (char c : publicKey) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                RESP_ERR(cb, "公钥格式错误（包含非十六进制字符）"); return;
            }
        }

        auto &db = PayDb::instance();

        // 检查公钥是否已被注册
        auto exist = db.queryOne("SELECT id FROM device_keys WHERE public_key=?", {publicKey});
        if (!exist.empty()) { RESP_ERR(cb, "该设备已注册"); return; }

        long long now = std::time(nullptr);
        db.exec("INSERT INTO device_keys(user_type,user_id,username,public_key,device_name,"
                "device_info,last_used_at,state,created_at,updated_at) "
                "VALUES(2,?,?,?,?,?,0,1,?,?)",
                {mchId, username, publicKey, deviceName, deviceInfo,
                 std::to_string(now), std::to_string(now)});

        Json::Value data;
        data["public_key"] = publicKey;
        data["device_name"] = deviceName;
        RESP_JSON(cb, AjaxResult::success("设备注册成功", data));
    }

    // 查看已注册设备列表方法
    void listDevices(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id");

        auto &db = PayDb::instance();
        auto rows = db.query(
            "SELECT id,public_key,device_name,device_info,last_used_at,state,created_at "
            "FROM device_keys WHERE user_type=2 AND user_id=? ORDER BY created_at DESC",
            {mchId});

        Json::Value list(Json::arrayValue);
        for (auto &row : rows) {
            Json::Value item;
            item["id"] = std::stoi(row["id"]);
            item["public_key"] = row["public_key"];
            item["device_name"] = row["device_name"];
            item["device_info"] = row["device_info"];
            item["last_used_at"] = std::stoll(row["last_used_at"]);
            item["state"] = std::stoi(row["state"]);
            item["created_at"] = std::stoll(row["created_at"]);
            list.append(item);
        }

        RESP_OK(cb, list);
    }

    // 撤销设备方法
    void revokeDevice(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb, // 响应回调函数
                      const std::string &deviceId) { // 设备 ID
        std::string mchId = req->getHeader("X-Mch-Id");

        auto &db = PayDb::instance();

        // 验证设备是否属于当前商户
        auto device = db.queryOne(
            "SELECT id FROM device_keys WHERE id=? AND user_type=2 AND user_id=?",
            {deviceId, mchId});
        if (device.empty()) { RESP_ERR(cb, "设备不存在或无权操作"); return; }

        // 删除设备
        db.exec("DELETE FROM device_keys WHERE id=?", {deviceId});

        RESP_MSG(cb, "设备已撤销");
    }
};
