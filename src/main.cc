// Step 255（DatabaseInit + PG）+ Step 1475（启动 banner，运行日志可见）+ 813（NotifyTask 定时器）
// 截断在 CORS lambda 与 banner 输出。
//
// 入口流程：
//   1. 切到 exe 目录，读取 config.json
//   2. 打开 SQLite，可选连接 PostgreSQL
//   3. DatabaseInit::run()
//   4. 加载 JWT 配置
//   5. 注册 AdminAuthFilter
//   6. CORS
//   7. 启动定时器：NotifyTaskService::processPending() 每 30s
//   8. 输出 banner，drogon::app().run()

#include <drogon/drogon.h>
#include <json/json.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <csignal>
#include <thread>
#include <mutex>
#include <sstream>
#include <cstdlib>
#include <sstream>
#include <cstdlib>

#ifdef _WIN32
#  include <windows.h>
#endif

// 所有控制器、插件、过滤器、服务的 include 统一收拢到 AppIncludes.h
// 新增模块只需修改该文件，无需动 main.cc
#include "AppIncludes.h"
#ifdef WEPAY_EMBEDDED_FRONTEND
#include "common/EmbeddedFrontend.h"
#endif

#include "common/CrashHandler.h"
#include "common/ConfigGenerator.h"
#include "common/XpayBridge.h"
#include "common/AppContext.h"

