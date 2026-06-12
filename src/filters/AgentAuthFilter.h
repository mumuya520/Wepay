// WePay-Cpp — 代理后台 JWT 鉴权中间件
// 保护 /agent/api/** 路由，token sub 格式: "agent:<id>:<username>"
#pragma once // 防止头文件重复包含
#include <drogon/HttpMiddleware.h> // Drogon HTTP 中间件
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/SimpleJwt.h" // JWT 令牌处理

// 代理认证过滤器类
class AgentAuthFilter : public drogon::HttpMiddleware<AgentAuthFilter> {
public:
    static constexpr bool isAutoCreation = false; // 禁用自动创建

    // 中间件调用方法
    void invoke(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                drogon::MiddlewareNextCallback &&nextCb, // 下一个中间件回调
                drogon::MiddlewareCallback &&mcb) override { // 中间件完成回调
        std::string auth = req->getHeader("Authorization"); // 获取 Authorization 头
        if (auth.empty()) auth = req->getHeader("authorization"); // 尝试小写版本
        if (auth.empty()) { reject(mcb, "未授权"); return; } // 认证头为空

        try {
            std::string token = SimpleJwt::fromHeader(auth); // 从头部提取 token
            std::string sub = SimpleJwt::verify(token); // 验证 token 并获取 sub
            if (sub.rfind("agent:", 0) != 0) { reject(mcb, "Token 类型错误"); return; } // 检查 token 类型
            auto p1 = sub.find(':', 6); // 查找冒号分隔符
            if (p1 == std::string::npos) { reject(mcb, "Token 格式错误"); return; } // 格式错误
            std::string idStr = sub.substr(6, p1 - 6); // 提取代理 ID
            std::string user  = sub.substr(p1 + 1); // 提取用户名
            req->addHeader("X-Agent-Id",   idStr); // 添加代理 ID 头
            req->addHeader("X-Agent-User", user); // 添加用户名头
            nextCb(std::move(mcb)); // 继续下一个中间件
        } catch (...) { // 捕获所有异常
            reject(mcb, "Token 无效或已过期"); // 返回错误响应
        }
    }

private:
    // 拒绝请求方法
    static void reject(drogon::MiddlewareCallback &mcb, const char *msg) { // 中间件回调和错误消息
        auto r = drogon::HttpResponse::newHttpJsonResponse(AjaxResult::error(401, msg)); // 创建 JSON 响应
        r->setStatusCode(drogon::k401Unauthorized); // 设置状态码为 401
        mcb(r); // 返回响应
    }
};
