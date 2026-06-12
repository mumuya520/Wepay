// WePay-Cpp — 全局子进程管理器
// ProcessManager.h — 全局子进程管理器
// 单一 Job Object（KILL_ON_JOB_CLOSE），所有子进程统一加入
// 主进程正常退出/崩溃/被杀时，OS 关闭 hJob_ 句柄，Job Object 自动杀死所有子进程
// 内置动态注册表：记录每个子进程的名称、PID、重启次数、最后启动时间
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <map> // 映射容器
#include <mutex>
#include <chrono>
#include <json/json.h>
#include <drogon/drogon.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

// 子进程条目结构体
// 记录每个子进程的名称、可执行文件路径、重启次数、启动时间等信息
struct ProcEntry {
    // 子进程名称（用于健康检查显示）
    std::string name;
    // 可执行文件路径
    std::string exe;
    // 重启次数计数
    int         restartCount{0};
    // 最后启动时间（高精度时钟）
    std::chrono::steady_clock::time_point lastStarted{};
    // 启动时间戳（Unix 时间）
    std::int64_t startedUnix{0};
// Windows 特定字段
#ifdef _WIN32
    // 进程句柄
    HANDLE hProcess{INVALID_HANDLE_VALUE};
    // 进程 ID
    DWORD  pid{0};
#else
    // Unix 进程 ID
    int pid{0};
#endif
};