int main(int argc, char *argv[]) {
    // 最先安装崩溃捕获（VEH/SEH/terminate/signal/Minidump 全套，输出到 logs/crash_*.log）
    CrashHandler::install("./logs");

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // 日志时间显示为本地时间（尽早设置，确保所有 LOG 输出都用本地时间）
    trantor::Logger::setDisplayLocalTime(true);

    // 全局 Job Object：主进程死则所有子进程跟死
    ProcessManager::init();

    // 初始化 libcurl (SyncHttp)
    SyncHttp::globalInit();

    // ── 切换到 exe 所在目录 ──────────────────────────────────────
    {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        auto exeDir = std::filesystem::path(buf).parent_path();
#else
        std::error_code ec;
        auto exeDir = std::filesystem::canonical("/proc/self/exe", ec).parent_path();
#endif
        if (!exeDir.empty()) std::filesystem::current_path(exeDir);
    }

    // ── 解析 --config ────────────────────────────────────────────
    std::string configFile = "config.json";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--config") { configFile = argv[i + 1]; break; }

    if (!ConfigGenerator::ensureConfig(configFile)) {
        return 1;
    }

    Json::Value cfg;
    {
        std::ifstream f(configFile);
        Json::CharReaderBuilder rb;
        std::string errs;
        if (!Json::parseFromStream(rb, f, &cfg, &errs)) {
            std::cerr << "[错误] config.json 解析失败: " << errs << std::endl;
            return 1;
        }
    }

    // 注册应用上下文（配置文件所在目录，供所有模块使用）
    AppContext::instance().setConfigPath(std::filesystem::absolute(configFile));
    AppContext::instance().setConfigDir(std::filesystem::absolute(configFile).parent_path());

    std::filesystem::create_directories("logs");
    std::filesystem::create_directories("upload");

    // ── JSON 日志（尽早初始化，捕获全部启动日志）──
    trantor::Logger::setDisplayLocalTime(true);
    {
        auto &logCfg = cfg["app"]["log"];
        std::string logDir  = logCfg.get("log_path", "./logs").asString();
        std::string logBase = logCfg.get("logfile_base_name", "wepay").asString();
        int logSize  = logCfg.get("log_size_limit", 52428800).asInt();
        int logKeep  = logCfg.get("log_keep_files", 5).asInt();
        JsonLogger::instance().init(logDir, logBase, logSize, logKeep);
    }

    // ── 数据库初始化：SQLite 作为本地库（可选启用）───────────────────
    bool sqliteEnabled = cfg["sqlite"].get("enabled", true).asBool();
    if (sqliteEnabled) {
        std::string dbPath = cfg["sqlite"].get("path", "wepay.db").asString();
        std::string dbEncKey = cfg["sqlite"].get("encrypt_key", "").asString();
        try {
            if (!dbEncKey.empty())
                PayDb::instance().setEncryptionKey(dbEncKey);
            PayDb::instance().open(dbPath);
        } catch (const std::exception &e) {
            std::cerr << "[错误] SQLite 初始化失败: " << e.what() << std::endl;
            return 1;
        }
    } else {
        LOG_INFO << "[PayDb] SQLite 已禁用，仅使用 PostgreSQL";
    }
    // 尝试连接 PG：成功 → PG 主库 + SQLite 从库; 失败 → 单 SQLite 模式
    if (cfg.isMember("pg") && cfg["pg"].isMember("connStr")) {
        std::string pgConn = cfg["pg"]["connStr"].asString();
        if (!pgConn.empty())
            PayDb::instance().connectPg(pgConn);
    }
    DatabaseInit::run();

    // ── 初始化设备密钥登录 (libsodium) ──────────────────────────
    try {
        DeviceKeyUtils::init();
        LOG_INFO << "[DeviceKey] libsodium 初始化成功";
    } catch (const std::exception &e) {
        LOG_ERROR << "[DeviceKey] libsodium 初始化失败: " << e.what();
        std::cerr << "[错误] 设备密钥登录初始化失败，该功能将不可用" << std::endl;
    }

    // ── 授权验证（必须通过才能启动）─────────────────────────────
    LicenseClient::exportFingerprintFile(); // 首次运行生成 license.lic 申请文件
    LicenseClient::instance().configure(cfg); // 读取可选的远程服务器配置
    if (!LicenseClient::instance().verify()) {
        LicenseClient::instance().printPanel();
        std::cerr << "\n[License] 授权验证失败，程序无法启动。\n" << std::endl;
        std::cout << "按回车键退出..." << std::endl;
        std::cin.get();
        return 1;
    }
    LicenseClient::instance().printPanel();

    // ── 同步已安装插件 (插件市场模式) ───────────────────────────
    ChannelPluginRegistry::instance().syncFromDb();

    // ── 启动 xpay-go 子进程 (HTTP 代理模式, 避免 Go runtime 冲突) ──
    {
        auto &xc = cfg["xpay_go"];
        bool xpayEnabled = xc.get("enabled", true).asBool();
        if (xpayEnabled) {
            std::string exePath  = xc.get("exe",    "./xpay-go/xpay.exe").asString();
            std::string cfgDir   = xc.get("cfg_dir","./xpay-go").asString();
            int         port     = xc.get("port",   8888).asInt();
            std::string backendUrl = "http://127.0.0.1:" + std::to_string(port);

            if (std::filesystem::exists(exePath)) {
                // 新版 xpay.exe 自动 os.Chdir 到 EXE 目录，无需显式设置 CWD
                ProcessManager::spawn(exePath, "", cfgDir, false, "xpay-go");
                XpayBridge::instance().configure(backendUrl);
                LOG_INFO << "[xpay-go] 子进程启动: " << exePath
                         << " (cwd=" << cfgDir << ") | proxy=" << backendUrl;
            } else {
                LOG_WARN << "[xpay-go] exe 未找到: " << exePath
                         << " — /xpay/** 路由将不可用";
            }
        }
    }

    // ── 缓存/消息队列(可选) ─────────────────────────────────────
    if (cfg.isMember("cache")) CacheService::instance().configure(cfg["cache"]);
    else                        CacheService::instance().configure(Json::Value{});
    if (cfg.isMember("mq"))    MqService::instance().configure(cfg["mq"]);

    // ── 通知通道(SMS/Email/OSS) ─────────────────────────────────
    if (cfg.isMember("sms"))   SmsService::instance().configure(cfg["sms"]);
    if (cfg.isMember("email")) EmailService::instance().configure(cfg["email"]);
    OssService::instance().configure(cfg.get("oss", Json::Value{}));

    // ── Drogon 配置 ─────────────────────────────────────────────
    drogon::app().loadConfigFile(configFile);

    // ── UPay 反向代理优先注册 (/upay/* → 8090) ──────────────────
    // 必须早于前端静态/Spa 404 托管，否则 /upay/* 可能被主站前端吞掉。
    UpayProxy::setup(cfg);

    // JsonLogger 已在启动最前面初始化，这里不需要重复

    SimpleJwt::load();
    // 注册所有 HttpMiddleware (isAutoCreation=false, 需手动注册)
    extern void registerWepayMiddlewares();
    registerWepayMiddlewares();

    // ── 前端托管（两种模式二选一）─────────────────────────────────
    // 模式A: "frontend"          — 外部 web/ 目录（方便热更新）
    // 模式B: "embedded_frontend" — 编译进 exe（单文件分发）
    {
        bool extEnabled = cfg.isMember("frontend") &&
                          cfg["frontend"].get("enabled", false).asBool();
        bool embEnabled = cfg.isMember("embedded_frontend") &&
                          cfg["embedded_frontend"].get("enabled", false).asBool();

        if (extEnabled && embEnabled) {
            std::cerr << "\n[错误] frontend 和 embedded_frontend 不能同时启用！\n"
                      << "  请在 config.json 中只启用其中一个。\n" << std::endl;
            return 1;
        }

        // 模式A: 外部目录托管
        if (extEnabled) {
            auto& fc = cfg["frontend"];
            std::string distPath  = fc.get("dist_path", "./web").asString();
            bool        spaMode   = fc.get("spa_mode", true).asBool();
            std::string apiPrefix = fc.get("api_prefix", "/prod-api").asString();
            int         cacheSec  = fc.get("cache_seconds", 3600).asInt();

            if (std::filesystem::exists(distPath) &&
                std::filesystem::exists(distPath + "/index.html")) {

                drogon::app().setDocumentRoot(distPath);
                drogon::app().setStaticFilesCacheTime(cacheSec);
                LOG_INFO << "[Frontend] 外部目录模式: " << std::filesystem::absolute(distPath).string()
                         << " | SPA=" << spaMode << " | API=" << apiPrefix
                         << " | cache=" << cacheSec << "s";

                if (!apiPrefix.empty() && apiPrefix != "/") {
                    drogon::app().registerPreRoutingAdvice(
                        [apiPrefix](const drogon::HttpRequestPtr& req,
                                    drogon::AdviceCallback&&,
                                    drogon::AdviceChainCallback&& ccb) {
                            std::string path = req->path();
                            if (path.rfind(apiPrefix, 0) == 0) {
                                std::string newPath = path.substr(apiPrefix.size());
                                if (newPath.empty()) newPath = "/";
                                req->setPath(newPath);
                            }
                            ccb();
                        });
                }

                // /cashier/ 子目录静态文件 — 在 SPA 404 之前拦截
                {
                    auto cashierPath = std::filesystem::absolute(std::filesystem::path(distPath) / "cashier");
                    std::string cashierDir = cashierPath.string();
                    if (std::filesystem::exists(cashierPath)) {
                        LOG_INFO << "[Frontend] Cashier assets: " << cashierDir;
                        drogon::app().registerPreRoutingAdvice(
                            [cashierDir](const drogon::HttpRequestPtr& req,
                                         drogon::AdviceCallback&& cb,
                                         drogon::AdviceChainCallback&& ccb) {
                                std::string path = req->path();
                                if (path.rfind("/cashier/", 0) == 0 && path.find("..") == std::string::npos) {
                                    std::string rel = path.substr(9); // 去掉 "/cashier/"
                                    if (rel.empty()) { ccb(); return; }
                                    // 用 filesystem::path 拼接，避免 Windows 路径分隔符问题
                                    auto fp = std::filesystem::path(cashierDir) / rel;
                                    if (std::filesystem::exists(fp) && std::filesystem::is_regular_file(fp)) {
                                        auto resp = drogon::HttpResponse::newFileResponse(fp.string());
                                        resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
                                        cb(resp);
                                        return;
                                    }
                                }
                                ccb();
                            });
                    } else {
                        LOG_WARN << "[Frontend] Cashier dir not found: " << cashierDir;
                    }
                }

                if (spaMode) {
                    std::string indexPath = std::filesystem::absolute(distPath + "/index.html").string();
                    drogon::app().setCustom404Page(
                        drogon::HttpResponse::newFileResponse(indexPath), false);
                }
            } else {
                LOG_WARN << "[Frontend] dist_path 不存在或缺少 index.html: " << distPath;
            }
        }

        // 模式B: 嵌入式（编译进 exe）
#ifdef WEPAY_EMBEDDED_FRONTEND
        if (embEnabled) {
            auto& ec = cfg["embedded_frontend"];
            bool        spaMode   = ec.get("spa_mode", true).asBool();
            std::string apiPrefix = ec.get("api_prefix", "/prod-api").asString();
            EmbeddedFrontend::registerHandlers(apiPrefix, spaMode);
        }
#else
        if (embEnabled) {
            std::cerr << "\n[错误] embedded_frontend 已启用，但本次编译未嵌入前端！\n"
                      << "  请用 cmake -DEMBED_FRONTEND=ON 重新编译。\n" << std::endl;
            return 1;
        }
#endif
    }

    // ── AgPay Plus 反向代理 (/plus/* → 内部端口) ────────────────
    AgPayProxy::setup(cfg);

    // ── Dujiao 嵌入 + SSO 反代 (/dujiao/* → dujiao.dll 后台 HTTP) ────
    DujiaoProxy::setup(cfg);

    // ── MPay v2 Webman 反向代理 (/mpay/* → PHP Workerman :8787) ────
    MpayProxy::setup(cfg);

    // ── PHP-Pay 项目反向代理 (/phppay/* → PHP 内置服务器 :61111) ──
    PhpPayProxy::setup(cfg);

    // ── SamWaf WAF 子进程托管 (/waf/ → SamWaf 管理后台) ────────────
    SamWafProxy::setup(cfg);

    // ── 进程内 nginx 功能（限流/IP黑白名单/反代/访问日志）+ HTTPS 跳转 ─
    // 取代外部 nginx：Windows/Linux 一致，无需安装/托管 nginx.exe。
    // AgPay 后台前端已由 AgPayProxy(/plus/*) 反代，不再依赖 nginx 多端口虚拟主机。
    // HTTPS 监听由 Drogon 原生 listeners(config.json cert/key/https) 处理。
    wepay::nginx_like::registerAll(cfg);
    wepay::HttpsRedirect::setup(cfg);

    // ── [已弃用] 外部 Nginx 子进程托管（仅 nginx.enabled=true 时启动）──
    // 默认 enabled=false。Linux 上无 watchdog 支持，建议改用上面的进程内方案。
    NginxProxy::setup(cfg);

    // ── AI 智能防护网关 + AI 助手初始化 ──────────────────────────
    AiSecurityGateway::setup(cfg);

    // ── KoboldCpp 本地 LLM 子进程托管 ─────────────────────────────
    KoboldCppManager::setup(cfg);

    // ── BEpusdt 加密货币支付网关 (/crypto/* → bepusdt.dll 内部 HTTP) ─
    BepusdtProxy::setup(cfg);

    // ── HashiCorp Vault 子进程托管 + 敏感信息初始化 ───────────────
    VaultProxy::setup(cfg);

    // ── 插件热加载：扫描 ./plugins/ 目录自动加载 ─────────────────
    {
        std::string pluginsDir = cfg.get("plugins_dir", "./plugins").asString();
        PluginManager::instance().loadAll(pluginsDir);
    }

    // ── 插件统一转发路由 /plugin/{name}/... ───────────────────────
    // 路由在启动时注册一次，插件热加载后无需重启即可响应请求
    drogon::app().registerHandler(
        "/plugin/{pluginName}/{path}",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb,
           const std::string& pluginName,
           const std::string& subPath) {
            auto& pm = PluginManager::instance();
            if (!pm.isLoaded(pluginName)) {
                Json::Value body; body["code"] = 404; body["msg"] = "插件不存在或未加载";
                auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
                resp->setStatusCode(drogon::k404NotFound);
                cb(resp); return;
            }
            // 构造 PluginRequest
            PluginRequest preq;
            preq.method   = req->methodString();
            preq.subPath  = subPath;
            preq.body     = std::string(req->body());
            preq.clientIp = req->getPeerAddr().toIp();
            for (auto& [k, v] : req->getParameters()) preq.params[k] = v;
            for (auto& [k, v] : req->getHeaders()) preq.headers[k] = v;

            PluginResponse presp;
            auto plugins = pm.list();
            for (auto& p : plugins) {
                if (p.meta.name == pluginName) {
                    try {
                        p.instance->handle(preq, presp);
                    } catch (const std::exception& e) {
                        presp.statusCode = 500;
                        presp.body = "{\"code\":500,\"msg\":\"" + std::string(e.what()) + "\"}";
                    }
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode((drogon::HttpStatusCode)presp.statusCode);
                    resp->setBody(presp.body);
                    resp->setContentTypeString(presp.contentType);
                    for (auto& [k, v] : presp.headers) resp->addHeader(k, v);
                    cb(resp); return;
                }
            }
            Json::Value body; body["code"] = 404; body["msg"] = "插件未找到";
            cb(drogon::HttpResponse::newHttpJsonResponse(body));
        },
        {drogon::Get, drogon::Post, drogon::Put, drogon::Delete});

    // ── CORS ────────────────────────────────────────────────────
    {
        std::vector<std::string> origins;
        std::string methods = "GET,POST,PUT,DELETE,OPTIONS";
        std::string headers = "*";
        bool credentials = false;
        if (cfg.isMember("cors")) {
            auto &c = cfg["cors"];
            if (c.isMember("allow_origins"))
                for (auto &o : c["allow_origins"]) origins.push_back(o.asString());
            if (c.isMember("allow_methods")) {
                std::string m;
                for (auto &v : c["allow_methods"]) { if (!m.empty()) m += ','; m += v.asString(); }
                methods = m;
            }
            if (c.isMember("allow_headers")) {
                std::string h;
                for (auto &v : c["allow_headers"]) { if (!h.empty()) h += ','; h += v.asString(); }
                headers = h;
            }
            credentials = c.get("allow_credentials", false).asBool();
        }
        drogon::app().registerPreHandlingAdvice(
            [](const drogon::HttpRequestPtr &req) {
                req->getAttributes()->insert("_req_start", std::chrono::steady_clock::now());
            });
        drogon::app().registerPostHandlingAdvice(
            [origins, methods, headers, credentials](
                const drogon::HttpRequestPtr &req, const drogon::HttpResponsePtr &resp) {
                std::string origin = req->getHeader("Origin");
                std::string allow = "*";
                if (!origins.empty()) {
                    if (origins.size() == 1 && origins[0] == "*") allow = "*";
                    else {
                        bool ok = false;
                        for (auto &o : origins) if (o == origin) { ok = true; break; }
                        if (ok) allow = origin;
                        else if (!origins.empty()) allow = origins[0];
                    }
                }
                resp->addHeader("Access-Control-Allow-Origin", allow);
                resp->addHeader("Access-Control-Allow-Methods", methods);
                resp->addHeader("Access-Control-Allow-Headers", headers);
                if (credentials) resp->addHeader("Access-Control-Allow-Credentials", "true");
                // 安全头
                SecurityHeaderFilter::addSecurityHeaders(req, resp);
                // 运维请求统计
                auto code = resp->statusCode();
                double ms = 0;
                try {
                    auto start = req->getAttributes()->get<std::chrono::steady_clock::time_point>("_req_start");
                    ms = std::chrono::duration<double,std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                } catch (...) {}
                OpsMetrics::instance().recordRequest((int)code, ms);
                // 5xx 错误自动记入内存错误日志
                if ((int)code >= 500) {
                    OpsMetrics::instance().addError(
                        std::string("[HTTP ") + std::to_string((int)code) + "] " +
                        req->methodString() + " " + req->path());
                }
            });
        // 处理 OPTIONS 预检
        drogon::app().registerHandler("/{path}",
            [](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb,
               const std::string &) {
                if (req->method() == drogon::Options) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(drogon::k204NoContent);
                    cb(r);
                } else {
                    auto r = drogon::HttpResponse::newNotFoundResponse();
                    cb(r);
                }
            }, {drogon::Options});
    }

    // ── CronService / MsgNotice 表初始化 ─────────────────────────
    CronService::initTables();
    MsgNoticeService::initTables();

    // ── GoPay 服务启动 ──────────────────────────────────────────────
