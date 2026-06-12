// WePay-Cpp — Bepusdt 代理
// BepusdtProxy.h — 嵌入 bepusdt.dll，托管 BEpusdt 多链加密货币支付网关
// 双模式:
//   FFI 模式 (ffi=true):  不监听额外端口，与 WePay 共用 8088
//                         /crypto → BEpusdt 管理后台入口
//                         /api/conf/* /api/wallet/* /api/order/* /api/rate/*
//                         /api/dashboard/* /api/v1/* → BEpusdt API
//                         /payment/* /secure/* → 静态资源
//                         /pay/checkout-counter/* /pay/cashier/* /pay/check-status/*
//   HTTP 模式 (ffi=false): BEpusdt 监听独立端口，BepusdtPlugin 内部 loopback 调用
#pragma once
#include <drogon/drogon.h>
#include <json/json.h>
#include <string>
#include <atomic>
#include <algorithm>
#include "BepusdtLib.h"
#include "SimpleJwt.h"

class BepusdtProxy {
public:
    static void setup(const Json::Value &cfg) {
        if (!cfg.isMember("bepusdt")) return;
        const auto &c = cfg["bepusdt"];
        if (!c.get("enabled", false).asBool()) return;

        std::string dllPath    = c.get("dll_path", "./bepusdt.dll").asString();
        std::string prefix     = c.get("prefix", "/crypto").asString();
        int         port       = c.get("internal_port", 8180).asInt();
        bool        autostart  = c.get("autostart", true).asBool();
        bool        useFFI     = c.get("ffi", true).asBool();

        std::string sqlitePath = c.get("sqlite_path", "./bepusdt.db").asString();
        std::string mysqlDsn   = c.get("mysql_dsn", "").asString();
        std::string pgDsn      = c.get("postgres_dsn", "").asString();
        std::string logPath    = c.get("log_path", "./logs/bepusdt/").asString();

        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

        auto &lib = BepusdtLib::instance();
        if (!lib.load(dllPath)) {
            LOG_ERROR << "[BepusdtProxy] DLL 加载失败: " << dllPath;
            return;
        }

        std::string ver = lib.version();
        LOG_INFO << "[BepusdtProxy] BEpusdt version: " << ver;

        // ── 选择模式 ──
        bool ffiMode = useFFI && lib.hasFFI();
        if (useFFI && !lib.hasFFI()) {
            LOG_WARN << "[BepusdtProxy] DLL 不含 FFI 导出，降级为 HTTP 模式";
        }
        ffiMode_ = ffiMode;
        internalPort_ = port;

        if (ffiMode) {
            // FFI 模式: 构建 gin engine，不监听端口
            std::string result = lib.buildEngine(sqlitePath, mysqlDsn, pgDsn, logPath);
            if (result.rfind("ok:", 0) == 0) {
                running_.store(true);
                LOG_INFO << "[BepusdtProxy] FFI " << result;
            } else {
                LOG_ERROR << "[BepusdtProxy] FFI buildEngine failed: " << result;
                return;
            }

            // 获取安全入口路径（动态的，如 /b1d8b50648）
            std::string securePath = lib.getSecurePath();
            if (!securePath.empty()) {
                securePath_ = securePath;
                LOG_INFO << "[BepusdtProxy] 安全入口: " << securePath;
            }

            // 注册路由拦截
            registerFFIRoutes(prefix);
            LOG_INFO << "[BepusdtProxy] FFI 模式，管理后台: http://localhost:8088" << prefix << "/";
        } else if (autostart) {
            // HTTP 模式: 监听独立端口
            bool publicAccess = c.get("public_access", true).asBool();
            std::string listen = (publicAccess ? "0.0.0.0:" : "127.0.0.1:")
                               + std::to_string(port);
            std::string result = lib.start(listen, sqlitePath, mysqlDsn, pgDsn, logPath);
            if (result.rfind("ok:", 0) == 0) {
                running_.store(true);
                LOG_INFO << "[BepusdtProxy] HTTP " << result;
                LOG_INFO << "[BepusdtProxy] 管理后台: http://"
                         << (publicAccess ? "服务器IP" : "127.0.0.1")
                         << ":" << port << "/";
            } else {
                LOG_ERROR << "[BepusdtProxy] start failed: " << result;
                return;
            }
        }
    }

    static void shutdown() {
        if (!running_.load()) return;
        auto result = BepusdtLib::instance().stop();
        running_.store(false);
        LOG_INFO << "[BepusdtProxy] " << result;
    }