// 全局子进程管理器类
// 职责：
//   1. 创建全局 Job Object，所有子进程统一加入
//   2. 主进程正常退出/崩溃/被杀时，OS 关闭 hJob_ 句柄，Job Object 自动杀死所有子进程
//   3. 内置动态注册表：记录每个子进程的名称、PID、重启次数、最后启动时间
//   4. 提供进程启动、状态查询、终止等接口
class ProcessManager {
public:
    // 初始化子进程管理器
    // 在 main() 最早处调用，创建全局 Job Object
    // Windows 平台：创建 Job Object 并设置 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 标志
    // Unix 平台：仅记录启动时间戳
    static void init() {
// Windows 平台特定初始化
#ifdef _WIN32
        // 如果 Job Object 已创建，直接返回
        if (hJob_ != nullptr)
            return;
        // 创建全局 Job Object
        hJob_ = CreateJobObjectA(nullptr, nullptr);
        // 如果创建成功
        if (hJob_) {
            // 创建 Job Object 扩展限制信息结构体
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            // 设置 Job Object 限制标志：当 Job Object 句柄关闭时杀死所有子进程
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            // 设置 Job Object 信息
            SetInformationJobObject(hJob_, JobObjectExtendedLimitInformation,
                                    &jeli, sizeof(jeli));
            // 记录成功日志
            LOG_INFO << "[ProcessManager] Job Object 已创建，子进程将跟随主进程死亡";
        } else {
            // 记录失败日志
            LOG_WARN << "[ProcessManager] Job Object 创建失败: " << GetLastError();
        }
#endif
        // 记录主进程启动时间戳
        startedUnix_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 检查可执行文件是否存在
    // 供 watchdog 跳过不存在的服务
    // 参数 exePath：可执行文件路径
    // 返回：文件是否存在
    static bool exeExists(const std::string &exePath) {
// Windows 平台实现
#ifdef _WIN32
        // 绝对路径缓冲区
        char absPath[MAX_PATH] = {};
        // 指向路径的指针
        const char *p = exePath.c_str();
        // 获取绝对路径
        if (GetFullPathNameA(p, MAX_PATH, absPath, nullptr))
            p = absPath;
        // 检查文件属性是否有效
        return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
        // Unix 平台：使用 access 检查文件是否存在
        return ::access(exePath.c_str(), F_OK) == 0;
#endif
    }

    // 启动子进程并加入 Job Object，自动更新注册表
    // 参数 exePath：可执行文件路径
    // 参数 cmdArgs：命令行参数
    // 参数 workDir：工作目录
    // 参数 showWindow：是否显示窗口
    // 参数 name：用于健康检查显示的服务名
    // 参数 extraEnv：额外的环境变量
    // 返回：进程句柄（Windows）或 PID（Unix）
#ifdef _WIN32
    static HANDLE spawn(const std::string &exePath,
                        const std::string &cmdArgs = "",
                        const std::string &workDir = "",
                        bool showWindow = false,
                        const std::string &name = "",
                        const std::map<std::string,std::string> &extraEnv = {}) {
        // ── 解析绝对路径 ──────────────────────────────
        // 绝对路径缓冲区
        char absPath[MAX_PATH] = {};
        // 最终使用的可执行文件路径
        std::string finalExe = exePath;
        // 最终使用的工作目录
        std::string finalCwd = workDir;
        // 获取绝对路径
        if (GetFullPathNameA(exePath.c_str(), MAX_PATH, absPath, nullptr)) {
            // 使用绝对路径
            finalExe = absPath;
            // 如果未指定工作目录，使用可执行文件所在目录
            if (finalCwd.empty()) {
                std::string p(absPath);
                // 查找最后一个路径分隔符
                auto pos = p.find_last_of("\\/");
                // 提取目录部分
                if (pos != std::string::npos)
                    finalCwd = p.substr(0, pos);
            }
        }
        // 如果可执行文件不存在，静默跳过
        if (GetFileAttributesA(finalExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 记录跳过日志
            LOG_INFO << "[ProcessManager] 跳过（文件不存在）: " << finalExe;
            // 返回无效句柄
            return INVALID_HANDLE_VALUE;
        }

        // ── 构建命令行 ──────────────────────────────
        // 命令行字符串
        std::string cmdLine = "\"" + finalExe + "\"";
        // 如果有命令行参数，追加到命令行
        if (!cmdArgs.empty())
            cmdLine += " " + cmdArgs;

        // ── 启动信息结构体 ──────────────────────────────
        // 启动信息
        STARTUPINFOA si{};
        // 设置结构体大小
        si.cb = sizeof(si);
        // 如果不显示窗口
        if (!showWindow) {
            // 设置显示窗口标志
            si.dwFlags     = STARTF_USESHOWWINDOW;
            // 隐藏窗口
            si.wShowWindow = SW_HIDE;
        }
        // 进程信息
        PROCESS_INFORMATION pi{};
        // 进程创建标志
        DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
        // 如果不显示窗口，添加 CREATE_NO_WINDOW 标志
        if (!showWindow)
            flags |= CREATE_NO_WINDOW;
        // 工作目录指针
        const char *cwd = finalCwd.empty() ? nullptr : finalCwd.c_str();

        // ── 构建环境变量块 ──────────────────────────────
        // 环境变量块
        std::vector<char> envBlock;
        // 环境变量指针
        void *lpEnv = nullptr;
        // 如果有额外的环境变量
        if (!extraEnv.empty()) {
            // 读取父进程环境变量
            LPCH parentEnv = GetEnvironmentStringsA();
            // 合并后的环境变量
            std::map<std::string,std::string> merged;
            // 如果成功读取父进程环境
            if (parentEnv) {
                // 遍历父进程环境变量
                for (LPCH p = parentEnv; *p; ) {
                    // 获取一个环境变量字符串
                    std::string kv(p);
                    // 查找等号位置
                    auto eq = kv.find('=');
                    // 如果找到有效的键值对
                    if (eq != std::string::npos && eq > 0)
                        // 添加到合并字典
                        merged[kv.substr(0, eq)] = kv.substr(eq + 1);
                    // 移动到下一个环境变量
                    p += kv.size() + 1;
                }
                // 释放父进程环境变量
                FreeEnvironmentStringsA(parentEnv);
            }
            // 使用额外环境变量覆盖/追加
            for (auto &[k, v] : extraEnv)
                merged[k] = v;
            // 构建环境变量块
            for (auto &[k, v] : merged) {
                // 构建键值对字符串
                std::string kv = k + "=" + v;
                // 添加到环境变量块
                envBlock.insert(envBlock.end(), kv.begin(), kv.end());
                // 添加空字符分隔符
                envBlock.push_back('\0');
            }
            // 添加块结束标记
            envBlock.push_back('\0');
            // 设置环境变量指针
            lpEnv = envBlock.data();
        }

        // ── 创建进程 ──────────────────────────────
        // 创建进程
        if (!CreateProcessA(finalExe.c_str(),
                            cmdLine.size() > finalExe.size() + 2
                                ? const_cast<char*>(cmdLine.c_str()) : nullptr,
                            nullptr, nullptr, FALSE,
                            flags, lpEnv, cwd, &si, &pi)) {
            // 记录创建失败日志
            LOG_WARN << "[ProcessManager] 启动失败: " << GetLastError()
                     << " exe=" << finalExe;
            // 返回无效句柄
            return INVALID_HANDLE_VALUE;
        }

        // ── 将进程加入 Job Object ──────────────────────────────
        // 如果 Job Object 已创建
        if (hJob_) {
            // 将进程分配给 Job Object
            if (!AssignProcessToJobObject(hJob_, pi.hProcess))
                // 记录分配失败日志
                LOG_WARN << "[ProcessManager] AssignProcessToJobObject 失败: " << GetLastError();
        }

        // ── 恢复线程执行 ──────────────────────────────
        // 恢复主线程执行
        ResumeThread(pi.hThread);
        // 关闭线程句柄
        CloseHandle(pi.hThread);

        // ── 记录启动日志 ──────────────────────────────
        LOG_INFO << "[ProcessManager] 已启动 " << (name.empty() ? finalExe : name)
                 << " (pid=" << pi.dwProcessId << ")";

        // ── 更新注册表 ──────────────────────────────
        // 服务名称
        std::string svcName = name.empty() ? exePath : name;
        // 更新注册表
        updateRegistry(svcName, exePath, pi.hProcess, pi.dwProcessId);

        // 返回进程句柄
        return pi.hProcess;
    }

    // 检查进程是否仍在运行
    // 参数 hProcess：进程句柄
    // 返回：进程是否仍在运行
    static bool isAlive(HANDLE hProcess) {
        // 如果句柄无效，返回 false
        if (hProcess == INVALID_HANDLE_VALUE || hProcess == nullptr)
            return false;
        // 进程退出代码
        DWORD code = 0;
        // 获取进程退出代码，如果仍在运行则返回 STILL_ACTIVE
        return GetExitCodeProcess(hProcess, &code) && code == STILL_ACTIVE;
    }
#else
    // Unix 平台：启动子进程
    // 参数 exePath：可执行文件路径
    // 参数 cmdArgs：命令行参数
    // 参数 workDir：工作目录
    // 参数 showWindow：是否显示窗口（Unix 平台忽略）
    // 参数 name：用于健康检查显示的服务名
    // 参数 extraEnv：额外的环境变量
    // 返回：进程 ID
    static int spawn(const std::string &exePath,
                     const std::string &cmdArgs = "",
                     const std::string &workDir = "",
                     bool showWindow = false,
                     const std::string &name = "",
                     const std::map<std::string,std::string> &extraEnv = {}) {
        // 构建命令字符串
        std::string cmd;
        // 如果指定了工作目录，先切换目录
        if (!workDir.empty())
            cmd += "cd \"" + workDir + "\" && ";
        // 设置额外环境变量
        for (auto &[k, v] : extraEnv)
            cmd += k + "=\"" + v + "\" ";
        // 添加可执行文件路径
        cmd += "\"" + exePath + "\"";
        // 如果有命令行参数，追加到命令行
        if (!cmdArgs.empty())
            cmd += " " + cmdArgs;
        // 后台运行
        cmd += " &";
        // 忽略 showWindow 参数
        (void)showWindow;
        // 执行命令
        ::system(cmd.c_str());
        // 返回 0（Unix 平台不返回 PID）
        return 0;
    }
    // 检查进程是否仍在运行
    // 参数 pid：进程 ID
    // 返回：进程是否仍在运行
    static bool isAlive(int pid) {
        // 如果 PID 有效且 kill(pid, 0) 成功，进程仍在运行
        return pid > 0 && ::kill(pid, 0) == 0;
    }
#endif

    // 动态查询所有子进程状态
    // 供健康检查接口使用
    // 返回：包含主进程和所有子进程信息的 JSON 对象
    static Json::Value getStatusJson() {
        // 创建根 JSON 对象
        Json::Value root;
        // ── 主进程信息 ──────────────────────────────
        // 设置主进程状态
        root["main"]["status"] = "running";
        // 计算主进程运行时间（秒）
        root["main"]["uptime_seconds"] = (Json::Int64)(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() - startedUnix_);
// Windows 平台特定信息
#ifdef _WIN32
        // 设置主进程 ID
        root["main"]["pid"] = (Json::Int)GetCurrentProcessId();
#endif

        // ── 子进程列表 ──────────────────────────────
        // 加锁保护注册表访问
        std::lock_guard<std::mutex> lk(mtx_);
        // 创建子进程数组
        Json::Value procs(Json::arrayValue);
        // 遍历所有已注册的子进程
        for (auto &e : registry_) {
            // 创建子进程 JSON 对象
            Json::Value p;
            // 设置子进程名称
            p["name"]          = e.name;
            // 设置可执行文件路径
            p["exe"]           = e.exe;
            // 设置重启次数
            p["restart_count"] = e.restartCount;
            // 设置启动时间戳
            p["started_at"]    = (Json::Int64)e.startedUnix;
// Windows 平台特定信息
#ifdef _WIN32
            // 检查进程是否仍在运行
            bool alive = isAlive(e.hProcess);
            // 设置进程状态
            p["status"] = alive ? "running" : "stopped";
            // 设置进程 ID
            p["pid"]    = (Json::Int)e.pid;
#else
            // Unix 平台：状态未知
            p["status"] = "unknown";
            // 设置进程 ID
            p["pid"]    = e.pid;
#endif
            // 添加到子进程数组
            procs.append(p);
        }
        // 设置子进程列表
        root["processes"] = procs;
        // 返回 JSON 对象
        return root;
    }

    // 终止指定名称的子进程
    // watchdog 会在下个心跳自动重启
    // 参数 name：子进程名称
    // 返回：是否成功发出终止信号
    static bool killByName(const std::string &name) {
        // 加锁保护注册表访问
        std::lock_guard<std::mutex> lk(mtx_);
        // 遍历所有已注册的子进程
        for (auto &e : registry_) {
            // 如果名称不匹配，跳过
            if (e.name != name)
                continue;
// Windows 平台实现
#ifdef _WIN32
            // 如果进程句柄无效，返回 false
            if (e.hProcess == INVALID_HANDLE_VALUE)
                return false;
            // 如果进程已停止，返回 true
            if (!isAlive(e.hProcess))
                return true;
            // 终止进程
            BOOL ok = TerminateProcess(e.hProcess, 0);
            // 如果终止成功
            if (ok) {
                // 记录成功日志
                LOG_INFO << "[ProcessManager] 已终止 " << name << " (pid=" << e.pid << ")，等待 watchdog 重启";
            } else {
                // 记录失败日志
                LOG_WARN << "[ProcessManager] 终止 " << name << " 失败: " << GetLastError();
            }
            // 返回是否成功
            return ok != 0;
#else
            // Unix 平台：发送 SIGKILL 信号
            // 如果 PID 无效，返回 false
            if (e.pid <= 0)
                return false;
            // 发送 SIGKILL 信号（9）
            return ::kill(e.pid, 9) == 0;
#endif
        }
        // 如果未找到指定名称的进程，返回 false
        return false;
    }

    // 列出所有已注册子进程名称
    // 返回：子进程名称列表
    static std::vector<std::string> listNames() {
        // 加锁保护注册表访问
        std::lock_guard<std::mutex> lk(mtx_);
        // 创建名称列表
        std::vector<std::string> names;
        // 遍历所有已注册的子进程
        for (auto &e : registry_)
            // 添加子进程名称
            names.push_back(e.name);
        // 返回名称列表
        return names;
    }

// Windows 平台特定方法
#ifdef _WIN32
    // 供 NginxProxy 等 daemon 式服务：绕过 spawn，直接注入 master 进程 handle
    // 参数 exe：可执行文件路径
    // 参数 hMaster：master 进程句柄
    // 参数 masterPid：master 进程 ID
    static void updateNginxEntry(const std::string &exe, HANDLE hMaster, DWORD masterPid) {
        // Nginx 服务名称
        const std::string name = "nginx";
        // 加锁保护注册表访问
        std::lock_guard<std::mutex> lk(mtx_);
        // 获取当前时间
        auto now = std::chrono::system_clock::now();
        // 转换为 Unix 时间戳
        auto nowUnix = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        // 遍历所有已注册的子进程
        for (auto &e : registry_) {
            // 如果名称不匹配，跳过
            if (e.name != name)
                continue;
            // 如果旧句柄有效，关闭它
            if (e.hProcess != INVALID_HANDLE_VALUE)
                CloseHandle(e.hProcess);
            // 更新进程句柄
            e.hProcess    = hMaster;
            // 更新进程 ID
            e.pid         = masterPid;
            // 增加重启计数
            e.restartCount++;
            // 更新启动时间戳
            e.startedUnix = nowUnix;
            // 返回
            return;
        }
        // 首次：创建新条目
        ProcEntry entry;
        // 设置服务名称
        entry.name        = name;
        // 设置可执行文件路径
        entry.exe         = exe;
        // 设置进程句柄
        entry.hProcess    = hMaster;
        // 设置进程 ID
        entry.pid         = masterPid;
        // 初始化重启计数
        entry.restartCount = 0;
        // 设置启动时间戳
        entry.startedUnix  = nowUnix;
        // 添加到注册表
        registry_.push_back(std::move(entry));
    }
#endif

    // 获取主进程启动时间戳
    // 供 OpsService 等使用
    // 返回：主进程启动时间戳（Unix 时间）
    static std::int64_t getStartedUnix() {
        // 返回启动时间戳
        return startedUnix_;
    }

    // 获取主进程运行时间
    // 供 OpsService 等使用
    // 返回：主进程运行时间（秒）
    static std::int64_t getUptimeSeconds() {
        // 计算当前时间与启动时间的差值
        return (std::int64_t)(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() - startedUnix_);
    }

// 私有区域
private:
// Windows 平台特定静态成员
#ifdef _WIN32
    // 全局 Job Object 句柄
    static inline HANDLE hJob_{nullptr};
#endif
    // 主进程启动时间戳（Unix 时间）
    static inline std::int64_t             startedUnix_{0};
    // 互斥锁，保护注册表访问
    static inline std::mutex               mtx_;
    // 子进程注册表
    static inline std::vector<ProcEntry>   registry_;

// Windows 平台特定方法
#ifdef _WIN32
    // 更新子进程注册表
    // 参数 name：子进程名称
    // 参数 exe：可执行文件路径
    // 参数 hProc：进程句柄
    // 参数 pid：进程 ID
    static void updateRegistry(const std::string &name, const std::string &exe,
                                HANDLE hProc, DWORD pid) {
        // 加锁保护注册表访问
        std::lock_guard<std::mutex> lk(mtx_);
        // 获取当前时间
        auto now = std::chrono::system_clock::now();
        // 转换为 Unix 时间戳
        auto nowUnix = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        // 遍历所有已注册的子进程
        for (auto &e : registry_) {
            // 如果名称匹配
            if (e.name == name) {
                // 如果旧句柄有效，关闭它
                if (e.hProcess != INVALID_HANDLE_VALUE)
                    CloseHandle(e.hProcess);
                // 更新进程句柄
                e.hProcess     = hProc;
                // 更新进程 ID
                e.pid          = pid;
                // 增加重启计数
                e.restartCount++;
                // 更新启动时间戳
                e.startedUnix  = nowUnix;
                // 返回
                return;
            }
        }
        // 未找到，创建新条目
        ProcEntry entry;
        // 设置服务名称
        entry.name        = name;
        // 设置可执行文件路径
        entry.exe         = exe;
        // 设置进程句柄
        entry.hProcess    = hProc;
        // 设置进程 ID
        entry.pid         = pid;
        // 初始化重启计数
        entry.restartCount = 0;
        // 设置启动时间戳
        entry.startedUnix  = nowUnix;
        // 添加到注册表
        registry_.push_back(std::move(entry));
    }
#endif
};
