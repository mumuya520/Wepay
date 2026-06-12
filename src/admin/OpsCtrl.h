// WePay-Cpp — 运维监控面板控制器
// 职责：系统资源监控、通道健康度、请求统计、告警管理、日志查看、部署信息等
//
// API 端点：
// GET  /admin/api/ops/overview      系统资源概况(CPU/内存/磁盘/运行时间) + 业务健康度
// GET  /admin/api/ops/channels      通道健康度(成功率/失败率/平均响应)
// GET  /admin/api/ops/requests      请求统计(QPS/错误率/慢请求)
// GET  /admin/api/ops/alerts        告警列表
// GET  /admin/api/ops/errors        近期错误日志
// POST /admin/api/ops/alert/clear   清除告警
// GET  /admin/api/ops/dependencies  依赖健康检查(DB/Redis/Config)
// GET  /admin/api/ops/dashboard     业务运营总览(今日成交/退款/待结算/健康分/趋势)
// GET  /admin/api/ops/channelDaily  通道日统计列表
// GET  /admin/api/ops/callbackLogs  回调日志列表
// GET  /admin/api/ops/notifyTasks   通知任务列表
// POST /admin/api/ops/notifyRetry   通知任务手动重试
// GET  /admin/api/ops/logTail       日志文件尾部(扫描错误关键字)
// GET  /admin/api/ops/deploy        部署环境信息
// GET  /admin/api/ops/configCheck   配置校验
// POST /admin/api/ops/backup        在线备份(SQLite)
// GET  /admin/api/ops/backupList    备份文件列表
// POST /admin/api/ops/syncDailyStat 手动同步通道日统计
// POST /admin/api/ops/autoCheck     手动触发自动告警检测
// GET  /admin/api/ops/deployLog     部署操作日志
// GET  /admin/api/ops/deployTips    部署建议
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <fstream> // 文件流库
#include <sstream> // 字符串流库
#include <chrono> // 时间库
#include <atomic> // 原子操作库
#include <mutex> // 互斥锁库
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <json/json.h>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/ProcessManager.h"
#include "../common/PermCheck.h"
#include "../common/NotifyTaskService.h"
#include "../common/OplogService.h"
#include "../common/OpsService.h"
#include "../filters/AdminAuthFilter.h"

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#  include <io.h>
#else
#  include <sys/sysinfo.h>
#  include <sys/statvfs.h>
#  include <unistd.h>
#endif

// ══════════════════════════════════════════════════════════════
// 运维指标采集器 (单例)
// ══════════════════════════════════════════════════════════════
class OpsMetrics {
public:
    static OpsMetrics& instance() {
        static OpsMetrics inst;
        return inst;
    }

    // ── 请求计数 (在 filter 或 main 里调用) ──
    void recordRequest(int statusCode, double latencyMs) {
        std::lock_guard<std::mutex> lk(mu_);
        totalReqs_++;
        if (statusCode >= 400) errorReqs_++;
        if (latencyMs > 3000) slowReqs_++;
        latencySum_ += latencyMs;

        // 滚动窗口 (最近 60s QPS)
        auto now = std::chrono::steady_clock::now();
        recentReqs_.push_back({now, statusCode, latencyMs});
        while (!recentReqs_.empty() &&
               std::chrono::duration_cast<std::chrono::seconds>(
                   now - recentReqs_.front().ts).count() > 60) {
            recentReqs_.pop_front();
        }
    }

    // ── 告警 ──
    struct Alert {
        std::string level;   // warning / error / critical
        std::string source;
        std::string message;
        int64_t     ts;
    };

    void addAlert(const std::string& level, const std::string& source,
                  const std::string& message) {
        std::lock_guard<std::mutex> lk(mu_);
        int64_t now = std::time(nullptr);
        alerts_.push_back({level, source, message, now});
        if (alerts_.size() > 500) alerts_.pop_front();
    }

    void clearAlerts() {
        std::lock_guard<std::mutex> lk(mu_);
        alerts_.clear();
    }

    // ── 错误日志 ──
    void addError(const std::string& msg) {
        std::lock_guard<std::mutex> lk(mu_);
        int64_t now = std::time(nullptr);
        errorLogs_.push_back({now, msg});
        if (errorLogs_.size() > 200) errorLogs_.pop_front();
    }

    // ── 读取 ──
    Json::Value getRequestStats() {
        std::lock_guard<std::mutex> lk(mu_);
        Json::Value v;
        v["total_requests"]  = (Json::UInt64)totalReqs_;
        v["error_requests"]  = (Json::UInt64)errorReqs_;
        v["slow_requests"]   = (Json::UInt64)slowReqs_;
        v["avg_latency_ms"]  = totalReqs_ > 0 ? latencySum_ / totalReqs_ : 0.0;

        // 最近60s QPS
        int recent = (int)recentReqs_.size();
        double recentErrors = 0, recentLatency = 0;
        for (auto& r : recentReqs_) {
            if (r.status >= 400) recentErrors++;
            recentLatency += r.latencyMs;
        }
        v["qps_60s"]         = recent / 60.0;
        v["error_rate_60s"]  = recent > 0 ? recentErrors / recent * 100.0 : 0.0;
        v["avg_latency_60s"] = recent > 0 ? recentLatency / recent : 0.0;
        v["recent_count"]    = recent;
        return v;
    }

    Json::Value getAlerts() {
        std::lock_guard<std::mutex> lk(mu_);
        Json::Value arr(Json::arrayValue);
        for (auto it = alerts_.rbegin(); it != alerts_.rend(); ++it) {
            Json::Value a;
            a["level"]   = it->level;
            a["source"]  = it->source;
            a["message"] = it->message;
            a["ts"]      = (Json::Int64)it->ts;
            arr.append(a);
        }
        return arr;
    }

