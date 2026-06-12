// WePay-Cpp — 管理后台: 对账与结算控制器
// 职责：对账差异管理、日报表查询、自动结算触发等对账结算功能
//
// API 端点：
// GET  /admin/api/reconcile/diff         差异列表
// POST /admin/api/reconcile/diff/resolve  处理差异
// GET  /admin/api/reconcile/report       日报表
// POST /admin/api/reconcile/settle/auto  触发自动结算
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ReconcileService.h" // 对账服务
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class AdminReconcileCtrl : public drogon::HttpController<AdminReconcileCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminReconcileCtrl::diffList,     "/admin/api/reconcile/diff",        drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminReconcileCtrl::resolveDiff,  "/admin/api/reconcile/diff/resolve", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminReconcileCtrl::dayReport,     "/admin/api/reconcile/report",      drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminReconcileCtrl::triggerSettle, "/admin/api/reconcile/settle/auto",  drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    // 差异列表
    void diffList(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int page = safeInt(req->getParameter("page"), 1);
        int size = safeInt(req->getParameter("size"), 20);
        std::string date   = req->getParameter("date");       // YYYY-MM-DD
        int mchId     = safeInt(req->getParameter("mch_id"), 0);
        int diffType  = safeInt(req->getParameter("diff_type"), -1);

        // 默认取昨天的对账差异
        if (date.empty()) {
            std::time_t t = std::time(nullptr) - 86400;
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
            date = buf;
        }

        auto diffs = ReconcileService::listDiffs(date, mchId, diffType, page, size);

        // 统计
        auto statRow = db.queryOne(
            "SELECT COUNT(*) as total,"
            "SUM(CASE WHEN diff_type=1 THEN 1 ELSE 0 END) as missing,"
            "SUM(CASE WHEN diff_type=2 THEN 1 ELSE 0 END) as amt_mismatch,"
            "SUM(CASE WHEN state=0 THEN 1 ELSE 0 END) as pending "
            "FROM reconcile_diff WHERE reconcile_date=?",
            {date});

        Json::Value arr(Json::arrayValue);
        for (auto &d : diffs) {
            Json::Value item;
            item["id"]               = (Json::Int64)d.id;
            item["reconcile_date"]    = d.reconcile_date;
            item["mch_id"]           = d.mch_id;
            item["channel_code"]      = d.channel_code;
            item["order_id"]          = d.order_id;
            item["channel_order_no"]  = d.channel_order_no;
            item["diff_type"]         = d.diff_type;
            item["platform_amount"]   = d.platform_amount;
            item["upstream_amount"]   = d.upstream_amount;
            item["diff_detail"]       = d.diff_detail;
            item["state"]             = d.state;
            item["created_at"]        = (Json::Int64)d.created_at;
            arr.append(item);
        }

        Json::Value data;
        data["list"]   = arr;
        data["date"]   = date;
        if (!statRow.empty()) {
            data["stat_total"]    = safeInt(statRow, "total", 0);
            data["stat_missing"]  = safeInt(statRow, "missing", 0);
            data["stat_amt_mismatch"] = safeInt(statRow, "amt_mismatch", 0);
            data["stat_pending"]  = safeInt(statRow, "pending", 0);
        }
        RESP_OK(cb, data);
    }

    // 处理差异（确认/忽略）
    void resolveDiff(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        int64_t diffId = (*body).get("id", 0).asInt64();
        int state      = (*body).get("state", 0).asInt();  // 1=确认 2=忽略
        std::string remark = (*body).get("remark", "").asString();

        if (diffId <= 0 || (state != 1 && state != 2)) { RESP_ERR(cb, "参数错误"); return; }

        if (ReconcileService::resolveDiff(diffId, state, remark)) {
            RESP_MSG(cb, state == 1 ? "已确认" : "已忽略");
        } else {
            RESP_ERR(cb, "操作失败");
        }
    }

    // 日报表
    void dayReport(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string date = req->getParameter("date");
        int mchId = safeInt(req->getParameter("mch_id"), 0);

        if (date.empty()) {
            std::time_t t = std::time(nullptr) - 86400;
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
            date = buf;
        }

        auto r = ReconcileService::genDayReport(date, mchId);

        Json::Value data;
        data["date"]           = r.date;
        data["mch_id"]        = r.mch_id;
        data["total_orders"]   = r.total_orders;
        data["total_amount"]   = ChannelService::fmtAmount(r.total_amount);
        data["total_fee"]     = ChannelService::fmtAmount(r.total_fee);
        data["total_income"]  = ChannelService::fmtAmount(r.total_income);
        data["refund_count"]  = r.refund_count;
        data["refund_amount"] = ChannelService::fmtAmount(r.refund_amount);
        data["settle_amount"] = ChannelService::fmtAmount(r.settle_amount);
        RESP_OK(cb, data);
    }

    // 触发自动结算
    void triggerSettle(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        double minAmt = 100.0;
        std::string date;

        if (body) {
            minAmt = (*body).get("min_amount", 100.0).asDouble();
            date = (*body).get("date", "").asString();
        }

        // 默认 T+1 结算昨日
        if (date.empty()) {
            std::time_t t = std::time(nullptr) - 86400;
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
            date = buf;
        }

        int count = ReconcileService::autoSettle(date, minAmt);

        Json::Value data;
        data["settle_date"] = date;
        data["min_amount"]  = minAmt;
        data["created"]     = count;
        RESP_OK(cb, data);
    }

private:
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
