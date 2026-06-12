// WePay-Cpp — 管理后台: 配置管理控制器
// 职责：系统配置的查询、修改、状态检查、密码修改等配置管理功能
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <chrono> // 时间库
#include <random> // 随机数库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式化库
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../filters/AdminAuthFilter.h"

class ConfigMgrCtrl : public drogon::HttpController<ConfigMgrCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(ConfigMgrCtrl::getConfig,      "/admin/api/config/get",          drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(ConfigMgrCtrl::saveConfig,     "/admin/api/config/save",         drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(ConfigMgrCtrl::getStatus,      "/admin/api/config/status",       drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(ConfigMgrCtrl::resetKey,       "/admin/api/config/resetKey",     drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(ConfigMgrCtrl::changePassword, "/admin/api/user/changePassword", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(ConfigMgrCtrl::getDashboard,   "/admin/api/dashboard",           drogon::Get,  "AdminAuthFilter");
        // 旧路由兼容
        ADD_METHOD_TO(ConfigMgrCtrl::getConfig,      "/api/config/get",          drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(ConfigMgrCtrl::getDashboard,   "/api/dashboard",           drogon::Get,  "AdminAuthFilter");
    METHOD_LIST_END

    void getConfig(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        Json::Value data;
        data["key"]       = db.getSetting("key");
        data["notifyUrl"] = db.getSetting("notifyUrl");
        data["returnUrl"] = db.getSetting("returnUrl");
        data["site_url"]  = db.getSetting("site_url", "");  // 站点根 URL，用于生成回调地址
        data["close"]     = db.getSetting("close", "5");
        data["payQf"]     = db.getSetting("payQf", "1");
        data["wxpay"]     = db.getSetting("wxpay");
        data["zfbpay"]    = db.getSetting("zfbpay");
        data["pid"]       = db.getSetting("pid", "1");
        data["siteName"]  = db.getSetting("siteName", "WePay");
        data["user"]      = db.getSetting("user", "admin");
        data["jkstate"]   = db.getSetting("jkstate", "-1");
        data["lastheart"] = db.getSetting("lastheart", "0");
        data["lastpay"]   = db.getSetting("lastpay",  "0");
        data["templateId"] = db.getSetting("templateId", "default");
        RESP_OK(cb, data);
    }

    void saveConfig(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &db = PayDb::instance();
        static const std::vector<std::string> keys = {
            "key", "notifyUrl", "returnUrl", "site_url",
            "close", "payQf", "wxpay", "zfbpay",
            "pid", "siteName", "user", "pass", "templateId"
        };
        for (auto &k : keys) {
            if ((*body).isMember(k))
                db.setSetting(k, (*body)[k].asString());
        }
        RESP_MSG(cb, "配置保存成功");
    }

    void getStatus(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        std::string lastheart = db.getSetting("lastheart", "0");
        std::string jkstate   = db.getSetting("jkstate", "-1");
        std::string lastpay   = db.getSetting("lastpay",  "0");

        long long now = std::time(nullptr);
        long long lh  = 0;
        try { lh = std::stoll(lastheart); } catch (...) {}
        long long diffSec = now - lh;

        int monitorStatus = 0;  // 0=未知 1=正常 2=异常
        if (lh > 0) {
            if (diffSec < 180) {
                monitorStatus = 1;
                if (jkstate != "1") { db.setSetting("jkstate", "1"); jkstate = "1"; }
            } else {
                monitorStatus = 2;
                if (jkstate != "0") { db.setSetting("jkstate", "0"); jkstate = "0"; }
            }
        } else {
            if (jkstate != "-1") { db.setSetting("jkstate", "-1"); jkstate = "-1"; }
        }

        long long todayTs = todayStart();
        auto cnt = [&](const std::string &sql, const std::vector<std::string> &p) {
            auto r = db.queryOne(sql, p);
            return r.empty() ? 0 : std::stoi(r.begin()->second);
        };
        auto sum = [&](const std::string &sql, const std::vector<std::string> &p) {
            auto r = db.queryOne(sql, p);
            return r.empty() ? 0.0 : std::stod(r.begin()->second);
        };

        Json::Value data;
        data["online"]            = (jkstate == "1");
        data["monitorStatus"]     = monitorStatus;
        data["lastHeart"]         = (Json::Int64)lh;
        data["jkstate"]           = jkstate;
        data["jkstateText"]       = (jkstate == "1") ? "在线" : "离线";
        data["lastpay"]           = lastpay;
        data["heartAgoSecs"]      = (int)diffSec;
        data["todayOrder"]        = cnt("SELECT COUNT(*) AS c FROM pay_order WHERE created_at>=?",
                                        {std::to_string(todayTs)});
        data["todaySuccessOrder"] = cnt("SELECT COUNT(*) AS c FROM pay_order WHERE state=1 AND created_at>=?",
                                        {std::to_string(todayTs)});
        data["todayCloseOrder"]   = cnt("SELECT COUNT(*) AS c FROM pay_order WHERE state=-1 AND created_at>=?",
                                        {std::to_string(todayTs)});
        data["todayMoney"]        = sum("SELECT COALESCE(SUM(CAST(real_amount AS REAL)),0) AS s FROM pay_order WHERE state=1 AND created_at>=?",
                                        {std::to_string(todayTs)});
        data["totalOrder"]        = cnt("SELECT COUNT(*) AS c FROM pay_order", {});
        data["totalMoney"]        = sum("SELECT COALESCE(SUM(CAST(real_amount AS REAL)),0) AS s FROM pay_order WHERE state=1", {});
        data["dbBackend"]         = db.backendInfo();
        RESP_OK(cb, data);
    }

    void resetKey(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string newKey = randomKey(32);
        PayDb::instance().setSetting("key", newKey);
        Json::Value d; d["key"] = newKey;
        RESP_OK(cb, d);
    }

    void changePassword(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string oldPwd = (*body).get("oldPassword", "").asString();
        std::string newPwd = (*body).get("newPassword", "").asString();
        if (newPwd.size() < 6) { RESP_ERR(cb, "新密码至少 6 位"); return; }
        auto &db = PayDb::instance();
        std::string stored = db.getSetting("pass", "admin");
        if (oldPwd != stored) { RESP_ERR(cb, "旧密码错误"); return; }
        db.setSetting("pass", newPwd);
        RESP_MSG(cb, "密码修改成功");
    }

    void getDashboard(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        long long todayTs = todayStart();

        auto cnt = [&](const std::string &sql, const std::vector<std::string> &p) {
            auto r = db.queryOne(sql, p);
            return r.empty() ? 0 : std::stoi(r.begin()->second);
        };
        auto sum = [&](const std::string &sql, const std::vector<std::string> &p) {
            auto r = db.queryOne(sql, p);
            return r.empty() ? 0.0 : std::stod(r.begin()->second);
        };

        std::string jkstate = db.getSetting("jkstate", "-1");
        auto recentRows = db.query("SELECT * FROM pay_order ORDER BY id DESC LIMIT 10", {});
        Json::Value recent(Json::arrayValue);
        for (auto &r : recentRows) {
            Json::Value j;
            for (auto &[k, v] : r) j[k] = v;
            recent.append(j);
        }

        Json::Value data;
        data["totalOrders"]   = cnt("SELECT COUNT(*) AS c FROM pay_order", {});
        data["paidOrders"]    = cnt("SELECT COUNT(*) AS c FROM pay_order WHERE state=1", {});
        data["todayOrders"]   = cnt("SELECT COUNT(*) AS c FROM pay_order WHERE created_at>=?",
                                    {std::to_string(todayTs)});
        data["totalAmount"]   = sum("SELECT COALESCE(SUM(CAST(real_amount AS REAL)),0) AS s FROM pay_order WHERE state=1", {});
        data["todayAmount"]   = sum("SELECT COALESCE(SUM(CAST(real_amount AS REAL)),0) AS s FROM pay_order WHERE state=1 AND created_at>=?",
                                    {std::to_string(todayTs)});
        data["jkstate"]       = jkstate;
        data["monitorOnline"] = (jkstate == "1");
        data["lastHeart"]     = (Json::Int64)std::stoll(db.getSetting("lastheart", "0"));
        data["recentOrders"]  = recent;
        RESP_OK(cb, data);
    }

private:
    static long long todayStart() {
        time_t now = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
        return (long long)std::mktime(&tmv);
    }
    static std::string randomKey(int n) {
        static const char *cs = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s; s.reserve(n);
        for (int i = 0; i < n; ++i) s += cs[rng() % 36];
        return s;
    }
};