    Json::Value getErrors(int limit = 50) {
        std::lock_guard<std::mutex> lk(mu_);
        Json::Value arr(Json::arrayValue);
        int count = 0;
        for (auto it = errorLogs_.rbegin(); it != errorLogs_.rend() && count < limit; ++it, ++count) {
            Json::Value e;
            e["ts"]  = (Json::Int64)it->first;
            e["msg"] = it->second;
            arr.append(e);
        }
        return arr;
    }

private:
    OpsMetrics() = default;
    std::mutex mu_;
    uint64_t totalReqs_ = 0, errorReqs_ = 0, slowReqs_ = 0;
    double   latencySum_ = 0;

    struct ReqRecord { std::chrono::steady_clock::time_point ts; int status; double latencyMs; };
    std::deque<ReqRecord> recentReqs_;
    std::deque<Alert> alerts_;
    std::deque<std::pair<int64_t, std::string>> errorLogs_;
};


// ══════════════════════════════════════════════════════════════
// 运维面板控制器
// ══════════════════════════════════════════════════════════════
class OpsCtrl : public drogon::HttpController<OpsCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(OpsCtrl::overview,      "/admin/api/ops/overview",      drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::channels,      "/admin/api/ops/channels",      drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::requests,      "/admin/api/ops/requests",      drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::alerts,        "/admin/api/ops/alerts",        drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::errors,        "/admin/api/ops/errors",        drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::clearAlerts,   "/admin/api/ops/alert/clear",   drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::dependencies,  "/admin/api/ops/dependencies",  drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::dashboard,     "/admin/api/ops/dashboard",     drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::channelDaily,  "/admin/api/ops/channelDaily",  drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::callbackLogs,  "/admin/api/ops/callbackLogs",  drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::notifyTasks,   "/admin/api/ops/notifyTasks",   drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::notifyRetry,   "/admin/api/ops/notifyRetry",   drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::logTail,       "/admin/api/ops/logTail",       drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::deploy,        "/admin/api/ops/deploy",        drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::configCheck,   "/admin/api/ops/configCheck",   drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::backup,        "/admin/api/ops/backup",        drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::backupList,    "/admin/api/ops/backupList",    drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::syncDailyStat, "/admin/api/ops/syncDailyStat", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::autoCheck,     "/admin/api/ops/autoCheck",     drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::deployLog,     "/admin/api/ops/deployLog",     drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(OpsCtrl::deployTips,    "/admin/api/ops/deployTips",    drogon::Get,  "AdminAuthFilter");
    METHOD_LIST_END

    // ── 系统资源概况 ──
    void overview(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        Json::Value data;

        // 主进程运行时间
        data["process"] = ProcessManager::getStatusJson();

#ifdef _WIN32
        // Windows: 内存
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            data["memory"]["total_mb"]     = (Json::UInt64)(mem.ullTotalPhys / 1048576);
            data["memory"]["used_mb"]      = (Json::UInt64)((mem.ullTotalPhys - mem.ullAvailPhys) / 1048576);
            data["memory"]["available_mb"] = (Json::UInt64)(mem.ullAvailPhys / 1048576);
            data["memory"]["usage_pct"]    = (int)mem.dwMemoryLoad;
        }

        // Windows: 磁盘
        ULARGE_INTEGER freeBytes, totalBytes;
        if (GetDiskFreeSpaceExA(".", &freeBytes, &totalBytes, nullptr)) {
            uint64_t total = totalBytes.QuadPart / 1048576;
            uint64_t free_ = freeBytes.QuadPart / 1048576;
            data["disk"]["total_mb"] = (Json::UInt64)total;
            data["disk"]["free_mb"]  = (Json::UInt64)free_;
            data["disk"]["used_mb"]  = (Json::UInt64)(total - free_);
            data["disk"]["usage_pct"]= total > 0 ? (int)((total - free_) * 100 / total) : 0;
        }

        // Windows: CPU (通过 GetSystemTimes 近似)
        FILETIME idle1, kernel1, user1, idle2, kernel2, user2;
        GetSystemTimes(&idle1, &kernel1, &user1);
        Sleep(200);
        GetSystemTimes(&idle2, &kernel2, &user2);
        auto ft2u = [](FILETIME a, FILETIME b) -> uint64_t {
            uint64_t va = ((uint64_t)a.dwHighDateTime << 32) | a.dwLowDateTime;
            uint64_t vb = ((uint64_t)b.dwHighDateTime << 32) | b.dwLowDateTime;
            return vb - va;
        };
        uint64_t idleDiff   = ft2u(idle1, idle2);
        uint64_t kernelDiff = ft2u(kernel1, kernel2);
        uint64_t userDiff   = ft2u(user1, user2);
        uint64_t totalDiff  = kernelDiff + userDiff;
        data["cpu"]["usage_pct"] = totalDiff > 0 ? (int)((totalDiff - idleDiff) * 100 / totalDiff) : 0;

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        data["cpu"]["cores"] = (int)si.dwNumberOfProcessors;

        // Windows: 本进程内存
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            data["self"]["rss_mb"] = (Json::UInt64)(pmc.WorkingSetSize / 1048576);
            data["self"]["vms_mb"] = (Json::UInt64)(pmc.PrivateUsage / 1048576);
        }
