// WePay-Cpp — PHP 支付代理
// PhpPayProxy.h — 将 /phppay/* 透明代理到 php-pay 项目 (PHP 内置服务器)
// 配置节：config.json["php_pay"]
// 用法：
//   1. php-pay 项目需要先启动内置服务器: php -S 127.0.0.1:61111 -t ./php-pay
//   2. 或配置独立的 php-cgi 端口
//   3. C++ 会将 /phppay/* 请求转发到 PHP 后端
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h>
#include <string>
#include <algorithm>

class PhpPayProxy {
public:
    // 初始化并注册反向代理路由
    static void setup(const Json::Value &cfg) {
        if (!cfg.isMember("php_pay")) return;
        const auto &c = cfg["php_pay"];
        if (!c.get("enabled", false).asBool()) return;

        std::string prefix  = c.get("prefix", "/phppay").asString();
        std::string backend = c.get("backend", "http://127.0.0.1:61111").asString();

        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        prefix_ = prefix;

        // 解析 backend URL
        parseBackend(backend, scheme_, host_, port_);

        LOG_INFO << "[PhpPayProxy] " << prefix_ << "/* → " << backend;

        // 注册预路由拦截
        const std::string scheme = scheme_;
        const std::string host   = host_;
        const int         bport  = port_;
        const std::string pfx   = prefix_;

        drogon::app().registerPreRoutingAdvice(
            [pfx, scheme, host, bport]
            (const drogon::HttpRequestPtr  &req,
             drogon::AdviceCallback        &&cb,
             drogon::AdviceChainCallback  &&ccb)
            {
                const std::string &path = req->path();
                // 匹配 /phppay 或 /phppay/...
                bool match = (path == pfx) ||
                             (path.size() > pfx.size() &&
                              path[pfx.size()] == '/' &&
                              path.rfind(pfx, 0) == 0);
                if (!match) { ccb(); return; }

                // 剥离前缀，保留路径
                std::string subPath = path.substr(pfx.size());
                if (subPath.empty()) subPath = "/";

                // 转发到 PHP 后端
                PhpPayProxy::forward(req, std::move(cb), scheme, host, bport, subPath);
            });

        LOG_INFO << "[PhpPayProxy] registered: " << prefix_ << "/* → "
                 << scheme_ << "://" << host_ << ":" << port_;
    }

    // 反向代理转发实现
    static void forward(const drogon::HttpRequestPtr &req,
                        drogon::AdviceCallback &&cb,
                        const std::string &scheme,
                        const std::string &host,
                        int bport,
                        const std::string &subPath)
    {
        std::string fullPath = subPath;
        if (fullPath.empty()) fullPath = "/";

        // 追加查询参数
        std::string qs = req->query();
        if (!qs.empty()) fullPath += "?" + qs;

        // 构造转发请求
        auto fwdReq = drogon::HttpRequest::newHttpRequest();
        fwdReq->setMethod(req->method());
        fwdReq->setPath(fullPath);

        // 复制请求头（跳过 Host）
        for (auto &[k, v] : req->headers()) {
            std::string lk = k;
            std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
            if (lk == "host") continue;
            fwdReq->addHeader(k, v);
        }

        // 添加 X-Forwarded-For 头（保留原始客户端 IP）
        std::string clientIp = req->getPeerAddr().toIp();
        fwdReq->addHeader("X-Forwarded-For", clientIp);
        fwdReq->addHeader("X-Real-IP", clientIp);

        // 复制请求体
        std::string body(req->body());
        if (!body.empty()) {
            fwdReq->setBody(std::move(body));
            std::string ct = req->getHeader("content-type");
            if (!ct.empty()) fwdReq->addHeader("Content-Type", ct);
        }

        // 创建 HTTP 客户端并转发
        auto client = drogon::HttpClient::newHttpClient(
            scheme + "://" + host + ":" + std::to_string(bport));

        client->sendRequest(fwdReq,
            [cb = std::move(cb)](drogon::ReqResult result,
                                 const drogon::HttpResponsePtr &resp) mutable
            {
                if (result == drogon::ReqResult::Ok && resp) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(resp->statusCode());

                    // 复制响应头（过滤代理相关头）
                    std::string upCT;
                    for (auto &kv : resp->headers()) {
                        std::string lk = kv.first;
                        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                        if (lk == "content-length"  ||
                            lk == "transfer-encoding" ||
                            lk == "connection"       ||
                            lk == "keep-alive"       ||
                            lk == "proxy-authenticate" ||
                            lk == "proxy-authorization" ||
                            lk == "te"               ||
                            lk == "trailers"         ||
                            lk == "upgrade") continue;
                        if (lk == "content-type") { upCT = kv.second; continue; }
                        r->addHeader(kv.first, kv.second);
                    }

                    // 复制响应体
                    std::string bd{resp->body()};
                    r->setBody(std::move(bd));

                    if (!upCT.empty()) r->setContentTypeString(upCT);

                    // 添加 CORS 头（允许跨域访问）
                    r->addHeader("Access-Control-Allow-Origin", "*");

                    cb(r);
                } else {
                    // 后端不可用
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(drogon::k502BadGateway);
                    r->setContentTypeCode(drogon::CT_TEXT_HTML);
                    r->setBody(R"(<!DOCTYPE html><html><body>
                        <h2>502 Bad Gateway</h2>
                        <p>PHP Pay Service is unavailable. Please ensure:</p>
                        <ul>
                            <li>PHP built-in server is running: <code>php -S 127.0.0.1:61111 -t ./php-pay</code></li>
                            <li>Or configure correct backend in config.json</li>
                        </ul>
                        </body></html>)");
                    cb(r);
                }
            });
    }

private:
    static inline std::string prefix_{"/phppay"};
    static inline std::string scheme_{"http"};
    static inline std::string host_{"127.0.0.1"};
    static inline int         port_{61111};

    static void parseBackend(const std::string &url,
                             std::string &scheme,
                             std::string &host,
                             int &port)
    {
        size_t p = url.find("://");
        if (p == std::string::npos) return;
        scheme = url.substr(0, p);
        std::string rest = url.substr(p + 3);

        size_t cp = rest.rfind(':');
        if (cp != std::string::npos) {
            host = rest.substr(0, cp);
            try { port = std::stoi(rest.substr(cp + 1)); } catch (...) {}
        } else {
            host = rest;
            port = (scheme == "https") ? 443 : 80;
        }
    }
};
