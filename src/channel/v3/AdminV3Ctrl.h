#pragma once
#include <drogon/HttpController.h>
#include <json/json.h>
#include <ctime>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "common/PayDb.h"
#include "common/AjaxResult.h"
#include "channel/v3/EmailService.h"
#include "channel/v3/WepayV3Plugin.h"
#include "common/SmtpUtils.h"
#include "common/NotifyTaskService.h"

namespace wepay {
namespace v3 {

// V3 管理后台接口（JWT 保护，供前端调用）
class AdminV3Ctrl : public drogon::HttpController<AdminV3Ctrl> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminV3Ctrl::getStats,           "/admin/api/v3/stats",              drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getDevices,         "/admin/api/v3/devices",            drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::deleteDevice,       "/admin/api/v3/device/delete",      drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::getOrders,          "/admin/api/v3/orders",             drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getOrderDetail,     "/admin/api/v3/order/detail",       drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::manualCallback,     "/admin/api/v3/order/callback",     drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::getSystemConfig,    "/admin/api/v3/config",             drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::saveSystemConfig,   "/admin/api/v3/config",             drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::getIpWhitelist,     "/admin/api/v3/ip-whitelist",       drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::addIpWhitelist,     "/admin/api/v3/ip-whitelist/add",   drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::delIpWhitelist,     "/admin/api/v3/ip-whitelist/del",   drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::getAuditLog,          "/admin/api/v3/audit-log",            drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getCallbackLog,       "/admin/api/v3/callback-log",         drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getEmailLog,          "/admin/api/v3/email-log",            drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getMerchantConfigs,   "/admin/api/v3/merchant-configs",     drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::saveMerchantConfig,   "/admin/api/v3/merchant-config/save", drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::getOrderStatusLog,    "/admin/api/v3/order/status-log",     drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getOrderTrend,         "/admin/api/v3/order/trend",          drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getAlertConfig,        "/admin/api/v3/alert/config",         drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::saveAlertConfig,       "/admin/api/v3/alert/config",         drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::getEmailConfig,        "/admin/api/v3/email-config",              drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::saveEmailConfig,       "/admin/api/v3/email-config",              drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::testEmailConfig,       "/admin/api/v3/email-config/test",        drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::listEmailAccounts,     "/admin/api/v3/email-accounts",           drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::saveEmailAccount,      "/admin/api/v3/email-account/save",       drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::delEmailAccount,       "/admin/api/v3/email-account/del",        drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::testEmailAccount,      "/admin/api/v3/email-account/test",       drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::resendEmail,          "/admin/api/v3/email/resend",            drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::listDeviceQrcodes,    "/admin/api/v3/device-qrcodes",          drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::getQrSelectionLog,    "/admin/api/v3/qrcode-selection-log",     drogon::Get);
    ADD_METHOD_TO(AdminV3Ctrl::saveDeviceQrcode,     "/admin/api/v3/device-qrcode/save",      drogon::Post);
    ADD_METHOD_TO(AdminV3Ctrl::deleteDeviceQrcode,   "/admin/api/v3/device-qrcode/delete",    drogon::Post);
    METHOD_LIST_END