#else
        // Linux: 内存
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            uint64_t totalMb = (uint64_t)si.totalram * si.mem_unit / 1048576;
            uint64_t freeMb  = (uint64_t)si.freeram  * si.mem_unit / 1048576;
            data["memory"]["total_mb"]     = (Json::UInt64)totalMb;
            data["memory"]["used_mb"]      = (Json::UInt64)(totalMb - freeMb);
            data["memory"]["available_mb"] = (Json::UInt64)freeMb;
            data["memory"]["usage_pct"]    = totalMb > 0 ? (int)((totalMb - freeMb) * 100 / totalMb) : 0;
        }

        // Linux: 磁盘
        struct statvfs svfs;
        if (statvfs(".", &svfs) == 0) {
            uint64_t total = (uint64_t)svfs.f_blocks * svfs.f_frsize / 1048576;
            uint64_t free_ = (uint64_t)svfs.f_bavail * svfs.f_frsize / 1048576;
            data["disk"]["total_mb"] = (Json::UInt64)total;
            data["disk"]["free_mb"]  = (Json::UInt64)free_;
            data["disk"]["used_mb"]  = (Json::UInt64)(total - free_);
            data["disk"]["usage_pct"]= total > 0 ? (int)((total - free_) * 100 / total) : 0;
        }

        // Linux: CPU (从 /proc/stat 读取)
        data["cpu"]["usage_pct"] = readLinuxCpuPercent();
        data["cpu"]["cores"]     = (int)sysconf(_SC_NPROCESSORS_ONLN);

        // Linux: 本进程内存 (从 /proc/self/status)
        data["self"]["rss_mb"] = readProcRssKb() / 1024;
#endif

        // 数据库概况
        data["db"] = getDbStats();

        // 业务健康度摘要 (参考 MPay AdminDashboardService.health)
        data["health"] = getHealthSummary();

        // 系统时间
        data["server_time"] = (Json::Int64)std::time(nullptr);

        // 应用信息
        data["application"]["framework"] = "Drogon (WePay-Cpp)";
#ifdef _WIN32
        data["application"]["os"] = "Windows";
#else
        data["application"]["os"] = "Linux";
