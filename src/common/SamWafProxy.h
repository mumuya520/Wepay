// WePay-Cpp — SamWaf 代理管理
// SamWafProxy.h — 将 SamWaf 作为子进程托管
// 配置项 samwaf.autostart=true 时，WePay 启动时自动拉起 samwaf.exe
// 使用 Job Object 确保父进程退出/崩溃时子进程跟死
// 管理后台直接访问 http://localhost:<admin_port>（默认 26666）
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h> // JSON 库
#include <string> // 字符串库
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <algorithm>
#include "ProcessManager.h"
#include "SyncHttp.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <signal.h>
#  include <cstdlib>
#endif

// SamWaf WAF 代理管理类
// 职责：
//   1. 作为子进程托管 SamWaf（Windows Job Object / Unix fork）
//   2. 自动启动和监控 SamWaf 进程（看门狗）
//   3. 反向代理 /waf/ 路由到 SamWaf 管理后台
//   4. 自动配置 WAF 主机和 capJs PoW 挑战
class SamWafProxy {
public:
    // 初始化 SamWaf 代理
    // 参数 cfg：配置对象（JSON）
    // 配置项：
    //   samwaf.enabled=true - 启用 SamWaf
    //   samwaf.exe - SamWaf 可执行文件路径
    //   samwaf.admin_port - 管理后台端口（默认 26666）
    //   samwaf.admin_user - 管理员用户名（默认 admin）
    //   samwaf.admin_pass - 管理员密码（默认 admin868）
    //   samwaf.listen_port - WAF 监听端口（默认 80）
    //   samwaf.backend_port - 后端服务端口（默认 8088）
    //   samwaf.autostart - 自动启动（默认 true）
    //   samwaf.autoprovision - 自动配置 WAF 主机（默认 true）
    //   samwaf.watchdog - 启用看门狗监控（默认 true）
    //   samwaf.watchdog_interval_sec - 监控间隔（秒，默认 15）
    //   samwaf.db - PostgreSQL 数据库配置
    static void setup(const Json::Value &cfg) {
        // 检查是否配置了 samwaf
        if (!cfg.isMember("samwaf"))
            return;
        // 获取 samwaf 配置
        const auto &c = cfg["samwaf"];
        // 检查是否启用
        if (!c.get("enabled", false).asBool())
            return;

        // ── 读取配置参数 ──────────────────────────────
        // 设置可执行文件路径
        exe_         = c.get("exe",          "./samwaf/samwaf.exe").asString();
        // 设置管理后台端口
        adminPort_   = c.get("admin_port",   26666).asInt();
        // 设置管理员用户名
        adminUser_   = c.get("admin_user",   "admin").asString();
        // 设置管理员密码
        adminPass_   = c.get("admin_pass",   "admin868").asString();
        // 设置 WAF 监听端口
        listenPort_  = c.get("listen_port",  80).asInt();
        // 设置后端服务端口
        backendPort_ = c.get("backend_port", 8088).asInt();
        // 读取自动启动标志
        bool autostart    = c.get("autostart",    true).asBool();
        // 读取自动配置标志
        bool autoprovision = c.get("autoprovision", true).asBool();
        // 读取看门狗启用标志
        bool watchdog     = c.get("watchdog",     true).asBool();
        // 读取监控间隔（秒）
        int  interval     = c.get("watchdog_interval_sec", 15).asInt();

        // ── 读取 PostgreSQL 配置 ──────────────────────────────
        // 如果配置了数据库且 host 不为空
        if (c.isMember("db") && !c["db"].get("host", "").asString().empty()) {
            // 获取数据库配置
            const auto &db = c["db"];
            // 设置 PostgreSQL 主机
            dbEnv_["SAMWAF_PG_HOST"]     = db.get("host",    "127.0.0.1").asString();
            // 设置 PostgreSQL 端口
            dbEnv_["SAMWAF_PG_PORT"]     = std::to_string(db.get("port", 5432).asInt());
            // 设置 PostgreSQL 用户
            dbEnv_["SAMWAF_PG_USER"]     = db.get("user",    "postgres").asString();
            // 设置 PostgreSQL 密码
            dbEnv_["SAMWAF_PG_PASSWORD"] = db.get("password", "").asString();
            // 设置 PostgreSQL 数据库名
            dbEnv_["SAMWAF_PG_DB"]       = db.get("dbname",  "wepaywaf").asString();
            // 设置 PostgreSQL SSL 模式
            dbEnv_["SAMWAF_PG_SSLMODE"]  = db.get("sslmode", "disable").asString();
            // 记录数据库配置日志
            LOG_INFO << "[SamWafProxy] 数据库使用 PostgreSQL: "
                     << dbEnv_["SAMWAF_PG_HOST"] << ":" << dbEnv_["SAMWAF_PG_PORT"]
                     << "/" << dbEnv_["SAMWAF_PG_DB"];
        }

        // 记录初始化日志
        LOG_INFO << "[SamWafProxy] exe=" << exe_ << " admin_port=" << adminPort_;

        // ── 自动启动 SamWaf ──────────────────────────────
        // 如果启用自动启动
        if (autostart) {
            // 启动 SamWaf 进程
            launch();
            // 等待 SamWaf 就绪（3 秒）
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        // ── 自动配置 WAF 主机 ──────────────────────────────
        // 如果启用自动启动和自动配置
        if (autostart && autoprovision) {
            // 创建后台线程进行自动配置
            std::thread([]() {
                // 额外等待 SamWaf API 完全就绪（5 秒）
                std::this_thread::sleep_for(std::chrono::seconds(5));
                // 尝试自动配置
                try {
                    // 调用自动配置函数
                    autoProvision();
                } catch (const std::exception &e) {
                    // 捕获异常并记录日志
                    LOG_WARN << "[SamWafProxy] autoprovision 异常已忽略: " << e.what();
                } catch (...) {
                    // 捕获未知异常并记录日志
                    LOG_WARN << "[SamWafProxy] autoprovision 未知异常已忽略";
                }
            }).detach();
        }

        // ── 启动看门狗监控 ──────────────────────────────
        // 如果启用看门狗和自动启动
        if (watchdog && autostart) {
            // 创建后台线程进行监控
            std::thread([interval]() {
                // 无限循环监控
                while (true) {
                    // 等待指定间隔
                    std::this_thread::sleep_for(std::chrono::seconds(interval));
                    // 加锁保护
                    std::lock_guard<std::mutex> lk(mtx_);
                    // 检查可执行文件是否存在
                    if (!ProcessManager::exeExists(exe_))
                        continue;
                    // 检查进程是否运行
                    if (!isRunning()) {
                        // 记录重启日志
                        LOG_WARN << "[SamWafProxy] samwaf 已停止，正在重启...";
                        // 重新启动进程
                        launch();
                    }
                }
            }).detach();
        }

        // ── 注册 /waf/ 反向代理 ──────────────────────────────
        // 反向代理 /waf/ 路由到 SamWaf 管理后台
        // 路由前缀
        const std::string pfx       = "/waf";
        // 管理后台端口
        const int         port      = adminPort_;
        // 注册前置路由建议（拦截 /waf/ 请求）
        drogon::app().registerPreRoutingAdvice(
            [pfx, port]
            (const drogon::HttpRequestPtr &req,
             drogon::AdviceCallback        &&cb,
             drogon::AdviceChainCallback   &&ccb)
            {
                // 获取请求路径
                const std::string &path = req->path();
                // 检查路径是否匹配 /waf 或 /waf/*
                bool match = (path == pfx) ||
                             (path.size() > pfx.size() && path[pfx.size()] == '/' &&
                              path.rfind(pfx, 0) == 0);
                // 如果不匹配，继续处理下一个路由
                if (!match) {
                    ccb();
                    return;
                }

                // ── 提取子路径和查询字符串 ──────────────────────────────
                // 提取 /waf 之后的路径
                std::string subPath = path.substr(pfx.size());
                // 如果子路径为空，设置为 /
                if (subPath.empty())
                    subPath = "/";
                // 获取查询字符串
                std::string qs = req->query();
                // 如果有查询字符串，添加到子路径
                if (!qs.empty())
                    subPath += "?" + qs;

                // ── 构建转发请求 ──────────────────────────────
                // 创建新的 HTTP 请求
                auto fwdReq = drogon::HttpRequest::newHttpRequest();
                // 设置请求方法
                fwdReq->setMethod(req->method());
                // 设置请求路径
                fwdReq->setPath(subPath);
                // 复制请求头（除了 Host）
                for (auto &[k, v] : req->headers()) {
                    // 转换为小写进行比较
                    std::string lk = k;
                    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                    // 跳过 Host 头
                    if (lk == "host")
                        continue;
                    // 添加其他头
                    fwdReq->addHeader(k, v);
                }
                // 获取请求体
                std::string body(req->body());
                // 如果有请求体
                if (!body.empty()) {
                    // 设置请求体
                    fwdReq->setBody(std::move(body));
                    // 获取 Content-Type
                    std::string ct = req->getHeader("content-type");
                    // 如果有 Content-Type，添加到转发请求
                    if (!ct.empty())
                        fwdReq->addHeader("Content-Type", ct);
                }

                // ── 发送转发请求 ──────────────────────────────
                // 创建 HTTP 客户端
                auto client = drogon::HttpClient::newHttpClient(
                    "http://127.0.0.1:" + std::to_string(port));
                // 发送转发请求
                client->sendRequest(fwdReq,
                    [cb = std::move(cb)](drogon::ReqResult result,
                                         const drogon::HttpResponsePtr &resp) {
                        // 如果请求成功且有响应
                        if (result == drogon::ReqResult::Ok && resp) {
                            // 创建响应对象
                            auto r = drogon::HttpResponse::newHttpResponse();
                            // 设置状态码
                            r->setStatusCode(resp->statusCode());
                            // 保存 Content-Type
                            std::string upstreamCT;
                            // 复制响应头（过滤掉某些头）
                            for (auto &kv : resp->headers()) {
                                // 转换为小写进行比较
                                std::string lk = kv.first;
                                std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                                // 跳过 hop-by-hop 头
                                if (lk == "content-length" || lk == "transfer-encoding" ||
                                    lk == "connection"     || lk == "keep-alive" ||
                                    lk == "proxy-authenticate" || lk == "proxy-authorization" ||
                                    lk == "te" || lk == "trailers" || lk == "upgrade")
                                    continue;
                                // 特殊处理 Content-Type
                                if (lk == "content-type") {
                                    upstreamCT = kv.second;
                                    continue;
                                }
                                // 添加其他头
                                r->addHeader(kv.first, kv.second);
                            }
                            // 获取响应体
                            std::string body{resp->body()};
                            // 设置响应体
                            r->setBody(std::move(body));
                            // 如果有 Content-Type，设置到响应
                            if (!upstreamCT.empty())
                                r->setContentTypeString(upstreamCT);
                            // 返回响应
                            cb(r);
                        } else {
                            // 请求失败，返回 502 Bad Gateway
                            auto r = drogon::HttpResponse::newHttpResponse();
                            // 设置状态码为 502
                            r->setStatusCode(drogon::k502BadGateway);
                            // 设置 Content-Type 为 JSON
                            r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                            // 设置错误消息
                            r->setBody("{\"code\":502,\"msg\":\"SamWaf 管理后台不可用\"}");
                            // 返回响应
                            cb(r);
                        }
                    });
            });

        // 记录反向代理配置日志
        LOG_INFO << "[SamWafProxy] /waf/ → http://127.0.0.1:" << adminPort_
                 << "  (管理后台也可直接访问 http://localhost:" << adminPort_ << ")";
    }

// 私有区域
private:
    // 可执行文件路径
    static inline std::string exe_;
    // 管理后台端口
    static inline int         adminPort_{26666};
    // 管理员用户名
    static inline std::string adminUser_{"admin"};
    // 管理员密码
    static inline std::string adminPass_{"admin868"};
    // WAF 监听端口
    static inline int         listenPort_{80};
    // 后端服务端口
    static inline int         backendPort_{8088};
    // 环境变量字典（传给 SamWaf 子进程）
    static inline std::map<std::string,std::string> dbEnv_;
    // 互斥锁，保护进程状态
    static inline std::mutex  mtx_;
// Windows 平台特定成员
#ifdef _WIN32
    // Windows 进程句柄
    static inline HANDLE hProcess_{INVALID_HANDLE_VALUE};
// Unix 平台特定成员
#else
    // Unix 进程 ID
    static inline pid_t samwafPid_{0};
#endif

    // 启动 SamWaf 子进程
    // 在 Windows 上使用 ProcessManager::spawn（Job Object）
    // 在 Unix 上使用 fork + exec
    static void launch() {
// Windows 平台实现
#ifdef _WIN32
        // 如果已有进程句柄，关闭它
        if (hProcess_ != INVALID_HANDLE_VALUE) {
            // 关闭旧句柄
            CloseHandle(hProcess_);
            // 重置为无效句柄
            hProcess_ = INVALID_HANDLE_VALUE;
        }
        // 使用 ProcessManager 启动子进程（自动加入 Job Object）
        hProcess_ = ProcessManager::spawn(exe_, "", "", false, "samwaf", dbEnv_);
        // 检查启动是否成功
        if (hProcess_ == INVALID_HANDLE_VALUE)
            LOG_WARN << "[SamWafProxy] 启动 samwaf 失败";
// Unix 平台实现
#else
        // Linux: 使用 fork + exec 启动子进程，保存 PID
        // 创建子进程
        pid_t pid = fork();
        // 检查 fork 是否失败
        if (pid < 0) {
            // 记录错误日志
            LOG_ERROR << "[SamWafProxy] fork 失败";
            // 返回
            return;
        }
        // 子进程代码
        if (pid == 0) {
            // 子进程：设置环境变量
            // 遍历所有环境变量
            for (auto &[k, v] : dbEnv_) {
                // 设置环境变量
                setenv(k.c_str(), v.c_str(), 1);
            }
            // exec 启动 SamWaf（替换进程镜像）
            execl(exe_.c_str(), exe_.c_str(), nullptr);
            // 如果 exec 失败（不应该执行到这里）
            LOG_ERROR << "[SamWafProxy] exec 失败: " << exe_;
            // 子进程退出
            _exit(1);
        }
        // 父进程：保存 PID
        samwafPid_ = pid;
        // 记录启动日志
        LOG_INFO << "[SamWafProxy] 已启动 samwaf, pid=" << pid;
#endif
    }

    // 检查 SamWaf 进程是否运行
    // 返回：进程是否存活
    static bool isRunning() {
// Windows 平台实现
#ifdef _WIN32
        // 使用 ProcessManager 检查进程是否存活
        return ProcessManager::isAlive(hProcess_);
// Unix 平台实现
#else
        // Linux: 检查进程是否存活
        // 使用 kill(pid, 0) 检查进程是否存在
        return samwafPid_ > 0 && ::kill(samwafPid_, 0) == 0;
#endif
    }

    // 登录 SamWaf 管理 API，获取访问令牌
    // 返回：访问令牌（如果登录失败返回空字符串）
    static std::string apiLogin() {
        // 构建登录 URL
        std::string url = "http://127.0.0.1:" + std::to_string(adminPort_)
                        + "/samwaf/public/login";
        // 创建请求体
        Json::Value body;
        // 设置登录账号
        body["loginAccount"]  = adminUser_;
        // 设置登录密码
        body["loginPassword"] = adminPass_;
        // 创建 JSON 写入器
        Json::FastWriter fw;
        // 发送 POST 请求
        auto resp = SyncHttp::postJson(url, fw.write(body), {}, 8);
        // 检查响应是否成功
        if (!resp.success || resp.status != 200) {
            // 记录登录失败日志
            LOG_WARN << "[SamWafProxy] 登录失败 status=" << resp.status;
            // 返回空字符串
            return "";
        }
        // 解析响应 JSON
        Json::Value j;
        // 创建 JSON 读取器
        Json::Reader r;
        // 解析 JSON
        if (!r.parse(resp.body, j))
            return "";
        // 检查响应格式
        if (!j.isObject() || !j["data"].isObject())
            return "";
        // 返回访问令牌
        return j["data"].get("accessToken", "").asString();
    }

    // 检查 WAF 主机是否已存在
    // 参数 token：访问令牌
    // 返回：主机是否存在
    static bool hostExists(const std::string &token) {
        // 构建查询 URL
        std::string url = "http://127.0.0.1:" + std::to_string(adminPort_)
                        + "/samwaf/wafhost/host/list";
        // 创建请求体
        Json::Value body;
        // 设置分页参数
        body["current_page"] = 1;
        // 设置每页大小
        body["page_size"]    = 100;
        // 创建 JSON 写入器
        Json::FastWriter fw;
        // 创建请求头
        std::map<std::string,std::string> hdrs;
        // 设置访问令牌
        hdrs["X-Token"] = token;
        // 发送 POST 请求
        auto resp = SyncHttp::postJson(url, fw.write(body), hdrs, 8);
        // 检查响应是否成功
        if (!resp.success)
            return false;
        // 解析响应 JSON
        Json::Value j;
        // 创建 JSON 读取器
        Json::Reader r;
        // 解析 JSON
        if (!r.parse(resp.body, j))
            return false;
        // 检查响应格式
        if (!j.isObject() || !j["data"].isObject())
            return false;
        // 获取主机列表
        const auto &list = j["data"]["list"];
        // 检查列表是否为数组
        if (!list.isArray())
            return false;
        // 遍历主机列表
        for (const auto &h : list) {
            // 检查主机端口是否匹配
            if (h.isObject() && h.get("port", 0).asInt() == listenPort_)
                return true;
        }
        // 主机不存在
        return false;
    }

    // 自动配置 WAF 主机
    // 功能：
    //   1. 登录 SamWaf 管理 API
    //   2. 检查主机是否已存在
    //   3. 添加 WAF 主机配置（listenPort_ → 127.0.0.1:backendPort_）
    //   4. 启用 capJs PoW 挑战（拦截爬虫/F12 自动化工具）
    static void autoProvision() {
        // ── 登录获取令牌 ──────────────────────────────
        // 登录 SamWaf 管理 API
        std::string token = apiLogin();
        // 检查令牌是否为空
        if (token.empty()) {
            // 记录警告日志
            LOG_WARN << "[SamWafProxy] autoprovision: 无法获取 token，跳过";
            // 返回
            return;
        }
        // ── 检查主机是否已存在 ──────────────────────────────
        // 检查主机是否已存在
        if (hostExists(token)) {
            // 记录信息日志
            LOG_INFO << "[SamWafProxy] autoprovision: port " << listenPort_
                     << " 已存在，跳过";
            // 返回
            return;
        }

        // ── 构建 capJs PoW 挑战配置 ──────────────────────────────
        // capJs PoW 挑战配置说明：
        //   is_enable_captcha=1 - 对所有请求启用挑战
        //   engine_type=capJs - 使用工作量证明（浏览器才能通过，爬虫/自动化工具无法通过）
        //   exclude_urls - 排除的 URL（/gateway/、/notify/、/health）
        //   cap_js_config - capJs 配置（难度、过期时间等）
        const std::string captchaJson =
            // 启用验证码、使用 capJs 引擎
            R"({"is_enable_captcha":1,"engine_type":"capJs",)"
            // 验证码过期时间（24 小时）、IP 模式
            R"("expire_time":24,"ip_mode":"nic",)"
            // 排除的 URL（支付网关、回调、健康检查）
            R"("exclude_urls":"/gateway/\n/notify/\n/health",)"
            // capJs 配置（挑战数、大小、难度、过期时间）
            R"("cap_js_config":{"challengeCount":50,"challengeSize":32,)"
            R"("challengeDifficulty":4,"expiresMs":600000,)"
            // 验证码标题和文本（中英文）
            R"("infoTitle":{"zh":"安全验证","en":"Security Check"},)"
            R"("infoText":{"zh":"正在验证您的浏览器，请稍候...",)"
            R"("en":"Verifying your browser, please wait..."}}})";

        // ── 构建添加主机请求 ──────────────────────────────
        // 构建添加主机 URL
        std::string url = "http://127.0.0.1:" + std::to_string(adminPort_)
                        + "/samwaf/wafhost/host/add";
        // 创建请求体
        Json::Value body;
        // 设置主机名（* 表示所有主机）
        body["host"]         = "*";
        // 设置监听端口
        body["port"]         = listenPort_;
        // 设置后端 IP
        body["remote_ip"]    = "127.0.0.1";
        // 设置后端端口
        body["remote_port"]  = backendPort_;
        // 设置 SSL（0 = 不使用 SSL）
        body["ssl"]          = 0;
        // 设置启动状态（0 = 不启动）
        body["start_status"] = 0;
        // 设置备注
        body["remarks"]      = "WePay backend (auto-configured)";
        // 设置验证码配置
        body["captcha_json"] = captchaJson;
        // 创建 JSON 写入器
        Json::FastWriter fw;
        // 创建请求头
        std::map<std::string,std::string> hdrs;
        // 设置访问令牌
        hdrs["X-Token"] = token;
        // 发送 POST 请求
        auto resp = SyncHttp::postJson(url, fw.write(body), hdrs, 8);
        // ── 检查响应 ──────────────────────────────
        // 检查响应是否成功
        if (resp.success && resp.status == 200) {
            // 记录成功日志
            LOG_INFO << "[SamWafProxy] autoprovision: 已添加 WAF 主机"
                     << " port=" << listenPort_
                     << " → 127.0.0.1:" << backendPort_
                     << "（已启用 capJs PoW 挑战，拦截爬虫/F12自动化）";
        } else {
            // 记录失败日志
            LOG_WARN << "[SamWafProxy] autoprovision: 添加失败 status="
                     << resp.status << " " << resp.body;
        }
    }
};
