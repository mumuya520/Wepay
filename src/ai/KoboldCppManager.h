// KoboldCppManager.h — KoboldCpp 子进程生命周期管理
// 读取 config.json["koboldcpp"]，启动 koboldcpp.py（或指定的 bat/exe），
// 后台线程每 15s 做健康检查，失败则自动重启。
// 管理接口（需要 JWT）:
//   GET  /admin/api/ai/kobold/status   — 进程状态 + 模型信息
//   POST /admin/api/ai/kobold/restart  — 手动重启
//   POST /admin/api/ai/kobold/stop     — 手动停止
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <map> // 映射容器
#include <thread> // 线程库
#include <atomic> // 原子操作
#include <chrono> // 时间库
#include <filesystem> // 文件系统库
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <json/json.h> // JSON 库
#include <trantor/utils/Logger.h> // 日志库
#include "../common/ProcessManager.h" // 进程管理器
#include "../common/SimpleJwt.h" // JWT 令牌处理
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "KoboldCppService.h" // KoboldCpp 服务

// KoboldCpp 管理器配置结构体
struct KoboldCppMgrConfig {
        bool        enabled     = false; // 是否启用
        bool        external    = false;          // true: 外部已启动，本程序只做健康检查
        std::string launchCmd;           // 优先：cmd /c xxx.bat 或 xxx.exe
        std::string pythonExe   = "python"; // Python 可执行文件
        std::string scriptPath;          // koboldcpp.py 路径
        std::string modelPath; // 模型文件路径
        std::string mmProjPath;          // 多模态投影层（可选）
        std::string host        = "127.0.0.1"; // 服务主机
        int         port        = 5001; // 服务端口
        int         threads     = 8; // 线程数
        int         contextSize = 4096; // 上下文大小
        int         gpuLayers   = 99; // GPU 层数
        bool        useGpu      = true; // 是否使用 GPU
        std::string gpuMode     = "normal mmq"; // cublas 模式
        std::string workDir; // 工作目录
        bool        autoRestart = true; // 是否自动重启
        bool        showWindow  = false; // 是否显示窗口
};

class KoboldCppManager : public drogon::HttpController<KoboldCppManager> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(KoboldCppManager::status,  "/admin/api/ai/kobold/status",  drogon::Get);
        ADD_METHOD_TO(KoboldCppManager::restart, "/admin/api/ai/kobold/restart", drogon::Post);
        ADD_METHOD_TO(KoboldCppManager::stop,    "/admin/api/ai/kobold/stop",    drogon::Post);
    METHOD_LIST_END

    using Config = KoboldCppMgrConfig;

    KoboldCppManager() = default;
    static KoboldCppManager& instance() { static KoboldCppManager i; return i; }

    static void setup(const Json::Value& appCfg) {
        if (!appCfg.isMember("koboldcpp")) return;
        const auto& c = appCfg["koboldcpp"];
        auto& mgr = instance();
        auto& cfg = mgr.cfg_;

        cfg.enabled     = c.get("enabled", false).asBool();
        if (!cfg.enabled) return;

        cfg.launchCmd   = c.get("launch_cmd",    "").asString();
        cfg.pythonExe   = c.get("python_exe",    "python").asString();
        cfg.scriptPath  = c.get("script_path",   "").asString();
        cfg.modelPath   = c.get("model_path",     "").asString();
        cfg.mmProjPath  = c.get("mmproj_path",    "").asString();
        cfg.host        = c.get("host",           "127.0.0.1").asString();
        cfg.port        = c.get("port",           5001).asInt();
        cfg.threads     = c.get("threads",        8).asInt();
        cfg.contextSize = c.get("context_size",   4096).asInt();
        cfg.gpuLayers   = c.get("gpu_layers",     99).asInt();
        cfg.useGpu      = c.get("use_gpu",        true).asBool();
        cfg.gpuMode     = c.get("gpu_mode",       "normal mmq").asString();
        cfg.workDir     = c.get("work_dir",       "").asString();
        cfg.autoRestart = c.get("auto_restart",   true).asBool();
        cfg.showWindow  = c.get("show_window",    false).asBool();
        cfg.external    = c.get("external",       false).asBool();

        KoboldCppService::instance().setPort(cfg.port);

        if (cfg.external) {
            LOG_INFO << "[KoboldCppManager] 外部模式启用：不拉起子进程，仅连接 "
                     << cfg.host << ":" << cfg.port;
            mgr.startHealthCheck();           // 只检查不重启
        } else {
            mgr.doStart();
            mgr.startWatchdog();
            LOG_INFO << "[KoboldCppManager] 启动完成，端口=" << cfg.port;
        }
    }

    // ── 管理 API ────────────────────────────────────────────────────────────
    void status(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!auth(req)) { RESP_401(cb); return; }
        bool ready = KoboldCppService::instance().isReady();
        Json::Value d;
        d["enabled"]   = cfg_.enabled;
        d["external"]  = cfg_.external;
#ifdef _WIN32
        d["running"]   = cfg_.external ? ready
                                       : (proc_ != INVALID_HANDLE_VALUE && proc_ != nullptr);
#else
        d["running"]   = ready;
