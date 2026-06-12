// WePay-Cpp — 内置测试回调接收端
// 当订单创建时未提供 notify_url，且商户也无默认地址时，
// /gateway/create 会兜底使用 {host}/api/test/notify 作为回调地址。
// 这里负责接收回调并记录到 pay_callback_log + setting，方便管理员查看。
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include <json/json.h>
#include "../common/PayDb.h"

class TestNotifyCtrl : public drogon::HttpController<TestNotifyCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(TestNotifyCtrl::notify, "/api/test/notify",         drogon::Get, drogon::Post);
        ADD_METHOD_TO(TestNotifyCtrl::ret,    "/api/test/return",         drogon::Get, drogon::Post);
        ADD_METHOD_TO(TestNotifyCtrl::recent, "/admin/api/test/callback", drogon::Get);
    METHOD_LIST_END

    // 商户回调接收 → 返回 success（满足异步通知协议）
    void notify(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        recordCallback(req, "notify");
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setBody("success");
        cb(r);
    }

    // 同步跳转接收 → 返回简单 HTML
    void ret(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        recordCallback(req, "return");
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setContentTypeCode(drogon::CT_TEXT_HTML);
        r->setBody("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>支付完成</title></head>"
                   "<body style=\"font-family:sans-serif;text-align:center;padding:40px;\">"
                   "<h2 style=\"color:#52c41a;\">✅ 支付测试完成</h2>"
                   "<p>这是平台内置的测试同步跳转页。</p>"
                   "<p style=\"color:#999;font-size:13px;\">"
                   "若需自动跳转回商户站点，请在创建订单时填写 return_url。</p>"
                   "</body></html>");
        cb(r);
    }

    // 管理端查看最近 50 条测试回调（可选 ?plugin=xxx 按插件过滤）
    void recent(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string plugin = req->getParameter("plugin");
        std::string sql = "SELECT order_id,http_status,response,success,plugin,created_at "
                          "FROM pay_callback_log";
        std::vector<std::string> params;
        if (!plugin.empty()) { sql += " WHERE plugin=?"; params.push_back(plugin); }
        sql += " ORDER BY id DESC LIMIT 50";
        auto rows = PayDb::instance().query(sql, params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value v;
            v["order_id"]   = r["order_id"];
            v["response"]   = r["response"];
            v["success"]    = r["success"];
            v["plugin"]     = r.count("plugin") ? r.at("plugin") : "";
            v["created_at"] = r["created_at"];
            arr.append(v);
        }
        Json::Value data;
        data["code"] = 1;
        data["data"] = arr;
        cb(drogon::HttpResponse::newHttpJsonResponse(data));
    }

private:
    static void recordCallback(const drogon::HttpRequestPtr &req,
                               const std::string &kind) {
        auto &db = PayDb::instance();
        // 收集所有参数
        Json::Value params;
        for (auto &kv : req->getParameters()) params[kv.first] = kv.second;
        Json::FastWriter w;
        std::string body = w.write(params);
        std::string orderId = req->getParameter("payId");
        if (orderId.empty()) orderId = req->getParameter("trade_no");
        // 从订单表查出所属插件
        std::string plugin;
        if (!orderId.empty()) {
            auto o = db.queryOne(
                "SELECT pc.plugin FROM pay_order po "
                "LEFT JOIN pay_channel pc ON pc.id=po.channel_id "
                "WHERE po.order_id=? OR po.mch_order_no=?", {orderId, orderId});
            if (!o.empty()) plugin = o["plugin"];
        }
        long long now = std::time(nullptr);
        db.exec(
            "INSERT INTO pay_callback_log(order_id,notify_url,http_status,response,success,plugin,created_at)"
            " VALUES(?,?,?,?,?,?,?)",
            {orderId, "/api/test/" + kind, "200",
             body.substr(0, 1000), "1", plugin, std::to_string(now)});
        LOG_INFO << "[TestNotify] kind=" << kind << " orderId=" << orderId
                 << " params=" << body.substr(0, 500);
        db.setSetting("last_test_callback", body.substr(0, 2000));
        db.setSetting("last_test_callback_at", std::to_string(now));
    }
};
