// WePay-Cpp — Dujiao 代理
// DujiaoProxy.h — 把 dujiao.dll 嵌入 WePay 同进程，并把 /dujiao/* 反代到内部 HTTP
// SSO：根据 WePay 管理员 JWT 自动注入 dujiao Bearer token
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h> // JSON 库
#include <string> // 字符串库
#include <atomic> // 原子操作
#include <filesystem> // 文件系统库
#include "DujiaoLib.h"
#include "SimpleJwt.h"

// Dujiao 代理类
// 职责：
//   1. 加载 dujiao.dll（独角兽支付系统）
//   2. 支持两种模式：FFI（零拷贝直派）和 HTTP（loopback）
//   3. 静态托管 SPA 前端（admin 和 user）
//   4. SSO 注入：自动将 WePay JWT 转换为 Dujiao token
class DujiaoProxy {
public:
    // 初始化 Dujiao 代理
    // 参数 cfg：配置对象（JSON）
    // 配置项：
    //   dujiao.enabled - 启用 Dujiao（默认 false）
    //   dujiao.lib_path - DLL 文件路径（默认 dujiao.dll）
    //   dujiao.config_dir - 配置目录（默认 dujiao）
    //   dujiao.prefix - 路由前缀（默认 /dujiao）
    //   dujiao.internal_port - 内部 HTTP 端口（默认 8081）
    //   dujiao.mode - 运行模式（api/web，默认 api）
    //   dujiao.autostart - 自动启动（默认 true）
    //   dujiao.ffi - 启用 FFI 模式（默认 true，不支持则自动降级）
    //   dujiao.admin_dist - 管理员前端目录（默认 ./dujiao-admin）
    //   dujiao.user_dist - 用户前端目录（默认 ./dujiao-user）
    // 功能：
    //   1. 加载 DLL → 初始化 → 构造 engine（FFI）或启动 HTTP
    //   2. 注册 /dujiao/* 路由（反向代理）
    //   3. 静态托管 SPA 前端
    static void setup(const Json::Value &cfg) {
        // 检查是否配置了 dujiao
        if (!cfg.isMember("dujiao"))
            return;
        // 获取 dujiao 配置
        const auto &c = cfg["dujiao"];
        // 检查是否启用
        if (!c.get("enabled", false).asBool())
            return;

        // ── 读取配置参数 ──────────────────────────────
        // DLL 文件路径
        std::string libPath    = c.get("lib_path",   "dujiao.dll").asString();
        // 配置目录
        std::string configDir  = c.get("config_dir", "dujiao").asString();
        // 路由前缀
        std::string prefix     = c.get("prefix",     "/dujiao").asString();
        // 内部 HTTP 端口
        int         port       = c.get("internal_port", 8081).asInt();
        // 运行模式（api/web）
        std::string mode       = c.get("mode",       "api").asString();
        // 自动启动标志
        bool        autostart  = c.get("autostart",  true).asBool();
        // FFI 模式标志（默认开，若 DLL 不支持则自动降级为 loopback HTTP）
        bool        useFFI     = c.get("ffi", true).asBool();
        // 管理员前端目录
        std::string adminDist  = c.get("admin_dist", "./dujiao-admin").asString();
        // 用户前端目录
        std::string userDist   = c.get("user_dist",  "./dujiao-user").asString();

        // ── 清理路由前缀（移除末尾斜杠）──────────────────────────────
        // 移除末尾的斜杠
        while (!prefix.empty() && prefix.back() == '/')
            prefix.pop_back();

        // ── 解析前端 SPA 目录 ──────────────────────────────
        // 解析前端 dist 绝对路径（若存在 index.html 则启用）
        // 管理员前端根目录
        std::string adminRoot;
        // 用户前端根目录
        std::string userRoot;
        // 管理员 index.html 路径
        std::string adminIndex;
        // 用户 index.html 路径
        std::string userIndex;
        {
            // 错误代码
            std::error_code ec;
            // 获取管理员前端绝对路径
            auto aAbs = std::filesystem::absolute(adminDist, ec);
            // 检查 index.html 是否存在
            if (!ec && std::filesystem::exists(aAbs / "index.html", ec)) {
                // 设置管理员前端根目录
                adminRoot  = aAbs.string();
                // 设置管理员 index.html 路径
                adminIndex = (aAbs / "index.html").string();
                // 记录日志
                LOG_INFO << "[DujiaoProxy] Admin SPA: " << adminRoot;
            } else {
                // 记录警告日志
                LOG_WARN << "[DujiaoProxy] Admin dist 未就绪: " << adminDist
                         << " (访问 " << prefix << "/admin/ 将走 FFI)";
            }
            // 获取用户前端绝对路径
            auto uAbs = std::filesystem::absolute(userDist, ec);
            // 检查 index.html 是否存在
            if (!ec && std::filesystem::exists(uAbs / "index.html", ec)) {
                // 设置用户前端根目录
                userRoot  = uAbs.string();
                // 设置用户 index.html 路径
                userIndex = (uAbs / "index.html").string();
                // 记录日志
                LOG_INFO << "[DujiaoProxy] User SPA: " << userRoot;
            } else {
                // 记录警告日志
                LOG_WARN << "[DujiaoProxy] User dist 未就绪: " << userDist
                         << " (访问 " << prefix << "/user/ 将走 FFI)";
            }
        }

        // ── 加载 DLL ──────────────────────────────
        // 获取 DujiaoLib 单例
        auto &lib = DujiaoLib::instance();
        // 加载 DLL
        bool dllOk = lib.load(libPath);
        // 检查 DLL 是否加载成功
        if (!dllOk) {
            // 记录错误日志
            LOG_ERROR << "[DujiaoProxy] DLL 加载失败，但 SPA 静态托管仍会注册";
        }

        // ── 初始化 Dujiao ──────────────────────────────
        // FFI 模式是否激活
        bool ffiActive = false;
        // 如果 DLL 加载成功且启用自动启动
        if (dllOk && autostart) do {
            // 初始化 Dujiao
            int rc = lib.init(configDir);
            // 检查初始化是否成功
            if (rc != 0) {
                // 记录错误日志
                LOG_ERROR << "[DujiaoProxy] DJ_Init failed rc=" << rc
                          << " err=" << lib.lastError()
                          << "  → API 调用将返回 503，但 SPA 静态资源仍可访问";
                // 跳出循环
                break;
            }

            // ── 尝试启用 FFI 模式 ──────────────────────────────
            // 如果启用 FFI 且 DLL 支持 FFI
            if (useFFI && lib.hasFFI()) {
                // 构建 Dujiao Gin engine
                rc = lib.buildEngine();
                // 检查是否成功
                if (rc != 0) {
                    // 记录错误日志
                    LOG_ERROR << "[DujiaoProxy] DJ_BuildEngine failed rc=" << rc
                              << " err=" << lib.lastError()
                              << "  → API 调用将返回 503";
                    // 跳出循环
                    break;
                }
                // 标记 FFI 模式激活
                ffiActive = true;
                // 记录日志
                LOG_INFO << "[DujiaoProxy] FFI 模式启用 (零拷贝直派)";
            } else {
                // ── 启用 HTTP 模式 ──────────────────────────────
                // 启动 Dujiao HTTP 服务器
                rc = lib.startServer(mode);
                // 检查是否成功
                if (rc != 0) {
                    // 记录错误日志
                    LOG_ERROR << "[DujiaoProxy] DJ_StartServer failed rc=" << rc
                              << " err=" << lib.lastError();
                    // 跳出循环
                    break;
                }
                // 记录日志
                LOG_INFO << "[DujiaoProxy] HTTP 模式启用 internal=127.0.0.1:" << port;
            }
            // 标记服务运行
            running_.store(true);
        } while (false);

        // 保存 FFI 模式状态
        ffiActive_.store(ffiActive);

        // ── 注册前置路由建议 ──────────────────────────────
        // 路由前缀
        const std::string pfx = prefix;
        // 内部服务端口
        const int svcPort = port;

        // 注册前置路由建议（拦截 /dujiao/* 请求）
        drogon::app().registerPreRoutingAdvice(
            [pfx, svcPort, adminRoot, adminIndex, userRoot, userIndex]
            (const drogon::HttpRequestPtr &req,
             drogon::AdviceCallback        &&cb,
             drogon::AdviceChainCallback   &&ccb)
            {
                // ── 检查路由是否匹配 ──────────────────────────────
                // 获取请求路径
                const std::string &path = req->path();
                // 检查路径是否匹配 /dujiao 或 /dujiao/*
                bool match = (path == pfx) ||
                             (path.size() > pfx.size() && path[pfx.size()] == '/' &&
                              path.rfind(pfx, 0) == 0);
                // 如果不匹配，继续处理下一个路由
                if (!match) {
                    ccb();
                    return;
                }

                // ── 静态 SPA 托管：/dujiao/admin/* 和 /dujiao/user/* ──────────────────────────────
                // 功能：
                //   1. 命中真实文件 → 返回文件（带缓存头）
                //   2. 命中子目录或不存在 → 返回对应 index.html（SPA 路由 fallback）
                //   3. 支持 SPA 深层路由（如 /dujiao/admin/users/123）
                auto tryServeSpa = [&](const std::string &subPrefix,
                                       const std::string &root,
                                       const std::string &indexHtml) -> bool {
                    // 如果根目录为空，返回 false
                    if (root.empty())
                        return false;
                    // 构建完整前缀（如 /dujiao/admin）
                    std::string full = pfx + subPrefix;
                    // 检查路径是否匹配前缀
                    if (path != full &&
                        !(path.size() > full.size() && path[full.size()] == '/' &&
                          path.rfind(full, 0) == 0)) {
                        return false;
                    }
                    // 提取相对路径
                    std::string rel = path.substr(full.size());
                    // 移除前导斜杠
                    if (!rel.empty() && rel.front() == '/')
                        rel.erase(0, 1);
                    // 防止目录遍历攻击
                    if (rel.find("..") != std::string::npos)
                        return false;

                    // 构建响应
                    drogon::HttpResponsePtr r;
                    // 如果相对路径为空，返回 index.html
                    if (rel.empty()) {
                        r = drogon::HttpResponse::newFileResponse(indexHtml);
                    } else {
                        // 构建文件路径
                        std::filesystem::path fp =
                            std::filesystem::path(root) / rel;
                        // 错误代码
                        std::error_code ec;
                        // 检查文件是否存在且是普通文件
                        if (std::filesystem::exists(fp, ec) &&
                            std::filesystem::is_regular_file(fp, ec)) {
                            // 返回文件
                            r = drogon::HttpResponse::newFileResponse(fp.string());
                            // ── 为带 hash 的静态资源设置长缓存 ──────────────────────────────
                            // 检查是否是静态资源（assets、js、css）
                            if (rel.rfind("assets/", 0) == 0 ||
                                rel.rfind("js/", 0) == 0 ||
                                rel.rfind("css/", 0) == 0) {
                                // 设置长期缓存（1 年）
                                r->addHeader("Cache-Control",
                                             "public, max-age=31536000, immutable");
                            }
                        } else {
                            // ── SPA 深层路由 fallback ──────────────────────────────
                            // 如果文件不存在，返回 index.html（SPA 路由处理）
                            r = drogon::HttpResponse::newFileResponse(indexHtml);
                        }
                    }
                    // 返回响应
                    cb(r);
                    return true;
                };

                // ── 尝试服务 admin SPA ──────────────────────────────
                if (tryServeSpa("/admin", adminRoot, adminIndex))
                    return;
                // ── 尝试服务 user SPA ──────────────────────────────
                if (tryServeSpa("/user",  userRoot,  userIndex))
                    return;

                // ── 检查服务是否运行 ──────────────────────────────
                // 如果服务未运行
                if (!running_.load()) {
                    // 创建 503 响应
                    auto r = drogon::HttpResponse::newHttpResponse();
                    // 设置状态码
                    r->setStatusCode(drogon::k503ServiceUnavailable);
                    // 设置 Content-Type
                    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    // 设置错误消息
                    r->setBody("{\"code\":503,\"msg\":\"dujiao 服务未启动\"}");
                    // 返回响应
                    cb(r);
                    return;
                }

                // ── 提取子路径 ──────────────────────────────
                // 提取 /dujiao 之后的路径
                std::string subPath = path.substr(pfx.size());
                // 如果子路径为空，设置为 /
                if (subPath.empty())
                    subPath = "/";

                // ── 收集请求头并提取 WePay Authorization ──────────────────────────────
                // 请求头字典
                std::map<std::string, std::string> hdrs;
                // WePay Authorization 头
                std::string wepayAuth;
                // 遍历所有请求头
                for (auto &[k, v] : req->headers()) {
                    // 转换为小写进行比较
                    std::string lk = k;
                    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                    // 跳过 Host 头
                    if (lk == "host")
                        continue;
                    // 提取 Authorization 头
                    if (lk == "authorization") {
                        wepayAuth = v;
                        continue;
                    }
                    // 添加其他头
                    hdrs[k] = v;
                }

                // ── SSO：注入 Dujiao Bearer token ──────────────────────────────
                // 从 WePay JWT 中提取 username
                std::string username = extractWepayUsername(wepayAuth);
                // 如果成功提取 username
                if (!username.empty()) {
                    // 为 username 签发 Dujiao token
                    std::string djToken = DujiaoLib::instance().issueToken(username);
                    // 如果签发成功
                    if (!djToken.empty()) {
                        // 注入 Dujiao Bearer token
                        hdrs["Authorization"] = "Bearer " + djToken;
                    }
                } else if (!wepayAuth.empty()) {
                    // ── 非 WePay JWT 直接透传 ──────────────────────────────
                    // 如果是 Dujiao 原生 token 或其他 token，直接透传
                    hdrs["Authorization"] = wepayAuth;
                }

                // 获取请求体
                std::string body(req->body());

                // ── 根据模式选择处理方式 ──────────────────────────────
                // 如果 FFI 模式激活
                if (ffiActive_.load()) {
                    // ── FFI 模式：直接调用 Dujiao Gin engine ──────────────────────────────
                    // 获取请求方法字符串
                    std::string methodStr = req->methodString();
                    // 如果方法为空，默认为 GET
                    if (methodStr.empty())
                        methodStr = "GET";
                    // 直接调用 FFI 处理请求
                    auto resp = DujiaoLib::instance().handleRequest(
                        methodStr, subPath, req->query(), hdrs, body);
                    // 检查是否成功
                    if (!resp.ok) {
                        // 创建 502 响应
                        auto r = drogon::HttpResponse::newHttpResponse();
                        // 设置状态码
                        r->setStatusCode(drogon::k502BadGateway);
                        // 设置 Content-Type
                        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                        // 设置错误消息
                        r->setBody("{\"code\":502,\"msg\":\"dujiao FFI 错误: " + resp.err + "\"}");
                        // 返回响应
                        cb(r);
                        return;
                    }
                    // 创建响应对象
                    auto r = drogon::HttpResponse::newHttpResponse();
                    // 设置状态码
                    r->setStatusCode(static_cast<drogon::HttpStatusCode>(resp.status));
                    // 复制响应头（过滤掉 hop-by-hop 头）
                    for (auto &kv : resp.headers) {
                        // 转换为小写进行比较
                        std::string lk = kv.first;
                        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                        // 跳过 hop-by-hop 头
                        if (lk == "content-length" || lk == "transfer-encoding" ||
                            lk == "connection")
                            continue;
                        // 添加响应头
                        r->addHeader(kv.first, kv.second);
                    }
                    // 设置响应体
                    r->setBody(std::move(resp.body));
                    // 返回响应
                    cb(r);
                    return;
                }

                // ── Loopback HTTP 模式：转发到内部 HTTP 服务器 ──────────────────────────────
                // 构建子路径
                std::string sub = subPath;
                // 获取查询字符串
                std::string qs = req->query();
                // 如果有查询字符串，添加到子路径
                if (!qs.empty())
                    sub += "?" + qs;

                // 创建转发请求
                auto fwdReq = drogon::HttpRequest::newHttpRequest();
                // 设置请求方法
                fwdReq->setMethod(req->method());
                // 设置请求路径
                fwdReq->setPath(sub);
                // 复制请求头
                for (auto &kv : hdrs)
                    fwdReq->addHeader(kv.first, kv.second);
                // 如果有请求体，设置请求体
                if (!body.empty())
                    fwdReq->setBody(body);

                // 构建后端 URL
                std::string backendUrl = "http://127.0.0.1:" + std::to_string(svcPort);
                // 创建 HTTP 客户端
                auto client = drogon::HttpClient::newHttpClient(backendUrl);
                // 发送转发请求
                client->sendRequest(fwdReq,
                    [cb = std::move(cb)](drogon::ReqResult result,
                                         const drogon::HttpResponsePtr &resp) {
                        // 如果请求成功且有响应
                        if (result == drogon::ReqResult::Ok && resp) {
                            // 直接返回响应
                            cb(resp);
                        } else {
                            // 请求失败，返回 502 Bad Gateway
                            auto r = drogon::HttpResponse::newHttpResponse();
                            // 设置状态码
                            r->setStatusCode(drogon::k502BadGateway);
                            // 设置 Content-Type
                            r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                            // 设置错误消息
                            r->setBody("{\"code\":502,\"msg\":\"dujiao 服务不可用\"}");
                            // 返回响应
                            cb(r);
                        }
                    });
            });

        // 记录反向代理配置日志
        LOG_INFO << "[DujiaoProxy] 反代已注册: " << pfx << "/*"
                 << (ffiActive ? " (FFI 模式)"
                               : " → http://127.0.0.1:" + std::to_string(port));
    }

