#pragma once // 防止头文件重复包含
#include <drogon/HttpMiddleware.h> // Drogon HTTP 中间件
#include <openssl/sha.h> // SHA256 哈希
#include <sstream> // 字符串流
#include <iomanip> // 输入输出格式化
#include "../common/SimpleJwt.h" // JWT 令牌处理
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/RbacService.h" // RBAC 服务
#include "../common/PayDb.h" // 数据库操作
#include "../common/DeviceKeyEnforcement.h" // 设备密钥强制检查

// JWT 认证中间件 — 保护 /admin/api/** 路由
// 验证 token + 黑名单检查 + 加载 sys_user 身份 + 强制设备密钥绑定
// 写入请求属性供下游使用:
//   X-Admin-User  : 用户名
//   X-Admin-Id    : sys_user.id (字符串)
//   X-Admin-Super : "1" or "0"
//   X-User-Id     : sys_user.id (字符串，兼容旧代码)
//   X-Username    : 用户名（兼容旧代码）
class AdminAuthFilter : public drogon::HttpMiddleware<AdminAuthFilter> { // 管理员认证过滤器
public:
    static constexpr bool isAutoCreation = false; // 禁用自动创建

    // 中间件调用方法
    void invoke(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                drogon::MiddlewareNextCallback &&nextCb, // 下一个中间件回调
                drogon::MiddlewareCallback &&mcb) override { // 中间件完成回调
        std::string auth = req->getHeader("Authorization"); // 获取 Authorization 头
        if (auth.empty()) auth = req->getHeader("authorization"); // 尝试小写版本

        if (auth.empty()) { reject(mcb, "未授权，请先登录"); return; } // 认证头为空

        try {
            std::string token = SimpleJwt::fromHeader(auth); // 从头部提取 token

            // 黑名单检查
            std::string th = sha256Hex(token); // 计算 token 哈希
            auto bl = PayDb::instance().queryOne( // 查询黑名单
                "SELECT 1 FROM jwt_blacklist WHERE token_hash=?", {th}); // 按哈希查询
            if (!bl.empty()) { reject(mcb, "Token 已失效"); return; } // Token 在黑名单中

            std::string user = SimpleJwt::verify(token); // 验证 token 并获取用户名

            // 加载 sys_user 身份(若不存在则回退到旧版 setting.admin_user 模式)
            auto info = RbacService::loadUser(user); // 加载用户信息
            int userId = 0; // 用户 ID
            if (info.id > 0) { // 如果用户存在
                if (info.state != 1) { reject(mcb, "账号已被禁用"); return; } // 账号已禁用
                userId = info.id; // 获取用户 ID
                req->addHeader("X-Admin-Id",    std::to_string(info.id)); // 添加用户 ID 头
                req->addHeader("X-Admin-Super", info.isSuper ? "1" : "0"); // 添加超管标志头
            } else { // 否则使用旧版模式
                req->addHeader("X-Admin-Id",    "0"); // 用户 ID 为 0
                req->addHeader("X-Admin-Super", "1");  // 老 admin 视为超管
            }
            req->addHeader("X-Admin-User", user); // 添加用户名头
            req->addHeader("X-User-Id", std::to_string(userId));  // 兼容旧代码
            req->addHeader("X-Username", user);                   // 兼容旧代码

            // 强制设备密钥绑定检查
            std::string path = req->getPath(); // 获取请求路径
            if (DeviceKeyEnforcement::requiresDeviceKeySetup(1, userId, path)) { // 检查是否需要设备密钥
                rejectDeviceKeyRequired(mcb); // 拒绝请求，要求绑定设备密钥
                return;
            }

            nextCb(std::move(mcb)); // 继续下一个中间件
        } catch (const std::exception &e) { // 捕获异常
            LOG_ERROR << "[AdminAuthFilter] " << e.what(); // 记录错误
            reject(mcb, "Token 无效或已过期，请重新登录"); // 返回错误响应
        } catch (...) { // 捕获所有异常
            LOG_ERROR << "[AdminAuthFilter] unknown exception"; // 记录未知错误
            reject(mcb, "Token 无效或已过期，请重新登录"); // 返回错误响应
        }
    }

private:
    // 拒绝请求方法
    static void reject(drogon::MiddlewareCallback &mcb, const char *msg) { // 中间件回调和错误消息
        auto resp = drogon::HttpResponse::newHttpJsonResponse( // 创建 JSON 响应
            AjaxResult::error(401, msg)); // 401 错误响应
        resp->setStatusCode(drogon::k401Unauthorized); // 设置状态码为 401
        mcb(resp); // 返回响应
    }

    // 拒绝请求（需要设备密钥）方法
    static void rejectDeviceKeyRequired(drogon::MiddlewareCallback &mcb) { // 中间件回调
        Json::Value data; // 响应数据
        data["code"] = 403; // 错误代码
        data["msg"] = "您还未绑定设备密钥，请先完成设备绑定才能使用系统"; // 错误消息
        data["data"]["require_device_key_setup"] = true; // 需要设备密钥标志
        data["data"]["setup_url"] = "/admin/api/auth/register-device"; // 设备注册 URL

        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        resp->setStatusCode(drogon::k403Forbidden); // 设置状态码为 403
        mcb(resp); // 返回响应
    }

    // SHA256 哈希方法
    static std::string sha256Hex(const std::string &data) { // 输入数据
        unsigned char hash[SHA256_DIGEST_LENGTH]; // 哈希缓冲区
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash); // 计算 SHA256
        std::ostringstream oss; // 字符串流
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) // 遍历每个字节
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i]; // 转换为十六进制
        return oss.str(); // 返回十六进制字符串
    }
};
