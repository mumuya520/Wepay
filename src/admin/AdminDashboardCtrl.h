// WePay-Cpp — 管理后台: 数据仪表盘控制器
// 职责：提供管理员仪表盘的核心数据、趋势分析、商户排行等统计功能
//
// API 端点：
// GET  /admin/api/dashboard/summary     今日/昨日核心指标
// GET  /admin/api/dashboard/trend       近N日趋势
// GET  /admin/api/dashboard/top-merchants  商户营收排行
// GET  /admin/api/dashboard/channels    通道分布
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <algorithm> // 算法库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ChannelService.h" // 通道服务
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class AdminDashboardCtrl : public drogon::HttpController<AdminDashboardCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminDashboardCtrl::summary,   "/admin/api/dashboard/summary",        drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminDashboardCtrl::trend,    "/admin/api/dashboard/trend",         drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminDashboardCtrl::topMerchants, "/admin/api/dashboard/top-merchants", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminDashboardCtrl::channels, "/admin/api/dashboard/channels",      drogon::Get, "AdminAuthFilter");
    METHOD_LIST_END

    // ── 核心指标 ─────────────────────────────────────────────
    void summary(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        auto now = std::time(nullptr);

        // 今日区间
        auto dayStart = getDayStart(now);
        auto ydStart  = dayStart - 86400;
        auto ydEnd    = dayStart;

        Json::Value data;

        // 今日
        data["today"] = queryPeriod(db, dayStart, now);
        // 昨日
        data["yesterday"] = queryPeriod(db, ydStart, ydEnd);
        // 商户总数
        auto mchCnt = db.queryOne(
            "SELECT COUNT(*) AS c FROM merchant WHERE state=1", {});
        data["total_merchants"] = mchCnt.empty() ? 0 : std::stoi(mchCnt[0]["c"]);

        // 通道总数
        auto chCnt = db.queryOne(
            "SELECT COUNT(*) AS c FROM pay_channel WHERE state=1", {});
        data["total_channels"] = chCnt.empty() ? 0 : std::stoi(chCnt[0]["c"]);

        // 冻结余额（平台担保账户）
        auto frozen = db.queryOne(
            "SELECT SUM(CAST(frozen AS REAL)) AS total FROM merchant WHERE state=1", {});
        data["total_frozen"] = frozen.empty() ? "0.00" : frozen[0]["c"].empty() ? "0.00" : frozen[0]["c"];

        RESP_OK(cb, data);
    }

    // ── 趋势数据 ─────────────────────────────────────────────
    void trend(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int days = safeInt(req->getParameter("days"), 7);
        if (days < 1 || days > 90) days = 7;

        auto now = std::time(nullptr);
        auto end = getDayStart(now) + 86400; // 今天结束

        Json::Value series;
        for (int i = days - 1; i >= 0; --i) {
            long long dStart = end - (long long)(i + 1) * 86400;
            long long dEnd   = dStart + 86400;
            auto day = queryPeriod(db, dStart, dEnd);
            day["date"] = formatDate(dStart);
            series.append(day);
        }

        Json::Value data;
        data["series"] = series;
        data["days"]   = days;
        RESP_OK(cb, data);
    }

    // ── 商户排行 ─────────────────────────────────────────────
    void topMerchants(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int days = safeInt(req->getParameter("days"), 7);
        int limit = safeInt(req->getParameter("limit"), 10);
        if (limit < 1) limit = 10;
        if (limit > 50) limit = 50;

        auto now = std::time(nullptr);
        auto dayStart = getDayStart(now - (days - 1) * 86400);
        auto dayEnd   = getDayStart(now) + 86400;

        auto rows = db.query(
            "SELECT m.id,m.mch_no,m.mch_name,"
            "COUNT(p.id) AS order_count,"
            "COALESCE(SUM(CAST(p.amount AS REAL)),0) AS total_amount,"
            "COALESCE(SUM(CAST(p.mch_fee AS REAL)),0) AS total_fee "
            "FROM merchant m "
            "LEFT JOIN pay_order p ON p.mch_id=m.id AND p.state=1 "
            "AND p.pay_time>=? AND p.pay_time<? "
            "WHERE m.state=1 "
            "GROUP BY m.id,m.mch_no,m.mch_name "
            "ORDER BY total_amount DESC LIMIT ?",
            {std::to_string(dayStart), std::to_string(dayEnd), std::to_string(limit)});

        Json::Value list;
        int rank = 1;
        for (auto &r : rows) {
            Json::Value item;
            item["rank"] = rank++;
            item["mch_id"]    = std::stoi(r.at("id"));
            item["mch_no"]   = r.at("mch_no");
            item["mch_name"] = r.count("mch_name") ? r.at("mch_name") : "";
            item["order_count"]  = safeInt(r, "order_count", 0);
            item["total_amount"] = r.count("total_amount") ? r.at("total_amount") : "0.00";
            item["total_fee"]    = r.count("total_fee") ? r.at("total_fee") : "0.00";
            double amt = 0, fee = 0;
            try { amt = std::stod(r.at("total_amount")); } catch (...) {}
            try { fee = std::stod(r.at("total_fee")); } catch (...) {}
            item["net_income"] = ChannelService::fmtAmount(amt - fee);
            list.append(item);
        }

        Json::Value data;
        data["list"] = list;
        data["period_days"] = days;
        RESP_OK(cb, data);
    }

    // ── 通道分布 ─────────────────────────────────────────────
    void channels(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int days = safeInt(req->getParameter("days"), 30);
        auto now = std::time(nullptr);
        auto dayStart = getDayStart(now) - (days - 1) * 86400;
        auto dayEnd   = getDayStart(now) + 86400;

        auto rows = db.query(
            "SELECT c.channel_code,c.channel_name,c.pay_type,"
            "COUNT(p.id) AS order_count,"
            "COALESCE(SUM(CAST(p.amount AS REAL)),0) AS total_amount "
            "FROM pay_channel c "
            "LEFT JOIN pay_order p ON p.channel_id=c.id AND p.state=1 "
            "AND p.pay_time>=? AND p.pay_time<? "
            "WHERE c.state=1 "
            "GROUP BY c.id,c.channel_code,c.channel_name,c.pay_type "
            "ORDER BY total_amount DESC", {});

        double grandTotal = 0;
        for (auto &r : rows) {
            double a = 0;
            try { a = std::stod(r.at("total_amount")); } catch (...) {}
            grandTotal += a;
        }

        Json::Value list;
        for (auto &r : rows) {
            Json::Value item;
            item["channel_code"]  = r.at("channel_code");
            item["channel_name"] = r.count("channel_name") ? r.at("channel_name") : r.at("channel_code");
            item["pay_type"]     = r.count("pay_type") ? r.at("pay_type") : "";
            item["order_count"]  = safeInt(r, "order_count", 0);
            item["total_amount"] = r.count("total_amount") ? r.at("total_amount") : "0.00";
            double a = 0;
            try { a = std::stod(r.at("total_amount")); } catch (...) {}
            item["share"] = grandTotal > 0 ? (a / grandTotal * 100.0) : 0;
            list.append(item);
        }

        Json::Value data;
        data["list"] = list;
        data["period_days"] = days;
        data["grand_total"] = ChannelService::fmtAmount(grandTotal);
        RESP_OK(cb, data);
    }

