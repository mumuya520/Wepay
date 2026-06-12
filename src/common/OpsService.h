// WePay-Cpp — 运维服务 (部署环境检测 / 配置校验 / 自动告警 / 通道日统计 / 备份)
// 配合 OpsCtrl.h 使用，拆分业务逻辑到独立文件
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <unordered_map> // 哈希表
#include <ctime> // C 时间库
#include <cstdio> // C 标准输入输出库
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <json/json.h>
#include "PayDb.h"
#include "ProcessManager.h"

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#else
#  include <sys/stat.h>
#  include <sys/statvfs.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#endif

// 运维服务类
// 提供部署环境检测、配置校验、通道统计、自动告警、数据库备份等运维功能
class OpsService {
public:
    // ══════════════════════════════════════════════════════════════
    //  1. 部署环境检测
    // ══════════════════════════════════════════════════════════════
    // 获取部署信息
    // 收集系统、编译、运行时等部署相关信息
    // 返回：JSON 对象，包含部署信息
    static Json::Value getDeployInfo() {
        // 创建部署信息对象
        Json::Value d;
        // 设置部署模式（Docker/Windows/Linux systemd/Linux 直接运行）
        d["deploy_mode"] = detectDeployMode();
        // 设置主机名
        d["hostname"]    = getHostname();
        // 设置操作系统信息
        d["os"]          = getOsInfo();
        // 设置架构（64 位或 32 位）
        d["arch"]        = sizeof(void*) == 8 ? "x86_64" : "x86";
        // 设置进程 ID
        d["pid"]         = (int)opsGetpid();
        // 设置当前工作目录
        d["cwd"]         = getCwd();
        // 设置可执行文件路径
        d["exe"]         = getExePath();
        
        // ── 启动时间 ──────────────────────────────
        {
            // 获取启动时间戳
            time_t st = (time_t)ProcessManager::getStartedUnix();
            // 时间缓冲区
            char tb[32];
            // 时间结构体
            struct tm tm;
// 根据平台选择时间转换函数
#ifdef _WIN32
            // Windows 平台：使用 localtime_s
            localtime_s(&tm, &st);
#else
            // Unix/Linux 平台：使用 localtime_r
            localtime_r(&st, &tm);
#endif
            // 格式化时间为字符串
            std::strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tm);
            // 设置启动时间
            d["start_time"] = std::string(tb);
        }

        // ── 编译信息 ──────────────────────────────
        // 设置编译器信息
        d["compiler"]    = getCompilerInfo();
        // 设置 C++ 标准版本
        d["cpp_standard"] = (Json::Int64)__cplusplus;
// 根据编译模式设置构建类型
#ifdef NDEBUG
        // Release 模式
        d["build_type"]  = "Release";
#else
        // Debug 模式
        d["build_type"]  = "Debug";
#endif
        // 设置构建日期和时间
        d["build_date"]  = std::string(__DATE__) + " " + __TIME__;

        // ── Drogon 框架版本 ──────────────────────────────
// 如果定义了 Drogon 版本宏
#ifdef DROGON_VERSION
        d["drogon_version"] = DROGON_VERSION;
#else
        d["drogon_version"] = "unknown";
#endif

        // ── Docker 检测 ──────────────────────────────
        // 检测是否运行在 Docker 容器中
        d["in_docker"]    = isInDocker();

        // ── 数据库后端 ──────────────────────────────
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 设置数据库后端类型
        d["db_backend"]   = db.isUsingSqlite() ? "SQLite" : "PostgreSQL";
        // 如果支持 PostgreSQL，标记为 true
        if (db.hasPg())
            d["db_pg"] = true;

        // ── 配置文件路径 ──────────────────────────────
        // 设置配置文件路径
        d["config_file"] = findConfigPath();

        // ── 运行时间 ──────────────────────────────
        {
            // 获取运行时间（秒）
            int64_t elapsed = ProcessManager::getUptimeSeconds();
            // 设置运行时间（秒）
            d["uptime_seconds"] = (Json::Int64)elapsed;
            // 设置运行时间（格式化文本）
            d["uptime_text"]    = fmtUptime(elapsed);
        }

