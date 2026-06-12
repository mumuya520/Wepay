// WePay-Cpp — HTTPS 重定向
// =============================================================
// HttpsRedirect.h — 可选的 HTTP→HTTPS 强制跳转
// HTTPS 监听本身由 Drogon 原生 listeners 配置处理（config.json:
//   "listeners": [{ "address":"0.0.0.0","port":443,"https":true,
//                   "cert":"ssl/server.crt","key":"ssl/server.key" }]）。
// 本模块仅在 config.json 顶层 "https" 段开启 force_redirect 时，
// 把非 HTTPS 端口的请求 301 跳转到 HTTPS 端口。
//
//   "https": {
//     "force_redirect": false,   // 是否强制 HTTP→HTTPS
//     "https_port": 443          // 跳转目标端口
//   }
//
// 用法（main.cc loadConfigFile 之后）：wepay::HttpsRedirect::setup(cfg);
// =============================================================
#pragma once

#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include <string>

namespace wepay {

class HttpsRedirect {
public:
    static void setup(const Json::Value& cfg) {
        if (!cfg.isMember("https")) return;
        const auto& h = cfg["https"];
        if (!h.get("force_redirect", false).asBool()) return;
        int httpsPort = h.get("https_port", 443).asInt();

        drogon::app().registerPreRoutingAdvice(
            [httpsPort](const drogon::HttpRequestPtr& req,
                        drogon::AdviceCallback&& acb,
                        drogon::AdviceChainCallback&& accb) {
                // 已是 HTTPS 端口 → 放行
                if (req->localAddr().toPort() == (uint16_t)httpsPort) { accb(); return; }
                // /health 豁免（负载均衡 HTTP 探测）
                if (std::string(req->path()) == "/health") { accb(); return; }
                // 从 Host 头取主机名（去端口）
                std::string host = req->getHeader("Host");
                auto colon = host.rfind(':');
                if (colon != std::string::npos) host = host.substr(0, colon);
                if (host.empty()) host = req->localAddr().toIp();
                std::string location = "https://" + host;
                if (httpsPort != 443) location += ":" + std::to_string(httpsPort);
                location += std::string(req->path());
                if (!std::string(req->query()).empty())
                    location += "?" + std::string(req->query());
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k301MovedPermanently);
                resp->addHeader("Location", location);
                acb(resp);
            });
        LOG_INFO << "[HTTPS] HTTP→HTTPS 强制跳转已启用, https_port=" << httpsPort;
    }
};

}  // namespace wepay
