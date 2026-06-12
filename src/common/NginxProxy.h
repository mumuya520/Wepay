// WePay-Cpp — Nginx 代理管理
// NginxProxy.h — 将 nginx 作为子进程托管
// 用于反向代理官方 AgPay 前端 (jeepay-ui-mgr / mch / agent)
// 配置项 nginx.autostart=true 时，WePay 启动时自动拉起 nginx.exe
// 使用 Job Object 确保父进程退出/崩溃时子进程跟死
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h> // JSON 库
#include <string> // 字符串库
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include "ProcessManager.h"

#ifdef _WIN32
#  include <windows.h>
#  include <cstdio>
#endif

class NginxProxy {
public:
    static void setup(const Json::Value &cfg) {
        if (!cfg.isMember("nginx")) return;
        const auto &c = cfg["nginx"];
        if (!c.get("enabled", false).asBool()) {
            LOG_INFO << "[NginxProxy] 未启用，跳过";
            return;
        }

        exe_      = c.get("exe",      "./Nginx/nginx.exe").asString();
        workDir_  = c.get("work_dir", "./Nginx").asString();
        bool autostart = c.get("autostart", true).asBool();
        bool watchdog  = c.get("watchdog",  true).asBool();
        int  interval  = c.get("watchdog_interval_sec", 15).asInt();

        if (!ProcessManager::exeExists(exe_)) {
            LOG_WARN << "[NginxProxy] nginx.exe 不存在 (" << exe_ << ")，跳过";
            return;
        }

        LOG_INFO << "[NginxProxy] exe=" << exe_ << " workDir=" << workDir_;

        if (autostart) {
            launch();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (watchdog && autostart) {
            std::thread([interval]() {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::seconds(interval));
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (!ProcessManager::exeExists(exe_)) continue;
                    if (!isRunning()) {
                        LOG_WARN << "[NginxProxy] nginx 已停止，正在重启...";
                        launch();
                    }
                }
            }).detach();
        }

        LOG_INFO << "[NginxProxy] 已启动，反代 AgPay 官方前端";
    }

private:
    static inline std::string exe_;
    static inline std::string workDir_;
    static inline std::mutex  mtx_;

    static void launch() {
#ifdef _WIN32
        if (masterHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(masterHandle_);
            masterHandle_ = INVALID_HANDLE_VALUE;
        }
        // 先用 -s stop 清掉旧实例，防止端口冲突
        std::string stopArgs = "-p \"" + workDir_ + "\" -s stop";
        HANDLE hStop = spawnLocal(exe_, stopArgs, workDir_);
        if (hStop != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(hStop, 2000);
            CloseHandle(hStop);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 启动新的 nginx（launcher 会立即退出，master 是另一个 PID）
        std::string args = "-p \"" + workDir_ + "\"";
        HANDLE hLaunch = spawnLocal(exe_, args, workDir_);
        if (hLaunch != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(hLaunch, 3000);
            CloseHandle(hLaunch);
        }

        // 等 nginx master 写入 pid 文件
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        masterHandle_ = openMasterHandle();
        if (masterHandle_ != INVALID_HANDLE_VALUE) {
            DWORD masterPid = GetProcessId(masterHandle_);
            ProcessManager::updateNginxEntry(exe_, masterHandle_, masterPid);
            LOG_INFO << "[NginxProxy] nginx master pid=" << masterPid;
        } else {
            LOG_WARN << "[NginxProxy] 启动 nginx 失败或 pid 文件未就绪";
            ProcessManager::updateNginxEntry(exe_, INVALID_HANDLE_VALUE, 0);
        }
#else
        std::string args = "-p \"" + workDir_ + "\"";
        ProcessManager::spawn(exe_, args, workDir_, false, "nginx", {});
        LOG_INFO << "[NginxProxy] 已启动 nginx";
#endif
    }

#ifdef _WIN32
    // 直接调用 Win32 API 启动进程（不走 ProcessManager 注册表）
    static HANDLE spawnLocal(const std::string &exe, const std::string &args,
                              const std::string &cwd) {
        std::string cmd = "\"" + exe + "\" " + args;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(exe.c_str(), const_cast<char*>(cmd.c_str()),
                            nullptr, nullptr, FALSE,
                            CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                            nullptr,
                            cwd.empty() ? nullptr : cwd.c_str(),
                            &si, &pi)) {
            return INVALID_HANDLE_VALUE;
        }
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    // 读 logs/nginx.pid 打开 master 进程 handle
    static HANDLE openMasterHandle() {
        std::string pidFile = workDir_ + "/logs/nginx.pid";
        FILE *f = fopen(pidFile.c_str(), "r");
        if (!f) return INVALID_HANDLE_VALUE;
        DWORD pid = 0;
        fscanf(f, "%lu", &pid);
        fclose(f);
        if (pid == 0) return INVALID_HANDLE_VALUE;
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
        return h ? h : INVALID_HANDLE_VALUE;
    }
#endif

    static bool isRunning() {
#ifdef _WIN32
        if (masterHandle_ == INVALID_HANDLE_VALUE) {
            // 尝试从 pid 文件恢复 handle
            masterHandle_ = openMasterHandle();
        }
        if (masterHandle_ == INVALID_HANDLE_VALUE) return false;
        DWORD code = 0;
        bool alive = GetExitCodeProcess(masterHandle_, &code) && code == STILL_ACTIVE;
        if (!alive) {
            CloseHandle(masterHandle_);
            masterHandle_ = INVALID_HANDLE_VALUE;
        }
        return alive;
#else
        return false;
#endif
    }

#ifdef _WIN32
    static inline HANDLE masterHandle_{INVALID_HANDLE_VALUE};
#endif
};