#endif

        RESP_OK(cb, data);
    }

    // ── 通道健康度 ──
    void channels(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        auto &db = PayDb::instance();
        std::string hours = req->getParameter("hours");
        if (hours.empty()) hours = "720";
        int h = 720; try { h = std::stoi(hours); } catch (...) {}
        int64_t since = std::time(nullptr) - h * 3600;

        // 按通道统计成功/失败/金额
        auto rows = db.query(
            "SELECT c.channel_name, c.plugin, o.channel_id, "
            "  COUNT(*) AS total, "
            "  SUM(CASE WHEN o.state=1 THEN 1 ELSE 0 END) AS success, "
            "  SUM(CASE WHEN o.state=-1 THEN 1 ELSE 0 END) AS failed, "
            "  SUM(CASE WHEN o.state=1 THEN CAST(o.amount AS NUMERIC(12,2)) ELSE 0 END) AS success_amount "
            "FROM pay_order o "
            "LEFT JOIN pay_channel c ON c.id=o.channel_id "
            "WHERE o.created_at >= ? "
            "GROUP BY o.channel_id, c.channel_name, c.plugin "
            "ORDER BY total DESC",
            {std::to_string(since)});

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value ch;
            ch["channel_name"]   = r.count("channel_name") ? r["channel_name"] : "未知";
            ch["plugin"]         = r.count("plugin") ? r["plugin"] : "";
            ch["channel_id"]     = r.count("channel_id") ? std::stoi(r["channel_id"]) : 0;
            int total   = std::stoi(r["total"]);
            int success = std::stoi(r["success"]);
            int failed  = std::stoi(r["failed"]);
            ch["total"]          = total;
            ch["success"]        = success;
            ch["failed"]         = failed;
            ch["pending"]        = total - success - failed;
            ch["success_rate"]   = total > 0 ? (double)success / total * 100.0 : 0.0;
            ch["success_amount"] = r["success_amount"];
            // 状态判断
            double rate = total > 0 ? (double)success / total : 1.0;
            ch["health"] = rate >= 0.9 ? "good" : (rate >= 0.5 ? "warning" : "critical");
            arr.append(ch);
        }

        Json::Value data;
        data["hours"]    = h;
        data["channels"] = arr;
        RESP_OK(cb, data);
    }

    // ── 请求统计 ──
    void requests(const drogon::HttpRequestPtr &,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        RESP_OK(cb, OpsMetrics::instance().getRequestStats());
    }

    // ── 告警列表 ──
    void alerts(const drogon::HttpRequestPtr &,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        Json::Value data;
        data["alerts"] = OpsMetrics::instance().getAlerts();
        RESP_OK(cb, data);
    }

    // ── 错误日志 ──
    void errors(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int limit = 50;
        try { limit = std::stoi(req->getParameter("limit")); } catch (...) {}
        Json::Value data;
        data["errors"] = OpsMetrics::instance().getErrors(limit);
        RESP_OK(cb, data);
    }

    // ── 清除告警 ──
    void clearAlerts(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:manage");
        OpsMetrics::instance().clearAlerts();
        OplogService::adminLog(req, "ops", "clearAlerts", "", "清除全部告警");
        RESP_MSG(cb, "告警已清除");
    }

    // ══════════════════════════════════════════════════════════════
    // 依赖健康检查 (参考 MPay SystemOpsStatusService.dependencies)
    // ══════════════════════════════════════════════════════════════
    void dependencies(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        Json::Value arr(Json::arrayValue);

        // 1. 数据库连通性
        {
            Json::Value d;
            d["name"] = "SQLite";
            auto t0 = std::chrono::steady_clock::now();
            try {
                auto &db = PayDb::instance();
                db.queryOne("SELECT 1 AS ok");
                auto ms = std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                d["status"] = "ok";
                d["status_text"] = "连接正常";
                d["message"] = "SELECT 1 OK";
                d["latency_text"] = fmtMs(ms);
                d["tone"] = "success";
            } catch (std::exception &e) {
                auto ms = std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                d["status"] = "failed";
                d["status_text"] = "异常";
                d["message"] = e.what();
                d["latency_text"] = fmtMs(ms);
                d["tone"] = "danger";
            }
            arr.append(d);
        }

        // 2. 配置文件
        {
            Json::Value d;
            d["name"] = "配置文件";
            auto t0 = std::chrono::steady_clock::now();
            bool ok = true;
            std::string msg;
#ifdef _WIN32
            std::string cfgPath = "config.json";
#else
            std::string cfgPath = "config.json";
#endif
            std::ifstream f(cfgPath);
            if (!f.good()) {
                ok = false;
                msg = "缺少 config.json";
            } else {
                Json::Value root;
                Json::Reader reader;
                std::string content((std::istreambuf_iterator<char>(f)), {});
                if (!reader.parse(content, root)) {
                    ok = false;
                    msg = "config.json 解析失败";
                } else {
                    msg = "基础配置存在";
                }
            }
            auto ms = std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            d["status"] = ok ? "ok" : "failed";
            d["status_text"] = ok ? "正常" : "异常";
            d["message"] = msg;
            d["latency_text"] = fmtMs(ms);
            d["tone"] = ok ? "success" : "danger";
            arr.append(d);
        }

        // 3. 日志目录可写
        {
            Json::Value d;
            d["name"] = "日志目录";
            std::string logDir = "logs";
#ifdef _WIN32
            bool writable = (_access(logDir.c_str(), 2) == 0);
#else
            bool writable = (access(logDir.c_str(), W_OK) == 0);
#endif
            d["status"] = writable ? "ok" : "failed";
            d["status_text"] = writable ? "可写" : "不可写";
            d["message"] = logDir;
            d["latency_text"] = "0.0 ms";
            d["tone"] = writable ? "success" : "danger";
            arr.append(d);
        }

        // 4. 上传目录
        {
            Json::Value d;
            d["name"] = "上传目录";
            std::string upDir = "upload";
#ifdef _WIN32
            bool writable = (_access(upDir.c_str(), 2) == 0);
#else
            bool writable = (access(upDir.c_str(), W_OK) == 0);
#endif
            d["status"] = writable ? "ok" : "failed";
            d["status_text"] = writable ? "可写" : "不可写";
            d["message"] = upDir;
            d["latency_text"] = "0.0 ms";
            d["tone"] = writable ? "success" : (std::string)"warning";
            arr.append(d);
        }

        Json::Value data;
        data["dependencies"] = arr;
        RESP_OK(cb, data);
    }

    // ══════════════════════════════════════════════════════════════
    // 业务运营总览 (参考 MPay AdminDashboardService.overview)
    // ══════════════════════════════════════════════════════════════
    void dashboard(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        auto &db = PayDb::instance();
        int64_t now = std::time(nullptr);
        int64_t todayStart = now - (now % 86400);  // UTC 当天起点
        Json::Value data;

        // 今日支付摘要
        auto pay = db.queryOne(
            "SELECT COUNT(*) AS total, "
            "  SUM(CASE WHEN state=1 THEN 1 ELSE 0 END) AS success, "
            "  SUM(CASE WHEN state=-1 THEN 1 ELSE 0 END) AS failed, "
            "  SUM(CASE WHEN state=1 THEN CAST(amount AS NUMERIC(12,2)) ELSE 0 END) AS pay_amount "
            "FROM pay_order WHERE created_at >= ?",
            {std::to_string(todayStart)});
        int payTotal = pay.empty() ? 0 : si(pay["total"]);
        int paySuccess = pay.empty() ? 0 : si(pay["success"]);
        int payFailed = pay.empty() ? 0 : si(pay["failed"]);
        data["today_pay"]["total"] = payTotal;
        data["today_pay"]["success"] = paySuccess;
        data["today_pay"]["failed"] = payFailed;
        data["today_pay"]["pending"] = payTotal - paySuccess - payFailed;
        data["today_pay"]["pay_amount"] = pay.empty() ? "0.00" : pay["pay_amount"];
        data["today_pay"]["success_rate"] = payTotal > 0 ? (double)paySuccess / payTotal * 100.0 : 100.0;

        // 今日退款摘要
        auto refund = db.queryOne(
            "SELECT COUNT(*) AS total, "
            "  SUM(CASE WHEN state=1 THEN CAST(refund_amount AS NUMERIC(12,2)) ELSE 0 END) AS refund_amount "
            "FROM refund_order WHERE created_at >= ?",
            {std::to_string(todayStart)});
        data["today_refund"]["total"] = refund.empty() ? 0 : si(refund["total"]);
        data["today_refund"]["refund_amount"] = refund.empty() ? "0.00" : refund["refund_amount"];

        // 待结算
        auto settle = db.queryOne(
            "SELECT COUNT(*) AS c, "
            "  COALESCE(SUM(CAST(amount AS NUMERIC(12,2))),0) AS amt "
            "FROM settle_order WHERE state=0");
        data["pending_settlement"]["count"] = settle.empty() ? 0 : si(settle["c"]);
        data["pending_settlement"]["amount"] = settle.empty() ? "0.00" : settle["amt"];

        // 通知任务失败数
        auto notify = db.queryOne(
            "SELECT COUNT(*) AS c FROM pay_notify_task WHERE status=-1");
        data["notify_failed"] = notify.empty() ? 0 : si(notify["c"]);

        // 通道异常数 (近7天成功率<50%的通道)
        int64_t sevenDaysAgo = todayStart - 7 * 86400;
        auto abnormal = db.query(
            "SELECT o.channel_id, c.channel_name, COUNT(*) AS total, "
            "  SUM(CASE WHEN o.state=1 THEN 1 ELSE 0 END) AS succ "
            "FROM pay_order o "
            "LEFT JOIN pay_channel c ON c.id=o.channel_id "
            "WHERE o.created_at >= ? "
            "GROUP BY o.channel_id, c.channel_name "
            "HAVING CAST(SUM(CASE WHEN o.state=1 THEN 1 ELSE 0 END) AS NUMERIC(12,2))/COUNT(*) < 0.5 AND COUNT(*) >= 3",
            {std::to_string(sevenDaysAgo)});
        data["abnormal_channels"] = (int)abnormal.size();
        Json::Value abArr(Json::arrayValue);
        for (auto &r : abnormal) {
            Json::Value a;
            a["channel_name"] = r.count("channel_name") ? r["channel_name"] : "未知";
            a["total"] = si(r["total"]);
            a["success"] = si(r["succ"]);
            a["success_rate"] = si(r["total"]) > 0
                ? (double)si(r["succ"]) / si(r["total"]) * 100.0 : 0.0;
            abArr.append(a);
        }
        data["abnormal_channel_list"] = abArr;

        // 健康分 (参考 MPay: 100分起扣)
        int score = 100;
        double successRate = payTotal > 0 ? (double)paySuccess / payTotal * 100.0 : 100.0;
        score -= std::max(0, (int)std::ceil((95.0 - successRate) * 2));
        score -= std::min(20, (notify.empty() ? 0 : si(notify["c"])) * 2);
        score -= std::min(20, (int)abnormal.size() * 5);
        score = std::max(0, std::min(100, score));
        data["health_score"] = score;
        data["health_status"] = score >= 90 ? "稳定" : (score >= 70 ? "关注" : "异常");
        data["health_tone"] = score >= 90 ? "success" : (score >= 70 ? "warning" : "danger");

        // 近7天趋势
        Json::Value trend(Json::arrayValue);
        for (int i = 6; i >= 0; --i) {
            int64_t dayStart = todayStart - i * 86400;
            int64_t dayEnd = dayStart + 86400;
            auto row = db.queryOne(
                "SELECT COUNT(*) AS total, "
                "  SUM(CASE WHEN state=1 THEN 1 ELSE 0 END) AS success, "
                "  SUM(CAST(amount AS NUMERIC(12,2))) AS pay_amount "
                "FROM pay_order WHERE created_at >= ? AND created_at < ?",
                {std::to_string(dayStart), std::to_string(dayEnd)});
            Json::Value t;
            // 格式化日期
            time_t ts = dayStart;
            char dateBuf[16];
            std::strftime(dateBuf, sizeof(dateBuf), "%m-%d", std::localtime(&ts));
            t["date"] = dateBuf;
            t["total"] = row.empty() ? 0 : si(row["total"]);
            t["success"] = row.empty() ? 0 : si(row["success"]);
            t["pay_amount"] = row.empty() ? "0.00" : row["pay_amount"];
            trend.append(t);
        }
        data["pay_trend"] = trend;

        // 支付方式分布
        auto typeRows = db.query(
            "SELECT pay_type, COUNT(*) AS cnt, "
            "  SUM(CAST(amount AS NUMERIC(12,2))) AS amt "
            "FROM pay_order WHERE created_at >= ? "
            "GROUP BY pay_type ORDER BY amt DESC",
            {std::to_string(todayStart - 30 * 86400)});
        Json::Value typeArr(Json::arrayValue);
        for (auto &r : typeRows) {
            Json::Value t;
            t["pay_type"] = r.count("pay_type") ? r["pay_type"] : "未知";
            t["count"] = si(r["cnt"]);
            t["amount"] = r["amt"];
            typeArr.append(t);
        }
        data["pay_type_share"] = typeArr;

        // 最近10笔订单
        auto recentRows = db.query(
            "SELECT o.order_id, o.mch_order_no, o.amount, o.state, o.created_at, o.notify_url, "
            "  m.mch_name, c.channel_name "
            "FROM pay_order o "
            "LEFT JOIN merchant m ON m.id=o.mch_id "
            "LEFT JOIN pay_channel c ON c.id=o.channel_id "
            "ORDER BY o.id DESC LIMIT 10", {});
        Json::Value orderArr(Json::arrayValue);
        for (auto &r : recentRows) {
            Json::Value o;
            o["pay_no"] = r.count("order_id") ? r["order_id"] : "";
            o["mch_order_no"] = r.count("mch_order_no") ? r["mch_order_no"] : "";
            o["amount"] = r.count("amount") ? r["amount"] : "0";
            o["state"] = si(r["state"]);
            o["state_text"] = si(r["state"]) == 1 ? "成功" : (si(r["state"]) == -1 ? "失败" : "处理中");
            o["created_at"] = r.count("created_at") ? r["created_at"] : "";
            o["notify_url"] = r.count("notify_url") ? r["notify_url"] : "";
            o["mch_name"] = r.count("mch_name") ? r["mch_name"] : "未知";
            o["channel_name"] = r.count("channel_name") ? r["channel_name"] : "未知";
            orderArr.append(o);
        }
        data["recent_orders"] = orderArr;

        data["generated_at"] = (Json::Int64)now;
        RESP_OK(cb, data);
    }

    // ══════════════════════════════════════════════════════════════
    // 通道日统计 (参考 MPay ChannelDailyStatController)
    // ══════════════════════════════════════════════════════════════
    void channelDaily(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        auto &db = PayDb::instance();
        int days = 30;
        try { days = std::stoi(req->getParameter("days")); } catch (...) {}
        days = std::max(1, std::min(90, days));

        int64_t now = std::time(nullptr);
        int64_t todayStart = now - (now % 86400);
        Json::Value arr(Json::arrayValue);

        for (int i = days - 1; i >= 0; --i) {
            int64_t dayStart = todayStart - i * 86400;
            int64_t dayEnd = dayStart + 86400;
            auto rows = db.query(
                "SELECT c.id AS channel_id, c.channel_name, c.plugin, "
                "  COUNT(*) AS total, "
                "  SUM(CASE WHEN o.state=1 THEN 1 ELSE 0 END) AS success, "
                "  SUM(CASE WHEN o.state=-1 THEN 1 ELSE 0 END) AS failed, "
                "  SUM(CASE WHEN o.state=1 THEN CAST(o.amount AS NUMERIC(12,2)) ELSE 0 END) AS pay_amount, "
                "  SUM(CASE WHEN o.state=-1 THEN CAST(o.amount AS NUMERIC(12,2)) ELSE 0 END) AS fail_amount "
                "FROM pay_order o "
                "LEFT JOIN pay_channel c ON c.id=o.channel_id "
                "WHERE o.created_at >= ? AND o.created_at < ? "
                "GROUP BY o.channel_id, c.id, c.channel_name, c.plugin ORDER BY total DESC",
                {std::to_string(dayStart), std::to_string(dayEnd)});

            time_t ts = dayStart;
            char dateBuf[16];
            std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", std::localtime(&ts));

            for (auto &r : rows) {
                Json::Value row;
                row["date"] = dateBuf;
                row["channel_id"] = si(r["channel_id"]);
                row["channel_name"] = r.count("channel_name") ? r["channel_name"] : "未知";
                row["plugin"] = r.count("plugin") ? r["plugin"] : "";
                int total = si(r["total"]);
                int success = si(r["success"]);
                int failed = si(r["failed"]);
                row["total"] = total;
                row["success"] = success;
                row["failed"] = failed;
                row["pay_amount"] = r["pay_amount"];
                row["fail_amount"] = r["fail_amount"];
                double rate = total > 0 ? (double)success / total : 1.0;
                row["success_rate"] = rate * 100.0;
                // 健康分: 100起, 成功率低扣分, 失败数扣分
                int hscore = 100;
                hscore -= std::max(0, (int)std::ceil((95.0 - rate * 100.0) * 2));
                hscore -= std::min(30, failed * 3);
                hscore = std::max(0, std::min(100, hscore));
                row["health_score"] = hscore;
                arr.append(row);
            }
        }

        Json::Value data;
        data["days"] = days;
        data["stats"] = arr;
        RESP_OK(cb, data);
    }

    // ══════════════════════════════════════════════════════════════
    // 回调日志列表 (参考 MPay PayCallbackLogController)
    // ══════════════════════════════════════════════════════════════
    void callbackLogs(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        auto &db = PayDb::instance();
        int page = 1, pageSize = 20;
        try { page = std::stoi(req->getParameter("page")); } catch (...) {}
        try { pageSize = std::stoi(req->getParameter("page_size")); } catch (...) {}
        page = std::max(1, page);
        pageSize = std::max(1, std::min(100, pageSize));
        int offset = (page - 1) * pageSize;

        std::string where = "1=1";
        std::vector<std::string> params;
        std::string orderId = req->getParameter("order_id");
        std::string plugin = req->getParameter("plugin");
        if (!orderId.empty()) { where += " AND order_id=?"; params.push_back(orderId); }
        if (!plugin.empty()) { where += " AND plugin=?"; params.push_back(plugin); }

        auto countR = db.queryOne("SELECT COUNT(*) AS c FROM pay_callback_log WHERE " + where, params);
        int total = countR.empty() ? 0 : si(countR["c"]);

        auto limitParams = params;
        limitParams.push_back(std::to_string(pageSize));
        limitParams.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT * FROM pay_callback_log WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", limitParams);

        Json::Value list(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value row;
            for (auto &kv : r) row[kv.first] = kv.second;
            list.append(row);
        }

        Json::Value data;
        data["list"] = list;
        data["total"] = total;
        data["page"] = page;
        data["page_size"] = pageSize;
        RESP_OK(cb, data);
    }

    // ══════════════════════════════════════════════════════════════
    // 通知任务列表 (参考 MPay MerchantNotifyTaskController)
    // ══════════════════════════════════════════════════════════════
    void notifyTasks(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        auto &db = PayDb::instance();
        int page = 1, pageSize = 20;
        try { page = std::stoi(req->getParameter("page")); } catch (...) {}
        try { pageSize = std::stoi(req->getParameter("page_size")); } catch (...) {}
        page = std::max(1, page);
        pageSize = std::max(1, std::min(100, pageSize));
        int offset = (page - 1) * pageSize;

        std::string where = "1=1";
        std::vector<std::string> params;
        std::string orderId = req->getParameter("order_id");
        std::string status = req->getParameter("status");
        std::string plugin = req->getParameter("plugin");
        if (!orderId.empty()) { where += " AND order_id=?"; params.push_back(orderId); }
        if (!status.empty()) { where += " AND status=?"; params.push_back(status); }
        if (!plugin.empty()) { where += " AND plugin=?"; params.push_back(plugin); }

        auto countR = db.queryOne("SELECT COUNT(*) AS c FROM pay_notify_task WHERE " + where, params);
        int total = countR.empty() ? 0 : si(countR["c"]);

        auto limitParams = params;
        limitParams.push_back(std::to_string(pageSize));
        limitParams.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT id,order_id,plugin,notify_full_url,status,retry_cnt,"
            "next_retry_at,last_response,created_at,updated_at"
            " FROM pay_notify_task WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", limitParams);

        Json::Value list(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value row;
            for (auto &kv : r) row[kv.first] = kv.second;
            list.append(row);
        }

        Json::Value data;
        data["list"] = list;
        data["total"] = total;
        data["page"] = page;
        data["page_size"] = pageSize;
        RESP_OK(cb, data);
    }

    // ══════════════════════════════════════════════════════════════
    // 通知任务手动重试 (参考 MPay MerchantNotifyTaskController.retry)
    // ══════════════════════════════════════════════════════════════
    void notifyRetry(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        if (id.empty()) { RESP_ERR(cb, "缺少 id"); return; }

        auto &db = PayDb::instance();
        auto task = db.queryOne("SELECT * FROM pay_notify_task WHERE id=?", {id});
        if (task.empty()) { RESP_ERR(cb, "通知任务不存在"); return; }

        // 重置状态为待发送
        db.exec("UPDATE pay_notify_task SET status=0, retry_cnt=retry_cnt+1, "
                "next_retry_at=? WHERE id=?",
                {std::to_string(std::time(nullptr)), id});

        OplogService::adminLog(req, "ops", "notifyRetry", id, "手动重试通知");
        RESP_MSG(cb, "已提交重试");
    }

    // ══════════════════════════════════════════════════════════════
    // 日志文件尾部 (参考 MPay SystemOpsStatusService.logSummary)
    // ══════════════════════════════════════════════════════════════
    void logTail(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        int lines = 100;
        try { lines = std::stoi(req->getParameter("lines")); } catch (...) {}
        lines = std::max(10, std::min(500, lines));

        // 日志文件列表
        std::vector<std::string> logFiles;
#ifdef _WIN32
        logFiles.push_back("logs\\wepay.log");
        // 按日期格式查找当天日志
        {
            time_t now = std::time(nullptr);
            char dateBuf[16];
            std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", std::localtime(&now));
            std::string todayLog = std::string("logs\\wepay-") + dateBuf + ".log";
            logFiles.push_back(todayLog);
        }
#else
        logFiles.push_back("logs/wepay.log");
        {
            time_t now = std::time(nullptr);
            char dateBuf[16];
            std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", std::localtime(&now));
            std::string todayLog = std::string("logs/wepay-") + dateBuf + ".log";
            logFiles.push_back(todayLog);
        }
#endif

        Json::Value data;
        Json::Value files(Json::arrayValue);
        int totalErrors = 0;
        Json::Value recentErrors(Json::arrayValue);

        for (auto &path : logFiles) {
            std::ifstream f(path, std::ios::ate | std::ios::binary);
            Json::Value fi;
            fi["name"] = path;
            fi["exists"] = f.good();
            if (!f.good()) {
                fi["size_text"] = "—";
                fi["lines"] = Json::Value(Json::arrayValue);
                files.append(fi);
                continue;
            }

            // 文件大小
            auto size = f.tellg();
            fi["size_text"] = fmtBytes((int64_t)size);

            // 读尾部 (最多128KB)
            int64_t readSize = std::min((int64_t)size, (int64_t)(128 * 1024));
            f.seekg(-readSize, std::ios::end);
            std::string content(readSize, '\0');
            f.read(&content[0], readSize);

            // 拆行
            std::vector<std::string> allLines;
            std::istringstream iss(content);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                allLines.push_back(line);
            }

            // 取最后N行
            int start = std::max(0, (int)allLines.size() - lines);
            Json::Value lineArr(Json::arrayValue);
            for (int i = start; i < (int)allLines.size(); ++i) {
                lineArr.append(allLines[i]);
                // 扫描错误关键字
                auto &l = allLines[i];
                if (l.find("error") != std::string::npos ||
                    l.find("ERROR") != std::string::npos ||
                    l.find("exception") != std::string::npos ||
                    l.find("FATAL") != std::string::npos ||
                    l.find("失败") != std::string::npos ||
                    l.find("异常") != std::string::npos) {
                    totalErrors++;
                    if (recentErrors.size() < 20) {
                        Json::Value e;
                        e["file"] = path;
                        e["message"] = l;
                        recentErrors.append(e);
                    }
                }
            }
            fi["lines"] = lineArr;
            files.append(fi);
        }

        data["files"] = files;
        data["error_count"] = totalErrors;
        data["recent_errors"] = recentErrors;
        RESP_OK(cb, data);
    }

    // ── 部署环境信息 ──
    void deploy(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        RESP_OK(cb, OpsService::getDeployInfo());
    }

    // ── 配置校验 ──
    void configCheck(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        RESP_OK(cb, OpsService::validateConfig());
    }

    // ── 在线备份 (SQLite) ──
    void backup(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:manage");
        auto result = OpsService::backupDatabase();
        OplogService::adminLog(req, "ops", "backup", "",
            result["ok"].asBool() ? result["file"].asString() : "failed");
        // 记录到部署日志
        auto &db = PayDb::instance();
        db.exec("INSERT INTO ops_deploy_log(action,operator,detail,result,created_at) VALUES(?,?,?,?,?)",
            {"backup", req->getHeader("X-User") , result["file"].asString(),
             result["ok"].asBool() ? "success" : "failed",
             std::to_string(std::time(nullptr))});
        if (result["ok"].asBool())
            RESP_OK(cb, result);
        else
            RESP_ERR(cb, result["message"].asString());
    }

    // ── 备份列表 ──
    void backupList(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        Json::Value data;
        data["backups"] = OpsService::listBackups();
        RESP_OK(cb, data);
    }

    // ── 手动同步通道日统计 ──
    void syncDailyStat(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:manage");
        auto json = req->getJsonObject();
        std::string date;
        if (json && json->isMember("date"))
            date = (*json)["date"].asString();
        OpsService::syncChannelDailyStat(date);
        OplogService::adminLog(req, "ops", "syncDailyStat", date, "手动同步通道日统计");
        RESP_MSG(cb, "同步完成");
    }

    // ── 手动触发自动告警检测 ──
    void autoCheck(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:manage");
        auto alerts = OpsService::runAutoCheck();
        auto &metrics = OpsMetrics::instance();
        for (auto &a : alerts) {
            metrics.addAlert(a.level, a.source, a.message);
        }
        Json::Value data;
        data["detected"] = (int)alerts.size();
        Json::Value arr(Json::arrayValue);
        for (auto &a : alerts) {
            Json::Value item;
            item["level"]   = a.level;
            item["source"]  = a.source;
            item["message"] = a.message;
            arr.append(item);
        }
        data["alerts"] = arr;
        RESP_OK(cb, data);
    }

    // ── 部署操作日志 ──
    void deployLog(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        auto &db = PayDb::instance();
        int page = 1, pageSize = 20;
        auto pp = req->getParameter("page");
        auto ps = req->getParameter("page_size");
        if (!pp.empty()) try { page = std::stoi(pp); } catch (...) {}
        if (!ps.empty()) try { pageSize = std::stoi(ps); } catch (...) {}
        if (pageSize > 100) pageSize = 100;
        int offset = (page - 1) * pageSize;

        auto total = db.queryOne("SELECT COUNT(*) AS c FROM ops_deploy_log");
        auto rows = db.query(
            "SELECT * FROM ops_deploy_log ORDER BY id DESC LIMIT ? OFFSET ?",
            {std::to_string(pageSize), std::to_string(offset)});

        Json::Value data;
        data["total"] = total.empty() ? 0 : std::stoi(total["c"]);
        Json::Value list(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]         = r.count("id") ? r.at("id") : "";
            item["action"]     = r.count("action") ? r.at("action") : "";
            item["operator"]   = r.count("operator") ? r.at("operator") : "";
            item["detail"]     = r.count("detail") ? r.at("detail") : "";
            item["ip"]         = r.count("ip") ? r.at("ip") : "";
            item["result"]     = r.count("result") ? r.at("result") : "";
            item["created_at"] = r.count("created_at") ? r.at("created_at") : "";
            list.append(item);
        }
        data["list"] = list;
        RESP_OK(cb, data);
    }

    // ── 部署建议 ──
    void deployTips(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "ops:view");
        Json::Value data;
        data["tips"] = OpsService::getDeployTips();
        data["deploy_mode"] = OpsService::getDeployInfo()["deploy_mode"];
        RESP_OK(cb, data);
    }

