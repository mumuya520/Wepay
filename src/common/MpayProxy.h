// WePay-Cpp — MPay 代理
// MpayProxy.h — 将 /mpay/* 透明代理到 MPay v2 Webman (PHP/Workerman)
// 自动拉起 mpay-service.exe（负责管理 PHP 进程并自动重启）
// 配置节：config.json["mpay"]
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h> // JSON 库
#include <string> // 字符串库
#include <atomic> // 原子操作
#include <thread>
#include <algorithm>
#include "ProcessManager.h"

#ifdef _WIN32
#  include <windows.h>
#endif

class MpayProxy {
public:
    static void setup(const Json::Value &cfg) {
        if (!cfg.isMember("mpay")) return;
        const auto &c = cfg["mpay"];
        if (!c.get("enabled", false).asBool()) return;

        std::string prefix   = c.get("prefix",  "/mpay").asString();
        std::string backend  = c.get("backend", "http://127.0.0.1:8787").asString();
        std::string svcExe   = c.get("service_exe", "").asString();
        bool autostart       = c.get("autostart", true).asBool();
        bool watchdog        = c.get("watchdog",  true).asBool();
        int  interval        = c.get("watchdog_interval_sec", 10).asInt();
        bool console         = c.get("console", false).asBool();

        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        prefix_ = prefix;

        // 解析 backend
        parseBackend(backend, scheme_, host_, port_);

        LOG_INFO << "[MpayProxy] " << prefix << " → " << backend;

        // 自动拉起 mpay-service.exe
        if (autostart && !svcExe.empty()) {
            launch(svcExe, console);
            std::this_thread::sleep_for(std::chrono::seconds(4));
        }

        // 看门狗
        if (watchdog && autostart && !svcExe.empty()) {
            svcExe_ = svcExe;
            console_ = console;
            std::thread([interval]() {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::seconds(interval));
#ifdef _WIN32
                    if (!ProcessManager::isAlive(hProc_)) {
                        LOG_WARN << "[MpayProxy] mpay-service 已停止，正在重启...";
                        launch(svcExe_, console_);
                    }
#endif
                }
            }).detach();
        }

        // 注册反向代理路由
        // prefixes: /mpay（带前缀剥离）+ 直通路径（不剥离前缀）
        const std::string scheme  = scheme_;
        const std::string host    = host_;
        const int         bport   = port_;

        // /mpay/* → 剥掉 /mpay 后转发
        {
            const std::string pfx = prefix_;
            // SPA 入口：无尾斜杠时 301 → 加斜杠，确保相对路径解析正确
            static const std::vector<std::string> spaEntries = {
                "/admin", "/mer", "/cashier", "/payment", "/install", "/docs"
            };
            drogon::app().registerPreRoutingAdvice(
                [pfx, scheme, host, bport]
                (const drogon::HttpRequestPtr  &req,
                 drogon::AdviceCallback        &&cb,
                 drogon::AdviceChainCallback   &&ccb)
                {
                    const std::string &path = req->path();
                    bool match = (path == pfx) ||
                                 (path.size() > pfx.size() &&
                                  path[pfx.size()] == '/' &&
                                  path.rfind(pfx, 0) == 0);
                    if (!match) { ccb(); return; }

                    std::string subPath = path.substr(pfx.size());
                    if (subPath.empty()) subPath = "/";

                    // SPA 入口无尾斜杠 → 301 重定向
                    for (auto &entry : spaEntries) {
                        if (subPath == entry) {
                            auto r = drogon::HttpResponse::newHttpResponse();
                            r->setStatusCode(drogon::k301MovedPermanently);
                            r->addHeader("Location", pfx + entry + "/");
                            cb(r);
                            return;
                        }
                    }

                    MpayProxy::forward(req, std::move(cb), scheme, host, bport, subPath);
                });
        }

        // 直通路径：/adminapi /merapi /cashier /payment（不剥离前缀，原样转发）
        for (const std::string &pfx : {"/adminapi", "/merapi", "/cashier", "/payment"}) {
            drogon::app().registerPreRoutingAdvice(
                [pfx, scheme, host, bport]
                (const drogon::HttpRequestPtr  &req,
                 drogon::AdviceCallback        &&cb,
                 drogon::AdviceChainCallback   &&ccb)
                {
                    const std::string &path = req->path();
                    bool match = (path == pfx) ||
                                 (path.size() > pfx.size() &&
                                  path[pfx.size()] == '/' &&
                                  path.rfind(pfx, 0) == 0);
                    if (!match) { ccb(); return; }
                    MpayProxy::forward(req, std::move(cb), scheme, host, bport, path);
                });
            LOG_INFO << "[MpayProxy] " << pfx << "/* → " << scheme << "://" << host << ":" << bport;
        }

        LOG_INFO << "[MpayProxy] " << prefix_ << "/* → "
                 << scheme_ << "://" << host_ << ":" << port_ << " (strip prefix)";
    }

    // 内部转发实现（供多处复用）
    static void forward(const drogon::HttpRequestPtr &req,
                        drogon::AdviceCallback &&cb,
                        const std::string &scheme, const std::string &host, int bport,
                        const std::string &subPath)
    {
        std::string fullPath = subPath;
        if (fullPath.empty()) fullPath = "/";
        std::string qs = req->query();
        if (!qs.empty()) fullPath += "?" + qs;

        auto fwdReq = drogon::HttpRequest::newHttpRequest();
        fwdReq->setMethod(req->method());
        fwdReq->setPath(fullPath);
        for (auto &[k, v] : req->headers()) {
            std::string lk = k;
            std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
            if (lk == "host") continue;
            fwdReq->addHeader(k, v);
        }
        std::string body(req->body());
        if (!body.empty()) {
            fwdReq->setBody(std::move(body));
            std::string ct = req->getHeader("content-type");
            if (!ct.empty()) fwdReq->addHeader("Content-Type", ct);
        }

        auto client = drogon::HttpClient::newHttpClient(scheme + "://" + host + ":" + std::to_string(bport));
        client->sendRequest(fwdReq,
            [cb = std::move(cb)](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) mutable {
                if (result == drogon::ReqResult::Ok && resp) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(resp->statusCode());
                    std::string upCT;
                    for (auto &kv : resp->headers()) {
                        std::string lk = kv.first;
                        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                        if (lk == "content-length" || lk == "transfer-encoding" ||
                            lk == "connection"     || lk == "keep-alive" ||
                            lk == "proxy-authenticate" || lk == "proxy-authorization" ||
                            lk == "te" || lk == "trailers" || lk == "upgrade") continue;
                        if (lk == "content-type") { upCT = kv.second; continue; }
                        r->addHeader(kv.first, kv.second);
                    }
                    std::string bd{resp->body()};
                    r->setBody(std::move(bd));
                    if (!upCT.empty()) r->setContentTypeString(upCT);
                    cb(r);
                } else {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(drogon::k502BadGateway);
                    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    r->setBody(R"({"code":502,"msg":"MPay service unavailable"})");
                    cb(r);
                }
            });

    }

    // 优雅停止：WePay-Cpp 退出时调用，终止 mpay-service 子进程
    static void shutdown() {
#ifdef _WIN32
        if (hProc_ != INVALID_HANDLE_VALUE) {
            LOG_INFO << "[MpayProxy] stopping mpay-service...";
            DWORD pid = GetProcessId(hProc_);
            if (pid) GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
            if (WaitForSingleObject(hProc_, 5000) != WAIT_OBJECT_0) {
                TerminateProcess(hProc_, 0);
                LOG_WARN << "[MpayProxy] mpay-service force-killed";
            } else {
                LOG_INFO << "[MpayProxy] mpay-service stopped gracefully";
            }
            CloseHandle(hProc_);
            hProc_ = INVALID_HANDLE_VALUE;
        }
#else
        LOG_INFO << "[MpayProxy] mpay-service exited with parent";
#endif
    }

private:
    static inline std::string prefix_, scheme_{"http"}, host_{"127.0.0.1"}, svcExe_;
    static inline int         port_{8787};
    static inline bool        console_{false};
#ifdef _WIN32
    static inline HANDLE      hProc_{INVALID_HANDLE_VALUE};
#endif

    static void launch(const std::string &exe, bool console) {
#ifdef _WIN32
        if (hProc_ != INVALID_HANDLE_VALUE) {
            CloseHandle(hProc_);
            hProc_ = INVALID_HANDLE_VALUE;
        }
        hProc_ = ProcessManager::spawn(exe, "", "", console, "mpay-service");
        if (hProc_ == INVALID_HANDLE_VALUE)
            LOG_WARN << "[MpayProxy] 启动 mpay-service 失败: " << exe;
        else
            LOG_INFO << "[MpayProxy] mpay-service 已启动";
#else
        ProcessManager::spawn(exe, "", "", false, "mpay-service");
        LOG_INFO << "[MpayProxy] mpay-service 已启动";
#endif
    }

    static void parseBackend(const std::string &url,
                             std::string &scheme, std::string &host, int &port) {
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
