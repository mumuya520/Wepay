// WePay-Cpp — 管理后台: 认证控制器
// 职责：管理员登录、登出、Token 刷新、设备密钥认证等
//
// API 端点：
// POST /admin/api/auth/login              登录，返回 access + refresh token
// POST /admin/api/auth/device-key-login   设备密钥登录
// POST /admin/api/auth/refresh            用 refresh token 换新 access token
// POST /admin/api/auth/logout             登出，撤销 token
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/SimpleJwt.h" // JWT 令牌
#include "../common/TokenService.h" // Token 服务
#include "../common/PasswordUtils.h" // 密码工具
#include "../common/RbacService.h" // 基于角色的访问控制
#include "../common/OplogService.h" // 操作日志服务
#include "../common/LoginAttemptService.h" // 登录尝试服务
#include "../common/DeviceKeyUtils.h" // 设备密钥工具

// 管理员登录控制器类，继承自 Drogon HTTP 控制器
class AdminLoginCtrl : public drogon::HttpController<AdminLoginCtrl> {
public:
    // 开始注册 HTTP 方法列表
    METHOD_LIST_BEGIN
        // 注册登录路由，POST 方法，转发到 login() 处理
        ADD_METHOD_TO(AdminLoginCtrl::login,          "/admin/api/auth/login",            drogon::Post);
        // 注册设备密钥登录路由，POST 方法，转发到 deviceKeyLogin() 处理
        ADD_METHOD_TO(AdminLoginCtrl::deviceKeyLogin, "/admin/api/auth/device-key-login", drogon::Post);
        // 注册 Token 刷新路由，POST 方法，转发到 refresh() 处理
        ADD_METHOD_TO(AdminLoginCtrl::refresh,        "/admin/api/auth/refresh",          drogon::Post);
        // 注册登出路由，POST 方法，转发到 logout() 处理
        ADD_METHOD_TO(AdminLoginCtrl::logout,         "/admin/api/auth/logout",           drogon::Post);
        // 旧路由兼容：注册旧登录路由
        ADD_METHOD_TO(AdminLoginCtrl::login,   "/api/auth/login",   drogon::Post);
        // 旧路由兼容：注册旧登出路由
        ADD_METHOD_TO(AdminLoginCtrl::logout,  "/api/auth/logout",  drogon::Post);
    // 结束方法列表注册
    METHOD_LIST_END