    static int internalPort() { return internalPort_; }
    static bool isRunning() { return running_.load(); }
    static bool isFFI() { return ffiMode_; }

private:
    static inline std::atomic<bool> running_{false};
    static inline bool ffiMode_{false};
    static inline int internalPort_{8180};
    static inline std::string securePath_; // 动态安全入口路径，如 /b1d8b50648

    // ── 判断请求是否属于 BEpusdt ──
    // BEpusdt 的路由前缀（不与 WePay 冲突）:
    //   /api/conf/ /api/wallet/ /api/order/ /api/rate/
    //   /api/dashboard/ /api/v1/ → BEpusdt admin + public API
    //   /payment/ /secure/ → 静态资源
    //   /pay/checkout-counter/ /pay/cashier/ /pay/check-status/ → 收银台
    //   /submit.php → 易支付兼容
    //   /crypto (prefix) → 管理后台入口
    static bool isBepusdtPath(const std::string &path, const std::string &prefix) {
        // 1. /crypto 前缀
        if (path == prefix || (path.size() > prefix.size() &&
            path[prefix.size()] == '/' && path.rfind(prefix, 0) == 0))
            return true;

        // 2. BEpusdt API 路由
        static const char* apiPrefixes[] = {
            "/api/auth/", "/api/conf/", "/api/wallet/", "/api/order/",
            "/api/rate/", "/api/dashboard/", "/api/v1/"
        };
        for (auto &ap : apiPrefixes) {
            if (path.rfind(ap, 0) == 0) return true;
        }

        // 3. 静态资源
        if (path.rfind("/payment/", 0) == 0) return true;
        if (path.rfind("/secure/", 0) == 0) return true;

        // 4. 收银台
        if (path.rfind("/pay/checkout-counter/", 0) == 0) return true;
        if (path.rfind("/pay/cashier/", 0) == 0) return true;
        if (path.rfind("/pay/check-status/", 0) == 0) return true;

        // 5. 易支付兼容
        if (path == "/submit.php") return true;

        // 6. 动态安全入口路径（延迟加载：首次安装后才生成）
        if (securePath_.empty()) {
            securePath_ = BepusdtLib::instance().getSecurePath();
            if (!securePath_.empty()) {
                LOG_INFO << "[BepusdtProxy] 安全入口路径: " << securePath_;
            }
        }
        if (!securePath_.empty() && path == securePath_) return true;

        return false;
    }

    // 获取 HTTP 方法字符串
    static std::string methodStr(drogon::HttpMethod m) {
        switch (m) {
            case drogon::Get:     return "GET";
            case drogon::Post:    return "POST";
            case drogon::Put:     return "PUT";
            case drogon::Delete:  return "DELETE";
            case drogon::Patch:   return "PATCH";
            case drogon::Options: return "OPTIONS";
            case drogon::Head:    return "HEAD";
            default:              return "GET";
        }
    }

    // 从 WePay 的 Authorization: Bearer <jwt> 中验证并提取 username
    static std::string extractWepayUsername(const std::string &authHeader) {
        if (authHeader.empty()) return "";
        try {
            std::string token = SimpleJwt::fromHeader(authHeader);
            std::string sub   = SimpleJwt::verify(token);
            // 仅允许 admin（纯 username），不允许商户/代理
            if (sub.rfind("mch:",  0) == 0) return "";
            if (sub.rfind("agent:",0) == 0) return "";
            return sub;
        } catch (...) {
            return "";
        }
    }