    // 实时统计
    void getStats(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        Json::Value data;
        try {
            auto totRow  = db.queryOne("SELECT COUNT(*) as c FROM v3_device", {});
            auto onlRow  = db.queryOne("SELECT COUNT(*) as c FROM v3_device WHERE online=1", {});
            long todayTs = static_cast<long>(std::time(nullptr) / 86400) * 86400;
            auto orders  = db.query(
                "SELECT status, COUNT(*) as cnt, COALESCE(SUM(amount),0) as amt "
                "FROM v3_order WHERE created_at>=? GROUP BY status",
                {std::to_string(todayTs)});

            data["deviceTotal"]  = totRow.empty()  ? 0 : std::stoi(totRow["c"]);
            data["deviceOnline"] = onlRow.empty()  ? 0 : std::stoi(onlRow["c"]);
            int orderToday=0, orderSuccess=0, orderFailed=0;
            double amountToday=0;
            for (auto& row : orders) {
                int cnt = std::stoi(row["cnt"]);
                orderToday += cnt;
                if (row["status"] == "PAID")    { orderSuccess += cnt; amountToday += std::stod(row["amt"]); }
                if (row["status"] == "FAILED" || row["status"] == "TIMEOUT") orderFailed += cnt;
            }
            data["orderToday"]           = orderToday;
            data["orderSuccess"]         = orderSuccess;
            data["orderFailed"]          = orderFailed;
            data["amountToday"]          = amountToday;
            // 本实例 WS 连接数（单实例准确；多实例需汇聚 Redis）
            try {
                auto wsRow = db.queryOne(
                    "SELECT COUNT(*) as c FROM v3_device WHERE online=1 AND "
                    "updated_at >= ?", {std::to_string(static_cast<long>(std::time(nullptr)) - 30)});
                data["websocketConnections"] = wsRow.empty() ? 0 : std::stoi(wsRow["c"]);
            } catch (...) { data["websocketConnections"] = 0; }
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] getStats: " << e.what();
        }
        RESP_OK(cb, data);
    }

    // 设备列表
    void getDevices(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        int page = 1, size = 20;
        try { page = std::stoi(req->getParameter("page")); }     catch (...) {}
        try { size = std::stoi(req->getParameter("page_size")); } catch (...) {}
        int offset = (page - 1) * size;
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query(
                "SELECT device_id,ip,last_heartbeat,online,battery,network,app_version,created_at "
                "FROM v3_device ORDER BY online DESC, last_heartbeat DESC LIMIT ? OFFSET ?",
                {std::to_string(size), std::to_string(offset)});
            for (auto& r : rows) {
                Json::Value it;
                it["deviceId"]      = r["device_id"];
                it["ip"]            = r["ip"];
                it["lastHeartbeat"] = r["last_heartbeat"].empty() ? 0 : (Json::Int64)std::stoll(r["last_heartbeat"]);
                it["online"]        = r["online"] == "1";
                it["battery"]       = r["battery"].empty() ? -1 : std::stoi(r["battery"]);
                it["network"]       = r["network"];
                it["appVersion"]    = r["app_version"];
                arr.append(it);
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] getDevices: " << e.what();
        }
        RESP_OK(cb, arr);
    }

    // 订单列表（从 pay_order 主表查 wepay_v3 通道的订单）
    void getOrders(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        int page = 1, size = 20;
        try { page = std::stoi(req->getParameter("page")); }     catch (...) {}
        try { size = std::stoi(req->getParameter("page_size")); } catch (...) {}
        int offset = (page - 1) * size;
        std::string filterOrderId  = req->getParameter("order_id");
        std::string filterMchId    = req->getParameter("merchant_id");
        std::string filterStatus   = req->getParameter("status");
        std::string filterStart    = req->getParameter("start_time");
        std::string filterEnd      = req->getParameter("end_time");

        std::string sql =
            "SELECT o.order_id, o.mch_order_no, o.mch_id, o.pay_type, "
            "o.amount, o.state, o.notify_state, o.created_at, o.expire_time, "
            "o.subject, o.notify_url, o.notify_email "
            "FROM pay_order o "
            "INNER JOIN pay_channel c ON c.id=o.channel_id AND c.plugin='wepay_v3' "
            "WHERE 1=1";
        std::vector<std::string> params;
        if (!filterOrderId.empty()) { sql += " AND o.order_id=?";        params.push_back(filterOrderId); }
        if (!filterMchId.empty())   { sql += " AND o.mch_id=?";          params.push_back(filterMchId); }
        if (!filterStatus.empty())  {
            // 前端: pending/success/failed → 后端 state: 0/1/2
            std::string stateVal = filterStatus == "success" ? "1" : filterStatus == "failed" ? "2" : "0";
            sql += " AND o.state=?"; params.push_back(stateVal);
        }
        if (!filterStart.empty())   { sql += " AND o.created_at>=?";     params.push_back(filterStart); }
        if (!filterEnd.empty())     { sql += " AND o.created_at<=?";     params.push_back(filterEnd); }
        sql += " ORDER BY o.created_at DESC LIMIT ? OFFSET ?";
        params.push_back(std::to_string(size));
        params.push_back(std::to_string(offset));

        // 总数
        std::string countSql =
            "SELECT COUNT(*) as c FROM pay_order o "
            "INNER JOIN pay_channel c ON c.id=o.channel_id AND c.plugin='wepay_v3' WHERE 1=1";
        std::vector<std::string> countParams;
        if (!filterOrderId.empty()) { countSql += " AND o.order_id=?";   countParams.push_back(filterOrderId); }
        if (!filterMchId.empty())   { countSql += " AND o.mch_id=?";     countParams.push_back(filterMchId); }
        if (!filterStatus.empty())  {
            std::string stateVal = filterStatus == "success" ? "1" : filterStatus == "failed" ? "2" : "0";
            countSql += " AND o.state=?"; countParams.push_back(stateVal);
        }
        if (!filterStart.empty())   { countSql += " AND o.created_at>=?"; countParams.push_back(filterStart); }
        if (!filterEnd.empty())     { countSql += " AND o.created_at<=?"; countParams.push_back(filterEnd); }

        Json::Value arr(Json::arrayValue);
        int total = 0;
        try {
            auto cntRow = db.queryOne(countSql, countParams);
            if (!cntRow.empty()) try { total = std::stoi(cntRow["c"]); } catch (...) {}
            auto rows = db.query(sql, params);
            for (auto& r : rows) {
                Json::Value it;
                it["orderId"]         = r["order_id"];
                it["merchantOrderId"] = r["mch_order_no"];
                it["merchantId"]      = r["mch_id"];
                it["deviceId"]        = "";
                it["qrId"]            = "";
                it["qrCodeType"]      = "";
                it["qrCodeName"]      = "";
                it["qrCodeContent"]   = "";
                it["amount"]          = r["amount"].empty() ? 0.0 : std::stod(r["amount"]);
                it["payType"]         = r["pay_type"];
                int st = r["state"].empty() ? 0 : std::stoi(r["state"]);
                it["status"]          = st == 1 ? "success" : st == 2 ? "failed" : "pending";
                it["screenshotUrl"]   = "";
                it["createTime"]      = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                it["payTime"]         = 0;
                it["subject"]         = r["subject"];
                it["notifyUrl"]       = r["notify_url"];
                it["notifyEmail"]     = r["notify_email"];
                try {
                    auto v3 = db.queryOne(
                        "SELECT device_id,qr_id,qr_code_type,qr_code_name,qr_code_content,screenshot_url,pay_time FROM v3_order WHERE order_id=? LIMIT 1",
                        {r["order_id"]});
                    if (!v3.empty()) {
                        it["deviceId"] = v3.count("device_id") ? v3.at("device_id") : "";
                        it["qrId"] = v3.count("qr_id") ? v3.at("qr_id") : "";
                        it["qrCodeType"] = v3.count("qr_code_type") ? v3.at("qr_code_type") : "";
                        it["qrCodeName"] = v3.count("qr_code_name") ? v3.at("qr_code_name") : "";
                        it["qrCodeContent"] = v3.count("qr_code_content") ? v3.at("qr_code_content") : "";
                        it["screenshotUrl"] = v3.count("screenshot_url") ? v3.at("screenshot_url") : "";
                        it["payTime"] = v3.count("pay_time") && !v3.at("pay_time").empty() ? (Json::Int64)std::stoll(v3.at("pay_time")) : 0;
                    }
                } catch (...) {}
                arr.append(it);
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] getOrders: " << e.what();
        }
        Json::Value data;
        data["list"]  = arr;
        data["total"] = total;
        RESP_OK(cb, data);
    }

    // 删除设备（管理端，无需 HMAC）
    void deleteDevice(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string deviceId = (*body).get("device_id", "").asString();
        if (deviceId.empty()) { RESP_ERR(cb, "device_id 不能为空"); return; }
        auto& db = PayDb::instance();
        try {
            db.exec("DELETE FROM v3_device WHERE device_id=?", {deviceId});
            db.exec("DELETE FROM v3_device_merchant WHERE device_id=?", {deviceId});
            RESP_MSG(cb, "删除成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] deleteDevice: " << e.what();
            RESP_ERR(cb, "删除失败");
        }
    }

    // 订单详情
    void getOrderDetail(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        std::string orderId = req->getParameter("order_id");
        if (orderId.empty()) { RESP_ERR(cb, "order_id 不能为空"); return; }
        auto& db = PayDb::instance();
        try {
            auto r = db.queryOne(
                "SELECT order_id,mch_order_no,mch_id,amount,pay_type,state,"
                "notify_url,notify_email,subject,created_at,pay_time,expire_time "
                "FROM pay_order WHERE order_id=?", {orderId});
            auto v3 = db.queryOne(
                "SELECT device_id,qr_id,qr_code_type,qr_code_name,qr_code_content,screenshot_url,pay_time,updated_at "
                "FROM v3_order WHERE order_id=?", {orderId});
            if (r.empty()) { RESP_ERR(cb, "订单不存在"); return; }
            Json::Value it;
            it["orderId"]         = r["order_id"];
            it["merchantOrderId"] = r["mch_order_no"];
            it["merchantId"]      = r["mch_id"];
            it["deviceId"]        = v3.count("device_id") ? v3.at("device_id") : "";
            it["qrId"]            = v3.count("qr_id") ? v3.at("qr_id") : "";
            it["qrCodeType"]      = v3.count("qr_code_type") ? v3.at("qr_code_type") : "";
            it["qrCodeName"]      = v3.count("qr_code_name") ? v3.at("qr_code_name") : "";
            it["qrCodeContent"]   = v3.count("qr_code_content") ? v3.at("qr_code_content") : "";
            it["amount"]          = r["amount"].empty() ? 0.0 : std::stod(r["amount"]);
            it["payType"]         = r["pay_type"];
            int st = r["state"].empty() ? 0 : std::stoi(r["state"]);
            it["status"]          = st == 1 ? "success" : st == 2 ? "failed" : "pending";
            it["screenshotUrl"]   = v3.count("screenshot_url") ? v3.at("screenshot_url") : "";
            it["subject"]         = r["subject"];
            it["notifyUrl"]       = r["notify_url"];
            it["notifyEmail"]     = r["notify_email"];
            it["createTime"]      = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
            it["payTime"]         = v3.count("pay_time") && !v3.at("pay_time").empty()
                                        ? (Json::Int64)std::stoll(v3.at("pay_time"))
                                        : (r["pay_time"].empty() ? 0 : (Json::Int64)std::stoll(r["pay_time"]));
            it["expireTime"]      = r["expire_time"].empty() ? 0 : (Json::Int64)std::stoll(r["expire_time"]);
            it["updateTime"]      = v3.count("updated_at") && !v3.at("updated_at").empty() ? (Json::Int64)std::stoll(v3.at("updated_at")) : 0;
            RESP_OK(cb, it);
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] getOrderDetail: " << e.what();
            RESP_ERR(cb, "查询失败");
        }
    }

    // 手动回调（管理端触发）：模拟支付成功 → HTTP 回调 + 邮件通知
    void manualCallback(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string orderId = (*body).get("order_id", "").asString();
        if (orderId.empty()) { RESP_ERR(cb, "order_id 不能为空"); return; }
        auto& db = PayDb::instance();
        try {
            // 同时查 v3_order 和 pay_order（有些字段各在一张表）
            auto v3Row = db.queryOne(
                "SELECT order_id,merchant_order_id,merchant_id,device_id,"
                "amount,pay_type,status,notify_email,created_at "
                "FROM v3_order WHERE order_id=?",
                {orderId});
            auto payRow = db.queryOne(
                "SELECT order_id,mch_order_no,notify_url,state FROM pay_order WHERE order_id=?",
                {orderId});
            if (v3Row.empty() && payRow.empty()) { RESP_ERR(cb, "订单不存在"); return; }

            // 补全订单信息（v3_order 优先，pay_order 兜底）
            std::string mchOrderNo = v3Row.count("merchant_order_id") ? v3Row.at("merchant_order_id") : "";
            std::string merchantId = v3Row.count("merchant_id") ? v3Row.at("merchant_id") : "";
            std::string deviceId   = v3Row.count("device_id")    ? v3Row.at("device_id")    : "";
            std::string amount     = v3Row.count("amount")        ? v3Row.at("amount")        : "";
            std::string payType    = v3Row.count("pay_type")     ? v3Row.at("pay_type")     : "";
            std::string notifyEmail= v3Row.count("notify_email") ? v3Row.at("notify_email") : "";
            long long createTime   = v3Row.count("created_at")   ? std::stoll(v3Row.at("created_at")) : 0;
            if (mchOrderNo.empty() && !payRow.empty())
                mchOrderNo = payRow.count("mch_order_no") ? payRow.at("mch_order_no") : "";
            std::string notifyUrl;
            if (!payRow.empty())
                notifyUrl = payRow.count("notify_url") ? payRow.at("notify_url") : "";

            long long now = std::time(nullptr);
            int payState = (!payRow.empty() && !payRow["state"].empty()) ? std::stoi(payRow["state"]) : 0;

            // 标记为已支付
            if (payState == 0) {
                if (!payRow.empty())
                    db.exec("UPDATE pay_order SET state=1,pay_time=?,updated_at=? WHERE order_id=?",
                            {std::to_string(now), std::to_string(now), orderId});
            }
            if (!v3Row.empty()) {
                std::string oldStatus = v3Row.count("status") ? v3Row.at("status") : "PENDING";
                if (oldStatus != "PAID") {
                    db.exec("UPDATE v3_order SET status='PAID',pay_time=?,updated_at=? WHERE order_id=?",
                            {std::to_string(now), std::to_string(now), orderId});
                }
            }

            // 触发 HTTP 商户回调
            if (!notifyUrl.empty()) {
                NotifyTaskService::createTaskAndSend(orderId, notifyUrl, "WepayV3Plugin");
            }

            // 发邮件（走 EmailService，统一模板）
            auto orderSvc = WepayV3Plugin::getInstance().getOrderService();
            std::string emailNote;
            if (orderSvc) {
                try {
                    orderSvc->sendOrderEmail(orderId, "MANUAL_CALLBACK", false);
                    emailNote = "，邮件已发送";
                } catch (const std::exception& e) {
                    LOG_WARN << "[AdminV3] manualCallback email error: " << e.what();
                    emailNote = "，邮件发送失败";
                }
            } else {
                emailNote = "，订单服务未就绪";
            }

            RESP_MSG(cb, "回调已触发" + emailNote);
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] manualCallback: " << e.what();
            RESP_ERR(cb, "操作失败");
        }
    }
    // 系统配置读取
    void getSystemConfig(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query("SELECT config_key,config_value,value_type,description FROM v3_system_config ORDER BY id", {});
            for (auto& r : rows) {
                Json::Value it;
                it["key"]         = r["config_key"];
                it["value"]       = r["config_value"];
                it["type"]        = r["value_type"];
                it["description"] = r["description"];
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getSystemConfig: " << e.what(); }
        RESP_OK(cb, arr);
    }

    // 系统配置保存
    void saveSystemConfig(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body || !(*body).isArray()) { RESP_ERR(cb, "格式错误"); return; }
        auto& db = PayDb::instance();
        try {
            for (const auto& item : *body) {
                std::string key = item.get("key", "").asString();
                std::string val = item.get("value", "").asString();
                if (key.empty()) continue;
                db.exec("UPDATE v3_system_config SET config_value=?,updated_at=? WHERE config_key=?",
                        {val, std::to_string(std::time(nullptr)), key});
            }
            RESP_MSG(cb, "保存成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] saveSystemConfig: " << e.what();
            RESP_ERR(cb, "保存失败");
        }
    }

    // IP 白名单列表
    void getIpWhitelist(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query("SELECT id,merchant_id,ip_address,description,created_at FROM v3_ip_whitelist ORDER BY id DESC", {});
            for (auto& r : rows) {
                Json::Value it;
                it["id"]          = r["id"].empty() ? 0 : std::stoi(r["id"]);
                it["merchantId"]  = r["merchant_id"];
                it["ip"]          = r["ip_address"];
                it["description"] = r["description"];
                it["createdAt"]   = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getIpWhitelist: " << e.what(); }
        RESP_OK(cb, arr);
    }

    // 添加 IP 白名单
    void addIpWhitelist(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string ip   = (*body).get("ip", "").asString();
        std::string mch  = (*body).get("merchant_id", "").asString();
        std::string desc = (*body).get("description", "").asString();
        if (ip.empty()) { RESP_ERR(cb, "ip 不能为空"); return; }
        auto& db = PayDb::instance();
        try {
            long ts = std::time(nullptr);
            db.exec("INSERT INTO v3_ip_whitelist(merchant_id,ip_address,description,created_at,updated_at) VALUES(?,?,?,?,?)",
                    {mch, ip, desc, std::to_string(ts), std::to_string(ts)});
            RESP_MSG(cb, "添加成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] addIpWhitelist: " << e.what();
            RESP_ERR(cb, "添加失败");
        }
    }

    // 删除 IP 白名单
    void delIpWhitelist(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        if (id.empty()) { RESP_ERR(cb, "id 不能为空"); return; }
        auto& db = PayDb::instance();
        try {
            db.exec("DELETE FROM v3_ip_whitelist WHERE id=?", {id});
            RESP_MSG(cb, "删除成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] delIpWhitelist: " << e.what();
            RESP_ERR(cb, "删除失败");
        }
    }

    // 安全审计日志
    void getAuditLog(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        int page = 1, size = 20;
        try { page = std::stoi(req->getParameter("page")); }     catch (...) {}
        try { size = std::stoi(req->getParameter("page_size")); } catch (...) {}
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query(
                "SELECT request_ip,device_id,action,result,reason,created_at "
                "FROM v3_security_audit_log ORDER BY created_at DESC LIMIT ? OFFSET ?",
                {std::to_string(size), std::to_string((page-1)*size)});
            for (auto& r : rows) {
                Json::Value it;
                it["ip"]        = r["request_ip"];
                it["deviceId"]  = r["device_id"];
                it["action"]    = r["action"];
                it["result"]    = r["result"];
                it["reason"]    = r["reason"];
                it["createdAt"] = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getAuditLog: " << e.what(); }
        RESP_OK(cb, arr);
    }

    // 手动回调日志
    void getCallbackLog(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        int page = 1, size = 20;
        try { page = std::stoi(req->getParameter("page")); }     catch (...) {}
        try { size = std::stoi(req->getParameter("page_size")); } catch (...) {}
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query(
                "SELECT id,order_id,mch_id,callback_token,token_expire,callback_status,"
                "callback_time,client_ip,user_agent,created_at "
                "FROM v3_manual_callback_log ORDER BY created_at DESC LIMIT ? OFFSET ?",
                {std::to_string(size), std::to_string((page-1)*size)});
            long long now = std::time(nullptr);
            for (auto& r : rows) {
                Json::Value it;
                it["id"]              = r["id"].empty() ? 0 : (Json::Int64)std::stoll(r["id"]);
                it["order_id"]        = r["order_id"];
                it["mch_id"]         = r["mch_id"];
                it["callback_status"] = r["callback_status"].empty() ? 0 : std::stoi(r["callback_status"]);
                it["client_ip"]       = r["client_ip"];
                it["user_agent"]     = r["user_agent"];
                it["callback_time"]  = r["callback_time"].empty() ? 0 : (Json::Int64)std::stoll(r["callback_time"]);
                long long exp = 0;
                try { exp = std::stoll(r["token_expire"]); } catch (...) {}
                it["expired"]        = (now > exp && exp > 0);
                it["created_at"]     = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getCallbackLog: " << e.what(); }
        RESP_OK(cb, arr);
    }

    // V3 邮件发送日志列表
    void getEmailLog(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        int page = 1, size = 20;
        try { page = std::stoi(req->getParameter("page")); }      catch (...) {}
        try { size = std::stoi(req->getParameter("page_size")); } catch (...) {}
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query(
                "SELECT id,order_id,merchant_id,email_to,subject,send_status,"
                "fail_reason,send_time,created_at "
                "FROM v3_email_log ORDER BY created_at DESC LIMIT ? OFFSET ?",
                {std::to_string(size), std::to_string((page-1)*size)});
            for (auto& r : rows) {
                Json::Value it;
                it["id"]          = r["id"].empty() ? 0 : (Json::Int64)std::stoll(r["id"]);
                it["order_id"]    = r["order_id"];
                it["merchant_id"] = r["merchant_id"];
                it["to_email"]    = r["email_to"];
                it["subject"]     = r["subject"];
                it["status"]      = r["send_status"].empty() ? 0 : std::stoi(r["send_status"]);
                it["error_msg"]   = r["fail_reason"];
                it["send_time"]   = r["send_time"].empty() ? 0 : (Json::Int64)std::stoll(r["send_time"]);
                it["created_at"]  = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getEmailLog: " << e.what(); }
        RESP_OK(cb, arr);
    }

    // 设备收款码列表
    void listDeviceQrcodes(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        std::string deviceId = req->getParameter("device_id");
        std::string merchantId = req->getParameter("merchant_id");
        Json::Value arr(Json::arrayValue);
        try {
            std::string sql =
                "SELECT id,device_id,merchant_id,pay_type,code_type,code_name,code_content,state,sort_order,last_used_at,created_at,updated_at "
                "FROM v3_device_qrcode WHERE 1=1";
            std::vector<std::string> params;
            if (!deviceId.empty()) {
                sql += " AND device_id=?";
                params.push_back(deviceId);
            }
            if (!merchantId.empty()) {
                sql += " AND merchant_id=?";
                params.push_back(merchantId);
            }
            sql += " ORDER BY device_id ASC, pay_type ASC, sort_order ASC, id ASC";
            auto rows = db.query(sql, params);
            for (auto& r : rows) {
                Json::Value it;
                it["id"] = r["id"].empty() ? 0 : std::stoi(r["id"]);
                it["deviceId"] = r["device_id"];
                it["merchantId"] = r["merchant_id"];
                it["payType"] = r["pay_type"];
                it["codeType"] = r["code_type"];
                it["codeName"] = r["code_name"];
                it["codeContent"] = r["code_content"];
                it["state"] = r["state"].empty() ? 0 : std::stoi(r["state"]);
                it["sortOrder"] = r["sort_order"].empty() ? 0 : std::stoi(r["sort_order"]);
                it["lastUsedAt"] = r["last_used_at"].empty() ? 0 : (Json::Int64)std::stoll(r["last_used_at"]);
                it["createdAt"] = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                it["updatedAt"] = r["updated_at"].empty() ? 0 : (Json::Int64)std::stoll(r["updated_at"]);
                arr.append(it);
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] listDeviceQrcodes: " << e.what();
        }
        RESP_OK(cb, arr);
    }

    // 设备收款码选码诊断日志
    void getQrSelectionLog(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        int lines = 100;
        try { lines = std::stoi(req->getParameter("lines")); } catch (...) {}
        lines = std::max(10, std::min(500, lines));
        std::string orderId = req->getParameter("order_id");
        std::string deviceId = req->getParameter("device_id");
        std::string merchantId = req->getParameter("merchant_id");

        Json::Value result;
        result["file"] = "logs/v3-qrcode-select.jsonl";
        result["exists"] = false;
        result["list"] = Json::Value(Json::arrayValue);

        try {
            std::filesystem::path logPath = std::filesystem::path("logs") / "v3-qrcode-select.jsonl";
            if (!std::filesystem::exists(logPath)) {
                RESP_OK(cb, result);
                return;
            }

            result["exists"] = true;
            std::ifstream f(logPath.string(), std::ios::binary);
            if (!f.good()) {
                RESP_OK(cb, result);
                return;
            }

            std::vector<std::string> allLines;
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) allLines.push_back(line);
            }

            int matchedCount = 0;
            for (int i = static_cast<int>(allLines.size()) - 1; i >= 0; --i) {
                Json::CharReaderBuilder rb;
                Json::Value item;
                std::string errs;
                std::istringstream iss(allLines[i]);
                if (!Json::parseFromStream(rb, iss, &item, &errs)) {
                    continue;
                }
                if (!orderId.empty() && item.get("orderId", "").asString() != orderId) continue;
                if (!deviceId.empty() && item.get("deviceId", "").asString() != deviceId) continue;
                if (!merchantId.empty() && item.get("merchantId", "").asString() != merchantId) continue;
                result["list"].append(item);
                matchedCount++;
                if (matchedCount >= lines) break;
            }
            result["total"] = matchedCount;
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] getQrSelectionLog: " << e.what();
        }

        RESP_OK(cb, result);
    }

    // 保存设备收款码
    void saveDeviceQrcode(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto& j = *body;
        std::string deviceId = j.get("deviceId", "").asString();
        std::string payType = j.get("payType", "").asString();
        std::string codeContent = j.get("codeContent", "").asString();
        if (deviceId.empty() || payType.empty() || codeContent.empty()) {
            RESP_ERR(cb, "deviceId/payType/codeContent 必填");
            return;
        }
        auto& db = PayDb::instance();
        long ts = std::time(nullptr);
        std::string id = j.get("id", "").asString();
        try {
            if (id.empty() || id == "0") {
                db.exec(
                    "INSERT INTO v3_device_qrcode(device_id,merchant_id,pay_type,code_type,code_name,code_content,state,sort_order,last_used_at,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                    {deviceId,
                     j.get("merchantId", "").asString(),
                     payType,
                     j.get("codeType", "PERSONAL").asString(),
                     j.get("codeName", "").asString(),
                     codeContent,
                     j.get("state", 1).asBool() ? "1" : "0",
                     std::to_string(j.get("sortOrder", 0).asInt()),
                     "0",
                     std::to_string(ts),
                     std::to_string(ts)});
            } else {
                db.exec(
                    "UPDATE v3_device_qrcode SET merchant_id=?,pay_type=?,code_type=?,code_name=?,code_content=?,state=?,sort_order=?,updated_at=? WHERE id=?",
                    {j.get("merchantId", "").asString(),
                     payType,
                     j.get("codeType", "PERSONAL").asString(),
                     j.get("codeName", "").asString(),
                     codeContent,
                     j.get("state", 1).asBool() ? "1" : "0",
                     std::to_string(j.get("sortOrder", 0).asInt()),
                     std::to_string(ts),
                     id});
            }
            RESP_MSG(cb, "保存成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] saveDeviceQrcode: " << e.what();
            RESP_ERR(cb, "保存失败");
        }
    }

    // 删除设备收款码
    void deleteDeviceQrcode(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        if (id.empty()) { RESP_ERR(cb, "id 不能为空"); return; }
        auto& db = PayDb::instance();
        try {
            db.exec("DELETE FROM v3_device_qrcode WHERE id=?", {id});
            RESP_MSG(cb, "删除成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] deleteDeviceQrcode: " << e.what();
            RESP_ERR(cb, "删除失败");
        }
    }

    // V3 邮件手动重发（发送失败通知邮件，含手动回调按钮）
    void resendEmail(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string orderId = (*body).get("order_id", "").asString();
        if (orderId.empty()) { RESP_ERR(cb, "order_id 必填"); return; }

        auto& db = PayDb::instance();
        auto order = db.queryOne("SELECT * FROM v3_order WHERE order_id=?", {orderId});
        if (order.empty()) { RESP_ERR(cb, "V3 订单不存在"); return; }

        std::string mchId  = order.count("merchant_id")  ? order.at("merchant_id")  : "";
        std::string mchOrderNo = order.count("merchant_order_id") ? order.at("merchant_order_id") : "";
        std::string deviceId = order.count("device_id") ? order.at("device_id") : "";
        std::string amount  = order.count("amount")     ? order.at("amount")      : "0.00";
        std::string payType = order.count("pay_type")   ? order.at("pay_type")    : "";
        std::string toEmail = order.count("notify_email") ? order.at("notify_email") : "";
        if (toEmail.empty()) {
            auto mch = db.queryOne("SELECT notify_email FROM merchant WHERE id=?", {mchId});
            toEmail = mch.count("notify_email") ? mch.at("notify_email") : "";
        }
        if (toEmail.empty()) { RESP_ERR(cb, "商户未配置通知邮箱"); return; }

        // 生成回调令牌和 URL
        auto& cfg = WepayV3Config::getInstance();
        CallbackTokenGenerator::TokenData td;
        td.orderId    = orderId;
        td.merchantId = mchId;
        td.expireTime = std::time(nullptr) + 300;
        std::string token = CallbackTokenGenerator::generateToken(td, cfg.security.hmacSecret);
        std::string baseUrl = cfg.baseUrl.empty() ? db.getSetting("site_url", "http://localhost") : cfg.baseUrl;
        std::string callbackUrl = baseUrl + "/api/wepay/v3/callback/manual?order_id=" + orderId
                                 + "&mch_id=" + mchId + "&token=" + token;

        // 构造 EmailData
        EmailService::EmailData data;
        data.orderId         = orderId;
        data.merchantOrderId = mchOrderNo;
        data.merchantId     = mchId;
        data.deviceId       = deviceId;
        data.toEmail        = toEmail;
        data.money          = amount;
        data.payType        = payType;
        data.failReason     = "管理员手动重发通知";
        long long ct = 0;
        try { ct = std::stoll(order.count("created_at") ? order.at("created_at") : "0"); } catch (...) {}
        char ctBuf[32]; std::time_t ctt = (std::time_t)ct;
        struct tm ctm;
#ifdef _WIN32
        localtime_s(&ctm, &ctt);
#else
        localtime_r(&ctt, &ctm);
#endif
        std::strftime(ctBuf, sizeof(ctBuf), "%Y-%m-%d %H:%M:%S", &ctm);
        data.createTime = ctBuf;
        data.callbackUrl    = callbackUrl;
        data.resendEmailUrl = baseUrl + "/api/wepay/v3/email/resend?order_id=" + orderId
                             + "&mch_id=" + mchId;
        data.orderViewUrl   = baseUrl + "/admin/#/v3/orders?q=" + orderId;
        data.closeOrderUrl  = baseUrl + "/gateway/close";
        data.editOrderUrl   = baseUrl + "/admin/#/v3/orders?q=" + orderId;

        auto svc = EmailService::globalInstance();
        if (!svc) { RESP_ERR(cb, "邮件服务未初始化"); return; }

        if (svc->sendPayFailEmail(data)) {
            RESP_MSG(cb, "邮件已重新发送");
        } else {
            RESP_ERR(cb, "邮件发送失败，请检查 SMTP 配置");
        }
    }

    // 商户 V3 配置列表
    void getMerchantConfigs(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query(
                "SELECT merchant_id,merchant_name,hmac_secret,callback_url,notify_email,"
                "email_notify_enabled,notify_on_success,notify_on_fail,daily_summary_enabled,status "
                "FROM v3_merchant_config ORDER BY created_at DESC", {});
            for (auto& r : rows) {
                Json::Value it;
                it["merchantId"]           = r["merchant_id"];
                it["merchantName"]         = r["merchant_name"];
                it["hmacSecret"]           = r["hmac_secret"];
                it["callbackUrl"]          = r["callback_url"];
                it["notifyEmail"]          = r["notify_email"];
                it["emailNotifyEnabled"]   = r["email_notify_enabled"] == "1";
                it["notifyOnSuccess"]      = r["notify_on_success"] == "1";
                it["notifyOnFail"]         = r["notify_on_fail"] == "1";
                it["dailySummaryEnabled"]  = r["daily_summary_enabled"] == "1";
                it["status"]               = r["status"] == "1";
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getMerchantConfigs: " << e.what(); }
        RESP_OK(cb, arr);
    }

    // 保存商户 V3 配置（INSERT OR REPLACE）
    void saveMerchantConfig(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto& j = *body;
        std::string mchId = j.get("merchantId", "").asString();
        if (mchId.empty()) { RESP_ERR(cb, "merchantId 不能为空"); return; }
        long ts = std::time(nullptr);
        auto& db = PayDb::instance();
        try {
            db.exec(
                "INSERT INTO v3_merchant_config("
                "merchant_id,merchant_name,hmac_secret,callback_url,notify_email,"
                "email_notify_enabled,notify_on_success,notify_on_fail,daily_summary_enabled,status,"
                "created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(merchant_id) DO UPDATE SET "
                "merchant_name=excluded.merchant_name,"
                "hmac_secret=excluded.hmac_secret,"
                "callback_url=excluded.callback_url,"
                "notify_email=excluded.notify_email,"
                "email_notify_enabled=excluded.email_notify_enabled,"
                "notify_on_success=excluded.notify_on_success,"
                "notify_on_fail=excluded.notify_on_fail,"
                "daily_summary_enabled=excluded.daily_summary_enabled,"
                "status=excluded.status,"
                "updated_at=excluded.updated_at",
                {mchId,
                 j.get("merchantName","").asString(),
                 j.get("hmacSecret","").asString(),
                 j.get("callbackUrl","").asString(),
                 j.get("notifyEmail","").asString(),
                 j.get("emailNotifyEnabled",true).asBool() ? "1" : "0",
                 j.get("notifyOnSuccess",true).asBool()    ? "1" : "0",
                 j.get("notifyOnFail",true).asBool()       ? "1" : "0",
                 j.get("dailySummaryEnabled",true).asBool()? "1" : "0",
                 j.get("status",true).asBool()             ? "1" : "0",
                 std::to_string(ts), std::to_string(ts)});
            RESP_MSG(cb, "保存成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] saveMerchantConfig: " << e.what();
            RESP_ERR(cb, "保存失败");
        }
    }

    // 订单趋势（供监控图表）
    void getOrderTrend(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        std::string groupBy = req->getParameter("group_by");
        if (groupBy.empty()) groupBy = "hour";
        auto& db = PayDb::instance();
        Json::Value arr(Json::arrayValue);
        try {
            std::string sql;
            if (groupBy == "day") {
                sql = "SELECT strftime('%m-%d', datetime(created_at,'unixepoch','localtime')) as slot, "
                      "COUNT(*) as cnt, COALESCE(SUM(CASE WHEN status='PAID' THEN amount ELSE 0 END),0) as amt "
                      "FROM v3_order WHERE created_at >= strftime('%s','now','-7 days') "
                      "GROUP BY slot ORDER BY slot";
            } else {
                sql = "SELECT strftime('%H:00', datetime(created_at,'unixepoch','localtime')) as slot, "
                      "COUNT(*) as cnt, COALESCE(SUM(CASE WHEN status='PAID' THEN amount ELSE 0 END),0) as amt "
                      "FROM v3_order WHERE created_at >= strftime('%s','now','-24 hours') "
                      "GROUP BY slot ORDER BY slot";
            }
            auto rows = db.query(sql, {});
            for (auto& r : rows) {
                Json::Value it;
                it["time"]   = r["slot"];
                it["count"]  = r["cnt"].empty() ? 0 : std::stoi(r["cnt"]);
                it["amount"] = r["amt"].empty() ? 0.0 : std::stod(r["amt"]);
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getOrderTrend: " << e.what(); }
        RESP_OK(cb, arr);
    }

    // 告警配置（存入 v3_system_config）
    void getAlertConfig(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        Json::Value data;
        static const std::vector<std::string> keys = {
            "alert_enabled","dingtalk_webhook","wecom_webhook","alert_email",
            "alert_on_device_offline","alert_on_order_timeout","alert_on_security_event"
        };
        try {
            for (auto& k : keys) {
                auto r = db.queryOne("SELECT config_value FROM v3_system_config WHERE config_key=?", {k});
                data[k] = r.empty() ? "" : r["config_value"];
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getAlertConfig: " << e.what(); }
        RESP_OK(cb, data);
    }

    void saveAlertConfig(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto& db = PayDb::instance();
        long ts = std::time(nullptr);
        try {
            for (auto it = body->begin(); it != body->end(); ++it) {
                std::string key = it.key().asString();
                std::string val = (*body)[key].asString();
                db.exec(
                    "INSERT INTO v3_system_config(config_key,config_value,config_type,description,created_at,updated_at) "
                    "VALUES(?,?,'STRING','alert config',?,?) "
                    "ON CONFLICT(config_key) DO UPDATE SET config_value=excluded.config_value,updated_at=excluded.updated_at",
                    {key, val, std::to_string(ts), std::to_string(ts)});
            }
            RESP_MSG(cb, "保存成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] saveAlertConfig: " << e.what();
            RESP_ERR(cb, "保存失败");
        }
    }

    // ───── 全局邮件账号管理（单账号兼容 + 多账号轮询）─────

    // 获取账号列表（只有一个时兼容旧单账号页面）
    void getEmailConfig(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        listEmailAccounts(req, std::move(cb));
    }

    // 保存单账号（向后兼容，就是第一个账号）→转到 saveEmailAccount
    void saveEmailConfig(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        saveEmailAccount(req, std::move(cb));
    }

    // 发送测试邮件（所有账号轮询一次）
    void testEmailConfig(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        testEmailAccount(req, std::move(cb));
    }

    // 列出全部邮件账号
    void listEmailAccounts(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto& db = PayDb::instance();
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query(
                "SELECT id,name,smtp_host,smtp_port,username,from_email,from_name,"
                "use_ssl,status,send_count,created_at FROM v3_email_account ORDER BY id ASC", {});
            for (auto& r : rows) {
                Json::Value it;
                it["id"]         = r["id"].empty() ? 0 : std::stoi(r["id"]);
                it["name"]       = r["name"];
                it["smtpHost"]   = r["smtp_host"];
                it["smtpPort"]   = r["smtp_port"].empty() ? 465 : std::stoi(r["smtp_port"]);
                it["username"]   = r["username"];
                it["password"]   = "******"; // 密码打码
                it["fromEmail"]  = r["from_email"];
                it["fromName"]   = r["from_name"];
                it["useSsl"]     = r["use_ssl"] != "0";
                it["status"]     = r["status"] != "0";
                it["sendCount"]  = r["send_count"].empty() ? 0 : std::stoi(r["send_count"]);
                it["createdAt"]  = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                arr.append(it);
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] listEmailAccounts: " << e.what();
        }
        RESP_OK(cb, arr);
    }

    // 新增或修改账号（有 id 则更新，无 id 则新增）
    void saveEmailAccount(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto& j = *body;
        auto& db = PayDb::instance();
        long ts = std::time(nullptr);

        std::string name      = j.get("name","").asString();
        std::string host      = j.get("smtpHost","").asString();
        std::string port      = j.get("smtpPort","465").asString();
        std::string user      = j.get("username","").asString();
        std::string pass      = j.get("password","").asString();
        std::string fromEmail = j.get("fromEmail","").asString();
        std::string fromName  = j.get("fromName","WePay V3").asString();
        std::string useSsl    = j.get("useSsl",true).asBool() ? "1" : "0";
        std::string status    = j.get("status",true).asBool()  ? "1" : "0";
        int id                = j.get("id",0).asInt();

        if (host.empty() || fromEmail.empty()) {
            RESP_ERR(cb, "smtpHost 和 fromEmail 不能为空"); return;
        }

        try {
            if (id > 0) {
                // 更新已有账号
                if (pass == "******" || pass.empty()) {
                    // 密码未改，不更新密码字段
                    db.exec(
                        "UPDATE v3_email_account SET name=?,smtp_host=?,smtp_port=?,username=?,"
                        "from_email=?,from_name=?,use_ssl=?,status=?,updated_at=? WHERE id=?",
                        {name,host,port,user,fromEmail,fromName,useSsl,status,
                         std::to_string(ts),std::to_string(id)});
                } else {
                    db.exec(
                        "UPDATE v3_email_account SET name=?,smtp_host=?,smtp_port=?,username=?,"
                        "password=?,from_email=?,from_name=?,use_ssl=?,status=?,updated_at=? WHERE id=?",
                        {name,host,port,user,pass,fromEmail,fromName,useSsl,status,
                         std::to_string(ts),std::to_string(id)});
                }
            } else {
                // 新增账号
                db.exec(
                    "INSERT INTO v3_email_account(name,smtp_host,smtp_port,username,password,"
                    "from_email,from_name,use_ssl,status,send_count,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,0,?,?)",
                    {name,host,port,user,pass,fromEmail,fromName,useSsl,status,
                     std::to_string(ts),std::to_string(ts)});
            }
            // 热重载所有账号到 EmailService
            auto svc = wepay::v3::EmailService::globalInstance();
            if (svc) svc->loadAccountsFromDb();
            RESP_MSG(cb, "保存成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] saveEmailAccount: " << e.what();
            RESP_ERR(cb, "保存失败: " + std::string(e.what()));
        }
    }

    // 删除账号
    void delEmailAccount(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        int id = (*body).get("id",0).asInt();
        if (id <= 0) { RESP_ERR(cb, "id 无效"); return; }
        auto& db = PayDb::instance();
        try {
            db.exec("DELETE FROM v3_email_account WHERE id=?", {std::to_string(id)});
            auto svc = wepay::v3::EmailService::globalInstance();
            if (svc) svc->loadAccountsFromDb();
            RESP_MSG(cb, "删除成功");
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminV3] delEmailAccount: " << e.what();
            RESP_ERR(cb, "删除失败");
        }
    }

    // 从 v3_email_account 加载启用账号到 SmtpUtils
    static void reloadSmtpFromDb() {
        auto& db = PayDb::instance();
        try {
            auto rows = db.query(
                "SELECT smtp_host,smtp_port,username,password,from_email,from_name,use_ssl "
                "FROM v3_email_account WHERE status=1 ORDER BY id ASC LIMIT 1", {});
            if (rows.empty()) return;
            auto& r = rows[0];
            int port = r["smtp_port"].empty() ? 465 : std::stoi(r["smtp_port"]);
            std::string fromName = r["from_name"].empty() ? "WePay V3" : r["from_name"];
            SmtpUtils::instance().loadConfig(
                r["smtp_host"], port, fromName,
                {{ r["username"], r["password"] }});
        } catch (...) {}
    }

    // 发送测试邮件（使用 SmtpUtils 直接发送，无需 EmailService 初始化）
    void testEmailAccount(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string toEmail = (*body).get("email","").asString();
        if (toEmail.empty()) { RESP_ERR(cb, "email 不能为空"); return; }

        reloadSmtpFromDb();
        if (!SmtpUtils::instance().isConfigured()) {
            RESP_ERR(cb, "未找到已启用的邮箱账号，请先添加并启用账号"); return;
        }
        bool ok = SmtpUtils::instance().send(toEmail, "WePay V3 测试邮件",
            "这是一封来自 WePay V3 系统的测试邮件，如收到请忽略。");
        if (ok) RESP_MSG(cb, "测试邮件已发送到 " + toEmail);
        else    RESP_ERR(cb, "发送失败，请检查账号配置或查看系统日志");
    }

    // 订单状态流转日志
    void getOrderStatusLog(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        std::string orderId = req->getParameter("order_id");
        if (orderId.empty()) { RESP_ERR(cb, "order_id 不能为空"); return; }
        auto& db = PayDb::instance();
        Json::Value arr(Json::arrayValue);
        try {
            auto rows = db.query(
                "SELECT old_status,new_status,remark,created_at "
                "FROM v3_order_status_log WHERE order_id=? ORDER BY created_at ASC", {orderId});
            for (auto& r : rows) {
                Json::Value it;
                it["oldStatus"] = r["old_status"];
                it["newStatus"] = r["new_status"];
                it["remark"]    = r["remark"];
                it["createdAt"] = r["created_at"].empty() ? 0 : (Json::Int64)std::stoll(r["created_at"]);
                arr.append(it);
            }
        } catch (const std::exception& e) { LOG_WARN << "[AdminV3] getOrderStatusLog: " << e.what(); }
        RESP_OK(cb, arr);
    }
};

} // namespace v3
} // namespace wepay
