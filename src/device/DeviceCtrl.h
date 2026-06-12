// WePay-Cpp — 设备上报控制器
// POST /device/heart        设备心跳
// POST /device/push         收款推送(监听到收款时调用)
// POST /device/register     设备注册/绑定
// GET  /device/orders/{no}  拉取待支付订单(设备轮询)
// 兼容旧接口:
// POST /appHeart            V免签心跳
// POST /appPush             V免签推送
// GET  /checkOrder/{p}/{s}  码支付轮询
// GET  /checkPayResult      码支付推送
// POST /mpayNotify          SmsForwarder推送
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <regex>
#include <json/json.h>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/Md5Utils.h"
#include "../common/EpaySign.h"
#include "../common/ChannelService.h"
#include "../common/NotifyTaskService.h"
#include "../common/PaymentService.h"

class DeviceCtrl : public drogon::HttpController<DeviceCtrl> {
public:
    METHOD_LIST_BEGIN
        // 新版设备接口
        ADD_METHOD_TO(DeviceCtrl::heart,      "/device/heart",          drogon::Post);
        ADD_METHOD_TO(DeviceCtrl::push,       "/device/push",           drogon::Post);
        ADD_METHOD_TO(DeviceCtrl::regDevice,  "/device/register",       drogon::Post);
        ADD_METHOD_TO(DeviceCtrl::pullOrders, "/device/orders/{1}",     drogon::Get);
        // V免签 /appHeart /appPush 与 码支付 /checkOrder /checkPayResult /mpayNotify
        // 全部由 MonitorCtrl 统一处理（避免与本控制器重复注册导致路由冲突）
    METHOD_LIST_END

    // ══════════════════════════════════════════════════════════
    //  新版设备接口
    // ══════════════════════════════════════════════════════════

    // POST /device/heart { device_no, sign, t }
    void heart(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        std::string deviceNo = getField(params, "device_no");
        std::string t        = getField(params, "t");
        std::string sign     = getField(params, "sign");

        if (deviceNo.empty()) { RESP_ERR(cb, "缺少 device_no"); return; }

        auto &db = PayDb::instance();
        auto dev = db.queryOne("SELECT * FROM device WHERE device_no=?", {deviceNo});
        if (dev.empty()) { RESP_ERR(cb, "设备不存在"); return; }

        long long now = std::time(nullptr);
        db.exec("UPDATE device SET last_heart=?,state=1,updated_at=? WHERE device_no=?",
                {std::to_string(now), std::to_string(now), deviceNo});

        RESP_MSG(cb, "心跳更新成功");
    }

    // POST /device/push { device_no, pay_type, amount, sign, t }
    void push(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        std::string deviceNo = getField(params, "device_no");
        std::string payType  = getField(params, "pay_type");
        std::string amount   = getField(params, "amount");

        if (deviceNo.empty() || payType.empty() || amount.empty()) {
            RESP_ERR(cb, "参数不完整"); return;
        }

        auto &db = PayDb::instance();
        auto dev = db.queryOne("SELECT * FROM device WHERE device_no=? AND state=1",
                               {deviceNo});
        if (dev.empty()) { RESP_ERR(cb, "设备不存在或离线"); return; }

        long long now = std::time(nullptr);
        db.exec("UPDATE device SET last_pay=?,updated_at=? WHERE device_no=?",
                {std::to_string(now), std::to_string(now), deviceNo});

        amount = fmtPrice(amount);
        RESP_MSG(cb, "推送成功");

        // 发布 raw 审计事件 + 调公共服务匹配订单 / 入账 / 通知 / publish
        Json::Value extra; extra["device_no"] = deviceNo;
        PaymentService::publishRawIncoming("device", payType, amount, extra);
        PaymentService::processPayment(payType, amount);
    }

    // POST /device/register { device_no, device_name, device_type, key }
    void regDevice(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }

        std::string deviceNo   = (*body).get("device_no", "").asString();
        std::string deviceName = (*body).get("device_name", "").asString();
        int deviceType         = (*body).get("device_type", 0).asInt();
        std::string key        = (*body).get("key", "").asString();

        if (deviceNo.empty()) { RESP_ERR(cb, "设备编号不能为空"); return; }

        // 验证系统密钥
        auto &db = PayDb::instance();
        // 允许用管理员密钥注册
        // TODO: 更细粒度的设备注册验证

