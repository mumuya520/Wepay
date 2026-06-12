// WePay-Cpp — 管理后台: 订单管理控制器
// 职责：订单查询、详情、退款、同步等订单管理功能
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式化库
#include <unordered_set> // 无序集合库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/HttpCaller.h" // HTTP 调用工具
#include "../common/Md5Utils.h" // MD5 工具
#include "../common/EpaySign.h" // 签名工具
#include "../common/NotifyTaskService.h" // 通知任务服务
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class OrderMgrCtrl : public drogon::HttpController<OrderMgrCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(OrderMgrCtrl::list,        "/admin/api/order/list",            drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::detail,      "/admin/api/order/detail/{id}",     drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::closeOrder,  "/admin/api/order/close/{id}",      drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::supplement,  "/admin/api/order/supplement/{id}", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::reissue,     "/admin/api/order/reissue/{id}",    drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::deleteOrder, "/admin/api/order/{id}",            drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::deleteBatch, "/admin/api/order/batch",           drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::callback,    "/admin/api/order/callback",        drogon::Post,   "AdminAuthFilter");
        // 旧路由兼容
        ADD_METHOD_TO(OrderMgrCtrl::list,        "/api/order/list",            drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(OrderMgrCtrl::detail,      "/api/order/detail/{id}",     drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int page  = std::max(1, parseIntParam(req, "page",  1));
        int limit = std::max(1, parseIntParam(req, "limit", 10));
        std::string state   = req->getParameter("state");
        std::string type    = req->getParameter("type");
        std::string keyword = req->getParameter("keyword");

        std::string where = " WHERE 1=1";
        std::vector<std::string> params;
        if (!state.empty())   { where += " AND state=?";     params.push_back(state); }
        if (!type.empty())    { where += " AND pay_type=?";   params.push_back(type); }
        if (!keyword.empty()) { where += " AND (order_id LIKE ? OR mch_order_no LIKE ? OR subject LIKE ?)";
            std::string kw = "%" + keyword + "%";
            params.push_back(kw); params.push_back(kw); params.push_back(kw); }

        auto &db = PayDb::instance();
        auto cntRow = db.queryOne("SELECT COUNT(*) AS c FROM pay_order" + where, params);
        int total = cntRow.empty() ? 0 : std::stoi(cntRow["c"]);

        int offset = (page - 1) * limit;
        auto pp = params;
        pp.push_back(std::to_string(limit));
        pp.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT * FROM pay_order" + where + " ORDER BY id DESC LIMIT ? OFFSET ?", pp);

        Json::Value items(Json::arrayValue);
        for (auto &row : rows) items.append(rowToJson(row));
        Json::Value data; data["total"] = total; data["items"] = items;
        RESP_OK(cb, data);
    }

    void detail(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                std::string id) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM pay_order WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "订单不存在"); return; }
        RESP_OK(cb, rowToJson(row));
    }

    void closeOrder(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    std::string id) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM pay_order WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "订单不存在"); return; }
        if (row["state"] != "0") { RESP_ERR(cb, "只能关闭未支付的订单"); return; }
        long long now = std::time(nullptr);
        db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE id=?",
                {std::to_string(now), id});
        db.exec("DELETE FROM tmp_price WHERE oid=?", {row["order_id"]});
        RESP_MSG(cb, "关闭订单成功");
    }

    // 手动补单：把订单标为已支付并触发回调
    void supplement(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    std::string id) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM pay_order WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "订单不存在"); return; }
        if (row["state"] == "1") { RESP_ERR(cb, "该订单已支付"); return; }
        long long now = std::time(nullptr);
        db.exec("UPDATE pay_order SET state=1,pay_time=?,updated_at=? WHERE id=?",
                {std::to_string(now), std::to_string(now), id});
        db.exec("DELETE FROM tmp_price WHERE oid=?", {row["order_id"]});

        // 触发回调
        std::string notifyUrl = row.count("notify_url") ? row["notify_url"] : "";
        if (!notifyUrl.empty()) {
            std::string key   = db.getSetting("key", "");
            std::string payId = row.count("mch_order_no") ? row.at("mch_order_no") : "";
            std::string param = row.count("param") ? row.at("param") : "";
            std::string type  = row.count("pay_type") ? row.at("pay_type") : "";
            std::string oPrice = row.count("amount") ? row.at("amount") : "0";
            std::string realPrice = row.count("real_amount") ? row.at("real_amount") : "0";
            std::string sign = Md5Utils::notifySign(payId, param, type, oPrice, realPrice, key);
            std::string sep = (notifyUrl.find('?') == std::string::npos) ? "?" : "&";
            std::string fullUrl = notifyUrl + sep
                + "payId=" + payId + "&param=" + param
                + "&type=" + type + "&price=" + oPrice
                + "&reallyPrice=" + realPrice + "&sign=" + sign;
            NotifyTaskService::createTaskAndSend(row["order_id"], fullUrl, "OrderMgrCtrl");
        }
        RESP_MSG(cb, "补单成功");
    }

    // 重发回调
    void reissue(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                 std::string id) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM pay_order WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "订单不存在"); return; }
        std::string notifyUrl = row.count("notify_url") ? row["notify_url"] : "";
        if (notifyUrl.empty()) { RESP_ERR(cb, "该订单未配置 notify_url"); return; }
        std::string orderId = row["order_id"];

        auto taskRow = db.querySqliteDirect(
            "SELECT id FROM pay_notify_task WHERE order_id=?", {orderId});
        if (!taskRow.empty()) {
            NotifyTaskService::retryNow(orderId);
        } else {
            std::string key = db.getSetting("key", "");
            std::string payId = row.count("mch_order_no") ? row.at("mch_order_no") : "";
            std::string param = row.count("param") ? row.at("param") : "";
            std::string type  = row.count("pay_type") ? row.at("pay_type") : "";
            std::string oPrice = row.count("amount") ? row.at("amount") : "0";
            std::string realPrice = row.count("real_amount") ? row.at("real_amount") : "0";
            std::string sign = Md5Utils::notifySign(payId, param, type, oPrice, realPrice, key);
            std::string sep = (notifyUrl.find('?') == std::string::npos) ? "?" : "&";
            std::string fullUrl = notifyUrl + sep
                + "payId=" + payId + "&param=" + param
                + "&type=" + type + "&price=" + oPrice
                + "&reallyPrice=" + realPrice + "&sign=" + sign;
            NotifyTaskService::createTaskAndSend(orderId, fullUrl, "OrderMgrCtrl");
        }
        RESP_MSG(cb, "回调已重新发送");
    }

    void deleteOrder(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                     std::string id) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT order_id FROM pay_order WHERE id=?", {id});
        if (!row.empty()) db.exec("DELETE FROM tmp_price WHERE oid=?", {row["order_id"]});
        db.exec("DELETE FROM pay_order WHERE id=?", {id});
        RESP_MSG(cb, "删除成功");
    }

    void deleteBatch(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body || !(*body).isMember("ids")) { RESP_ERR(cb, "缺少 ids"); return; }
        auto &db = PayDb::instance();
        int n = 0;
        for (auto &v : (*body)["ids"]) {
            std::string id = v.isString() ? v.asString() : std::to_string(v.asInt64());
            auto row = db.queryOne("SELECT order_id FROM pay_order WHERE id=?", {id});
            if (!row.empty()) db.exec("DELETE FROM tmp_price WHERE oid=?", {row["order_id"]});
            db.exec("DELETE FROM pay_order WHERE id=?", {id});
            n++;
        }
        Json::Value d; d["deleted"] = n;
        RESP_OK(cb, d);
    }

    // 手动回调：标为已支付并触发商户通知，支持所有插件
    // 优先检查 v3_order 走 V3 插件逻辑，否则走标准易支付逻辑
    void callback(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string orderId = (*body).get("order_id", "").asString();
        if (orderId.empty()) { RESP_ERR(cb, "order_id 不能为空"); return; }

        auto &db = PayDb::instance();
        auto payRow = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (payRow.empty()) { RESP_ERR(cb, "订单不存在"); return; }

        std::string state = payRow.count("state") ? payRow.at("state") : "0";
        long long now = std::time(nullptr);

        // 标为已支付（仅未支付时才更新状态）
        if (state != "1") {
            db.exec("UPDATE pay_order SET state=1,pay_time=?,updated_at=? WHERE order_id=?",
                    {std::to_string(now), std::to_string(now), orderId});
            db.exec("DELETE FROM tmp_price WHERE oid=?", {orderId});
        }

        // 检测插件类型，优先查 v3_order
        auto v3Row = db.queryOne("SELECT merchant_id FROM v3_order WHERE order_id=?", {orderId});
        std::string notifyUrl = payRow.count("notify_url") ? payRow.at("notify_url") : "";

        if (!notifyUrl.empty()) {
            if (!v3Row.empty()) {
                // V3 插件：查询商户 HMAC 密钥
                std::string mchId  = v3Row.count("merchant_id") ? v3Row.at("merchant_id") : "";
                auto mchRow = db.queryOne(
                    "SELECT app_id FROM v3_merchant WHERE merchant_id=? AND disabled=0 LIMIT 1",
                    {mchId});
                if (!mchRow.empty()) {
                    std::string appId = mchRow.count("app_id") ? mchRow.at("app_id") : "";
                    auto cfgRow = db.queryOne(
                        "SELECT config_value FROM v3_system_config WHERE config_key='hmac_key' LIMIT 1");
                    std::string key = cfgRow.empty() ? "" : cfgRow.at("config_value");
                    // V3 回调构建：payId=orderId&trade_status=SUCCESS&amount=xxx&sign=hmac_sha256
                    std::ostringstream ss;
                    ss << "payId=" << Md5Utils::urlEncode(orderId)
                       << "&trade_status=SUCCESS"
                       << "&amount=" << (payRow.count("amount") ? payRow.at("amount") : "0")
                       << "&real_amount=" << (payRow.count("real_amount") ? payRow.at("real_amount") : "0")
                       << "&time=" << now;
                    std::string msg = ss.str();
                    std::string sig = Md5Utils::hmacSha256(key, msg);
                    std::string sep = notifyUrl.find('?') == std::string::npos ? "?" : "&";
                    std::string fullUrl = notifyUrl + sep + msg + "&sign=" + Md5Utils::urlEncode(sig);
                    NotifyTaskService::createTaskAndSend(orderId, fullUrl, "WepayV3Plugin");
                    RESP_MSG(cb, "V3 手动回调已触发");
                    return;
                }
            }
            // 标准易支付插件：payId + param + type + price + reallyPrice + key
            std::string key      = db.getSetting("key", "");
            std::string payId    = payRow.count("mch_order_no") ? payRow.at("mch_order_no") : orderId;
            std::string param    = payRow.count("param")        ? payRow.at("param")        : "";
            std::string type     = payRow.count("pay_type")     ? payRow.at("pay_type")     : "";
            std::string oPrice  = payRow.count("amount")       ? payRow.at("amount")       : "0";
            std::string realPrice= payRow.count("real_amount")  ? payRow.at("real_amount")  : "0";
            std::string sign     = Md5Utils::notifySign(payId, param, type, oPrice, realPrice, key);
            std::string sep = notifyUrl.find('?') == std::string::npos ? "?" : "&";
            std::string fullUrl = notifyUrl + sep
                + "payId=" + Md5Utils::urlEncode(payId) + "&param=" + Md5Utils::urlEncode(param)
                + "&type=" + Md5Utils::urlEncode(type)  + "&price=" + Md5Utils::urlEncode(oPrice)
                + "&reallyPrice=" + Md5Utils::urlEncode(realPrice) + "&sign=" + Md5Utils::urlEncode(sign);
            NotifyTaskService::createTaskAndSend(orderId, fullUrl, "OrderMgrCtrl");
        }

        if (notifyUrl.empty()) {
            RESP_MSG(cb, "订单状态已更新为已支付（无 notify_url，未发通知）");
        } else {
            RESP_MSG(cb, "手动回调已触发");
        }
    }

private:
    static int parseIntParam(const drogon::HttpRequestPtr &req,
                             const std::string &k, int def) {
        auto s = req->getParameter(k);
        if (s.empty()) return def;
        try { return std::stoi(s); } catch (...) { return def; }
    }
    static Json::Value rowToJson(const std::unordered_map<std::string, std::string> &row) {
        Json::Value j;
        for (auto &[k, v] : row) {
            // 数字字段转 number
            if (k == "id" || k == "state" || k == "notify_state" || k == "mch_id" || k == "channel_id") {
                try { j[k] = std::stoi(v); } catch (...) { j[k] = v; }
            } else if (k == "created_at" || k == "updated_at" || k == "pay_time" || k == "expire_time") {
                try { j[k] = (Json::Int64)std::stoll(v); } catch (...) { j[k] = v; }
            } else if (k == "amount" || k == "real_amount" || k == "mch_fee_amount" || k == "channel_fee_amount" || k == "refund_amount") {
                try { j[k] = std::stod(v); } catch (...) { j[k] = v; }
            } else j[k] = v;
        }
        return j;
    }
};