#ifdef WEPAY_HAS_GOPAY
    {
        Json::Value gopayConfig = cfg.get("gopay", Json::Value());
        bool gopayEnabled = gopayConfig.get("enabled", false).asBool();
        
        if (gopayEnabled) {
            std::string gopayHost = gopayConfig.get("host", "127.0.0.1").asString();
            std::string gopayPort = gopayConfig.get("port", "9090").asString();
            
            LOG_INFO << "Starting GoPay service at " << gopayHost << ":" << gopayPort;
            
            if (GopayService::start(gopayHost, gopayPort)) {
                LOG_INFO << "✓ GoPay service started successfully";
            } else {
                LOG_WARN << "✗ Failed to start GoPay service - check if gopay.dll/gopay.so exists";
                LOG_WARN << "  Continuing without GoPay support";
            }
        } else {
            LOG_INFO << "GoPay service disabled in config (gopay.enabled=false)";
        }
    }
#else
    LOG_INFO << "GoPay support not compiled in (WEPAY_HAS_GOPAY not defined)";
#endif

#ifdef WEPAY_HAS_UPAY_SHARED
    {
        Json::Value upayConfig = cfg.get("upay_shared", Json::Value());
        bool upayEnabled = upayConfig.get("enabled", false).asBool();

        if (upayEnabled) {
            std::string libraryPath = upayConfig.get(
#ifdef _WIN32
                "library_path", "./upay_shared.dll"
#else
                "library_path", "./libupay_shared.so"
#endif
            ).asString();
            std::string baseUrl = upayConfig.get("base_url", "http://127.0.0.1:8090").asString();
            bool startServer = upayConfig.get("start_server", false).asBool();
            bool startCron = upayConfig.get("start_cron", false).asBool();

            LOG_INFO << "Starting UPay shared bridge from " << libraryPath
                     << " | base_url=" << baseUrl
                     << " | start_server=" << startServer
                     << " | start_cron=" << startCron;

            if (UpaySharedService::start(libraryPath, baseUrl, startServer, startCron)) {
                LOG_INFO << "✓ UPay shared bridge started successfully";
            } else {
                if (std::filesystem::exists(std::filesystem::absolute(libraryPath))) {
                    LOG_WARN << "✗ Failed to start UPay shared bridge - check shared library path and exported symbols";
                } else {
                    LOG_INFO << "UPay shared bridge skipped because optional library is missing: " << libraryPath;
                }
                LOG_WARN << "  Continuing without UPay shared support";
            }
        } else {
            LOG_INFO << "UPay shared bridge disabled in config (upay_shared.enabled=false)";
        }
    }