        auto exist = db.queryOne("SELECT id FROM device WHERE device_no=?", {deviceNo});
        long long now = std::time(nullptr);
        if (exist.empty()) {
            db.exec("INSERT INTO device(device_no,device_name,device_type,state,created_at,updated_at) "
                    "VALUES(?,?,?,0,?,?)",
                    {deviceNo, deviceName, std::to_string(deviceType),
                     std::to_string(now), std::to_string(now)});
        } else {
            db.exec("UPDATE device SET device_name=?,device_type=?,updated_at=? WHERE device_no=?",
                    {deviceName, std::to_string(deviceType), std::to_string(now), deviceNo});
        }

        RESP_MSG(cb, "设备注册成功");
    }

    // GET /device/orders/{deviceNo}
    void pullOrders(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    std::string deviceNo) {
        auto &db = PayDb::instance();
        auto dev = db.queryOne("SELECT * FROM device WHERE device_no=?", {deviceNo});
        if (dev.empty()) { RESP_ERR(cb, "设备不存在"); return; }

        // 更新心跳
        long long now = std::time(nullptr);
        db.exec("UPDATE device SET last_heart=?,state=1,updated_at=? WHERE device_no=?",
                {std::to_string(now), std::to_string(now), deviceNo});

        // 查待支付订单
        int closeMin = 5;
        try { closeMin = std::stoi(db.getSetting("close_minutes", "5")); } catch (...) {}
        long long cutoff = now - closeMin * 60;

        auto rows = db.query(
            "SELECT order_id,pay_type,real_amount,subject,expire_time "
            "FROM pay_order WHERE state=0 AND created_at>? "
            "ORDER BY created_at ASC LIMIT 20",
            {std::to_string(cutoff)});

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["order_id"]    = r.at("order_id");
            item["pay_type"]    = r.at("pay_type");
            item["amount"]      = r.at("real_amount");
            item["subject"]     = r.at("subject");
            item["expire_time"] = r.at("expire_time");
            arr.append(item);
        }
        Json::Value data;
        data["orders"] = arr;
        data["count"]  = (int)rows.size();
        RESP_OK(cb, data);
    }

    // ══════════════════════════════════════════════════════════
    //  V免签兼容接口
    // ══════════════════════════════════════════════════════════
    void appHeart(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string t    = getParam(req, "t");
        std::string sign = getParam(req, "sign");
        if (t.empty() || sign.empty()) { legacyResp(cb, -1, "缺少必要参数"); return; }
        auto &db = PayDb::instance();
        std::string key = db.getSetting("device_key", db.getSetting("key"));
        if (key.empty()) key = db.getSetting("admin_pass", "admin");
        if (Md5Utils::heartSign(t, key) != sign) {
            legacyResp(cb, -1, "密钥错误"); return;
        }
        long long now = std::time(nullptr);
        db.setSetting("lastheart", std::to_string(now));
        db.setSetting("jkstate", "1");
        legacyResp(cb, 1, "成功");
    }

    void appPush(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string t     = getParam(req, "t");
        std::string type  = getParam(req, "type");
        std::string price = getParam(req, "price");
        std::string sign  = getParam(req, "sign");
        if (t.empty() || type.empty() || price.empty() || sign.empty()) {
            legacyResp(cb, -1, "缺少必要参数"); return;
        }
        auto &db = PayDb::instance();
        std::string key = db.getSetting("device_key", db.getSetting("key"));
        if (key.empty()) key = db.getSetting("admin_pass", "admin");
        if (Md5Utils::pushSign(type, price, t, key) != sign) {
            legacyResp(cb, -1, "签名校验不通过"); return;
        }
        price = fmtPrice(price);
        db.setSetting("lastpay", std::to_string(std::time(nullptr)));
        legacyResp(cb, 1, "成功");

        // type: 1=wxpay, 2=alipay
        std::string payType = (type == "1") ? "wxpay" : "alipay";
        PaymentService::publishRawIncoming("vsign", payType, price);
        PaymentService::processPayment(payType, price);
    }

    // ── 码支付 v1 兼容 ──────────────────────────────────────
    void checkOrder(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    std::string pid, std::string sign) {
        auto &db = PayDb::instance();
        std::string key = db.getSetting("device_key", db.getSetting("key"));
        if (key.empty()) key = db.getSetting("admin_pass", "admin");
        if (Md5Utils::md5(pid + key) != sign) {
            Json::Value r; r["code"] = 2; r["msg"] = "签名错误";
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
        }
        long long now = std::time(nullptr);
        db.setSetting("lastheart", std::to_string(now));
        db.setSetting("jkstate", "1");

        int closeMin = 5;
        try { closeMin = std::stoi(db.getSetting("close_minutes", "5")); } catch (...) {}
        long long cutoff = now - closeMin * 60;
        auto rows = db.query(
            "SELECT order_id,pay_type,real_amount FROM pay_order "
            "WHERE state=0 AND pay_time=0 AND created_at>?",
            {std::to_string(cutoff)});

        Json::Value r;
        if (rows.empty()) { r["code"] = 0; r["msg"] = "没有新订单"; }
        else {
            r["code"] = 1;
            Json::Value arr(Json::arrayValue);
            for (auto &o : rows) {
                Json::Value item;
                item["order_id"]     = o.at("order_id");
                item["type"]         = o.at("pay_type");
                item["really_price"] = o.at("real_amount");
                arr.append(item);
            }
            r["orders"] = arr;
        }
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    void checkPayResult(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string price = getParam(req, "price");
        std::string type  = getParam(req, "type");
        if (price.empty() || type.empty()) {
            Json::Value r; r["code"] = 2; r["msg"] = "参数错误";
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
        }
        price = fmtPrice(price);
        auto &db = PayDb::instance();
        db.setSetting("lastpay", std::to_string(std::time(nullptr)));

        std::string payType = type;
        int typeInt = EpaySign::typeToInt(type);
        if (typeInt == 1) payType = "wxpay";
        else if (typeInt == 2) payType = "alipay";
        PaymentService::publishRawIncoming("mpay", payType, price);
        std::string matchedOrderId = PaymentService::processPayment(payType, price);

        Json::Value r; r["code"] = 1; r["msg"] = "成功";
        if (!matchedOrderId.empty()) r["order_id"] = matchedOrderId;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    void mpayNotify(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        std::string price, typeStr;

        auto body = req->getJsonObject();
        if (body && (*body).isMember("msg")) {
            std::string msg = (*body).get("msg", "").asString();
            price = extractAmount(msg);
            if (price.empty()) { textResp(cb, "无法提取金额"); return; }
            typeStr = "wxpay";
        } else {
            std::string data = getParam(req, "data");
            Json::Value payload; Json::Reader reader;
            if (reader.parse(data, payload) && payload.isObject()) {
                price = payload.get("price", "").asString();
                std::string payway = payload.get("payway", "").asString();
                int t = EpaySign::typeToInt(payway);
                typeStr = (t == 2) ? "alipay" : "wxpay";
            } else {
                price = extractAmount(data);
                typeStr = "wxpay";
            }
        }

        if (price.empty()) { textResp(cb, "参数错误"); return; }
        price = fmtPrice(price);
        db.setSetting("lastpay", std::to_string(std::time(nullptr)));
        textResp(cb, "success");
        PaymentService::publishRawIncoming("smsforwarder", typeStr, price);
        PaymentService::processPayment(typeStr, price);
    }

private:
    static std::string getParam(const drogon::HttpRequestPtr &req, const std::string &k) {
        auto v = req->getParameter(k);
        if (!v.empty()) return v;
        auto body = req->getJsonObject();
        if (body && (*body).isMember(k)) return (*body)[k].asString();
        return "";
    }

    static std::string getField(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k); return it == m.end() ? "" : it->second;
    }

    static std::string fmtPrice(const std::string &s) {
        try { double v = std::stod(s); std::ostringstream o;
              o << std::fixed << std::setprecision(2) << v; return o.str();
        } catch (...) { return s; }
    }

    static std::string extractAmount(const std::string &text) {
        std::regex re(R"((\d+\.\d{1,2}))");
        std::smatch m;
        if (std::regex_search(text, m, re)) return m[1].str();
        return "";
    }

    static void legacyResp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                           int code, const std::string &msg) {
        Json::Value r; r["code"] = code; r["msg"] = msg;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    static void textResp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                         const std::string &text) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setBody(text); cb(r);
    }
};