private:
    // 字符串转int (安全)
    static int si(const std::string &s, int def = 0) {
        try { return std::stoi(s); } catch (...) { return def; }
    }
    // 用 map 的 at 安全取值
    static int si(const std::map<std::string,std::string> &m, const std::string &k, int def = 0) {
        auto it = m.find(k);
        return it != m.end() ? si(it->second, def) : def;
    }

    // 格式化毫秒
    static std::string fmtMs(double ms) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f ms", ms);
        return buf;
    }

    // 格式化字节
    static std::string fmtBytes(int64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        double val = (double)bytes;
        int idx = 0;
        while (val >= 1024 && idx < 4) { val /= 1024; idx++; }
        char buf[32];
        if (idx == 0) std::snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
        else std::snprintf(buf, sizeof(buf), "%.2f %s", val, units[idx]);
        return buf;
    }

    // 业务健康度摘要 (参考 MPay AdminDashboardService.health)
    static Json::Value getHealthSummary() {
        Json::Value h;
        try {
            auto &db = PayDb::instance();
            int64_t now = std::time(nullptr);
            int64_t todayStart = now - (now % 86400);

            auto pay = db.queryOne(
                "SELECT COUNT(*) AS total, "
                "  SUM(CASE WHEN state=1 THEN 1 ELSE 0 END) AS success "
                "FROM pay_order WHERE created_at >= ?",
                {std::to_string(todayStart)});
            int payTotal = pay.empty() ? 0 : si(pay["total"]);
            int paySuccess = pay.empty() ? 0 : si(pay["success"]);
            double successRate = payTotal > 0 ? (double)paySuccess / payTotal * 100.0 : 100.0;

            auto notify = db.queryOne("SELECT COUNT(*) AS c FROM pay_notify_task WHERE status=-1");
            int notifyFailed = notify.empty() ? 0 : si(notify["c"]);

            auto chR = db.queryOne("SELECT COUNT(*) AS c FROM pay_channel WHERE state=1");
            int activeChannels = chR.empty() ? 0 : si(chR["c"]);

            int score = 100;
            score -= std::max(0, (int)std::ceil((95.0 - successRate) * 2));
            score -= std::min(20, notifyFailed * 2);
            score = std::max(0, std::min(100, score));

            h["score"] = score;
            h["status"] = score >= 90 ? "稳定" : (score >= 70 ? "关注" : "异常");
            h["tone"] = score >= 90 ? "success" : (score >= 70 ? "warning" : "danger");
            h["success_rate"] = successRate;
            h["today_orders"] = payTotal;
            h["notify_failed"] = notifyFailed;
            h["active_channels"] = activeChannels;
        } catch (...) {
            h["score"] = 0;
            h["status"] = "异常";
            h["tone"] = "danger";
        }
        return h;
    }

    static Json::Value getDbStats() {
        Json::Value v;
        auto &db = PayDb::instance();
        auto r1 = db.queryOne("SELECT COUNT(*) AS c FROM pay_order");
        auto r2 = db.queryOne("SELECT COUNT(*) AS c FROM merchant");
        auto r3 = db.queryOne("SELECT COUNT(*) AS c FROM transfer_order");
        auto r4 = db.queryOne("SELECT COUNT(*) AS c FROM refund_order");
        auto r5 = db.queryOne("SELECT COUNT(*) AS c FROM pay_channel WHERE state=1");
        v["orders"]           = r1.empty() ? 0 : std::stoi(r1["c"]);
        v["merchants"]        = r2.empty() ? 0 : std::stoi(r2["c"]);
        v["transfers"]        = r3.empty() ? 0 : std::stoi(r3["c"]);
        v["refunds"]          = r4.empty() ? 0 : std::stoi(r4["c"]);
        v["active_channels"]  = r5.empty() ? 0 : std::stoi(r5["c"]);
        v["backend"]          = db.backendInfo();
        return v;
    }

#ifndef _WIN32
    static int readLinuxCpuPercent() {
        auto read = []() -> std::pair<uint64_t, uint64_t> {
            std::ifstream f("/proc/stat");
            std::string line;
            std::getline(f, line);
            std::istringstream iss(line);
            std::string cpu;
            uint64_t user, nice, sys, idle, iowait, irq, softirq, steal;
            iss >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
            uint64_t total = user + nice + sys + idle + iowait + irq + softirq + steal;
            return {total, idle};
        };
        auto [t1, i1] = read();
        usleep(200000);
        auto [t2, i2] = read();
        uint64_t dt = t2 - t1, di = i2 - i1;
        return dt > 0 ? (int)((dt - di) * 100 / dt) : 0;
    }

    static int64_t readProcRssKb() {
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line);
                std::string label; int64_t val;
                iss >> label >> val;
                return val;
            }
        }
        return 0;
    }
#endif
};
