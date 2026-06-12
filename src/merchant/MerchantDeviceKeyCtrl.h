// WePay-Cpp — 商户端设备密钥登录（支持 EdDSA/RSA）+ 完整安全防护 + IP验证
// POST /merchant/api/auth/challenge          获取登录挑战值
// POST /merchant/api/auth/login-device       设备密钥签名登录
// POST /merchant/api/auth/register-device    注册设备公钥
// GET  /merchant/api/auth/devices            查看已注册设备列表
// PUT  /merchant/api/auth/device/:id/reset   重置设备密钥
// DELETE /merchant/api/auth/device/:id       撤销设备
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <sstream> // 字符串流
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/SimpleJwt.h" // JWT 令牌生成
#include "../common/DeviceKeyUtils.h" // 设备密钥工具
#include "../common/DeviceChallengeCache.h" // 设备挑战值缓存
#include "../common/LoginAttemptService.h" // 登录尝试服务
#include "../common/OplogService.h" // 操作日志服务
#include "../common/IpVerifyUtils.h" // IP 验证工具
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户设备密钥登录控制器类
class MerchantDeviceKeyCtrl : public drogon::HttpController<MerchantDeviceKeyCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantDeviceKeyCtrl::getChallenge,    "/merchant/api/auth/challenge",          drogon::Post); // 获取挑战值
        ADD_METHOD_TO(MerchantDeviceKeyCtrl::loginDevice,     "/merchant/api/auth/login-device",       drogon::Post); // 设备登录
        ADD_METHOD_TO(MerchantDeviceKeyCtrl::registerDevice,  "/merchant/api/auth/register-device",    drogon::Post, "MerchantAuthFilter"); // 注册设备
        ADD_METHOD_TO(MerchantDeviceKeyCtrl::listDevices,     "/merchant/api/auth/devices",            drogon::Get,  "MerchantAuthFilter"); // 设备列表
        ADD_METHOD_TO(MerchantDeviceKeyCtrl::resetDevice,     "/merchant/api/auth/device/{1}/reset",   drogon::Put,  "MerchantAuthFilter"); // 重置设备
        ADD_METHOD_TO(MerchantDeviceKeyCtrl::revokeDevice,    "/merchant/api/auth/device/{1}",         drogon::Delete, "MerchantAuthFilter"); // 撤销设备
    METHOD_LIST_END // 路由列表结束

    // 获取登录挑战值方法（公开接口，但有频率限制）
    void getChallenge(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; } // 请求体不是 JSON

        std::string publicKey = (*body).get("public_key", "").asString(); // 获取公钥
        if (publicKey.empty()) { RESP_ERR(cb, "public_key 必填"); return; } // 公钥为空

        std::string clientIp = req->getPeerAddr().toIp(); // 获取客户端 IP

        // 防止挑战值请求 DoS：同一 IP 每分钟最多 10 次
        static std::unordered_map<std::string, std::pair<int, long long>> challengeRateLimit; // 频率限制缓存
        static std::mutex rateLimitMutex; // 互斥锁
        {
            std::lock_guard<std::mutex> lock(rateLimitMutex); // 加锁
            long long now = std::time(nullptr); // 获取当前时间戳
            auto &limit = challengeRateLimit[clientIp]; // 获取该 IP 的限制
            if (now - limit.second > 60) { // 如果超过 60 秒
                limit = {1, now}; // 重置计数
            } else { // 否则
                limit.first++; // 增加计数
                if (limit.first > 10) { // 如果超过 10 次
                    RESP_ERR(cb, "请求过于频繁，请稍后再试"); return; // 返回错误
                }
            }
        }

        auto &db = PayDb::instance(); // 获取数据库单例

        // 验证公钥是否已注册
        auto row = db.queryOne( // 查询设备
            "SELECT user_id,username,state FROM device_keys WHERE public_key=? AND user_type=2", // 按公钥查询
            {publicKey}); // 公钥参数
        if (row.empty()) { RESP_ERR(cb, "设备未注册"); return; } // 设备未注册
        if (row["state"] != "1") { RESP_ERR(cb, "设备已被禁用"); return; } // 设备已禁用

        // 检查用户账号状态
        auto mch = db.queryOne("SELECT state FROM merchant WHERE id=?", {row["user_id"]}); // 查询商户
        if (mch.empty() || mch["state"] != "1") { // 如果商户不存在或已禁用
            RESP_ERR(cb, "账号已被禁用"); return; // 返回错误
        }

        // 生成挑战值
        std::string challenge = DeviceKeyUtils::generateChallenge(32); // 生成 32 字节挑战值
        long long timestamp = std::time(nullptr); // 获取当前时间戳

        // 存入内存缓存（5分钟有效）
        DeviceChallengeCache::instance().set(publicKey, challenge, timestamp); // 缓存挑战值

        Json::Value data; // 响应数据
        data["challenge"] = challenge; // 挑战值
        data["timestamp"] = (Json::Int64)timestamp; // 时间戳
        data["expires_in"] = 300;  // 5分钟过期时间
        RESP_OK(cb, data); // 返回成功响应
    }

    // 设备密钥签名登录方法
    void loginDevice(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
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
            "SELECT id,user_id,username,key_type,state,require_ip_verify,trusted_ips FROM device_keys WHERE public_key=? AND user_type=2",
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

        // 查询商户信息
        std::string userId = device["user_id"];
        auto mch = db.queryOne(
            "SELECT id,mch_no,mch_name,state FROM merchant WHERE id=?",
            {userId});
        if (mch.empty()) {
            LoginAttemptService::record(username, clientIp, ua, false, "商户不存在");
            RESP_ERR(cb, "商户不存在"); return;
        }
        if (mch["state"] != "1") {
            LoginAttemptService::record(username, clientIp, ua, false, "账号已被禁用");
            RESP_ERR(cb, "账号已被禁用"); return;
        }

        // 登录成功：记录尝试
        LoginAttemptService::record(username, clientIp, ua, true);

        // 更新设备最后使用时间和IP
        db.exec("UPDATE device_keys SET last_used_at=?,last_used_ip=?,updated_at=? WHERE id=?",
                {std::to_string(now), clientIp, std::to_string(now), device["id"]});

        // 记录操作日志
        OplogService::log(2, username, OplogService::MOD_AUTH, "login_device",
                          "设备密钥登录 (key_type=" + keyType + ")", "", clientIp);

        // 签发 JWT token
        std::string sub = "mch:" + userId + ":" + username;
        std::string token = SimpleJwt::sign(sub);

        Json::Value data;
        data["token"]    = token;
        data["mch_no"]   = mch["mch_no"];
        data["mch_name"] = mch["mch_name"];
        data["username"] = username;
        data["login_method"] = "device_key";
        data["key_type"] = keyType;
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
            "SELECT COUNT(*) as cnt FROM device_keys WHERE user_type=2 AND user_id=?",
            {mchId});
        int deviceCount = 0;
        try { deviceCount = std::stoi(count["cnt"]); } catch (...) {}
        if (deviceCount >= 10) {
            RESP_ERR(cb, "每个账号最多注册10个设备，请先删除不用的设备"); return;
        }

        long long now = std::time(nullptr);
        std::string clientIp = req->getPeerAddr().toIp();

        db.exec("INSERT INTO device_keys(user_type,user_id,username,key_type,public_key,device_name,"
                "device_info,last_used_at,last_used_ip,state,created_at,updated_at) "
                "VALUES(2,?,?,?,?,?,?,0,'',1,?,?)",
                {mchId, username, keyType, publicKey, deviceName, deviceInfo,
                 std::to_string(now), std::to_string(now)});

        // 记录操作日志
        OplogService::log(2, username, OplogService::MOD_AUTH, "register_device",
                          deviceName + " (" + keyType + ")", "", clientIp);

        Json::Value data;
        data["public_key"] = publicKey;
        data["device_name"] = deviceName;
        data["key_type"] = keyType;
        RESP_JSON(cb, AjaxResult::success("设备注册成功", data));
    }

    // 查看已注册设备列表方法（隐藏完整公钥）
    void listDevices(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id");

        auto &db = PayDb::instance();
        auto rows = db.query(
            "SELECT id,key_type,public_key,device_name,device_info,last_used_at,last_used_ip,state,created_at "
            "FROM device_keys WHERE user_type=2 AND user_id=? ORDER BY created_at DESC",
            {mchId});

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

    // 重置设备密钥方法（用户提供新公钥）
    void resetDevice(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb, // 响应回调函数
                     const std::string &deviceId) { // 设备 ID
        std::string mchId = req->getHeader("X-Mch-Id");
        std::string username = req->getHeader("X-Mch-Username");

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

        // 验证设备是否属于当前商户
        auto device = db.queryOne(
            "SELECT id,public_key,device_name FROM device_keys WHERE id=? AND user_type=2 AND user_id=?",
            {deviceId, mchId});
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

        // 记录操作日志
        std::string clientIp = req->getPeerAddr().toIp();
        OplogService::log(2, username, OplogService::MOD_AUTH, "reset_device",
                          device["device_name"] + " → " + keyType, "", clientIp);

        Json::Value data;
        data["new_public_key"] = newPublicKey;
        data["key_type"] = keyType;
        RESP_JSON(cb, AjaxResult::success("密钥重置成功", data));
    }

    // 撤销设备方法
    void revokeDevice(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb, // 响应回调函数
                      const std::string &deviceId) { // 设备 ID
        std::string mchId = req->getHeader("X-Mch-Id");
        std::string username = req->getHeader("X-Mch-Username");

        auto &db = PayDb::instance();

        // 验证设备是否属于当前商户
        auto device = db.queryOne(
            "SELECT id,public_key,device_name FROM device_keys WHERE id=? AND user_type=2 AND user_id=?",
            {deviceId, mchId});
        if (device.empty()) { RESP_ERR(cb, "设备不存在或无权操作"); return; }

        // 清除挑战值缓存
        DeviceChallengeCache::instance().remove(device["public_key"]);

        // 删除设备
        db.exec("DELETE FROM device_keys WHERE id=?", {deviceId});

        // 记录操作日志
        std::string clientIp = req->getPeerAddr().toIp();
        OplogService::log(2, username, OplogService::MOD_AUTH, "revoke_device",
                          device["device_name"], "", clientIp);

        RESP_MSG(cb, "设备已撤销");
    }
};