#else
    LOG_INFO << "UPay shared support not compiled in (WEPAY_HAS_UPAY_SHARED not defined)";
#endif

#ifdef WEPAY_HAS_V3
    // ── WePay V3 原生监控插件 ─────────────────────────────────────
    if (!wepay::v3::WepayV3Plugin::getInstance().init(configFile)) {
        LOG_WARN << "[V3] 插件初始化失败";
    }
#endif

    // ── 网页流水监听服务 ──────────────────────────────────────────
    ReceiptWatcherService::setup();

    // ── 后台定时任务 ─────────────────────────────────────────────
    drogon::app().getLoop()->runEvery(30.0, [] {
        try { NotifyTaskService::processPending(); }
        catch (const std::exception &e) {
            std::cerr << "[NotifyTask] " << e.what() << std::endl;
        }
    });
    // 每 30s 关闭过期未支付订单
    drogon::app().getLoop()->runEvery(60.0, [] {
        try { CronService::closeExpiredOrders(); }
        catch (const std::exception &e) {
            std::cerr << "[Cron:expire] " << e.what() << std::endl;
        }
    });
    // 每 30s: 飞鹅云打印任务重试
    drogon::app().getLoop()->runEvery(30.0, [] {
        try { PrintTaskService::processPending(); }
        catch (const std::exception &e) {
            std::cerr << "[PrintTask] " << e.what() << std::endl;
        }
    });
    // 每 5 分钟: 清理 + 补单检查
    drogon::app().getLoop()->runEvery(300.0, [] {
        try {
            TokenService::cleanup();
            CacheService::instance().cleanup();
            LoginAttemptService::cleanup();
            DeviceChallengeCache::instance().cleanup();  // 清理过期挑战值
            CronService::checkPendingOrders();
            RiskControlService::cleanup();
        } catch (const std::exception &e) {
            std::cerr << "[Cleanup] " << e.what() << std::endl;
        }
    });
    // 每 30s: 邮件通知免签 IMAP 轮询
    drogon::app().getLoop()->runEvery(30.0, [] {
        try { AlipayEmailPlugin::pollAllChannels(); }
        catch (const std::exception &e) {
            LOG_ERROR << "[AlipayEmail] " << e.what();
        }
    });

    // 每小时检查一次，过了 0:30 则生成昨日账单（同一天只跑一次）
    drogon::app().getLoop()->runEvery(3600.0, [] {
        try {
            time_t now = std::time(nullptr);
            struct tm t;
#ifdef _WIN32
            localtime_s(&t, &now);
#else
            localtime_r(&now, &t);
#endif
            if (t.tm_hour == 0) {
                // 00:01 通道日额度重置
                CronService::resetChannelDayQuota();
                // 00:30 每日统计 + 账单生成
                char today[16];
                std::strftime(today, sizeof(today), "%Y-%m-%d", &t);
                std::string yesterdayKey = std::string("bill_gen_") + today;
                if (PayDb::instance().getSetting(yesterdayKey).empty()) {
                    CronService::dailyOrderStats();
                    BillService::generateYesterdayBills();
                    PayDb::instance().setSetting(yesterdayKey, std::to_string(now));
                }
            } else if (t.tm_hour == 2) {
                // 02:00 自动结算
                CronService::autoSettle();
            } else if (t.tm_hour == 3) {
                // 03:00 清理过期数据
                CronService::cleanupExpiredData();
            } else if (t.tm_hour == 4) {
                // 04:00 自动转账
                CronService::autoTransfer();
            } else {
                return;
            }
        } catch (const std::exception &e) {
            std::cerr << "[BillCron] " << e.what() << std::endl;
        }
    });

    // 每 10 分钟加密落盘 SQLite（仅在启用加密时生效）
    drogon::app().getLoop()->runEvery(600.0, [] {
        try { PayDb::instance().flushEncrypt(); }
        catch (...) {}
    });

    // 运维自动检测：启动 5s 后执行一次，之后每 5 分钟执行一次
    drogon::app().getLoop()->runAfter(5.0, [] {
        try {
            auto alerts = OpsService::runAutoCheck();
            auto &metrics = OpsMetrics::instance();
            for (auto &a : alerts) metrics.addAlert(a.level, a.source, a.message);
            if (!alerts.empty())
                LOG_WARN << "[OpsAutoCheck] 启动检测发现 " << alerts.size() << " 条告警";
        } catch (...) {}
    });
    drogon::app().getLoop()->runEvery(300.0, [] {
        try {
            auto alerts = OpsService::runAutoCheck();
            auto &metrics = OpsMetrics::instance();
            for (auto &a : alerts) metrics.addAlert(a.level, a.source, a.message);
        } catch (...) {}
    });

    // ── Banner ─────────────────────────────────────────────────
    int port = 8088;
    if (!cfg["listeners"].empty())
        port = cfg["listeners"][0].get("port", 8088).asInt();

    {
        auto pluginList = ChannelPluginRegistry::instance().listPlugins();
        std::string pluginsStr;
        for (size_t i = 0; i < pluginList.size(); ++i) {
            if (i > 0) pluginsStr += ", ";
            pluginsStr += pluginList[i];
        }
        Banner::print(port, PayDb::instance().backendInfo(), pluginsStr);
    }