private:
    static Json::Value queryPeriod(PayDb &db, long long start, long long end) {
        Json::Value o;

        auto orders = db.query(
            "SELECT COUNT(*) AS c,SUM(CAST(amount AS REAL)) AS sum_amt,"
            "SUM(CAST(mch_fee AS REAL)) AS sum_fee "
            "FROM pay_order WHERE state=1 AND pay_time>=? AND pay_time<?",
            {std::to_string(start), std::to_string(end)});
        if (!orders.empty()) {
            o["order_count"]   = safeInt(orders[0], "c", 0);
            o["total_amount"]  = orders[0].count("sum_amt") ? orders[0].at("sum_amt") : "0.00";
            o["total_fee"]     = orders[0].count("sum_fee") ? orders[0].at("sum_fee") : "0.00";
        } else {
            o["order_count"] = 0; o["total_amount"] = "0.00"; o["total_fee"] = "0.00";
        }

        auto refunds = db.query(
            "SELECT COUNT(*) AS c,SUM(CAST(refund_amount AS REAL)) AS sum_amt "
            "FROM refund_order WHERE state=1 AND updated_at>=? AND updated_at<?",
            {std::to_string(start), std::to_string(end)});
        if (!refunds.empty()) {
            o["refund_count"]  = safeInt(refunds[0], "c", 0);
            o["refund_amount"] = refunds[0].count("sum_amt") ? refunds[0].at("sum_amt") : "0.00";
        } else {
            o["refund_count"] = 0; o["refund_amount"] = "0.00";
        }

        double amt = 0, fee = 0, ref = 0;
        try { amt = std::stod(o["total_amount"].asString()); } catch (...) {}
        try { fee = std::stod(o["total_fee"].asString()); } catch (...) {}
        try { ref = std::stod(o["refund_amount"].asString()); } catch (...) {}
        o["net_income"] = ChannelService::fmtAmount(amt - fee - ref);

        return o;
    }

    static long long getDayStart(long long t) {
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
        return (long long)std::mktime(&tmv);
    }

    static std::string formatDate(long long t) {
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
        return std::string(buf);
    }

    static int safeInt(const std::string &s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }
    static int safeInt(const std::map<std::string, std::string> &r,
                        const std::string &k, int def) {
        auto it = r.find(k);
        if (it == r.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }
};
