#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class HaipayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "haipay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("accessid", "accessid", "input");
        add("accesskey", "接入秘钥", "input");
        add("agent_no", "服务商编号", "input");
        add("merch_no", "商户编号", "input");
        add("pn", "终端号", "input");
        add("apptype", "支付方式", "input", "1", "支付宝：1扫码 2JS支付");
        add("appswitch", "环境", "select", "0", "0=生产 1=测试");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("accessid", "").asString().empty() || p.get("accesskey", "").asString().empty() ||
            p.get("agent_no", "").asString().empty() || p.get("merch_no", "").asString().empty() ||
            p.get("pn", "").asString().empty()) {
            r.errMsg = "海科支付参数不完整(accessid/accesskey/agent_no/merch_no/pn)";
            return r;
        }
        Json::Value body;
        body["agent_no"] = p.get("agent_no", "").asString();
        body["merch_no"] = p.get("merch_no", "").asString();
        body["pay_type"] = mapPayType(req.payType);
        body["pay_mode"] = mapPayMode(req.payType, p.get("apptype", "1").asString());
        body["out_trade_no"] = req.orderId;
        body["total_amount"] = fmtAmount(req.amount);
        body["pn"] = p.get("pn", "").asString();
        body["notify_url"] = req.notifyUrl;
        std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
        std::string appid = p.get("sub_appid", "").asString();
        if (!openid.empty()) body["openid"] = openid;
        if (!appid.empty()) body["appid"] = appid;
        if (body["pay_type"].asString() == "WX") body["extend_params"]["body"] = req.subject.empty() ? "商品" : req.subject;
        else if (body["pay_type"].asString() == "ALI") body["extend_params"]["subject"] = req.subject.empty() ? "商品" : req.subject;

        Json::Value j = request(p, "/api/v2/pay/pre-pay", body, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.channelOrderNo = j.get("trade_no", "").asString();
        std::string payType = body["pay_type"].asString();
        std::string payMode = body["pay_mode"].asString();
        if (payType == "ALI" && payMode == "NATIVE") {
            r.qrCode = j.get("ali_qr_code", "").asString();
            r.payUrl = r.qrCode;
        } else if (payType == "ALI") {
            r.payUrl = j.get("ali_trade_no", "").asString();
            r.extra["ali_trade_no"] = r.payUrl;
        } else if (payType == "WX" && payMode == "JSAPI") {
            r.extra["payinfo"] = j.get("wc_pay_data", "").asString();
        } else if (payType == "UNIONQR") {
            r.qrCode = j.get("uniqr_qr_code", "").asString();
            r.payUrl = r.qrCode;
        } else {
            r.qrCode = j.get("code_url", "").asString();
            r.payUrl = r.qrCode;
        }
        return r;
    }

    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        Json::Value body;
        body["merch_no"] = channelParams.get("merch_no", "").asString();
        body["trade_no"] = orderId;
        std::string raw, err;
        Json::Value j = request(channelParams, "/api/v2/pay/order-query", body, raw, err);
        if (!err.empty()) return r;
        r.success = true;
        r.tradeState = j.get("trade_status", "").asString() == "1" ? 1 : 0;
        r.channelOrderNo = j.get("trade_no", "").asString();
        try { r.paidAmount = std::stod(j.get("order_amount", "0").asString()); } catch (...) {}
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &,
                                     const std::string &rawBody,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "{\"return_code\":\"FAIL\",\"return_msg\":\"SIGN ERROR\"}";
        Json::Value j;
        if (!Json::Reader().parse(rawBody, j)) {
            r.responseText = "No data";
            return r;
        }
        r.verified = sign(jsonToMap(j), channelParams.get("accesskey", "").asString()) == j.get("sign", "").asString();
        if (!r.verified) return r;
        r.paid = j.get("trade_status", "").asString() == "1";
        r.orderId = j.get("out_trade_no", "").asString();
        r.channelOrderNo = j.get("trade_no", "").asString();
        r.buyerId = j.get("openid", "").asString();
        try { r.paidAmount = std::stod(j.get("order_amount", "0").asString()); } catch (...) {}
        r.responseText = "{\"return_code\":\"SUCCESS\"}";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        Json::Value body;
        body["agent_no"] = req.channelParams.get("agent_no", "").asString();
        body["merch_no"] = req.channelParams.get("merch_no", "").asString();
        body["trade_no"] = req.channelOrderNo;
        body["out_refund_no"] = req.refundNo;
        body["refund_amount"] = fmtAmount(req.refundAmount);
        body["pn"] = req.channelParams.get("pn", "").asString();
        Json::Value j = request(req.channelParams, "/api/v2/pay/refund", body, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.state = 1;
        r.channelRefundNo = j.get("refund_no", req.refundNo).asString();
        return r;
    }

    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        Json::Value body;
        body["merch_no"] = req.channelParams.get("merch_no", "").asString();
        body["trade_no"] = req.channelOrderNo.empty() ? req.orderId : req.channelOrderNo;
        std::string raw;
        Json::Value j = request(req.channelParams, "/api/v2/pay/close-order", body, raw, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        return r;
    }