        // 返回部署信息
        return d;
    }

    // ══════════════════════════════════════════════════════════════
    //  2. 配置校验
    // ══════════════════════════════════════════════════════════════
    // 校验系统配置
    // 检查管理员密码、站点名称、支付通道、商户、目录权限、磁盘空间、数据库连接等
    // 返回：JSON 对象，包含校验结果和错误/警告统计
    static Json::Value validateConfig() {
        // 创建结果对象
        Json::Value result;
        // 创建检查项数组
        Json::Value checks(Json::arrayValue);
        // 初始化错误和警告计数
        int errors = 0, warnings = 0;

        // 获取数据库实例
        auto &db = PayDb::instance();

        // ── 检查管理员密码 ──────────────────────────────
        // 获取管理员密码（默认为 "admin"）
        auto pw = db.getSetting("admin_pass", "admin");
        // 创建检查项
        auto chk1 = makeCheck("admin_password", pw != "admin" ? "ok" : "warning",
            pw != "admin" ? "管理员密码已修改" : "管理员密码仍为默认值 admin，请尽快修改");
        // 添加到检查项数组
        checks.append(chk1);
        // 如果密码仍为默认值，增加警告计数
        if (pw == "admin")
            warnings++;

        // ── 检查站点名称 ──────────────────────────────
        // 获取站点名称
        auto siteName = db.getSetting("siteName", "");
        // 创建检查项
        auto chk2 = makeCheck("site_name", !siteName.empty() ? "ok" : "warning",
            !siteName.empty() ? "站点名称: " + siteName : "未设置站点名称");
        // 添加到检查项数组
        checks.append(chk2);
        // 如果站点名称为空，增加警告计数
        if (siteName.empty())
            warnings++;

        // ── 检查启用的支付通道 ──────────────────────────────
        // 查询启用的支付通道数量
        auto chRow = db.queryOne("SELECT COUNT(*) AS c FROM pay_channel WHERE state=1");
        // 获取通道数量
        int chCount = chRow.empty() ? 0 : atoi(chRow["c"].c_str());
        // 创建检查项
        auto chk3 = makeCheck("active_channels", chCount > 0 ? "ok" : "error",
            chCount > 0 ? "启用通道: " + std::to_string(chCount) + " 个" : "没有启用的支付通道！");
        // 添加到检查项数组
        checks.append(chk3);
        // 如果没有启用的通道，增加错误计数
        if (chCount == 0)
            errors++;

        // ── 检查活跃商户 ──────────────────────────────
        // 查询启用的商户数量
        auto mchRow = db.queryOne("SELECT COUNT(*) AS c FROM merchant WHERE state=1");
        // 获取商户数量
        int mchCount = mchRow.empty() ? 0 : atoi(mchRow["c"].c_str());
        // 创建检查项
        auto chk4 = makeCheck("active_merchants", mchCount > 0 ? "ok" : "warning",
            mchCount > 0 ? "活跃商户: " + std::to_string(mchCount) + " 个" : "没有启用的商户");
        // 添加到检查项数组
        checks.append(chk4);
        // 如果没有启用的商户，增加警告计数
        if (mchCount == 0)
            warnings++;

        // ── 检查日志目录可写性 ──────────────────────────────
        // 检查日志目录是否可写
        bool logOk = isDirWritable("./logs");
        // 创建检查项
        auto chk5 = makeCheck("log_dir", logOk ? "ok" : "error",
            logOk ? "日志目录可写" : "日志目录不可写");
        // 添加到检查项数组
        checks.append(chk5);
        // 如果日志目录不可写，增加错误计数
        if (!logOk)
            errors++;

        // ── 检查上传目录可写性 ──────────────────────────────
        // 检查上传目录是否可写
        bool uploadOk = isDirWritable("./upload");
        // 创建检查项
        auto chk6 = makeCheck("upload_dir", uploadOk ? "ok" : "warning",
            uploadOk ? "上传目录可写" : "上传目录不存在或不可写");
        // 添加到检查项数组
        checks.append(chk6);
        // 如果上传目录不可写，增加警告计数
        if (!uploadOk)
            warnings++;

        // ── 检查磁盘空间 ──────────────────────────────
        // 获取可用磁盘空间（字节）
        int64_t freeBytes = getFreeDiskSpace(".");
        // 转换为 GB
        int64_t freeGB = freeBytes / (1024LL * 1024 * 1024);
        // 创建检查项（>=1GB 为 ok，>0 为 warning，<=0 为 error）
        auto chk7 = makeCheck("disk_space", freeGB >= 1 ? "ok" : (freeGB > 0 ? "warning" : "error"),
            "可用磁盘: " + std::to_string(freeGB) + " GB");
        // 添加到检查项数组
        checks.append(chk7);
        // 如果磁盘空间不足，增加错误或警告计数
        if (freeGB < 1) {
            freeGB > 0 ? warnings++ : errors++;
        }

        // ── 检查数据库连接 ──────────────────────────────
        // 初始化数据库连接状态
        bool dbOk = false;
        // 尝试执行简单查询
        try {
            db.queryOne("SELECT 1 AS ok");
            // 查询成功，标记为连接正常
            dbOk = true;
        } catch (...) {
            // 查询失败，忽略异常
        }
        // 创建检查项
        auto chk8 = makeCheck("database", dbOk ? "ok" : "error",
            dbOk ? "数据库连接正常" : "数据库连接失败!");
        // 添加到检查项数组
        checks.append(chk8);
        // 如果数据库连接失败，增加错误计数
        if (!dbOk)
            errors++;

        // ── 构建结果对象 ──────────────────────────────
        // 设置检查项数组
        result["checks"]   = checks;
        // 设置错误数量
        result["errors"]   = errors;
        // 设置警告数量
        result["warnings"] = warnings;
        // 设置总体状态（error > warning > ok）
        result["status"]   = errors > 0 ? "error" : (warnings > 0 ? "warning" : "ok");
        // 返回结果
        return result;
    }

    // ══════════════════════════════════════════════════════════════
    //  3. 通道日统计同步 (可由 CronService 定期调用)
    // ══════════════════════════════════════════════════════════════
    static void syncChannelDailyStat(const std::string &dateStr = "") {
        auto &db = PayDb::instance();
        std::string date = dateStr;
        if (date.empty()) {
            time_t now = std::time(nullptr);
            struct tm t;
#ifdef _WIN32
            localtime_s(&t, &now);
#else
            localtime_r(&now, &t);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
            date = buf;
        }

        // 按通道汇总当日订单
        auto rows = db.query(
            "SELECT channel_id, "
            "  COUNT(*) AS total, "
            "  SUM(CASE WHEN state=1 THEN 1 ELSE 0 END) AS success, "
            "  SUM(CASE WHEN state=-1 THEN 1 ELSE 0 END) AS failed, "
            "  SUM(CASE WHEN state=1 THEN CAST(amount AS NUMERIC(12,2)) ELSE 0 END) AS pay_amount, "
            "  SUM(CASE WHEN state=1 THEN CAST(fee AS NUMERIC(12,2)) ELSE 0 END) AS fee_amount "
            "FROM pay_order WHERE date(datetime(created_at,'unixepoch','localtime'))=? "
            "GROUP BY channel_id",
            {date});

        for (auto &r : rows) {
            std::string chId = r.count("channel_id") ? r.at("channel_id") : "0";
            int total   = safeInt(r, "total");
            int success = safeInt(r, "success");
            int failed  = safeInt(r, "failed");
            std::string payAmt = r.count("pay_amount") ? r.at("pay_amount") : "0";
            std::string feeAmt = r.count("fee_amount") ? r.at("fee_amount") : "0";

            // upsert
            auto exist = db.queryOne(
                "SELECT id FROM channel_daily_stat WHERE stat_date=? AND channel_id=?",
                {date, chId});
            if (exist.empty()) {
                db.exec(
                    "INSERT INTO channel_daily_stat(stat_date,channel_id,total_count,success_count,"
                    "failed_count,pay_amount,fee_amount,created_at) VALUES(?,?,?,?,?,?,?,?)",
                    {date, chId, std::to_string(total), std::to_string(success),
                     std::to_string(failed), payAmt, feeAmt,
                     std::to_string(std::time(nullptr))});
            } else {
                db.exec(
                    "UPDATE channel_daily_stat SET total_count=?,success_count=?,failed_count=?,"
                    "pay_amount=?,fee_amount=?,updated_at=? WHERE stat_date=? AND channel_id=?",
                    {std::to_string(total), std::to_string(success), std::to_string(failed),
                     payAmt, feeAmt, std::to_string(std::time(nullptr)), date, chId});
            }
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  4. 自动告警检测 (可由定时任务定期调用)
    // ══════════════════════════════════════════════════════════════
    struct AlertItem {
        std::string level;   // critical / error / warning
        std::string source;
        std::string message;
    };

    static std::vector<AlertItem> runAutoCheck() {
        std::vector<AlertItem> alerts;
        auto &db = PayDb::instance();

        // 磁盘空间
        int64_t freeGB = getFreeDiskSpace(".") / (1024LL * 1024 * 1024);
        if (freeGB < 1)
            alerts.push_back({"critical", "disk", "磁盘可用空间不足 1GB (" + std::to_string(freeGB) + " GB)"});
        else if (freeGB < 5)
            alerts.push_back({"warning", "disk", "磁盘可用空间较低 (" + std::to_string(freeGB) + " GB)"});

        // 通知失败堆积
        try {
            auto nt = db.queryOne("SELECT COUNT(*) AS c FROM pay_notify_task WHERE status=-1");
            int ntFailed = nt.empty() ? 0 : atoi(nt["c"].c_str());
            if (ntFailed > 50)
                alerts.push_back({"error", "notify", "通知失败堆积 " + std::to_string(ntFailed) + " 条"});
            else if (ntFailed > 10)
                alerts.push_back({"warning", "notify", "通知失败 " + std::to_string(ntFailed) + " 条待处理"});
        } catch (...) {}

        // 通道异常检测 (最近1小时成功率<50%)
        try {
            int64_t oneHourAgo = std::time(nullptr) - 3600;
            auto chRows = db.query(
                "SELECT c.channel_name, COUNT(*) AS total, "
                "  SUM(CASE WHEN o.state=1 THEN 1 ELSE 0 END) AS success "
                "FROM pay_order o JOIN pay_channel c ON o.channel_id=c.id "
                "WHERE o.created_at>=? GROUP BY o.channel_id, c.channel_name HAVING COUNT(*)>=5",
                {std::to_string(oneHourAgo)});
            for (auto &r : chRows) {
                int total = safeInt(r, "total"), success = safeInt(r, "success");
                double rate = total > 0 ? (double)success / total * 100.0 : 100.0;
                if (rate < 50.0)
                    alerts.push_back({"error", "channel",
                        r.at("channel_name") + " 近1h成功率仅 " + fmtPct(rate)});
            }
        } catch (...) {}

        // 待结算堆积
        try {
            auto st = db.queryOne("SELECT COUNT(*) AS c FROM settle_order WHERE state=0");
            int pending = st.empty() ? 0 : atoi(st["c"].c_str());
            if (pending > 20)
                alerts.push_back({"warning", "settle", "待审核结算单 " + std::to_string(pending) + " 笔"});
        } catch (...) {}

        return alerts;
    }

    // ══════════════════════════════════════════════════════════════
    //  5. 数据库备份 (SQLite)
    // ══════════════════════════════════════════════════════════════
    static Json::Value backupDatabase(const std::string &backupDir = "./data/backup") {
        Json::Value r;
        auto &db = PayDb::instance();
        if (!db.isUsingSqlite()) {
            r["ok"] = false;
            r["message"] = "仅支持 SQLite 在线备份，PostgreSQL 请使用 pg_dump";
            return r;
        }

#ifdef _WIN32
        _mkdir(backupDir.c_str());
#else
        ::mkdir(backupDir.c_str(), 0755);
#endif

        time_t now = std::time(nullptr);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char fname[64];
        std::strftime(fname, sizeof(fname), "wepay_%Y%m%d_%H%M%S.db", &t);
        std::string destPath = backupDir + "/" + fname;

        // 使用 SQLite VACUUM INTO (3.27.0+)
        bool ok = db.execSqliteDirect("VACUUM INTO '" + destPath + "'");
        r["ok"] = ok;
        r["file"] = ok ? destPath : "";
        r["message"] = ok ? "备份成功" : "备份失败";
        r["timestamp"] = (Json::Int64)now;
        return r;
    }

    // 列出备份文件
    static Json::Value listBackups(const std::string &backupDir = "./data/backup") {
        Json::Value arr(Json::arrayValue);
#ifdef _WIN32
        std::string pattern = backupDir + "/wepay_*.db";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                Json::Value f;
                f["name"] = fd.cFileName;
                LARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart  = fd.nFileSizeLow;
                f["size"]   = (Json::Int64)sz.QuadPart;
                f["size_text"] = fmtSize(sz.QuadPart);
                // 从文件名提取时间
                f["path"] = backupDir + "/" + fd.cFileName;
                arr.append(f);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#else
        // Linux: 简单 popen ls
        std::string cmd = "ls -lS " + backupDir + "/wepay_*.db 2>/dev/null";
        FILE *fp = popen(cmd.c_str(), "r");
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                std::string s(line);
                // 简单解析 ls -l 输出
                auto lastSpace = s.rfind(' ');
                if (lastSpace != std::string::npos) {
                    std::string name = s.substr(lastSpace + 1);
                    while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
                        name.pop_back();
                    Json::Value f;
                    f["name"] = name;
                    f["path"] = name;
                    arr.append(f);
                }
            }
            pclose(fp);
        }
#endif
        return arr;
    }

    // ══════════════════════════════════════════════════════════════
    //  6. 环境对比 — Docker vs 裸机 部署差异提示
    // ══════════════════════════════════════════════════════════════
    static Json::Value getDeployTips() {
        Json::Value tips(Json::arrayValue);
        bool docker = isInDocker();

        if (docker) {
            tips.append("当前运行在 Docker 容器中");
            tips.append("数据持久化: 确保 /app/data 和 /app/logs 已挂载宿主机卷");
            tips.append("配置热更新: 修改 config.json 后需 docker restart wepay-cpp");
            tips.append("日志收集: 建议用 docker logs -f wepay-cpp 或挂载 /app/logs");
            tips.append("扩容: 使用 docker-compose scale 或 K8s Deployment replicas");
            tips.append("健康检查: Dockerfile 内置 HEALTHCHECK，可直接用于编排探针");
            tips.append("备份: docker exec wepay-cpp sqlite3 /app/data/wepay.db .backup");
        } else {
            tips.append("当前以裸机/直接进程方式运行");
            tips.append("建议使用 systemd 管理服务，确保崩溃自动重启");
            tips.append("日志: 确保 ./logs 目录有足够磁盘空间");
            tips.append("备份: 通过运维面板的在线备份或手动 cp wepay.db");
            tips.append("配置: 修改 config.json 后重启进程即可生效");
#ifdef _WIN32
            tips.append("Windows: 可注册为 Windows 服务 (sc create / NSSM)");
#else
            tips.append("Linux: 推荐 systemctl enable wepay-cpp.service");
#endif
        }
        return tips;
    }

    // ══════════════════════════════════════════════════════════════
    //  辅助方法
    // ══════════════════════════════════════════════════════════════
