// WePay-Cpp — 安全响应头中间件
// 为所有响应添加安全头，防止 XSS/点击劫持/MIME嗅探等攻击
#pragma once // 防止头文件重复包含
#include <drogon/HttpFilter.h> // Drogon HTTP 过滤器

// 安全响应头过滤器类
class SecurityHeaderFilter : public drogon::HttpFilter<SecurityHeaderFilter> {
public:
    // 过滤方法
    void doFilter(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                  drogon::FilterCallback &&fcb, // 过滤器回调
                  drogon::FilterChainCallback &&fccb) override { // 过滤器链回调
        // 直接放行，安全头通过 PostHandlingAdvice 统一添加
        fccb(); // 继续过滤器链
    }

    // 在 main.cc 中通过 registerPostHandlingAdvice 调用此方法
    // 添加安全响应头方法
    static void addSecurityHeaders(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                                    const drogon::HttpResponsePtr &resp) { // HTTP 响应对象
        // 防止 XSS
        resp->addHeader("X-Content-Type-Options", "nosniff"); // 防止 MIME 嗅探
        resp->addHeader("X-XSS-Protection", "1; mode=block"); // XSS 防护

        // 防止点击劫持
        resp->addHeader("X-Frame-Options", "SAMEORIGIN"); // 只允许同源 iframe

        // 引用策略
        resp->addHeader("Referrer-Policy", "strict-origin-when-cross-origin"); // 引用策略

        // CSP (Content Security Policy) — 收银台页面需要内联脚本
        resp->addHeader("Content-Security-Policy", // 内容安全策略
            "default-src 'self'; script-src 'self' 'unsafe-inline'; " // 默认源和脚本源
            "style-src 'self' 'unsafe-inline'; img-src 'self' data:; " // 样式和图片源
            "connect-src 'self'"); // 连接源

        // 缓存控制(API 接口不缓存)
        std::string path = req->getPath(); // 获取请求路径
        if (path.find("/api/") != std::string::npos || // 如果是 API 路由
            path.find("/gateway/") != std::string::npos) { // 或网关路由
            resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate"); // 禁用缓存
            resp->addHeader("Pragma", "no-cache"); // 禁用缓存
        }

        // 隐藏服务器信息
        resp->addHeader("Server", "WePay"); // 设置服务器信息
    }
};