    // 用户名密码登录处理函数
    // 参数 req：HTTP 请求对象指针
    // 参数 cb：异步回调函数，用于返回 HTTP 响应
    void login(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // 获取请求体的 JSON 对象
        auto body = req->getJsonObject();
        // 初始化用户名和密码变量
        std::string username, password;
        // 如果请求体存在
        if (body) {
            // 从 JSON 中获取 username 字段，默认为空字符串
            username = (*body).get("username", "").asString();
            // 从 JSON 中获取 password 字段，默认为空字符串
            password = (*body).get("password", "").asString();
        } else {
            // 否则从 URL 参数中获取 username
            username = req->getParameter("username");
            // 从 URL 参数中获取 password
            password = req->getParameter("password");
        }
        // 检查用户名和密码是否为空
        if (username.empty() || password.empty()) {
            // 返回错误响应：用户名和密码不能为空
            RESP_ERR(cb, "用户名和密码不能为空");
            // 函数返回
            return;
        }

        // 获取客户端 IP 地址
        std::string clientIp = req->getPeerAddr().toIp();
        // 获取请求头中的 User-Agent
        std::string ua = req->getHeader("User-Agent");

        // 防爆破：检查账号是否已被临时锁定
        int lockedSec = LoginAttemptService::isLocked(username, clientIp);
        // 如果账号被锁定
        if (lockedSec > 0) {
            // 返回错误响应，告知剩余锁定时间
            RESP_ERR(cb, "账号已被临时锁定，剩余 " + std::to_string(lockedSec / 60 + 1) + " 分钟，请稍后再试");
            // 函数返回
            return;
        }

        // 获取数据库单例实例
        auto &db = PayDb::instance();

        // 优先查询 sys_user 表中的用户信息
        auto u = db.queryOne(
            "SELECT id,username,password,salt,state,is_super FROM sys_user WHERE username=?",
            {username});

        // 初始化密码验证标志
        bool ok = false;
        // 初始化用户 ID
        int userId = 0;
        // 初始化超级管理员标志
        bool isSuper = false;

        // 如果查询到用户
        if (!u.empty()) {
            // 检查用户状态是否为启用（1 表示启用）
            if (u["state"] != "1") {
                // 记录登录失败尝试
                LoginAttemptService::record(username, clientIp, ua, false, "账号已禁用");
                // 返回错误响应：账号已被禁用
                RESP_ERR(cb, "账号已被禁用");
                // 函数返回
                return;
            }
            // 尝试将用户 ID 转换为整数
            try { userId = std::stoi(u["id"]); } catch (...) {}
            // 检查是否为超级管理员（1 表示是）
            isSuper = (u["is_super"] == "1");

            // 密码验证：检查是否使用了加盐哈希
            if (!u["salt"].empty()) {
                // 使用加盐哈希验证密码
                ok = PasswordUtils::verify(password, u["salt"], u["password"]);
            } else {
                // 兼容旧明文密码：首次登录成功后自动升级为哈希
                if (password == u["password"]) {
                    // 设置验证成功标志
                    ok = true;
                    // 生成新的随机盐
                    std::string salt = PasswordUtils::generateSalt();
                    // 使用盐对密码进行哈希
                    std::string hash = PasswordUtils::hashPassword(password, salt);
                    // 更新数据库中的密码和盐
                    db.exec("UPDATE sys_user SET password=?,salt=? WHERE id=?",
                            {hash, salt, u["id"]});
                }
            }
        } else {
            // 兼容老版本的 setting 表中的 admin_user/admin_pass
            std::string storedUser = db.getSetting("admin_user", db.getSetting("user", "admin"));
            std::string storedPass = db.getSetting("admin_pass", db.getSetting("pass", "admin"));
            // 检查用户名和密码是否匹配
            if (username == storedUser && password == storedPass) {
                // 设置验证成功标志
                ok = true;
                // 标记为超级管理员
                isSuper = true;
            }
        }

        // 如果密码验证失败
        if (!ok) {
            // 记录登录失败尝试
            LoginAttemptService::record(username, clientIp, ua, false, "用户名或密码错误");
            // 返回错误响应：用户名或密码错误
            RESP_ERR(cb, "用户名或密码错误");
            // 函数返回
            return;
        }

        // 登录成功：记录登录尝试 + 更新最后登录时间
        LoginAttemptService::record(username, clientIp, ua, true);
        // 如果用户 ID 有效
        if (userId > 0) {
            // 更新用户的最后登录 IP 和时间
            db.exec("UPDATE sys_user SET last_login_ip=?, last_login_at=? WHERE id=?",
                    {clientIp,
                     std::to_string(std::time(nullptr)),
                     std::to_string(userId)});
        }

        // 调用 TokenService 生成 access token 和 refresh token
        auto pair = TokenService::issue(1, userId, username);

        // 创建响应数据 JSON 对象
        Json::Value data;
        // 设置 access token
        data["token"]         = pair.accessToken;
        // 设置 refresh token
        data["refresh_token"] = pair.refreshToken;
        // 设置 token 过期时间戳
        data["expires_at"]    = (Json::Int64)pair.accessExpires;
        // 设置用户名
        data["username"]      = username;
        // 设置是否为超级管理员（1 或 0）
        data["is_super"]      = isSuper ? 1 : 0;

        // 返回权限列表（前端用于按钮/菜单过滤）
        Json::Value permArr(Json::arrayValue);
        // 如果是超级管理员
        if (isSuper)
            // 添加通配符权限表示拥有所有权限
            permArr.append("*");
        // 否则如果用户 ID 有效
        else if (userId > 0) {
            // 加载用户的权限列表
            for (auto &p : RbacService::loadUserPermissions(userId))
                // 将每个权限添加到数组
                permArr.append(p);
        }
        // 设置权限数组
        data["permissions"] = permArr;

        // 记录登录操作日志
        OplogService::log(1, username, OplogService::MOD_AUTH, "login", username, "",
                          req->getPeerAddr().toIp());
        // 返回成功响应，包含登录数据
        RESP_JSON(cb, AjaxResult::success("登录成功", data));
    }