private:
    static std::string detectDeployMode() {
        if (isInDocker()) return "Docker";
#ifdef _WIN32
        return "Windows (直接运行)";
#else
        // 检查 systemd
        if (getppid() == 1) return "Linux (systemd)";
        return "Linux (直接运行)";
#endif
    }

    static bool isInDocker() {
#ifdef _WIN32
        return false;
#else
        // 检查 /.dockerenv 或 cgroup
        std::ifstream f("/.dockerenv");
        if (f.good()) return true;
        std::ifstream cg("/proc/1/cgroup");
        std::string line;
        while (std::getline(cg, line)) {
            if (line.find("docker") != std::string::npos ||
                line.find("containerd") != std::string::npos ||
                line.find("kubepods") != std::string::npos)
                return true;
        }
        return false;
#endif
    }

    static std::string getHostname() {
        char buf[256] = {};
#ifdef _WIN32
        DWORD sz = sizeof(buf);
        GetComputerNameA(buf, &sz);
#else
        gethostname(buf, sizeof(buf));
#endif
        return buf;
    }

    static std::string getOsInfo() {
#ifdef _WIN32
        OSVERSIONINFOA ov{};
        ov.dwOSVersionInfoSize = sizeof(ov);
        // GetVersionExA is deprecated but works for display
        return "Windows";
#else
        struct utsname u;
        if (uname(&u) == 0)
            return std::string(u.sysname) + " " + u.release + " " + u.machine;
        return "Linux";
#endif
    }

    static std::string getCwd() {
        char buf[512] = {};
#ifdef _WIN32
        _getcwd(buf, sizeof(buf));
#else
        if (getcwd(buf, sizeof(buf))) {}
#endif
        return buf;
    }

    static std::string getExePath() {
#ifdef _WIN32
        char buf[512] = {};
        GetModuleFileNameA(NULL, buf, sizeof(buf));
        return buf;
#else
        char buf[512] = {};
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) { buf[len] = 0; return buf; }
        return "";