private:
    static Json::Value request(const Json::Value &p, const std::string &path, Json::Value body, std::string &raw, std::string &err) {
        body["accessid"] = p.get("accessid", "").asString();
        body["req_id"] = reqId();
        body["sign"] = sign(jsonToMap(body), p.get("accesskey", "").asString());
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string req = Json::writeString(wb, body);
        auto resp = SyncHttp::postJson(gateway(p) + path, req, {{"Content-Type", "application/json; charset=utf-8"}});
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "海科响应解析失败"; return Json::Value(); }
        if (j.get("result_code", 0).asInt() != 10000) {
            err = j.get("result_msg", j.get("return_msg", "海科请求失败").asString()).asString();
            return Json::Value();
        }
        return j;
    }

    static std::map<std::string, std::string> jsonToMap(const Json::Value &j) {
        std::map<std::string, std::string> m;
        for (auto &k : j.getMemberNames()) {
            if (k == "sign") continue;
            if (j[k].isObject() || j[k].isArray()) m[k] = signValue(j[k]);
            else m[k] = j[k].asString();
        }
        return m;
    }

    static std::string signValue(const Json::Value &v) {
        if (!v.isArray() && !v.isObject()) return v.asString();
        if (v.isArray()) {
            std::vector<std::string> parts;
            for (auto &x : v) parts.push_back(signValue(x));
            std::string s;
            for (auto &p : parts) { if (!s.empty()) s += "&"; s += p; }
            return s;
        }
        std::map<std::string, std::string> m;
        for (auto &k : v.getMemberNames()) if (!v[k].isNull() && !(v[k].isString() && v[k].asString().empty())) m[k] = signValue(v[k]);
        std::string s;
        for (auto &kv : m) { if (!s.empty()) s += "&"; s += kv.first + "=" + kv.second; }
        return s;
    }

    static std::string sign(const std::map<std::string, std::string> &params, const std::string &key) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return upper(Md5Utils::md5(s + key));
    }

    static std::string gateway(const Json::Value &p) { return p.get("appswitch", "0").asString() == "1" ? "http://39.106.187.68:8080" : "https://saas-front.hkrt.cn"; }
    static std::string mapPayType(const std::string &payType) { if (payType == "wxpay" || payType == "wx_jsapi") return "WX"; if (payType == "bank") return "UNIONQR"; return "ALI"; }
    static std::string mapPayMode(const std::string &payType, const std::string &apptype) { if (payType == "ali_jsapi" || payType == "wx_jsapi" || apptype == "2") return "JSAPI"; return "NATIVE"; }
    static std::string fmtAmount(double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); }
    static std::string upper(std::string s) { for (char &c : s) c = (char)std::toupper((unsigned char)c); return s; }
    static std::string reqId() {
        auto t = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[20];
        std::strftime(b, sizeof(b), "%Y%m%d%H%M%S", &tmv);
        return std::string(b) + randomDigits(4);
    }
    static std::string randomDigits(int n) { std::mt19937 rng((unsigned)std::random_device{}()); std::string s; for(int i=0;i<n;++i) s += char('0' + rng()%10); return s; }
};

REGISTER_CHANNEL_PLUGIN(HaipayPlugin);
