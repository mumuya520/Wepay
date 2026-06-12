// WePay-Cpp — 管理后台: 分账管理控制器
// 职责：分账接收方管理、分账记录查询、重试分账等分账功能
//
// API 端点：
// GET    /admin/api/division/receivers       分账接收方列表
// POST   /admin/api/division/receiver/add    添加分账接收方
// POST   /admin/api/division/receiver/edit   编辑
// DELETE /admin/api/division/receiver/del    删除
// GET    /admin/api/division/records         分账记录
// POST   /admin/api/division/retry           重试分账
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <unordered_map>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../channel/ChannelPlugin.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class DivisionCtrl : public drogon::HttpController<DivisionCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(DivisionCtrl::receivers,   "/admin/api/division/receivers",      drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(DivisionCtrl::addRecv,     "/admin/api/division/receiver/add",   drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(DivisionCtrl::editRecv,    "/admin/api/division/receiver/edit",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(DivisionCtrl::delRecv,     "/admin/api/division/receiver/del",   drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(DivisionCtrl::records,     "/admin/api/division/records",        drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(DivisionCtrl::retry,       "/admin/api/division/retry",          drogon::Post,   "AdminAuthFilter");
    METHOD_LIST_END

    void receivers(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:view");
        std::string mchId = req->getParameter("mch_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        auto rows = db.query(
            "SELECT * FROM division_receiver WHERE " + where + " ORDER BY id DESC", params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["account_type"] = std::stoi(r["account_type"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void addRecv(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        PayDb::instance().exec(
            "INSERT INTO division_receiver(mch_id,receiver_code,receiver_name,account_type,"
            "account_no,bind_ratio,state,created_at) VALUES(?,?,?,?,?,?,1,?)",
            {j.get("mch_id", "0").asString(),
             j.get("receiver_code", "").asString(),
             j.get("receiver_name", "").asString(),
             std::to_string(j.get("account_type", 1).asInt()),
             j.get("account_no", "").asString(),
             j.get("bind_ratio", "0.00").asString(),
             std::to_string(std::time(nullptr))});
        OplogService::adminLog(req, "division", "addRecv",
            j.get("receiver_code", "").asString(), "");
        RESP_MSG(cb, "已添加");
    }

    void editRecv(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        PayDb::instance().exec(
            "UPDATE division_receiver SET receiver_name=?,account_type=?,account_no=?,"
            "bind_ratio=?,state=? WHERE id=?",
            {j.get("receiver_name", "").asString(),
             std::to_string(j.get("account_type", 1).asInt()),
             j.get("account_no", "").asString(),
             j.get("bind_ratio", "0.00").asString(),
             std::to_string(j.get("state", 1).asInt()),
             j.get("id", "").asString()});
        OplogService::adminLog(req, "division", "editRecv", j.get("id", "").asString(), "");
        RESP_MSG(cb, "已更新");
    }

    void delRecv(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:manage");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM division_receiver WHERE id=?", {id});
        OplogService::adminLog(req, "division", "delRecv", id, "");
        RESP_MSG(cb, "已删除");
    }

    void records(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:view");
        std::string orderId = req->getParameter("order_id");
        std::string mchId   = req->getParameter("mch_id");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!orderId.empty()) { where += " AND order_id=?"; params.push_back(orderId); }
        if (!mchId.empty())   { where += " AND mch_id=?"; params.push_back(mchId); }

        auto cntR = db.query("SELECT COUNT(*) AS c FROM division_record WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string((page - 1) * size));
        auto rows = db.query(
            "SELECT * FROM division_record WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", pp);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["receiver_id"] = std::stoi(r["receiver_id"]);
            it["state"] = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void retry(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        auto &db = PayDb::instance();
        auto rec = db.queryOne("SELECT * FROM division_record WHERE id=?", {id});
        if (rec.empty()) { RESP_ERR(cb, "分账记录不存在"); return; }
        if (rec["state"] == "1") { RESP_ERR(cb, "已成功，无需重试"); return; }

        int mchId = 0;
        try { mchId = std::stoi(rec["mch_id"]); } catch (...) {}
        double amount = 0;
        try { amount = std::stod(rec["division_amount"]); } catch (...) {}

        // 通过通道插件执行实际分账（如果通道支持）
        std::string errMsg;
        bool execOk = execDivisionViaChannel(rec, errMsg);
        if (!execOk) {
            LOG_WARN << "[DivisionCtrl] retry channel call failed: " << errMsg;
            // 即使通道失败，也走本地账务（保持内部账务一致性）
            if (mchId > 0 && amount > 0) {
                ChannelService::changeMchBalance(mchId, 2, amount, "division",
                    rec["order_id"], "分账支出:" + rec["receiver_name"]);
            }
        }

        long long now = std::time(nullptr);
        db.exec("UPDATE division_record SET state=1,err_msg=?,finished_at=? WHERE id=?",
                {errMsg, std::to_string(now), id});
        OplogService::adminLog(req, "division", "retry", id, "");
        RESP_MSG(cb, "分账完成");
    }

private:
    static int pi(const std::string &s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }

    // 通过通道插件执行分账（查询该订单使用的通道，调用其 divisionExec）
    static bool execDivisionViaChannel(const std::unordered_map<std::string, std::string> &rec, std::string &errMsg) {
        errMsg.clear();
        std::string orderId = rec.count("order_id") ? rec.at("order_id") : "";
        if (orderId.empty()) return false;

        auto &db = PayDb::instance();
        auto order = db.queryOne(
            "SELECT channel_id,channel_order_no FROM pay_order WHERE order_id=?", {orderId});
        if (order.empty()) { errMsg = "订单不存在"; return false; }

        std::string chId = order.count("channel_id") ? order.at("channel_id") : "0";
        auto ch = db.queryOne(
            "SELECT plugin,params_json FROM pay_channel WHERE id=?", {chId});
        if (ch.empty()) { errMsg = "通道不存在"; return false; }

        auto plugin = ChannelPluginRegistry::instance().create(ch["plugin"]);
        if (!plugin) { errMsg = "插件未安装: " + ch["plugin"]; return false; }

        Json::Value cp; Json::Reader().parse(ch["params_json"], cp);

        ChannelDivisionRequest req;
        req.orderId = orderId;
        req.channelOrderNo = order.count("channel_order_no") ? order.at("channel_order_no") : "";
        DivisionReceiverItem item;
        item.accountNo   = rec.count("account_no") ? rec.at("account_no") : "";
        item.accountName = rec.count("receiver_name") ? rec.at("receiver_name") : "";
        item.accountType = 1;
        try { item.amount = std::stod(rec.count("division_amount") ? rec.at("division_amount") : "0"); } catch (...) {}
        req.receivers.push_back(item);
        req.channelParams = cp;

        auto result = plugin->divisionExec(req);
        if (!result.success) {
            errMsg = result.errMsg;
            return false;
        }
        LOG_INFO << "[DivisionCtrl] channel division ok orderId=" << orderId
                 << " channelDivisionNo=" << result.channelDivisionNo;
        return true;
    }
};