#endif
    }

    static std::string getCompilerInfo() {
#if defined(__clang__)
        return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
        return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
        return "MSVC " + std::to_string(_MSC_VER);
#else
        return "Unknown";
#endif
    }

    static std::string findConfigPath() {
        const char *candidates[] = {"./config.json", "/app/config.json", "../config.json"};
        for (auto p : candidates) {
            std::ifstream f(p);
            if (f.good()) return p;
        }
        return "未找到";
    }

    static bool isDirWritable(const std::string &dir) {
#ifdef _WIN32
        return (_access(dir.c_str(), 2) == 0);  // 2 = write
#else
        return (access(dir.c_str(), W_OK) == 0);
#endif
    }

    static int64_t getFreeDiskSpace(const std::string &path) {
#ifdef _WIN32
        ULARGE_INTEGER avail;
        if (GetDiskFreeSpaceExA(path.c_str(), &avail, NULL, NULL))
            return (int64_t)avail.QuadPart;
        return 0;
#else
        struct statvfs sv;
        if (statvfs(path.c_str(), &sv) == 0)
            return (int64_t)sv.f_bavail * sv.f_frsize;
        return 0;
#endif
    }

    static int safeInt(const std::unordered_map<std::string,std::string> &m, const std::string &k) {
        auto it = m.find(k);
        if (it == m.end()) return 0;
        try { return std::stoi(it->second); } catch (...) { return 0; }
    }

    static std::string fmtUptime(int64_t sec) {
        int d = (int)(sec / 86400);
        int h = (int)((sec % 86400) / 3600);
        int m = (int)((sec % 3600) / 60);
        char buf[64];
        if (d > 0) std::snprintf(buf, sizeof(buf), "%d天 %02d:%02d", d, h, m);
        else std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, (int)(sec % 60));
        return buf;
    }

    static std::string fmtSize(int64_t bytes) {
        const char* u[] = {"B", "KB", "MB", "GB"};
        double v = (double)bytes;
        int i = 0;
        while (v >= 1024 && i < 3) { v /= 1024; i++; }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
        return buf;
    }

    static std::string fmtPct(double v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f%%", v);
        return buf;
    }

    static Json::Value makeCheck(const std::string &name, const std::string &status, const std::string &msg) {
        Json::Value c;
        c["name"]    = name;
        c["status"]  = status;
        c["message"] = msg;
        c["tone"]    = status == "ok" ? "success" : (status == "warning" ? "warning" : "danger");
        return c;
    }

    static int opsGetpid() {
#ifdef _WIN32
        return (int)GetCurrentProcessId();
#else
        return ::getpid();
#endif
    }
};
