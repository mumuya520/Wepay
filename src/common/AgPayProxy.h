// WePay-Cpp — AgPay 代理
// AgPayProxy.h — 将 /plus/* 透明代理到 AgPay Plus 后端
// 子进程通过全局 ProcessManager（单一 Job Object）托管，主进程死则全部跟死
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h> // JSON 库
#include <string> // 字符串库
#include <vector> // 向量容器
#include <atomic> // 原子操作
#include <thread>
#include <mutex>
#include <algorithm>
#include "ProcessManager.h"

#ifdef _WIN32
#  include <windows.h>
#endif

// AgPay 子服务结构体
// 用于描述一个 AgPay Plus 后端服务（pay/merchant/manager/agent）
struct AgPayService {
    // 服务名称（如 pay、merchant、manager、agent）
    std::string name;
    // 可执行文件路径（.NET DLL 或 EXE）
    std::string exe;
    // 路由前缀（如 /plus/pay、/plus/merchant）
    std::string prefix;
    // 监听端口（如 5819、5818、5817、5816）
    int         port{0};
    // 是否显示控制台窗口（Windows 专用）
    bool        console{false};
// 平台特定字段
#ifdef _WIN32
    // Windows 进程句柄
    HANDLE      hProcess{INVALID_HANDLE_VALUE};
#else
    // Unix 进程 ID
    int         pid{0};
#endif
};

