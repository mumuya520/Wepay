// WePay-Cpp — 管理后台: 转账管理控制器
// 职责：转账单的查询、创建、确认、取消等转账管理功能
//
// API 端点：
// GET  /admin/api/transfer/list      转账单列表
// POST /admin/api/transfer/create    发起转账(管理员代商户发起)
// POST /admin/api/transfer/confirm   手动确认完成
// POST /admin/api/transfer/cancel    取消转账
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <random> // 随机数库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../channel/ChannelPlugin.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class TransferCtrl : public drogon::HttpController<TransferCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(TransferCtrl::list,    "/admin/api/transfer/list",    drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(TransferCtrl::create,  "/admin/api/transfer/create",  drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(TransferCtrl::confirm, "/admin/api/transfer/confirm", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(TransferCtrl::cancel,  "/admin/api/transfer/cancel",  drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "transfer:view");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        std::string mchId = req->getParameter("mch_id");
        std::string state = req->getParameter("state");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        if (!state.empty()) { where += " AND state=?"; params.push_back(state); }

        auto cntR = db.query("SELECT COUNT(*) AS c FROM transfer_order WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string((page - 1) * size));
        auto rows = db.query(
            "SELECT * FROM transfer_order WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", pp);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["account_type"] = std::stoi(r["account_type"]);
            it["state"] = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void create(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "transfer:create");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;

        int mchId = j.get("mch_id", 0).asInt();
        double amount = 0;
        try { amount = std::stod(j.get("amount", "0").asString()); } catch (...) {}
        if (mchId <= 0 || amount <= 0) { RESP_ERR(cb, "商户ID和金额必填"); return; }

        auto &db = PayDb::instance();
        auto mch = db.queryOne("SELECT balance FROM merchant WHERE id=?", {std::to_string(mchId)});
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; }

        double balance = 0;
        try { balance = std::stod(mch["balance"]); } catch (...) {}
        if (balance < amount) { RESP_ERR(cb, "商户余额不足"); return; }

        // 扣款(冻结转账金额)
        if (!ChannelService::changeMchBalance(mchId, 2, amount, "transfer",
            "", "管理员发起转账")) {
            RESP_ERR(cb, "扣款失败"); return;
        }

        std::string transferNo = generateTransferNo();
        double fee = 0;  // 费率可配置
        double realAmount = amount - fee;
        long long now = std::time(nullptr);

        db.exec("INSERT INTO transfer_order(transfer_no,mch_id,mch_transfer_no,amount,fee,"
                "real_amount,account_type,account_no,account_name,remark,state,created_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,0,?)",
                {transferNo, std::to_string(mchId),
                 j.get("mch_transfer_no", "").asString(),
                 ChannelService::fmtAmount(amount),
                 ChannelService::fmtAmount(fee),
                 ChannelService::fmtAmount(realAmount),
                 std::to_string(j.get("account_type", 1).asInt()),
                 j.get("account_no", "").asString(),
                 j.get("account_name", "").asString(),
                 j.get("remark", "").asString(),
                 std::to_string(now)});

        // 调用通道插件真正打款
        int transferState = 0;
        std::string channelTransferNo;
        std::string errMsg;

        int accountType = j.get("account_type", 1).asInt();
        auto transferChannel = findTransferChannel(db, mchId, accountType);
        if (!transferChannel.empty()) {
            auto plugin = ChannelPluginRegistry::instance().create(transferChannel["plugin"]);
            if (plugin) {
                Json::Value cp;
                Json::Reader().parse(transferChannel["params_json"], cp);

                ChannelTransferRequest tr;
                tr.transferNo   = transferNo;
                tr.amount       = realAmount;
                tr.accountType  = accountType;
                tr.accountNo    = j.get("account_no", "").asString();
                tr.accountName  = j.get("account_name", "").asString();
                tr.remark       = j.get("remark", "管理员发起转账").asString();
                tr.notifyUrl    = "/notify/transfer/" + transferChannel["plugin"];
                tr.channelParams = cp;

                auto result = plugin->transfer(tr);
                if (result.success) {
                    transferState = result.state;
                    channelTransferNo = result.channelTransferNo;
                    long long now2 = std::time(nullptr);
                    db.exec("UPDATE transfer_order SET state=?,channel_transfer_no=?,"
                            "channel_id=?,finished_at=? WHERE transfer_no=?",
                            {std::to_string(transferState),
                             channelTransferNo,
                             transferChannel["id"],
                             std::to_string(transferState == 1 ? now2 : 0),
                             transferNo});
                } else {
                    errMsg = result.errMsg;
                    ChannelService::changeMchBalance(mchId, 1, amount,
                        "transfer_fail", transferNo, "转账失败退回");
                    db.exec("UPDATE transfer_order SET state=-1,err_msg=? WHERE transfer_no=?",
                            {errMsg, transferNo});
                    transferState = -1;
                }
            }
        }

        OplogService::adminLog(req, "transfer", "create", transferNo,
            "金额:" + ChannelService::fmtAmount(amount));

        Json::Value data;
        data["transfer_no"]         = transferNo;
        data["amount"]              = ChannelService::fmtAmount(amount);
        data["real_amount"]         = ChannelService::fmtAmount(realAmount);
        data["state"]               = transferState;
        data["channel_transfer_no"] = channelTransferNo;
        if (!errMsg.empty()) data["err_msg"] = errMsg;
        RESP_OK(cb, data);
    }

    void confirm(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "transfer:create");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string channelNo = (*body).get("channel_transfer_no", "").asString();

        auto &db = PayDb::instance();
        auto t = db.queryOne("SELECT state FROM transfer_order WHERE id=?", {id});
        if (t.empty()) { RESP_ERR(cb, "转账单不存在"); return; }
        if (t["state"] != "0") { RESP_ERR(cb, "当前状态不允许确认"); return; }

        db.exec("UPDATE transfer_order SET state=1,channel_transfer_no=?,finished_at=? WHERE id=?",
                {channelNo, std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "transfer", "confirm", id, channelNo);
        RESP_MSG(cb, "已确认完成");
    }

    void cancel(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "transfer:create");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string reason = (*body).get("reason", "").asString();

        auto &db = PayDb::instance();
        auto t = db.queryOne("SELECT mch_id,amount,state FROM transfer_order WHERE id=?", {id});
        if (t.empty()) { RESP_ERR(cb, "转账单不存在"); return; }
        if (t["state"] != "0") { RESP_ERR(cb, "只能取消处理中的转账"); return; }

        int mchId = 0; double amt = 0;
        try { mchId = std::stoi(t["mch_id"]); } catch (...) {}
        try { amt = std::stod(t["amount"]); } catch (...) {}

        // 退回商户余额
        if (mchId > 0 && amt > 0)
            ChannelService::changeMchBalance(mchId, 1, amt, "transfer_cancel",
                "", "转账取消退款");

        db.exec("UPDATE transfer_order SET state=-1,err_msg=?,finished_at=? WHERE id=?",
                {reason, std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "transfer", "cancel", id, reason);
        RESP_MSG(cb, "已取消");
    }