    // 程序退出前调用：优雅停止 dujiao 后台服务
    // 功能：停止 HTTP 服务器、清理资源、标记服务停止
    static void shutdown() {
        // 检查服务是否运行
        if (!running_.load())
            return;
        // 停止 Dujiao HTTP 服务器
        DujiaoLib::instance().stopServer();
        // 标记服务已停止
        running_.store(false);
        // 记录日志
        LOG_INFO << "[DujiaoProxy] dujiao 已停止";
    }

// 私有区域
private:
    // 服务是否运行
    static inline std::atomic<bool> running_{false};
    // FFI 模式是否激活
    static inline std::atomic<bool> ffiActive_{false};

    // 从 WePay 的 Authorization: Bearer <jwt> 中验证并提取 username
    // 参数 authHeader：Authorization 请求头值
    // 返回：用户名（如果验证失败或不是 admin token 返回空字符串）
    // 说明：
    //   1. 验证 JWT 签名
    //   2. 提取 subject 字段
    //   3. 只允许 admin token（纯 username）
    //   4. 拒绝 merchant token（mch:*:*）和 agent token（agent:*:*）
    //   5. 失败返回空字符串（访问者将命中 dujiao 自己的 401）
    static std::string extractWepayUsername(const std::string &authHeader) {
        // 检查请求头是否为空
        if (authHeader.empty())
            return "";
        // 尝试验证 JWT
        try {
            // 从请求头中提取 token（移除 "Bearer " 前缀）
            std::string token = SimpleJwt::fromHeader(authHeader);
            // 验证 token 并提取 subject
            std::string sub   = SimpleJwt::verify(token);
            // WePay token 的 subject 格式：
            //   admin: "username"
            //   merchant: "mch:<id>:<username>"
            //   agent: "agent:<id>:<username>"
            // 只允许 admin token（纯 username）
            // 检查是否是 merchant token
            if (sub.rfind("mch:",  0) == 0)
                return "";
            // 检查是否是 agent token
            if (sub.rfind("agent:",0) == 0)
                return "";
            // 返回 username
            return sub;
        } catch (...) {
            // 验证失败，返回空字符串
            return "";
        }
    }
};
