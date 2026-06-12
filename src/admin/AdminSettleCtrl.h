// WePay-Cpp — 管理后台: 结算管理控制器
// 职责：结算单审核、资金日志查询、手动调账等结算管理功能
//
// API 端点：
// GET  /admin/api/settle/list        结算单列表
// POST /admin/api/settle/approve     审核通过
// POST /admin/api/settle/reject      驳回
// GET  /admin/api/money/logs         资金日志
// POST /admin/api/money/manual       手动调账
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ChannelService.h" // 通道服务
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class AdminSettleCtrl : public drogon::HttpController<AdminSettleCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminSettleCtrl::list,    "/admin/api/settle/list",    drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminSettleCtrl::approve, "/admin/api/settle/approve", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSettleCtrl::reject,  "/admin/api/settle/reject",  drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSettleCtrl::logs,    "/admin/api/money/logs",     drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminSettleCtrl::manual,  "/admin/api/money/manual",   drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int page = safeInt(req->getParameter("page"), 1);
        int size = safeInt(req->getParameter("size"), 20);
        int offset = (page - 1) * size;

        std::string where = "1=1";
        std::vector<std::string> params;
        std::string stateP = req->getParameter("state");
        std::string mchP   = req->getParameter("mch_id");
        if (!stateP.empty()) { where += " AND s.state=?"; params.push_back(stateP); }
        if (!mchP.empty())   { where += " AND s.mch_id=?"; params.push_back(mchP); }

        auto cntR = db.query("SELECT COUNT(*) AS c FROM settle_order s WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);

        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT s.*,m.mch_no,m.mch_name FROM settle_order s "
            "LEFT JOIN merchant m ON m.id=s.mch_id WHERE " + where +
            " ORDER BY s.id DESC LIMIT ? OFFSET ?", pp);

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]           = std::stoi(r.at("id"));
            item["settle_no"]    = r.at("settle_no");
            item["mch_id"]       = std::stoi(r.at("mch_id"));
            item["mch_no"]       = r.count("mch_no") ? r.at("mch_no") : "";
            item["mch_name"]     = r.count("mch_name") ? r.at("mch_name") : "";
            item["amount"]       = r.at("amount");
            item["fee"]          = r.at("fee");
            item["real_amount"]  = r.at("real_amount");
            item["state"]        = std::stoi(r.at("state"));
            item["admin_remark"] = r.at("admin_remark");
            item["created_at"]   = (Json::Int64)std::stoll(r.at("created_at"));
            arr.append(item);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void approve(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string remark = (*body).get("remark", "").asString();

        auto &db = PayDb::instance();
        auto settle = db.queryOne("SELECT * FROM settle_order WHERE id=? AND state=0", {id});
        if (settle.empty()) { RESP_ERR(cb, "结算单不存在或已处理"); return; }

        long long now = std::time(nullptr);
        int mchId = std::stoi(settle["mch_id"]);
        double amount = std::stod(settle["amount"]);

        // 从冻结中扣款
        if (!ChannelService::changeMchBalance(mchId, 5, amount, "settle", settle["settle_no"], "结算出款")) {
            RESP_ERR(cb, "商户冻结余额不足"); return;
        }

        db.exec("UPDATE settle_order SET state=2,admin_remark=?,updated_at=? WHERE id=?",
                {remark, std::to_string(now), id});
        RESP_MSG(cb, "审核通过");
    }

    void reject(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string remark = (*body).get("remark", "").asString();

        auto &db = PayDb::instance();
        auto settle = db.queryOne("SELECT * FROM settle_order WHERE id=? AND state=0", {id});
        if (settle.empty()) { RESP_ERR(cb, "结算单不存在或已处理"); return; }

        long long now = std::time(nullptr);
        int mchId = std::stoi(settle["mch_id"]);
        double amount = std::stod(settle["amount"]);

        // 解冻
        ChannelService::changeMchBalance(mchId, 4, amount, "settle_reject", settle["settle_no"], "结算驳回解冻");
        db.exec("UPDATE settle_order SET state=3,admin_remark=?,updated_at=? WHERE id=?",
                {remark, std::to_string(now), id});
        RESP_MSG(cb, "已驳回");
    }

    void logs(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int page = safeInt(req->getParameter("page"), 1);
        int size = safeInt(req->getParameter("size"), 20);
        int offset = (page - 1) * size;

        std::string mchId = req->getParameter("mch_id");
        std::string where = "1=1";
        std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }

        auto cntR = db.query("SELECT COUNT(*) AS c FROM money_log WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);

        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT * FROM money_log WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", pp);

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]            = std::stoi(r.at("id"));
            item["mch_id"]        = std::stoi(r.at("mch_id"));
            item["change_type"]   = std::stoi(r.at("change_type"));
            item["change_amount"] = r.at("change_amount");
            item["before_amount"] = r.at("before_amount");
            item["after_amount"]  = r.at("after_amount");
            item["biz_type"]      = r.at("biz_type");
            item["biz_no"]        = r.at("biz_no");
            item["remark"]        = r.at("remark");
            item["created_at"]    = (Json::Int64)std::stoll(r.at("created_at"));
            arr.append(item);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void manual(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string mchIdStr  = (*body).get("mch_id", "").asString();
        int changeType        = (*body).get("change_type", 1).asInt();
        std::string amountStr = (*body).get("amount", "0").asString();
        std::string remark    = (*body).get("remark", "管理员手动调账").asString();
        int mchId = 0;
        try { mchId = std::stoi(mchIdStr); } catch (...) {}
        double amount = 0;
        try { amount = std::stod(amountStr); } catch (...) {}
        if (mchId <= 0 || amount <= 0) { RESP_ERR(cb, "参数错误"); return; }

        if (!ChannelService::changeMchBalance(mchId, changeType, amount, "manual", "", remark)) {
            RESP_ERR(cb, "操作失败(余额不足或商户不存在)"); return;
        }
        RESP_MSG(cb, "调账成功");
    }

private:
    static int safeInt(const std::string &s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }
};