    // 设备密钥登录
    void deviceKeyLogin(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &j = *body;

        std::string username  = j.get("username", "").asString();
        int deviceId          = j.get("device_id", 0).asInt();
        std::string challenge = j.get("challenge", "").asString();
        std::string signature = j.get("signature", "").asString();

        if (username.empty() || deviceId <= 0 || challenge.empty() || signature.empty()) {
            RESP_ERR(cb, "参数不完整"); return;
        }

        std::string clientIp = req->getPeerAddr().toIp();
        std::string ua = req->getHeader("User-Agent");

        auto &db = PayDb::instance();

        // 1. 查询管理员信息
        auto u = db.queryOne(
            "SELECT id,username,state,is_super FROM sys_user WHERE username=?",
            {username});

        if (u.empty()) { RESP_ERR(cb, "用户不存在"); return; }
        if (u["state"] != "1") { RESP_ERR(cb, "账号已被禁用"); return; }

        int userId = 0;
        bool isSuper = false;
        try { userId = std::stoi(u["id"]); } catch (...) {}
        isSuper = (u["is_super"] == "1");

        // 2. 查询设备密钥
        auto device = db.queryOne(
            "SELECT id,public_key,key_type,state,last_used_at FROM device_keys "
            "WHERE id=? AND user_id=? AND user_type=1",
            {std::to_string(deviceId), std::to_string(userId)});

        if (device.empty()) { RESP_ERR(cb, "设备不存在"); return; }
        if (device["state"] != "1") { RESP_ERR(cb, "设备已被撤销"); return; }

        // 3. 验证签名
        std::string publicKey = device["public_key"];
        std::string keyType   = device["key_type"];

        DeviceKeyUtils::KeyType kt;
        if (keyType == "ed25519") kt = DeviceKeyUtils::KeyType::ED25519;
        else if (keyType == "rsa2048") kt = DeviceKeyUtils::KeyType::RSA_2048;
        else if (keyType == "rsa4096") kt = DeviceKeyUtils::KeyType::RSA_4096;
        else { RESP_ERR(cb, "不支持的密钥类型"); return; }

        bool verified = DeviceKeyUtils::verifySignature(challenge, signature, publicKey, kt);
        if (!verified) { RESP_ERR(cb, "签名验证失败"); return; }

        // 4. 验证挑战值格式: login:<username>:<device_id>:<timestamp>
        std::string expectedPrefix = "login:" + username + ":" + std::to_string(deviceId) + ":";
        if (challenge.substr(0, expectedPrefix.size()) != expectedPrefix) {
            RESP_ERR(cb, "挑战值格式错误"); return;
        }

        // 5. 验证时间戳（防止重放攻击，5分钟内有效）
        try {
            size_t pos = challenge.rfind(':');
            if (pos == std::string::npos) { RESP_ERR(cb, "挑战值格式错误"); return; }
            long long timestamp = std::stoll(challenge.substr(pos + 1));
            long long now = std::time(nullptr) * 1000;
            if (std::abs(now - timestamp) > 300000) { // 5分钟
                RESP_ERR(cb, "挑战值已过期"); return;
            }
        } catch (...) {
            RESP_ERR(cb, "挑战值时间戳无效"); return;
        }

        // 6. 更新设备最后使用时间
        db.exec("UPDATE device_keys SET last_used_at=? WHERE id=?",
                {std::to_string(std::time(nullptr)), std::to_string(deviceId)});

        // 7. 更新管理员最后登录时间
        db.exec("UPDATE sys_user SET last_login_ip=?, last_login_at=? WHERE id=?",
                {clientIp, std::to_string(std::time(nullptr)), std::to_string(userId)});

        // 8. 生成 token
        auto pair = TokenService::issue(1, userId, username);

        Json::Value data;
        data["token"]         = pair.accessToken;
        data["refresh_token"] = pair.refreshToken;
        data["expires_at"]    = (Json::Int64)pair.accessExpires;
        data["username"]      = username;
        data["is_super"]      = isSuper ? 1 : 0;

        // 返回权限列表
        Json::Value permArr(Json::arrayValue);
        if (isSuper) permArr.append("*");
        else if (userId > 0) {
            for (auto &p : RbacService::loadUserPermissions(userId)) permArr.append(p);
        }
        data["permissions"] = permArr;

        OplogService::log(1, username, OplogService::MOD_AUTH, "device_key_login", username, "",
                          clientIp);
        RESP_JSON(cb, AjaxResult::success("登录成功", data));
    }

    void refresh(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string rt = (*body).get("refresh_token", "").asString();
        if (rt.empty()) { RESP_ERR(cb, "refresh_token 必填"); return; }

        // 通过 refresh 换新，但需要原 subject (username)
        // 从 jwt_refresh_token 查询 user_id → sys_user.username
        auto row = PayDb::instance().queryOne(
            "SELECT u.username FROM jwt_refresh_token rt "
            "LEFT JOIN sys_user u ON u.id=rt.user_id "
            "WHERE rt.refresh_token=? AND rt.user_type=1",
            {rt});
        if (row.empty()) { RESP_ERR(cb, "refresh_token 无效"); return; }

        auto r = TokenService::refresh(rt, row["username"]);
        if (!r.success) { RESP_ERR(cb, r.errMsg); return; }

        Json::Value data;
        data["token"]         = r.pair.accessToken;
        data["refresh_token"] = r.pair.refreshToken;
        data["expires_at"]    = (Json::Int64)r.pair.accessExpires;
        RESP_OK(cb, data);
    }

    void logout(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string auth = req->getHeader("Authorization");
        std::string tk;
        if (auth.size() > 7 && auth.substr(0, 7) == "Bearer ") tk = auth.substr(7);
        else tk = auth;

        auto body = req->getJsonObject();
        std::string rt;
        if (body) rt = (*body).get("refresh_token", "").asString();

        if (!tk.empty()) TokenService::revoke(tk, rt);
        RESP_MSG(cb, "退出成功");
    }
};
