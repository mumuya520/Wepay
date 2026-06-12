// WePay-Cpp — 管理后台: 设备密钥认证控制器
// 职责：设备密钥登录、注册、撤销等设备管理功能（支持 EdDSA/RSA）
//
// API 端点：
// POST /admin/api/auth/challenge          获取登录挑战值
// POST /admin/api/auth/login-device       设备密钥签名登录
// POST /admin/api/auth/register-device    注册设备公钥
// GET  /admin/api/auth/devices            查看已注册设备列表
// PUT  /admin/api/auth/device/:id/reset   重置设备密钥
// DELETE /admin/api/auth/device/:id       撤销设备
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <sstream> // 字符串流库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/TokenService.h" // Token 服务
#include "../common/DeviceKeyUtils.h" // 设备密钥工具
#include "../common/DeviceChallengeCache.h" // 设备挑战缓存
#include "../common/LoginAttemptService.h" // 登录尝试服务
#include "../common/RbacService.h" // 基于角色的访问控制
#include "../common/OplogService.h" // 操作日志服务
#include "../common/IpVerifyUtils.h" // IP 验证工具
#include "../filters/AdminAuthFilter.h"

class AdminDeviceKeyCtrl : public drogon::HttpController<AdminDeviceKeyCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminDeviceKeyCtrl::getChallenge,    "/admin/api/auth/challenge",          drogon::Post);
        ADD_METHOD_TO(AdminDeviceKeyCtrl::loginDevice,     "/admin/api/auth/login-device",       drogon::Post);
        ADD_METHOD_TO(AdminDeviceKeyCtrl::registerDevice,  "/admin/api/auth/register-device",    drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminDeviceKeyCtrl::listDevices,     "/admin/api/auth/devices",            drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminDeviceKeyCtrl::resetDevice,     "/admin/api/auth/device/{1}/reset",   drogon::Put,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminDeviceKeyCtrl::revokeDevice,    "/admin/api/auth/device/{1}",         drogon::Delete, "AdminAuthFilter");
    METHOD_LIST_END

    // 获取登录挑战值（公开接口，但有频率限制）
    void getChallenge(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string publicKey = (*body).get("public_key", "").asString();
        if (publicKey.empty()) { RESP_ERR(cb, "public_key 必填"); return; }

        std::string clientIp = req->getPeerAddr().toIp();

        // 防止挑战值请求 DoS：同一 IP 每分钟最多 10 次
        static std::unordered_map<std::string, std::pair<int, long long>> challengeRateLimit;
        static std::mutex rateLimitMutex;
        {
            std::lock_guard<std::mutex> lock(rateLimitMutex);
            long long now = std::time(nullptr);
            auto &limit = challengeRateLimit[clientIp];
            if (now - limit.second > 60) {
                limit = {1, now};
            } else {
                limit.first++;
                if (limit.first > 10) {
                    RESP_ERR(cb, "请求过于频繁，请稍后再试"); return;
                }
            }
        }

        auto &db = PayDb::instance();

        // 验证公钥是否已注册
        auto row = db.queryOne(
            "SELECT user_id,username,state FROM device_keys WHERE public_key=? AND user_type=1",
            {publicKey});
        if (row.empty()) { RESP_ERR(cb, "设备未注册"); return; }
        if (row["state"] != "1") { RESP_ERR(cb, "设备已被禁用"); return; }

        // 检查用户账号状态
        if (!row["user_id"].empty() && row["user_id"] != "0") {
            auto u = db.queryOne("SELECT state FROM sys_user WHERE id=?", {row["user_id"]});
            if (u.empty() || u["state"] != "1") {
                RESP_ERR(cb, "账号已被禁用"); return;
            }
        }

        // 生成挑战值
        std::string challenge = DeviceKeyUtils::generateChallenge(32);
        long long timestamp = std::time(nullptr);

        // 存入内存缓存（5分钟有效）
        DeviceChallengeCache::instance().set(publicKey, challenge, timestamp);

        Json::Value data;
        data["challenge"] = challenge;
        data["timestamp"] = (Json::Int64)timestamp;
        data["expires_in"] = 300;  // 5分钟
        RESP_OK(cb, data);
    }

    // 设备密钥签名登录
    void loginDevice(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string publicKey = (*body).get("public_key", "").asString();
        std::string signature = (*body).get("signature", "").asString();
        std::string challenge = (*body).get("challenge", "").asString();

        if (publicKey.empty() || signature.empty() || challenge.empty()) {
            RESP_ERR(cb, "public_key, signature, challenge 必填"); return;
        }

        std::string clientIp = req->getPeerAddr().toIp();
        std::string ua = req->getHeader("User-Agent");

        auto &db = PayDb::instance();

        // 查询设备信息（包含 IP 验证配置）
        auto device = db.queryOne(
            "SELECT id,user_id,username,key_type,state,require_ip_verify,trusted_ips FROM device_keys WHERE public_key=? AND user_type=1",
            {publicKey});
        if (device.empty()) {
            LoginAttemptService::record("device:" + publicKey.substr(0, 16), clientIp, ua, false, "设备未注册");
            RESP_ERR(cb, "设备未注册"); return;
        }

        std::string username = device["username"];

        // 防爆破: 检查是否已锁定
        int lockedSec = LoginAttemptService::isLocked(username, clientIp);
        if (lockedSec > 0) {
            RESP_ERR(cb, "登录失败次数过多，账号已被临时锁定 " + std::to_string(lockedSec / 60 + 1) + " 分钟");
            return;
        }

        if (device["state"] != "1") {
            LoginAttemptService::record(username, clientIp, ua, false, "设备已被禁用");
            RESP_ERR(cb, "设备已被禁用"); return;
        }

        // IP 验证（本地 IP 除外：192.168.*/127.*/localhost）
        bool requireIpVerify = (device["require_ip_verify"] == "1");
        std::string trustedIps = device["trusted_ips"];
        auto [ipAllowed, ipRejectReason] = IpVerifyUtils::verifyDeviceLoginIp(
            requireIpVerify, trustedIps, clientIp);
        if (!ipAllowed) {
            LoginAttemptService::record(username, clientIp, ua, false, "IP验证失败");
            RESP_ERR(cb, ipRejectReason); return;
        }

        // 获取并标记挑战值为已使用（一次性）
        auto cached = DeviceChallengeCache::instance().getAndMarkUsed(publicKey);
        if (cached.value.empty() || cached.used) {
            LoginAttemptService::record(username, clientIp, ua, false, "挑战值无效");
            RESP_ERR(cb, "挑战值已过期、不存在或已被使用"); return;
        }

        if (cached.value != challenge) {
            LoginAttemptService::record(username, clientIp, ua, false, "挑战值不匹配");
            RESP_ERR(cb, "挑战值不匹配"); return;
        }

        // 检查时间戳（5分钟内有效）
        long long now = std::time(nullptr);
        if (now - cached.timestamp > 300) {
            DeviceChallengeCache::instance().remove(publicKey);
            LoginAttemptService::record(username, clientIp, ua, false, "挑战值已过期");
            RESP_ERR(cb, "挑战值已过期"); return;
        }

        // 构造待验证消息
        std::ostringstream msgStream;
        msgStream << challenge << ":" << cached.timestamp;
        std::string message = msgStream.str();

        // 根据密钥类型验证签名
        std::string keyType = device["key_type"];
        DeviceKeyUtils::KeyType kt;
        if (keyType == "ed25519") kt = DeviceKeyUtils::KeyType::ED25519;
        else if (keyType == "rsa2048") kt = DeviceKeyUtils::KeyType::RSA_2048;
        else if (keyType == "rsa4096") kt = DeviceKeyUtils::KeyType::RSA_4096;
        else {
            LoginAttemptService::record(username, clientIp, ua, false, "不支持的密钥类型");
            RESP_ERR(cb, "不支持的密钥类型"); return;
        }

        if (!DeviceKeyUtils::verifySignature(message, signature, publicKey, kt)) {
            LoginAttemptService::record(username, clientIp, ua, false, "签名验证失败");
            RESP_ERR(cb, "签名验证失败"); return;
        }

        // 签名验证通过，清除挑战值
        DeviceChallengeCache::instance().remove(publicKey);

        // 查询用户信息
        std::string userId = device["user_id"];

        int userIdInt = 0;
        bool isSuper = false;

        if (!userId.empty() && userId != "0") {
            auto u = db.queryOne(
                "SELECT id,username,state,is_super FROM sys_user WHERE id=?",
                {userId});
            if (u.empty()) {
                LoginAttemptService::record(username, clientIp, ua, false, "用户不存在");
                RESP_ERR(cb, "用户不存在"); return;
            }
            if (u["state"] != "1") {
                LoginAttemptService::record(username, clientIp, ua, false, "账号已被禁用");
                RESP_ERR(cb, "账号已被禁用"); return;
            }

            try { userIdInt = std::stoi(u["id"]); } catch (...) {}
            isSuper = (u["is_super"] == "1");
        } else {
            // 兼容老管理员（无 sys_user 记录）
            isSuper = true;
        }

        // 登录成功：记录尝试
        LoginAttemptService::record(username, clientIp, ua, true);

        // 更新设备最后使用时间和IP
        db.exec("UPDATE device_keys SET last_used_at=?,last_used_ip=?,updated_at=? WHERE id=?",
                {std::to_string(now), clientIp, std::to_string(now), device["id"]});

        // 更新用户最后登录时间
        if (userIdInt > 0) {
            db.exec("UPDATE sys_user SET last_login_ip=?, last_login_at=? WHERE id=?",
                    {clientIp, std::to_string(now), std::to_string(userIdInt)});
        }

        // 记录操作日志
        OplogService::log(1, username, OplogService::MOD_AUTH, "login_device",
                          "设备密钥登录 (key_type=" + keyType + ")", "", clientIp);

        // 签发 JWT token
        auto pair = TokenService::issue(1, userIdInt, username);

        Json::Value data;
        data["token"]         = pair.accessToken;
        data["refresh_token"] = pair.refreshToken;
        data["expires_at"]    = (Json::Int64)pair.accessExpires;
        data["username"]      = username;
        data["is_super"]      = isSuper ? 1 : 0;
        data["login_method"]  = "device_key";
        data["key_type"]      = keyType;

        // 返回权限列表
        Json::Value permArr(Json::arrayValue);
        if (isSuper) permArr.append("*");
        else if (userIdInt > 0) {
            for (auto &p : RbacService::loadUserPermissions(userIdInt)) permArr.append(p);
        }
        data["permissions"] = permArr;

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

        // 限制设备名称长度
        if (deviceName.size() > 50) {
            RESP_ERR(cb, "设备名称不能超过50个字符"); return;
        }

        // 自动检测密钥类型
        std::string keyType;
        try {
            auto kt = DeviceKeyUtils::detectKeyType(publicKey);
            switch (kt) {
                case DeviceKeyUtils::KeyType::ED25519:   keyType = "ed25519"; break;
                case DeviceKeyUtils::KeyType::RSA_2048:  keyType = "rsa2048"; break;
                case DeviceKeyUtils::KeyType::RSA_4096:  keyType = "rsa4096"; break;
            }
        } catch (const std::exception &e) {
            RESP_ERR(cb, std::string("公钥格式错误: ") + e.what()); return;
        }

        auto &db = PayDb::instance();

        // 检查公钥是否已被注册
        auto exist = db.queryOne("SELECT id FROM device_keys WHERE public_key=?", {publicKey});
        if (!exist.empty()) { RESP_ERR(cb, "该设备已注册"); return; }

        // 限制每个用户最多注册 10 个设备
        auto count = db.queryOne(
            "SELECT COUNT(*) as cnt FROM device_keys WHERE user_type=1 AND user_id=?",
            {userIdStr});
        int deviceCount = 0;
        try { deviceCount = std::stoi(count["cnt"]); } catch (...) {}
        if (deviceCount >= 10) {
            RESP_ERR(cb, "每个账号最多注册10个设备，请先删除不用的设备"); return;
        }

        long long now = std::time(nullptr);
        std::string clientIp = req->getPeerAddr().toIp();

        db.exec("INSERT INTO device_keys(user_type,user_id,username,key_type,public_key,device_name,"
                "device_info,last_used_at,last_used_ip,state,created_at,updated_at) "
                "VALUES(1,?,?,?,?,?,?,0,'',1,?,?)",
                {userIdStr, username, keyType, publicKey, deviceName, deviceInfo,
                 std::to_string(now), std::to_string(now)});

        Json::Value data;
        data["public_key"] = publicKey;
        data["device_name"] = deviceName;
        data["key_type"] = keyType;

        OplogService::log(1, username, OplogService::MOD_AUTH, "register_device",
                          deviceName + " (" + keyType + ")", "", clientIp);
        RESP_JSON(cb, AjaxResult::success("设备注册成功", data));
    }

    // 查看已注册设备列表（隐藏完整公钥）
    void listDevices(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string userIdStr = req->getHeader("X-User-Id");

        auto &db = PayDb::instance();
        auto rows = db.query(
            "SELECT id,key_type,public_key,device_name,device_info,last_used_at,last_used_ip,state,created_at "
            "FROM device_keys WHERE user_type=1 AND user_id=? ORDER BY created_at DESC",
            {userIdStr});

        Json::Value list(Json::arrayValue);
        for (auto &row : rows) {
            Json::Value item;
            item["id"] = std::stoi(row["id"]);
            item["key_type"] = row["key_type"];

            // 安全：只返回公钥指纹，不返回完整公钥
            std::string pk = row["public_key"];
            if (pk.size() > 16) {
                item["public_key_fingerprint"] = pk.substr(0, 8) + "..." + pk.substr(pk.size() - 8);
            } else {
                item["public_key_fingerprint"] = pk;
            }

            item["device_name"] = row["device_name"];
            item["device_info"] = row["device_info"];
            item["last_used_at"] = static_cast<Json::Int64>(std::stoll(row["last_used_at"]));
            item["last_used_ip"] = row["last_used_ip"];
            item["state"] = std::stoi(row["state"]);
            item["created_at"] = static_cast<Json::Int64>(std::stoll(row["created_at"]));
            list.append(item);
        }

        RESP_OK(cb, list);
    }

    // 重置设备密钥（用户提供新公钥）
    void resetDevice(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                     const std::string &deviceId) {
        std::string userIdStr = req->getHeader("X-User-Id");
        std::string username = req->getHeader("X-Username");

        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string newPublicKey = (*body).get("new_public_key", "").asString();
        if (newPublicKey.empty()) { RESP_ERR(cb, "new_public_key 必填"); return; }

        // 自动检测新密钥类型
        std::string keyType;
        try {
            auto kt = DeviceKeyUtils::detectKeyType(newPublicKey);
            switch (kt) {
                case DeviceKeyUtils::KeyType::ED25519:   keyType = "ed25519"; break;
                case DeviceKeyUtils::KeyType::RSA_2048:  keyType = "rsa2048"; break;
                case DeviceKeyUtils::KeyType::RSA_4096:  keyType = "rsa4096"; break;
            }
        } catch (const std::exception &e) {
            RESP_ERR(cb, std::string("公钥格式错误: ") + e.what()); return;
        }

        auto &db = PayDb::instance();

        // 验证设备是否属于当前用户
        auto device = db.queryOne(
            "SELECT id,public_key,device_name FROM device_keys WHERE id=? AND user_type=1 AND user_id=?",
            {deviceId, userIdStr});
        if (device.empty()) { RESP_ERR(cb, "设备不存在或无权操作"); return; }

        // 检查新公钥是否已被其他设备使用
        auto exist = db.queryOne("SELECT id FROM device_keys WHERE public_key=? AND id!=?",
                                 {newPublicKey, deviceId});
        if (!exist.empty()) { RESP_ERR(cb, "该公钥已被其他设备使用"); return; }

        // 清除旧公钥的挑战值缓存
        DeviceChallengeCache::instance().remove(device["public_key"]);

        // 更新公钥和密钥类型
        long long now = std::time(nullptr);
        db.exec("UPDATE device_keys SET key_type=?,public_key=?,updated_at=? WHERE id=?",
                {keyType, newPublicKey, std::to_string(now), deviceId});

        Json::Value data;
        data["new_public_key"] = newPublicKey;
        data["key_type"] = keyType;

        OplogService::log(1, username, OplogService::MOD_AUTH, "reset_device",
                          device["device_name"] + " → " + keyType, "", req->getPeerAddr().toIp());
        RESP_JSON(cb, AjaxResult::success("密钥重置成功", data));
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
            "SELECT id,public_key,device_name FROM device_keys WHERE id=? AND user_type=1 AND user_id=?",
            {deviceId, userIdStr});
        if (device.empty()) { RESP_ERR(cb, "设备不存在或无权操作"); return; }

        // 清除挑战值缓存
        DeviceChallengeCache::instance().remove(device["public_key"]);

        // 删除设备
        db.exec("DELETE FROM device_keys WHERE id=?", {deviceId});

        OplogService::log(1, username, OplogService::MOD_AUTH, "revoke_device",
                          device["device_name"], "", req->getPeerAddr().toIp());
        RESP_MSG(cb, "设备已撤销");
    }
};
