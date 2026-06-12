// WePay-Cpp — 管理后台: 源兼容性控制器
// 职责：与旧版本 WePay 的兼容性接口，支持旧版本前端调用
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <random> // 随机数库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/PermCheck.h" // 权限检查
#include "../common/OplogService.h" // 操作日志服务
#include "../common/ChannelService.h" // 通道服务
#include "../common/NotifyTaskService.h" // 通知任务服务
#include "../common/PasswordUtils.h"
#include "../filters/AdminAuthFilter.h"

class AdminSourceCompatCtrl : public drogon::HttpController<AdminSourceCompatCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminSourceCompatCtrl::dashboard, "/admin/api/source/dashboard", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::domainList, "/admin/api/source/domain/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::domainReview, "/admin/api/source/domain/review", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::ticketList, "/admin/api/source/ticket/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::ticketReply, "/admin/api/source/ticket/reply", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::cdkList, "/admin/api/source/cdk/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::cdkCreate, "/admin/api/source/cdk/create", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::vipList, "/admin/api/source/vip/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::vipSave, "/admin/api/source/vip/save", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::rechargeList, "/admin/api/source/recharge/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::rechargeConfirm, "/admin/api/source/recharge/confirm", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::moneyAdjust, "/admin/api/source/money/adjust", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::clearUnpaidOrders, "/admin/api/source/clear/unpaidOrders", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::clearUnpaidRecharges, "/admin/api/source/clear/unpaidRecharges", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::orderMakeUp, "/admin/api/source/order/makeup", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::notifyRetry, "/admin/api/source/notify/retry", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::ticketCategoryList, "/admin/api/source/ticketCategory/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::ticketCategorySave, "/admin/api/source/ticketCategory/save", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::registerOrderList, "/admin/api/source/register/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::registerOrderConfirm, "/admin/api/source/register/confirm", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::channelAlertList, "/admin/api/source/channelAlert/list", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::channelAlertCreate, "/admin/api/source/channelAlert/create", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::noticeSettings, "/admin/api/source/notice/settings", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(AdminSourceCompatCtrl::noticeSettingsSave, "/admin/api/source/notice/settings/save", drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    void dashboard(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        Json::Value data;
        data["merchant_count"] = scalar(db, "SELECT COUNT(*) AS c FROM merchant");
        data["order_count"] = scalar(db, "SELECT COUNT(*) AS c FROM pay_order");
        data["paid_order_count"] = scalar(db, "SELECT COUNT(*) AS c FROM pay_order WHERE state=1");
        data["domain_pending"] = scalar(db, "SELECT COUNT(*) AS c FROM merchant_domain WHERE state=0");
        data["ticket_pending"] = scalar(db, "SELECT COUNT(*) AS c FROM support_ticket WHERE state=0");
        data["recharge_paid"] = scalar(db, "SELECT COUNT(*) AS c FROM recharge_order WHERE state=1");
        RESP_OK(cb, data);
    }

    void domainList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT d.*,m.mch_no,m.mch_name FROM merchant_domain d LEFT JOIN merchant m ON m.id=d.mch_id ORDER BY d.id DESC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void domainReview(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string id = (*j).get("id", "").asString();
        std::string state = std::to_string((*j).get("state", 1).asInt());
        std::string remark = (*j).get("remark", "").asString();
        PayDb::instance().exec("UPDATE merchant_domain SET state=?,remark=?,updated_at=? WHERE id=?", {state, remark, std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "source", "domainReview", id, state);
        RESP_MSG(cb, "已审核");
    }

    void ticketList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT t.*,m.mch_no,m.mch_name FROM support_ticket t LEFT JOIN merchant m ON m.id=t.mch_id ORDER BY t.id DESC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void ticketReply(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string id = (*j).get("id", "").asString();
        std::string reply = (*j).get("reply_content", "").asString();
        PayDb::instance().exec("UPDATE support_ticket SET reply_content=?,state=2,reply_at=? WHERE id=?", {reply, std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "source", "ticketReply", id, "");
        RESP_MSG(cb, "已回复");
    }

    void cdkList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT * FROM cdk_code ORDER BY id DESC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void cdkCreate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        int num = (*j).get("num", 1).asInt();
        if (num < 1) num = 1; if (num > 500) num = 500;
        int type = (*j).get("cdk_type", 1).asInt();
        std::string value = (*j).get("value", "0").asString();
        std::string prefix = (*j).get("prefix", "CDK").asString();
        Json::Value arr(Json::arrayValue);
        long long now = std::time(nullptr);
        for (int i = 0; i < num; ++i) {
            std::string code = prefix + "_" + randCode();
            PayDb::instance().exec("INSERT INTO cdk_code(code,cdk_type,value,state,created_at) VALUES(?,?,?,?,?)", {code, std::to_string(type), value, "0", std::to_string(now)});
            arr.append(code);
        }
        OplogService::adminLog(req, "source", "cdkCreate", "", std::to_string(num));
        RESP_OK(cb, arr);
    }

    void vipList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT * FROM vip_package ORDER BY id DESC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void vipSave(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string id = (*j).get("id", "").asString();
        if (id.empty() || id == "0") {
            PayDb::instance().exec("INSERT INTO vip_package(name,price,days,rate,state,created_at) VALUES(?,?,?,?,?,?)",
                {(*j).get("name", "VIP套餐").asString(), (*j).get("price", "0.00").asString(), std::to_string((*j).get("days", 30).asInt()), (*j).get("rate", "0.00").asString(), std::to_string((*j).get("state", 1).asInt()), std::to_string(std::time(nullptr))});
        } else {
            PayDb::instance().exec("UPDATE vip_package SET name=?,price=?,days=?,rate=?,state=? WHERE id=?",
                {(*j).get("name", "VIP套餐").asString(), (*j).get("price", "0.00").asString(), std::to_string((*j).get("days", 30).asInt()), (*j).get("rate", "0.00").asString(), std::to_string((*j).get("state", 1).asInt()), id});
        }
        OplogService::adminLog(req, "source", "vipSave", id, "");
        RESP_MSG(cb, "已保存");
    }

    void rechargeList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT r.*,m.mch_no,m.mch_name FROM recharge_order r LEFT JOIN merchant m ON m.id=r.mch_id ORDER BY r.id DESC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void rechargeConfirm(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string orderNo = (*j).get("order_no", "").asString();
        auto &db = PayDb::instance();
        auto r = db.queryOne("SELECT * FROM recharge_order WHERE order_no=?", {orderNo});
        if (r.empty()) { RESP_ERR(cb, "充值订单不存在"); return; }
        if (r["state"] == "1") { RESP_ERR(cb, "充值订单已完成"); return; }
        double amount = toDouble(r["amount"]);
        int mchId = toInt(r["mch_id"], 0);
        if (mchId <= 0 || amount <= 0) { RESP_ERR(cb, "充值订单数据异常"); return; }
        ChannelService::changeMchBalance(mchId, 1, amount, "recharge", orderNo, "后台确认充值");
        db.exec("UPDATE recharge_order SET state=1,paid_at=? WHERE order_no=?", {std::to_string(std::time(nullptr)), orderNo});
        OplogService::adminLog(req, "source", "rechargeConfirm", orderNo, r["amount"]);
        RESP_MSG(cb, "充值已确认");
    }

    void moneyAdjust(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        int mchId = (*j).get("mch_id", 0).asInt();
        double amount = toDouble((*j).get("amount", "0").asString());
        std::string remark = (*j).get("remark", "后台加扣款").asString();
        if (mchId <= 0 || amount == 0) { RESP_ERR(cb, "商户ID和金额必填"); return; }
        bool ok = ChannelService::changeMchBalance(mchId, amount > 0 ? 1 : 2, std::abs(amount), "admin_adjust", ChannelService::generateOrderId("ADJ"), remark);
        if (!ok) { RESP_ERR(cb, "余额不足或商户不存在"); return; }
        OplogService::adminLog(req, "source", "moneyAdjust", std::to_string(mchId), ChannelService::fmtAmount(amount));
        RESP_MSG(cb, "余额已调整");
    }

    void clearUnpaidOrders(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int days = toInt(req->getParameter("days"), 0);
        long long before = days > 0 ? std::time(nullptr) - (long long)days * 86400 : std::time(nullptr);
        PayDb::instance().exec("DELETE FROM pay_order WHERE state=0 AND created_at<?", {std::to_string(before)});
        OplogService::adminLog(req, "source", "clearUnpaidOrders", "", std::to_string(days));
        RESP_MSG(cb, "未支付订单已清理");
    }

    void clearUnpaidRecharges(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int days = toInt(req->getParameter("days"), 0);
        long long before = days > 0 ? std::time(nullptr) - (long long)days * 86400 : std::time(nullptr);
        PayDb::instance().exec("DELETE FROM recharge_order WHERE state=0 AND created_at<?", {std::to_string(before)});
        OplogService::adminLog(req, "source", "clearUnpaidRecharges", "", std::to_string(days));
        RESP_MSG(cb, "未支付充值订单已清理");
    }

    void orderMakeUp(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string orderId = (*j).get("order_id", "").asString();
        auto &db = PayDb::instance();
        auto order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (order.empty()) { RESP_ERR(cb, "订单不存在"); return; }
        if (order["state"] == "1") { RESP_ERR(cb, "订单已支付"); return; }
        long long now = std::time(nullptr);
        db.exec("UPDATE pay_order SET state=1,pay_time=?,updated_at=? WHERE order_id=?", {std::to_string(now), std::to_string(now), orderId});
        int mchId = toInt(order.count("mch_id") ? order["mch_id"] : "0", 0);
        double amount = toDouble(order.count("amount") ? order["amount"] : "0");
        double fee = toDouble(order.count("mch_fee_amount") ? order["mch_fee_amount"] : "0");
        if (mchId > 0 && amount > fee) ChannelService::changeMchBalance(mchId, 1, amount - fee, "order_makeup", orderId, "后台补单");
        OplogService::adminLog(req, "source", "orderMakeUp", orderId, "");
        RESP_MSG(cb, "补单成功");
    }

    void notifyRetry(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string orderId = (*j).get("order_id", "").asString();
        if (orderId.empty()) { RESP_ERR(cb, "订单号不能为空"); return; }
        std::string notifyUrl = (*j).get("notify_url", "").asString();
        if (!notifyUrl.empty()) NotifyTaskService::updateUrl(orderId, notifyUrl);
        NotifyTaskService::retryNow(orderId);
        OplogService::adminLog(req, "source", "notifyRetry", orderId, "");
        RESP_MSG(cb, "已发起通知重推");
    }

    void ticketCategoryList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT * FROM ticket_category ORDER BY sort_order ASC,id ASC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void ticketCategorySave(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string id = (*j).get("id", "").asString();
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO ticket_category(name,sort_order,state,created_at) VALUES(?,?,?,?)",
                {(*j).get("name", "").asString(), std::to_string((*j).get("sort_order", 0).asInt()), std::to_string((*j).get("state", 1).asInt()), std::to_string(std::time(nullptr))});
        } else {
            db.exec("UPDATE ticket_category SET name=?,sort_order=?,state=? WHERE id=?",
                {(*j).get("name", "").asString(), std::to_string((*j).get("sort_order", 0).asInt()), std::to_string((*j).get("state", 1).asInt()), id});
        }
        OplogService::adminLog(req, "source", "ticketCategorySave", id, "");
        RESP_MSG(cb, "已保存");
    }

    void registerOrderList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT * FROM register_order ORDER BY id DESC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void registerOrderConfirm(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string orderNo = (*j).get("order_no", "").asString();
        auto &db = PayDb::instance();
        auto ro = db.queryOne("SELECT * FROM register_order WHERE order_no=?", {orderNo});
        if (ro.empty()) { RESP_ERR(cb, "注册订单不存在"); return; }
        if (ro["state"] == "1") { RESP_ERR(cb, "注册订单已处理"); return; }
        auto exists = db.queryOne("SELECT id FROM merchant WHERE username=?", {ro["username"]});
        if (!exists.empty()) { RESP_ERR(cb, "用户名已存在"); return; }
        std::string salt = PasswordUtils::generateSalt();
        std::string pwd = ro["password"].empty() ? "123456" : ro["password"];
        std::string hash = PasswordUtils::hashPassword(pwd, salt);
        std::string key = PasswordUtils::generateKey(32);
        std::string mchNo = "M" + std::to_string(std::time(nullptr));
        long long now = std::time(nullptr);
        db.exec("INSERT INTO merchant(mch_no,username,password,salt,mch_name,contact,phone,email,mch_key,state,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            {mchNo, ro["username"], hash, salt, ro["mch_name"], ro["mch_name"], ro["phone"], ro["email"], key, "1", std::to_string(now), std::to_string(now)});
        auto mch = db.queryOne("SELECT id FROM merchant WHERE username=?", {ro["username"]});
        std::string mchId = mch.empty() ? "0" : mch["id"];
        db.exec("UPDATE register_order SET state=1,mch_id=?,paid_at=? WHERE order_no=?", {mchId, std::to_string(now), orderNo});
        OplogService::adminLog(req, "source", "registerConfirm", orderNo, mchId);
        RESP_MSG(cb, "注册已开通");
    }

    void channelAlertList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT a.*,m.mch_no,m.mch_name FROM channel_alert a LEFT JOIN merchant m ON m.id=a.mch_id ORDER BY a.id DESC", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void channelAlertCreate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        PayDb::instance().exec("INSERT INTO channel_alert(mch_id,channel_id,alert_type,title,content,state,created_at) VALUES(?,?,?,?,?,0,?)",
            {std::to_string((*j).get("mch_id", 0).asInt()), std::to_string((*j).get("channel_id", 0).asInt()), (*j).get("alert_type", "offline").asString(), (*j).get("title", "通道告警").asString(), (*j).get("content", "").asString(), std::to_string(std::time(nullptr))});
        OplogService::adminLog(req, "source", "channelAlertCreate", "", "");
        RESP_MSG(cb, "通道告警已记录");
    }

    void noticeSettings(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query("SELECT vkey,vvalue FROM setting WHERE vkey LIKE 'notify_%' OR vkey='paid_reg_price' ORDER BY vkey", {});
        RESP_OK(cb, rowsToJson(rows));
    }

    void noticeSettingsSave(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        auto &db = PayDb::instance();
        for (const auto &key : j->getMemberNames()) {
            if (key.rfind("notify_", 0) != 0 && key != "paid_reg_price") continue;
            db.setSetting(key, (*j)[key].asString());
        }
        OplogService::adminLog(req, "source", "noticeSettingsSave", "", "");
        RESP_MSG(cb, "配置已保存");
    }

private:
    static Json::Value rowsToJson(const std::vector<PayDb::Row> &rows) {
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) { Json::Value it; for (auto &[k, v] : r) it[k] = v; arr.append(it); }
        return arr;
    }
    static int scalar(PayDb &db, const std::string &sql) {
        auto r = db.queryOne(sql, {});
        if (r.empty()) return 0;
        try { return std::stoi(r.begin()->second); } catch (...) { return 0; }
    }
    static double toDouble(const std::string &s) { try { return std::stod(s); } catch (...) { return 0.0; } }
    static int toInt(const std::string &s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
    static std::string randCode() {
        static const char *cs = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        static std::mt19937 rng((unsigned)std::random_device{}());
        std::string s; for (int i = 0; i < 16; ++i) s.push_back(cs[rng() % 32]); return s;
    }
};
