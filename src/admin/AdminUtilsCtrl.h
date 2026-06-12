// WePay-Cpp — 管理后台: 实用工具控制器
// 职责：图表统计、账单生成、系统公告等实用工具功能
//
// 包含功能：
// 1. 交易趋势图表(近7天)
// 2. 支付方式占比分析
// 3. 商户排行 Top 10
// 4. 账单列表和生成
// 5. 系统公告管理
//
// API 端点：
// GET    /admin/api/chart/trend7        最近7天交易趋势
// GET    /admin/api/chart/payTypePie    支付方式占比(近7天)
// GET    /admin/api/chart/topMch        商户排行 Top 10
// GET    /admin/api/bill/list           账单列表
// POST   /admin/api/bill/generate       生成账单(指定日期/商户)
// GET    /admin/api/bill/detail         账单详情
// GET    /admin/api/article/list        公告列表(管理)
// POST   /admin/api/article/save        新建/编辑
// DELETE /admin/api/article/del         删除
// GET    /article/list                  公开公告(无需鉴权,商户/代理读取)
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <iomanip> // 输入输出格式化库
#include <sstream> // 字符串流库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/PermCheck.h" // 权限检查
#include "../common/OplogService.h" // 操作日志服务
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class AdminUtilsCtrl : public drogon::HttpController<AdminUtilsCtrl> {
public:
    METHOD_LIST_BEGIN
        // 图表
        ADD_METHOD_TO(AdminUtilsCtrl::trend7,     "/admin/api/chart/trend7",     drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminUtilsCtrl::payTypePie, "/admin/api/chart/payTypePie", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminUtilsCtrl::topMch,     "/admin/api/chart/topMch",     drogon::Get, "AdminAuthFilter");
        // 账单
        ADD_METHOD_TO(AdminUtilsCtrl::billList,   "/admin/api/bill/list",     drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminUtilsCtrl::billGen,    "/admin/api/bill/generate", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminUtilsCtrl::billDetail, "/admin/api/bill/detail",   drogon::Get,  "AdminAuthFilter");
        // 公告(管理)
        ADD_METHOD_TO(AdminUtilsCtrl::artList,    "/admin/api/article/list",  drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminUtilsCtrl::artSave,    "/admin/api/article/save",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminUtilsCtrl::artDel,     "/admin/api/article/del",   drogon::Delete, "AdminAuthFilter");
        // 公告(公开)
        ADD_METHOD_TO(AdminUtilsCtrl::publicArticles, "/article/list", drogon::Get);
    METHOD_LIST_END

    // ── 图表 ──────────────────────────────────────────────────
    void trend7(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "dashboard:view");
        long long now = std::time(nullptr);
        long long dayStart = now - (now % 86400);
        Json::Value arr(Json::arrayValue);
        auto &db = PayDb::instance();
        for (int i = 6; i >= 0; --i) {
            long long s = dayStart - i * 86400LL;
            long long e = s + 86400;
            auto r = db.queryOne(
                "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS amt "
                "FROM pay_order WHERE state=1 AND pay_time>=? AND pay_time<?",
                {std::to_string(s), std::to_string(e)});
            Json::Value d;
            d["date"]   = formatDate((time_t)s, "%m-%d");
            d["count"]  = r.empty() ? 0 : std::stoi(r["cnt"]);
            d["amount"] = r.empty() ? "0" : r["amt"];
            arr.append(d);
        }
        RESP_OK(cb, arr);
    }

    void payTypePie(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "dashboard:view");
        long long start = std::time(nullptr) - 7 * 86400LL;
        auto rows = PayDb::instance().query(
            "SELECT pay_type, COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS amt "
            "FROM pay_order WHERE state=1 AND pay_time>=? GROUP BY pay_type",
            {std::to_string(start)});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value d;
            d["name"]   = r["pay_type"];
            d["value"]  = std::stoi(r["cnt"]);
            d["amount"] = r["amt"];
            arr.append(d);
        }
        RESP_OK(cb, arr);
    }

    void topMch(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "dashboard:view");
        long long start = std::time(nullptr) - 30 * 86400LL;
        auto rows = PayDb::instance().query(
            "SELECT m.mch_no, m.mch_name, COUNT(o.id) AS cnt, "
            "COALESCE(SUM(CAST(o.amount AS REAL)),0) AS amt "
            "FROM pay_order o INNER JOIN merchant m ON m.id=o.mch_id "
            "WHERE o.state=1 AND o.pay_time>=? "
            "GROUP BY m.id ORDER BY amt DESC LIMIT 10",
            {std::to_string(start)});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value d;
            d["mch_no"]   = r["mch_no"];
            d["mch_name"] = r["mch_name"];
            d["count"]    = std::stoi(r["cnt"]);
            d["amount"]   = r["amt"];
            arr.append(d);
        }
        RESP_OK(cb, arr);
    }

    // ── 对账单 ────────────────────────────────────────────────
    void billList(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "dashboard:view");
        std::string mchId = req->getParameter("mch_id");
        std::string type  = req->getParameter("bill_type");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        if (!type.empty())  { where += " AND bill_type=?"; params.push_back(type); }
        auto rows = db.query("SELECT * FROM account_bill WHERE " + where +
                              " ORDER BY id DESC LIMIT 100", params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"]          = std::stoi(r["id"]);
            it["mch_id"]      = std::stoi(r["mch_id"]);
            it["bill_type"]   = std::stoi(r["bill_type"]);
            it["order_count"] = std::stoi(r["order_count"]);
            it["refund_count"]= std::stoi(r["refund_count"]);
            it["generated_at"]= (Json::Int64)std::stoll(r["generated_at"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    // 生成账单 body: { "bill_date": "2026-05-03"|"2026-05", "bill_type": 1|2, "mch_id": 0|<id> }
    void billGen(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "dashboard:view");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;

        std::string billDate = j.get("bill_date", "").asString();
        int billType = j.get("bill_type", 1).asInt();   // 1=日 2=月
        int mchId    = j.get("mch_id", 0).asInt();      // 0=全局

        if (billDate.empty()) { RESP_ERR(cb, "bill_date 必填(YYYY-MM-DD 或 YYYY-MM)"); return; }

        // 解析日期范围
        long long start = 0, end = 0;
        if (billType == 1) {  // 日账单
            if (!parseDay(billDate, start, end)) {
                RESP_ERR(cb, "bill_date 格式应为 YYYY-MM-DD"); return;
            }
        } else {              // 月账单
            if (!parseMonth(billDate, start, end)) {
                RESP_ERR(cb, "bill_date 格式应为 YYYY-MM"); return;
            }
        }

        auto &db = PayDb::instance();
        std::string mchFilter = (mchId > 0) ? " AND mch_id=" + std::to_string(mchId) : "";

        auto orderR = db.queryOne(
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS amt, "
            "COALESCE(SUM(CAST(mch_fee_amount AS REAL)),0) AS fee "
            "FROM pay_order WHERE state=1 AND pay_time>=? AND pay_time<?" + mchFilter,
            {std::to_string(start), std::to_string(end)});
        auto refundR = db.queryOne(
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(refund_amount AS REAL)),0) AS amt "
            "FROM refund_order WHERE state=1 AND finished_at>=? AND finished_at<?" + mchFilter,
            {std::to_string(start), std::to_string(end)});

        int orderCnt = orderR.empty() ? 0 : std::stoi(orderR["cnt"]);
        double totalAmt = orderR.empty() ? 0 : safeDouble(orderR["amt"]);
        double feeAmt   = orderR.empty() ? 0 : safeDouble(orderR["fee"]);
        int refundCnt   = refundR.empty() ? 0 : std::stoi(refundR["cnt"]);
        double refundAmt= refundR.empty() ? 0 : safeDouble(refundR["amt"]);
        double netAmt   = totalAmt - refundAmt - feeAmt;

        long long now = std::time(nullptr);

        // upsert
        db.exec("INSERT OR REPLACE INTO account_bill(bill_date,bill_type,mch_id,"
                "order_count,total_amount,refund_count,refund_amount,fee_amount,net_amount,"
                "generated_at) VALUES(?,?,?,?,?,?,?,?,?,?)",
                {billDate, std::to_string(billType), std::to_string(mchId),
                 std::to_string(orderCnt), fmtAmount(totalAmt),
                 std::to_string(refundCnt), fmtAmount(refundAmt),
                 fmtAmount(feeAmt), fmtAmount(netAmt),
                 std::to_string(now)});

        OplogService::adminLog(req, "config", "billGen", billDate,
            "type=" + std::to_string(billType) + " mch=" + std::to_string(mchId));

        Json::Value data;
        data["bill_date"]    = billDate;
        data["order_count"]  = orderCnt;
        data["total_amount"] = fmtAmount(totalAmt);
        data["refund_count"] = refundCnt;
        data["refund_amount"]= fmtAmount(refundAmt);
        data["fee_amount"]   = fmtAmount(feeAmt);
        data["net_amount"]   = fmtAmount(netAmt);
        RESP_OK(cb, data);
    }

    void billDetail(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "dashboard:view");
        std::string id = req->getParameter("id");
        auto r = PayDb::instance().queryOne("SELECT * FROM account_bill WHERE id=?", {id});
        if (r.empty()) { RESP_ERR(cb, "账单不存在"); return; }
        Json::Value data;
        for (auto &[k, v] : r) data[k] = v;
        data["id"]          = std::stoi(r["id"]);
        data["mch_id"]      = std::stoi(r["mch_id"]);
        data["bill_type"]   = std::stoi(r["bill_type"]);
        data["order_count"] = std::stoi(r["order_count"]);
        data["refund_count"]= std::stoi(r["refund_count"]);
        RESP_OK(cb, data);
    }

    // ── 系统公告 ──────────────────────────────────────────────
    void artList(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string target = req->getParameter("target");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!target.empty()) { where += " AND target=?"; params.push_back(target); }
        auto rows = db.query(
            "SELECT id,title,article_type,target,is_top,publisher,state,publish_at,created_at "
            "FROM sys_article WHERE " + where + " ORDER BY is_top DESC, id DESC LIMIT 100",
            params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"]           = std::stoi(r["id"]);
            it["article_type"] = std::stoi(r["article_type"]);
            it["is_top"]       = std::stoi(r["is_top"]);
            it["state"]        = std::stoi(r["state"]);
            it["publish_at"]   = (Json::Int64)std::stoll(r["publish_at"]);
            it["created_at"]   = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void artSave(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        std::string title = j.get("title", "").asString();
        if (title.empty()) { RESP_ERR(cb, "标题必填"); return; }

        long long now = std::time(nullptr);
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO sys_article(title,content,article_type,target,is_top,"
                    "publisher,state,publish_at,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
                    {title, j.get("content", "").asString(),
                     std::to_string(j.get("article_type", 1).asInt()),
                     j.get("target", "all").asString(),
                     std::to_string(j.get("is_top", 0).asInt()),
                     req->getHeader("X-Admin-User"),
                     std::to_string(j.get("state", 1).asInt()),
                     std::to_string(now), std::to_string(now)});
        } else {
            db.exec("UPDATE sys_article SET title=?,content=?,article_type=?,"
                    "target=?,is_top=?,state=? WHERE id=?",
                    {title, j.get("content", "").asString(),
                     std::to_string(j.get("article_type", 1).asInt()),
                     j.get("target", "all").asString(),
                     std::to_string(j.get("is_top", 0).asInt()),
                     std::to_string(j.get("state", 1).asInt()), id});
        }
        RESP_MSG(cb, "已保存");
    }

    void artDel(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM sys_article WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // 公开接口: 商户/代理首页拉取最近公告
    void publicArticles(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string target = req->getParameter("target");
        if (target.empty()) target = "all";
        auto rows = PayDb::instance().query(
            "SELECT id,title,article_type,is_top,publish_at FROM sys_article "
            "WHERE state=1 AND (target=? OR target='all') "
            "ORDER BY is_top DESC, id DESC LIMIT 10",
            {target});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"]           = std::stoi(r["id"]);
            it["title"]        = r["title"];
            it["article_type"] = std::stoi(r["article_type"]);
            it["is_top"]       = std::stoi(r["is_top"]);
            it["publish_at"]   = (Json::Int64)std::stoll(r["publish_at"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

private:
    static std::string formatDate(time_t ts, const char *fmt) {
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &ts);
#else
        localtime_r(&ts, &t);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), fmt, &t);
        return buf;
    }

    static double safeDouble(const std::string &s) {
        try { return std::stod(s); } catch(...) { return 0.0; }
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    // 解析 YYYY-MM-DD → 当天起止时间戳
    static bool parseDay(const std::string &s, long long &start, long long &end) {
        if (s.size() != 10) return false;
        int y, m, d;
        if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return false;
        struct tm t{}; t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
        start = (long long)std::mktime(&t);
        end = start + 86400;
        return true;
    }

    // 解析 YYYY-MM → 当月起止时间戳
    static bool parseMonth(const std::string &s, long long &start, long long &end) {
        if (s.size() != 7) return false;
        int y, m;
        if (std::sscanf(s.c_str(), "%d-%d", &y, &m) != 2) return false;
        struct tm t{}; t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = 1;
        start = (long long)std::mktime(&t);
        // 下个月1号
        if (m == 12) { y++; m = 1; } else { m++; }
        struct tm t2{}; t2.tm_year = y - 1900; t2.tm_mon = m - 1; t2.tm_mday = 1;
        end = (long long)std::mktime(&t2);
        return true;
    }
};