// AgPay Plus 代理类
// 职责：
//   1. 管理多个 AgPay Plus 后端服务（pay/merchant/manager/agent）
//   2. 支持自动启动和看门狗监控
//   3. 为每个服务注册独立的反向代理路由
//   4. 通过全局 ProcessManager 托管子进程
class AgPayProxy {
public:
    // 初始化 AgPay 代理
    // 参数 cfg：配置对象（JSON）
    // 配置项：
    //   agpayplus.enabled - 启用 AgPay Plus（默认 false）
    //   agpayplus.prefix - 全局路由前缀（默认 /plus）
    //   agpayplus.backend - 后端 URL（仅用于兼容旧配置）
    //   agpayplus.autostart - 自动启动所有服务（默认 false）
    //   agpayplus.watchdog - 启用看门狗监控（默认 false）
    //   agpayplus.watchdog_interval_sec - 看门狗检查间隔（秒，默认 10）
    //   agpayplus.services - 服务数组（新配置方式）
    //     - services[].name - 服务名称
    //     - services[].exe - 可执行文件路径
    //     - services[].port - 监听端口
    //     - services[].prefix - 路由前缀（可选，默认继承全局 prefix）
    //     - services[].console - 显示控制台（Windows 专用）
    // 功能：
    //   1. 解析配置，收集所有子服务
    //   2. 自动启动服务（如果启用）
    //   3. 启动看门狗线程监控服务健康状态
    //   4. 为每个服务注册反向代理路由
    static void setup(const Json::Value &cfg) {
        // 检查是否配置了 agpayplus
        if (!cfg.isMember("agpayplus"))
            return;
        // 获取 agpayplus 配置
        const auto &c = cfg["agpayplus"];
        // 检查是否启用
        if (!c.get("enabled", false).asBool())
            return;

        // ── 解析全局配置 ──────────────────────────────
        // 全局路由前缀
        std::string prefix  = c.get("prefix",  "/plus").asString();
        // 后端 URL（仅用于兼容旧配置）
        std::string backend = c.get("backend", "http://127.0.0.1:5819").asString();
        // 是否自动启动
        bool autostart      = c.get("autostart", false).asBool();
        // 是否启用看门狗
        bool watchdog       = c.get("watchdog",  false).asBool();
        // 看门狗检查间隔（秒）
        int  interval       = c.get("watchdog_interval_sec", 10).asInt();

        // ── 清理前缀末尾的斜杠 ──────────────────────────────
        while (!prefix.empty() && prefix.back() == '/')
            prefix.pop_back();

        // ── 收集所有子服务 ──────────────────────────────
        // 检查是否配置了服务数组
        if (c.isMember("services") && c["services"].isArray()) {
            // 遍历所有服务配置
            for (const auto &s : c["services"]) {
                // 创建服务对象
                AgPayService svc;
                // 设置服务名称
                svc.name = s.get("name", "").asString();
                // 设置可执行文件路径（支持 exe 和 dll 两种配置）
                svc.exe     = s.get("exe",  s.get("dll", "").asString()).asString();
                // 设置监听端口
                svc.port    = s.get("port", 0).asInt();
                // 设置是否显示控制台
                svc.console = s.get("console", false).asBool();
                // ── 设置服务独立代理前缀 ──────────────────────────────
                // 如果未配置，继承全局 prefix
                svc.prefix  = s.get("prefix", prefix).asString();
                // 清理末尾斜杠
                while (!svc.prefix.empty() && svc.prefix.back() == '/')
                    svc.prefix.pop_back();
                // 添加到服务列表
                services_.push_back(std::move(svc));
            }
        } else {
            // ── 兼容旧单服务配置 ──────────────────────────────
            // 支持 dotnet_dll 或 exe 配置项
            std::string exe = c.get("dotnet_dll", c.get("exe", "").asString()).asString();
            // 如果配置了可执行文件
            if (!exe.empty()) {
                // 创建默认 pay 服务
                AgPayService svc;
                svc.name = "pay";
                svc.exe = exe;
                svc.port = 5819;
                // 添加到服务列表
                services_.push_back(std::move(svc));
            }
        }

        // ── 自动启动所有服务 ──────────────────────────────
        if (autostart) {
            // 遍历所有服务
            for (auto &svc : services_) {
                // 启动服务
                launchService(svc);
            }
            // 等待所有服务就绪（5 秒）
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        // ── 启动看门狗线程 ──────────────────────────────
        // 看门狗功能：定期检查服务是否运行，如果崩溃则自动重启
        if (watchdog && autostart) {
            // 启动看门狗线程（分离线程）
            std::thread([interval]() {
                // 无限循环
                while (true) {
                    // 等待指定间隔
                    std::this_thread::sleep_for(std::chrono::seconds(interval));
                    // 加锁保护共享资源
                    std::lock_guard<std::mutex> lk(mtx_);
                    // 遍历所有服务
                    for (auto &svc : services_) {
                        // 检查可执行文件是否存在
                        if (!ProcessManager::exeExists(svc.exe))
                            continue;
                        // 检查服务是否运行
                        if (!isRunning(svc)) {
                            // 记录警告日志
                            LOG_WARN << "[AgPayProxy] " << svc.name << " 已停止，正在重启...";
                            // 重启服务
                            launchService(svc);
                        }
                    }
                }
            }).detach();
        }

        // 记录日志
        LOG_INFO << "[AgPayProxy] " << services_.size() << " services registered";

        // ── 为每个服务注册反向代理路由 ──────────────────────────────
        for (const auto &svc : services_) {
            // 初始化协议和主机
            std::string scheme = "http", host = "127.0.0.1";
            // 服务端口
            int         svcPort = svc.port;
            // 构建服务后端 URL
            std::string svcBackend = "http://127.0.0.1:" + std::to_string(svc.port);
            // 解析后端 URL（提取协议、主机、端口）
            parseBackend(svcBackend, scheme, host, svcPort);
            // 保存路由前缀和服务名称（用于 lambda 捕获）
            const std::string pfx     = svc.prefix;
            const std::string svcName = svc.name;

            // 记录反向代理配置日志
            LOG_INFO << "[AgPayProxy] " << pfx << " → http://127.0.0.1:" << svcPort;

            // ── 注册前置路由建议 ──────────────────────────────
            drogon::app().registerPreRoutingAdvice(
                [pfx, scheme, host, svcPort, svcName]
                (const drogon::HttpRequestPtr &req,
                 drogon::AdviceCallback        &&cb,
                 drogon::AdviceChainCallback   &&ccb)
                {
                    // ── 检查路由是否匹配 ──────────────────────────────
                    // 获取请求路径
                    const std::string &path = req->path();
                    // 检查路径是否匹配前缀
                    bool match = (path == pfx) ||
                                 (path.size() > pfx.size() && path[pfx.size()] == '/' &&
                                  path.rfind(pfx, 0) == 0);
                    // 如果不匹配，继续处理下一个路由
                    if (!match) {
                        ccb();
                        return;
                    }

                    // ── 提取子路径 ──────────────────────────────
                    // 提取前缀之后的路径
                    std::string subPath = path.substr(pfx.size());
                    // 如果子路径为空，设置为 /
                    if (subPath.empty())
                        subPath = "/";
                    // ── .NET SPA 入口处理 ──────────────────────────────
                    // .NET 默认未启用 UseDefaultFiles，需要手动改写 / 为 /index.html
                    // 这样 wwwroot 里的 SPA 入口才能被命中
                    if (subPath == "/")
                        subPath = "/index.html";
                    // 获取查询字符串
                    std::string qs = req->query();
                    // 如果有查询字符串，添加到子路径
                    if (!qs.empty())
                        subPath += "?" + qs;

                    // ── 创建转发请求 ──────────────────────────────
                    auto fwdReq = drogon::HttpRequest::newHttpRequest();
                    // 设置请求方法
                    fwdReq->setMethod(req->method());
                    // 设置请求路径
                    fwdReq->setPath(subPath);
                    // 复制请求头（跳过 Host）
                    for (auto &[k, v] : req->headers()) {
                        // 转换为小写进行比较
                        std::string lk = k;
                        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                        // 跳过 Host 头
                        if (lk == "host")
                            continue;
                        // 添加请求头
                        fwdReq->addHeader(k, v);
                    }
                    // 获取请求体
                    std::string body(req->body());
                    // 如果有请求体
                    if (!body.empty()) {
                        // 设置请求体
                        fwdReq->setBody(std::move(body));
                        // 获取 Content-Type 头
                        std::string ct = req->getHeader("content-type");
                        // 如果有 Content-Type，添加到转发请求
                        if (!ct.empty())
                            fwdReq->addHeader("Content-Type", ct);
                    }

                    // ── 发送转发请求 ──────────────────────────────
                    // 构建后端 URL
                    std::string backendUrl = scheme + "://" + host + ":" + std::to_string(svcPort);
                    // 创建 HTTP 客户端
                    auto client = drogon::HttpClient::newHttpClient(backendUrl);
                    // 发送转发请求
                    client->sendRequest(fwdReq,
                        [cb = std::move(cb), svcName](drogon::ReqResult result,
                                             const drogon::HttpResponsePtr &resp) {
                            // 如果请求成功且有响应
                            if (result == drogon::ReqResult::Ok && resp) {
                                // ── 重新构造响应 ──────────────────────────────
                                // 剥掉 hop-by-hop 头，避免重复 Content-Length 等
                                auto r = drogon::HttpResponse::newHttpResponse();
                                // 设置状态码
                                r->setStatusCode(resp->statusCode());
                                // 保存 Content-Type（需要单独处理）
                                std::string upstreamCT;
                                // 复制响应头（过滤掉 hop-by-hop 头）
                                for (auto &kv : resp->headers()) {
                                    // 转换为小写进行比较
                                    std::string lk = kv.first;
                                    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                                    // 跳过 hop-by-hop 头
                                    if (lk == "content-length" || lk == "transfer-encoding" ||
                                        lk == "connection" || lk == "keep-alive" ||
                                        lk == "proxy-authenticate" || lk == "proxy-authorization" ||
                                        lk == "te" || lk == "trailers" || lk == "upgrade")
                                        continue;
                                    // 单独处理 Content-Type
                                    if (lk == "content-type") {
                                        upstreamCT = kv.second;
                                        continue;
                                    }
                                    // 添加响应头
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
                                // ── 请求失败，返回 502 Bad Gateway ──────────────────────────────
                                auto r = drogon::HttpResponse::newHttpResponse();
                                // 设置状态码
                                r->setStatusCode(drogon::k502BadGateway);
                                // 设置 Content-Type
                                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                                // 设置错误消息
                                r->setBody("{\"code\":502,\"msg\":\"" + svcName + " 服务不可用，请稍后重试\"}");
                                // 返回响应
                                cb(r);
                            }
                        });
                });
        }
    }

// 私有区域
private:
    // 所有服务列表
    static inline std::vector<AgPayService> services_;
    // 互斥锁（保护 services_ 的并发访问）
    static inline std::mutex mtx_;

    // 启动服务
    // 参数 svc：要启动的服务
    // 功能：
    //   1. 关闭旧进程（如果存在）
    //   2. 通过 ProcessManager 启动新进程
    //   3. 记录启动结果日志
    static void launchService(AgPayService &svc) {
// 平台特定实现
#ifdef _WIN32
        // ── Windows 实现 ──────────────────────────────
        // 如果旧进程句柄有效
        if (svc.hProcess != INVALID_HANDLE_VALUE) {
            // 关闭旧进程句柄
            CloseHandle(svc.hProcess);
            // 标记为无效
            svc.hProcess = INVALID_HANDLE_VALUE;
        }
        // 通过 ProcessManager 启动新进程
        svc.hProcess = ProcessManager::spawn(svc.exe, "", "", svc.console, "agpay:" + svc.name);
        // 检查启动是否成功
        if (svc.hProcess == INVALID_HANDLE_VALUE)
            LOG_WARN << "[AgPayProxy] 启动 " << svc.name << " 失败";
#else
        // ── Unix 实现 ──────────────────────────────
        // 通过 ProcessManager 启动新进程
        svc.pid = ProcessManager::spawn(svc.exe, "", "", false, "agpay:" + svc.name);
        // 记录启动日志
        LOG_INFO << "[AgPayProxy] 已启动 " << svc.name;
#endif
    }

    // 检查服务是否运行
    // 参数 svc：要检查的服务
    // 返回：服务是否运行
    static bool isRunning(const AgPayService &svc) {
// 平台特定实现
#ifdef _WIN32
        // ── Windows 实现 ──────────────────────────────
        // 通过 ProcessManager 检查进程是否活着
        return ProcessManager::isAlive(svc.hProcess);
#else
        // ── Unix 实现 ──────────────────────────────
        // 检查 PID 是否有效且进程存在
        return svc.pid > 0 && ::kill(svc.pid, 0) == 0;
#endif
    }

    // 解析后端 URL
    // 参数 url：后端 URL（如 http://127.0.0.1:5819）
    // 参数 scheme：输出参数，协议（http/https）
    // 参数 host：输出参数，主机名
    // 参数 port：输出参数，端口号
    // 功能：
    //   1. 解析 URL 的协议、主机、端口
    //   2. 如果未指定端口，使用默认端口（http=80, https=443）
    static void parseBackend(const std::string &url,
                             std::string &scheme, std::string &host, int &port) {
        // 查找 :// 分隔符
        size_t p = url.find("://");
        // 如果找到分隔符
        if (p != std::string::npos) {
            // 提取协议
            scheme = url.substr(0, p);
            // 提取 :// 之后的部分
            std::string rest = url.substr(p + 3);
            // 查找最后一个冒号（端口分隔符）
            size_t cp = rest.rfind(':');
            // 如果找到冒号
            if (cp != std::string::npos) {
                // 提取主机名
                host = rest.substr(0, cp);
                // 提取端口号
                try {
                    port = std::stoi(rest.substr(cp + 1));
                } catch (...) {
                    // 转换失败，保持默认端口
                }
            } else {
                // 未指定端口，只有主机名
                host = rest;
                // 使用默认端口
                port = (scheme == "https") ? 443 : 80;
            }
        }
    }
};