#endif
        d["ready"]     = ready;
        d["port"]      = cfg_.port;
        d["model"]     = std::filesystem::path(cfg_.modelPath).filename().string();
        d["use_gpu"]   = cfg_.useGpu;
        d["gpu_layers"]= cfg_.gpuLayers;
        d["restarts"]  = restartCount_.load();
        RESP_OK(cb, d);
    }

    void restart(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!auth(req)) { RESP_401(cb); return; }
        doStop();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        doStart();
        Json::Value d; d["restarted"] = true;
        RESP_OK(cb, d);
    }

    void stop(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!auth(req)) { RESP_401(cb); return; }
        watchdogActive_ = false;
        doStop();
        Json::Value d; d["stopped"] = true;
        RESP_OK(cb, d);
    }

private:
    inline static Config            cfg_;
#ifdef _WIN32
    inline static HANDLE            proc_           = INVALID_HANDLE_VALUE;
#endif
    inline static std::atomic<bool> watchdogActive_;
    inline static std::atomic<int>  restartCount_;

    // ── 构建启动参数（不含 python.exe 本身）────────────────────────────
    std::string buildArgs() const {
        std::string args = "\"" + cfg_.scriptPath + "\"";
        args += " --model \""     + cfg_.modelPath + "\"";
        if (!cfg_.mmProjPath.empty())
            args += " --mmproj \"" + cfg_.mmProjPath + "\"";
        args += " --host "        + cfg_.host;
        args += " --port "        + std::to_string(cfg_.port);
        args += " --threads "     + std::to_string(cfg_.threads);
        args += " --contextsize " + std::to_string(cfg_.contextSize);
        if (cfg_.useGpu) {
            args += " --usecublas " + cfg_.gpuMode;
            args += " --gpulayers " + std::to_string(cfg_.gpuLayers);
        }
        args += " --skiplauncher";
        return args;
    }

    void doStart() {
        if (!cfg_.enabled || cfg_.modelPath.empty()) return;
#ifdef _WIN32
        if (!std::filesystem::exists(cfg_.scriptPath) &&
            cfg_.launchCmd.empty()) {
            LOG_WARN << "[KoboldCppManager] script_path 不存在: " << cfg_.scriptPath;
            return;
        }
        // launchCmd 模式：用户给的是完整 cmd，走 cmd /c
        if (!cfg_.launchCmd.empty()) {
            LOG_INFO << "[KoboldCppManager] 启动 (cmd /c): " << cfg_.launchCmd;
            proc_ = ProcessManager::spawn("cmd.exe", "/c " + cfg_.launchCmd,
                                          cfg_.workDir, cfg_.showWindow, "koboldcpp");
        } else {
            // 标准模式：通过 cmd.exe /c 让 shell 在 PATH 里查 python
            std::string args = "/c \"" + cfg_.pythonExe + "\" " + buildArgs();
            LOG_INFO << "[KoboldCppManager] 启动: cmd.exe " << args;
            proc_ = ProcessManager::spawn("cmd.exe", args,
                                          cfg_.workDir, cfg_.showWindow, "koboldcpp");
        }
        if (proc_ == INVALID_HANDLE_VALUE || proc_ == nullptr)
            LOG_WARN << "[KoboldCppManager] 进程启动失败";
#else
        LOG_WARN << "[KoboldCppManager] Linux 暂不支持进程托管";
#endif
    }

    void doStop() {
#ifdef _WIN32
        if (proc_ != INVALID_HANDLE_VALUE && proc_ != nullptr) {
            TerminateProcess(proc_, 0);
            CloseHandle(proc_);
            proc_ = INVALID_HANDLE_VALUE;
            LOG_INFO << "[KoboldCppManager] 进程已停止";
        }
#endif
    }

    // 外部模式：仅做连通性检测，不重启进程
    void startHealthCheck() {
        watchdogActive_ = true;
        std::thread([this] {
            bool wasReady = false;
            while (watchdogActive_) {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                if (!watchdogActive_) break;
                bool ok = KoboldCppService::instance().isReady();
                if (ok && !wasReady) {
                    LOG_INFO << "[KoboldCppManager] 外部 LLM 已连通: "
                             << cfg_.host << ":" << cfg_.port;
                } else if (!ok && wasReady) {
                    LOG_WARN << "[KoboldCppManager] 外部 LLM 失联（请检查 "
                             << cfg_.host << ":" << cfg_.port << "）";
                }
                wasReady = ok;
            }
        }).detach();
    }

    void startWatchdog() {
        if (!cfg_.autoRestart) return;
        watchdogActive_ = true;
        std::thread([this] {
            int failCount = 0;
            while (watchdogActive_) {
                std::this_thread::sleep_for(std::chrono::seconds(15));
                if (!watchdogActive_) break;
                bool ok = KoboldCppService::instance().isReady();
                if (!ok) {
                    ++failCount;
                    if (failCount >= 2) {  // 连续两次失败再重启（给模型加载留时间）
                        LOG_WARN << "[KoboldCppManager] 服务不可达，自动重启...";
                        doStop();
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        doStart();
                        ++restartCount_;
                        failCount = 0;
                    }
                } else {
                    failCount = 0;
                }
            }
        }).detach();
    }

    static bool auth(const drogon::HttpRequestPtr& req) {
        std::string a = req->getHeader("Authorization");
        if (a.empty()) a = req->getHeader("authorization");
        if (a.empty()) return false;
        try { SimpleJwt::verify(SimpleJwt::fromHeader(a)); return true; }
        catch (...) { return false; }
    }
};