#ifdef WEPAY_HAS_NACOS
    // ── Nacos 服务注册 ──────────────────────────────────────────
    if (!cfg["nacos"].empty() && cfg["nacos"].get("enabled", false).asBool()) {
        auto &nc = cfg["nacos"];
        int nacosPort = nc.get("port", 0).asInt();
        if (nacosPort <= 0) nacosPort = port;  // 0 = 跟随 listeners[0].port
        NacosService::instance().init(
            nc.get("server",            "127.0.0.1:8848").asString(),
            nc.get("service_name",      "wepay-cpp").asString(),
            nc.get("ip",               "127.0.0.1").asString(),
            nacosPort,
            nc.get("group",             "DEFAULT_GROUP").asString(),
            nc.get("namespace",         "").asString(),
            nc.get("heartbeat_seconds", 5).asInt()
        );
    }
#endif

    // ── xpay-go 全路径代理 /xpay/** (PreRoutingAdvice 拦截任意深度) ──
    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr &req,
           drogon::AdviceCallback &&cb,
           drogon::AdviceChainCallback &&ccb) {
            const std::string prefix = "/xpay";
            std::string path = req->path();
            if (path.compare(0, prefix.size(), prefix) != 0 ||
                (path.size() > prefix.size() && path[prefix.size()] != '/')) {
                ccb(); return;  // 非 /xpay 路径，继续正常路由
            }
            auto &bridge = XpayBridge::instance();
            if (!bridge.ready()) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k503ServiceUnavailable);
                r->setBody("xpay-go not ready");
                cb(r); return;
            }
            std::string p = path.substr(prefix.size());
            if (p.empty()) p = "/";
            std::string q = req->getQuery();
            std::string body(req->getBody());
            std::map<std::string, std::string> hdrs;
            for (auto &[k, v] : req->headers()) {
                std::string lk = k;
                std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                if (lk == "host" || lk == "content-length" ||
                    lk == "connection" || lk == "transfer-encoding") continue;
                hdrs[k] = v;
            }
            hdrs["X-Forwarded-For"] = req->getPeerAddr().toIp();
            auto br = bridge.request(req->methodString(), p, q, body, hdrs);
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode((drogon::HttpStatusCode)(br.status > 0 ? br.status : 502));
            resp->setBody(std::move(br.body));
            cb(resp);
        });

    // drogon 的 run() 会启动 AsyncFileLogger 并覆盖 setOutputFunction，
    // 照搬 server 版策略：在 beginningAdvice 中三次 rebind 强覆盖
    drogon::app().registerBeginningAdvice([] {
        JsonLogger::rebind();
        JsonLogger::instance().setConsole(false);   // 启动完成，运行时日志只写文件
        drogon::app().getLoop()->runAfter(0.1, [] { JsonLogger::rebind(); });
        drogon::app().getLoop()->runAfter(1.0, [] { JsonLogger::rebind(); });
    });
    drogon::app().run();
#ifdef WEPAY_HAS_NACOS
    NacosService::instance().shutdown();
#endif
    BepusdtProxy::shutdown();
    DujiaoProxy::shutdown();
    MpayProxy::shutdown();
#ifdef WEPAY_HAS_V3
    wepay::v3::WepayV3Plugin::getInstance().shutdown();
#endif
    return 0;
}