    // SSO 中间页: 写入 BEpusdt token 到 localStorage → 跳转 SPA
    static std::string buildSsoPage(const std::string &bepusdtToken) {
        return R"(<!DOCTYPE html><html><head><meta charset="utf-8"><title>BEpusdt SSO</title></head>
<body><p>正在登录 BEpusdt...</p><script>
try{var d=JSON.parse(localStorage.getItem('user-info')||'{}');
d.token=')" + bepusdtToken + R"(';localStorage.setItem('user-info',JSON.stringify(d));}catch(e){}
window.location.replace('/secure/#/home');
</script></body></html>)";
    }

    static void registerFFIRoutes(const std::string &prefix) {
        const std::string pfx = prefix;
        drogon::app().registerPreRoutingAdvice(
            [pfx](const drogon::HttpRequestPtr &req,
                  drogon::AdviceCallback &&cb,
                  drogon::AdviceChainCallback &&ccb)
            {
                const std::string &path = req->path();
                if (!isBepusdtPath(path, pfx)) { ccb(); return; }
                LOG_DEBUG << "[BepusdtProxy] 拦截: " << req->methodString() << " " << path;

                if (!running_.load()) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(drogon::k503ServiceUnavailable);
                    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    r->setBody(R"({"code":503,"msg":"BEpusdt 服务未启动"})");
                    cb(r); return;
                }

                // ── SSO: /crypto 或 /crypto/ 入口 → 检测 WePay JWT → 签发 token ──
                bool isPrefixEntry = (path == pfx || path == pfx + "/");
                if (isPrefixEntry) {
                    // 从 cookie 或 header 获取 WePay JWT
                    std::string wepayAuth;
                    auto authIt = req->headers().find("authorization");
                    if (authIt != req->headers().end()) wepayAuth = authIt->second;
                    // 也检查 cookie（admin 前端可能用 cookie 存 JWT）
                    if (wepayAuth.empty()) {
                        std::string cookie = req->getCookie("wepay_token");
                        if (!cookie.empty()) wepayAuth = "Bearer " + cookie;
                    }

                    std::string username = extractWepayUsername(wepayAuth);
                    if (!username.empty() && BepusdtLib::instance().hasSSO()) {
                        std::string bpToken = BepusdtLib::instance().issueToken(username);
                        if (!bpToken.empty()) {
                            // 返回 SSO 中间页
                            auto r = drogon::HttpResponse::newHttpResponse();
                            r->setStatusCode(drogon::k200OK);
                            r->setContentTypeCode(drogon::CT_TEXT_HTML);
                            r->setBody(buildSsoPage(bpToken));
                            cb(r); return;
                        }
                    }
                    // 无有效 WePay JWT → 正常显示 BEpusdt 自己的登录页
                }

                // 计算转发到 BEpusdt 的路径: /crypto/* → 剥前缀，其他保持原样
                std::string ffiPath = path;
                if (path == pfx) {
                    ffiPath = "/";
                } else if (path.size() > pfx.size() && path[pfx.size()] == '/' &&
                           path.rfind(pfx, 0) == 0) {
                    ffiPath = path.substr(pfx.size());
                    if (ffiPath.empty()) ffiPath = "/";
                }
                // 其他路径 (/api/conf/*, /payment/*, ...) 不需要改路径

                // 收集 headers（剔除 Host），提取 WePay Authorization
                std::map<std::string, std::string> hdrs;
                std::string wepayAuth;
                for (auto &[k, v] : req->headers()) {
                    std::string lk = k;
                    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                    if (lk == "host") continue;
                    if (lk == "authorization") {
                        wepayAuth = v;
                        continue; // 先不放入 hdrs
                    }
                    hdrs[k] = v;
                }

                // FFI 模式标记: 跳过 BEpusdt 安全入口 session 检查
                hdrs["X-FFI-Bypass"] = "1";

                // SSO 注入: 如果有 WePay JWT，签发 BEpusdt token 注入
                std::string username = extractWepayUsername(wepayAuth);
                if (!username.empty() && BepusdtLib::instance().hasSSO()) {
                    std::string bpToken = BepusdtLib::instance().issueToken(username);
                    if (!bpToken.empty()) {
                        hdrs["Authorization"] = bpToken;
                    }
                } else if (!wepayAuth.empty()) {
                    // 非 WePay JWT（可能是 BEpusdt 自己的 token），直接透传
                    hdrs["Authorization"] = wepayAuth;
                }

                std::string bodyStr(req->body());
                std::string method = methodStr(req->method());
                std::string query  = req->query();

                auto resp = BepusdtLib::instance().handleRequest(method, ffiPath, query, hdrs, bodyStr);
                if (!resp.ok) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(drogon::k502BadGateway);
                    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    r->setBody(R"({"code":502,"msg":"BEpusdt FFI 调用失败: )" + resp.err + "\"}");
                    cb(r); return;
                }

                LOG_DEBUG << "[BepusdtProxy] FFI 响应: status=" << resp.status
                          << " headers=" << resp.headers.size()
                          << " body=" << resp.body.size() << "B";
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode((drogon::HttpStatusCode)resp.status);
                for (auto &[k, v] : resp.headers) {
                    std::string lk = k;
                    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                    if (lk == "content-length" || lk == "transfer-encoding") continue;
                    // Content-Type 需要用 drogon 专用方法设置
                    if (lk == "content-type") {
                        r->setContentTypeString(v);
                        continue;
                    }
                    // 重写 Location: BEpusdt 内部重定向 /#/xxx → /crypto/#/xxx
                    if (lk == "location" && !v.empty() && v[0] == '/') {
                        r->addHeader(k, pfx + v);
                        continue;
                    }
                    r->addHeader(k, v);
                }
                r->setBody(resp.body);
                cb(r);
            });
    }
};
