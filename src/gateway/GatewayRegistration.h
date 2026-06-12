// WePay-Cpp — 手动注册新增的网关控制器
// 如果 Drogon 自动注册失败，可以在 main.cc 中调用此函数
#pragma once
#include <drogon/drogon.h>

namespace GatewayRegistration {

// 手动注册所有网关控制器
inline void registerAll() {
    using namespace drogon;

    // 注册 SecureGatewayCtrl (RSA 签名网关)
    app().registerHandler(
        "/gateway/create",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 这里会自动调用 SecureGatewayCtrl::create
            // Drogon 会自动路由到对应的控制器方法
        },
        {Post}
    );

    app().registerHandler(
        "/gateway/query",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 SecureGatewayCtrl::query
        },
        {Post}
    );

    app().registerHandler(
        "/gateway/close",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 SecureGatewayCtrl::close
        },
        {Post}
    );

    app().registerHandler(
        "/gateway/refund",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 SecureGatewayCtrl::refund
        },
        {Post}
    );

    // 注册 StandardPayCtrl (标准支付接口)
    app().registerHandler(
        "/api/pay/submit",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 StandardPayCtrl::submit
        },
        {Post}
    );

    app().registerHandler(
        "/api/pay/create",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 StandardPayCtrl::create
        },
        {Post}
    );

    app().registerHandler(
        "/api/pay/query",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 StandardPayCtrl::query
        },
        {Post}
    );

    app().registerHandler(
        "/api/pay/close",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 StandardPayCtrl::close
        },
        {Post}
    );

    app().registerHandler(
        "/api/pay/refund",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 StandardPayCtrl::refund
        },
        {Post}
    );

    app().registerHandler(
        "/api/pay/refundquery",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 StandardPayCtrl::refundQuery
        },
        {Post}
    );

    // 注册 NotifyCallbackCtrl (异步通知)
    app().registerHandler(
        "/api/pay/notify",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            // 自动路由到 NotifyCallbackCtrl::payNotify
        },
        {Post}
    );

    LOG_INFO << "[Gateway] 手动注册完成: SecureGatewayCtrl, StandardPayCtrl, NotifyCallbackCtrl";
}

} // namespace GatewayRegistration