private:
    // 查找可用的转账通道
    static PayDb::Row findTransferChannel(PayDb &db, int mchId, int accountType) {
        std::string payType;
        if (accountType == 1)      payType = "wxpay_transfer";
        else if (accountType == 2) payType = "alipay_transfer";
        else                       payType = "bank_transfer";

        auto rows = db.query(
            "SELECT c.id,c.plugin,c.params_json FROM pay_channel c "
            "JOIN merchant_channel mc ON mc.channel_id=c.id "
            "WHERE mc.mch_id=? AND c.pay_type=? AND c.state=1 AND mc.state=1 "
            "ORDER BY c.sort_order ASC LIMIT 1",
            {std::to_string(mchId), payType});
        if (!rows.empty()) return rows[0];

        rows = db.query(
            "SELECT id,plugin,params_json FROM pay_channel "
            "WHERE pay_type=? AND state=1 ORDER BY sort_order ASC LIMIT 1",
            {payType});
        return rows.empty() ? PayDb::Row{} : rows[0];
    }

    static int pi(const std::string &s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }
    static std::string generateTransferNo() {
        static std::mt19937 rng((unsigned)std::random_device{}());
        long long ts = std::time(nullptr);
        int rnd = rng() % 10000;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "T%lld%04d", ts, rnd);
        return buf;
    }
};
