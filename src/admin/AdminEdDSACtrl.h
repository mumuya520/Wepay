// WePay-Cpp — 管理后台: EdDSA 认证控制器
// 职责：基于 EdDSA (Ed25519) 的设备密钥登录和管理
//
// API 端点：
// POST /admin/api/auth/challenge          获取登录挑战值
// POST /admin/api/auth/login-eddsa        EdDSA 签名登录
// POST /admin/api/auth/register-device    注册设备公钥
// GET  /admin/api/auth/devices            查看已注册设备列表
// DELETE /admin/api/auth/device/:id       撤销设备
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <sstream> // 字符串流库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/TokenService.h" // Token 服务
#include "../common/EdDSAUtils.h" // EdDSA 工具
#include "../common/RbacService.h" // 基于角色的访问控制
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class AdminEdDSACtrl : public drogon::HttpController<AdminEdDSACtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminEdDSACtrl::getChallenge,    "/admin/api/auth/challenge",       drogon::Post);
        ADD_METHOD_TO(AdminEdDSACtrl::loginEdDSA,      "/admin/api/auth/login-eddsa",     drogon::Post);
        ADD_METHOD_TO(AdminEdDSACtrl::registerDevice,  "/admin/api/auth/register-device", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminEdDSACtrl::listDevices,     "/admin/api/auth/devices",         drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminEdDSACtrl::revokeDevice,    "/admin/api/auth/device/{1}",      drogon::Delete, "AdminAuthFilter");
    METHOD_LIST_END

    // 获取登录挑战值（公开接口，无需认证）
    void getChallenge(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string publicKey = (*body).get("public_key", "").asString();
        if (publicKey.empty()) { RESP_ERR(cb, "public_key 必填"); return; }

        // 验证公钥是否已注册
        auto &db = PayDb::instance();
        auto row = db.queryOne(
            "SELECT user_id,username,state FROM device_keys WHERE public_key=? AND user_type=1",
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

    // EdDSA 签名登录
    void loginEdDSA(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
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
            "SELECT id,user_id,username,state FROM device_keys WHERE public_key=? AND user_type=1",
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

        // 查询用户信息
        std::string userId = device["user_id"];
        std::string username = device["username"];

        int userIdInt = 0;
        bool isSuper = false;

        if (!userId.empty() && userId != "0") {
            auto u = db.queryOne(
                "SELECT id,username,state,is_super FROM sys_user WHERE id=?",
                {userId});
            if (u.empty()) { RESP_ERR(cb, "用户不存在"); return; }
            if (u["state"] != "1") { RESP_ERR(cb, "账号已被禁用"); return; }

            try { userIdInt = std::stoi(u["id"]); } catch (...) {}
            isSuper = (u["is_super"] == "1");
        } else {
            // 兼容老管理员（无 sys_user 记录）
            isSuper = true;
        }

        // 更新设备最后使用时间
        db.exec("UPDATE device_keys SET last_used_at=?,updated_at=? WHERE id=?",
                {std::to_string(now), std::to_string(now), device["id"]});

        // 更新用户最后登录时间
        std::string clientIp = req->getPeerAddr().toIp();
        if (userIdInt > 0) {
            db.exec("UPDATE sys_user SET last_login_ip=?, last_login_at=? WHERE id=?",
                    {clientIp, std::to_string(now), std::to_string(userIdInt)});
        }

        // 签发 JWT token
        auto pair = TokenService::issue(1, userIdInt, username);

        Json::Value data;
        data["token"]         = pair.accessToken;
        data["refresh_token"] = pair.refreshToken;
        data["expires_at"]    = (Json::Int64)pair.accessExpires;
        data["username"]      = username;
        data["is_super"]      = isSuper ? 1 : 0;
        data["login_method"]  = "eddsa";

        // 返回权限列表
        Json::Value permArr(Json::arrayValue);
        if (isSuper) permArr.append("*");
        else if (userIdInt > 0) {
            for (auto &p : RbacService::loadUserPermissions(userIdInt)) permArr.append(p);
        }
        data["permissions"] = permArr;

        OplogService::log(1, username, OplogService::MOD_AUTH, "login_eddsa", username, "",
                          clientIp);
        RESP_JSON(cb, AjaxResult::success("登录成功", data));
    }

    // 注册设备公钥（需要先用账号密码登录）
    void registerDevice(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string userIdStr = req->getHeader("X-User-Id");
        std::string username = req->getHeader("X-Username");

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
                "VALUES(1,?,?,?,?,?,0,1,?,?)",
                {userIdStr, username, publicKey, deviceName, deviceInfo,
                 std::to_string(now), std::to_string(now)});

        Json::Value data;
        data["public_key"] = publicKey;
        data["device_name"] = deviceName;

        OplogService::log(1, username, OplogService::MOD_AUTH, "register_device", deviceName, "",
                          req->getPeerAddr().toIp());
        RESP_JSON(cb, AjaxResult::success("设备注册成功", data));
    }

    // 查看已注册设备列表
    void listDevices(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string userIdStr = req->getHeader("X-User-Id");

        auto &db = PayDb::instance();
        auto rows = db.query(
            "SELECT id,public_key,device_name,device_info,last_used_at,state,created_at "
            "FROM device_keys WHERE user_type=1 AND user_id=? ORDER BY created_at DESC",
            {userIdStr});

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

    // 撤销设备
    void revokeDevice(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                      const std::string &deviceId) {
        std::string userIdStr = req->getHeader("X-User-Id");
        std::string username = req->getHeader("X-Username");

        auto &db = PayDb::instance();

        // 验证设备是否属于当前用户
        auto device = db.queryOne(
            "SELECT id,device_name FROM device_keys WHERE id=? AND user_type=1 AND user_id=?",
            {deviceId, userIdStr});
        if (device.empty()) { RESP_ERR(cb, "设备不存在或无权操作"); return; }

        // 删除设备
        db.exec("DELETE FROM device_keys WHERE id=?", {deviceId});

        OplogService::log(1, username, OplogService::MOD_AUTH, "revoke_device",
                          device["device_name"], "", req->getPeerAddr().toIp());
        RESP_MSG(cb, "设备已撤销");
    }
};
