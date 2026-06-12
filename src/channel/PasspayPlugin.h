#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class PasspayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "passpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appurl", "API接口地址", "input", "", "以http://或https://开头，以/结尾");
        add("appid", "商户编号", "input");
        add("appkey", "商户私钥", "textarea", "", "RSA私钥(Base64编码)");
        add("appsecret", "平台公钥", "textarea", "", "RSA公钥(Base64编码)");
        add("appmchid", "通道ID", "input", "", "不填写将进行子商户号轮训");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=PC/公众号 3=H5 4=小程序");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appurl", "").asString().empty() || p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "精秀支付参数不完整(appurl/appid/appkey)";
            return r;
        }
        std::string tradeType = getTradeType(req.payType, p.get("apptype", "1").asInt());
        Json::Value bizContent;
        bizContent["trade_type"] = tradeType;
        bizContent["pay_channel_id"] = p.get("appmchid", "").asString();
        bizContent["out_trade_no"] = req.orderId;
        bizContent["total_amount"] = fmtAmount(req.amount);
        bizContent["subject"] = req.subject.empty() ? "商品" : req.subject;
        bizContent["notify_url"] = req.notifyUrl;
        bizContent["return_url"] = req.returnUrl;
        bizContent["client_ip"] = req.clientIp;
        // JSAPI extend
        if (req.payType == "wx_jsapi" || req.payType == "wx_lite") {
            std::string subAppid = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
            if (!subAppid.empty() && !openid.empty()) {
                bizContent["sub_appid"] = subAppid;
                bizContent["user_id"] = openid;
                Json::Value ext;
                ext["is_raw"] = 1;
                bizContent["channe_expend"] = Json::writeString(Json::StreamWriterBuilder(), ext);
            }
        }

        auto resp = executeApi(p, "pay.order/create", bizContent);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("code", -1).asInt() != 1) {
            r.errMsg = result.get("msg", "下单失败").asString();
            return r;
        }
        r.success = true;
        Json::Value data = result["data"];
        r.channelOrderNo = data.get("trade_no", "").asString();
        if (data.isMember("payurl")) {
            r.payUrl = data["payurl"].asString();
        } else if (data.isMember("payInfo")) {
            r.payUrl = data["payInfo"].asString(); // JSAPI payInfo
        }
        r.extra["data"] = data;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "sign fail";
        auto signIt = params.find("sign");
        if (signIt == params.end() || signIt->second.empty()) { r.verified = false; return r; }
        // Build sign content: ksort, skip sign and empty values, concat k=v&
        std::string signContent = buildSignContent(params);
        std::string pubKey = channelParams.get("appsecret", "").asString();
        if (pubKey.empty()) { r.verified = false; return r; }
        r.verified = RsaUtils::verifySha256(signContent, signIt->second, normalizePublicKey(pubKey));
        if (!r.verified) return r;
        std::string orderStatus = get(params, "order_status");
        r.paid = (orderStatus == "SUCCESS");
        r.orderId = get(params, "out_trade_no");
        r.channelOrderNo = get(params, "trade_no");
        r.buyerId = get(params, "channel_order_sn");
        try { r.paidAmount = std::stod(get(params, "total_amount")); } catch (...) {}
        r.responseText = r.paid ? "success" : "status fail";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        Json::Value bizContent;
        bizContent["refund_amount"] = fmtAmount(req.refundAmount);
        bizContent["refund_reason"] = "订单退款";
        bizContent["out_refund_no"] = req.refundNo;
        bizContent["trade_no"] = req.channelOrderNo;

        auto resp = executeApi(p, "pay.order/refund", bizContent);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("code", -1).asInt() == 1) {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = result["data"].get("trade_no", "").asString();
        } else {
            r.errMsg = result.get("msg", "退款失败").asString();
        }
        return r;
    }

private:
    struct ApiResponse {
        bool success = false;
        std::string body;
        std::string errMsg;
    };

    static ApiResponse executeApi(const Json::Value &p, const std::string &method, const Json::Value &bizContent) {
        ApiResponse ar;
        std::string apiurl = p.get("appurl", "").asString();
        std::string mchid = p.get("appid", "").asString();
        std::string privateKey = p.get("appkey", "").asString();

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bizStr = Json::writeString(wb, bizContent);

        // Build public params
        std::map<std::string, std::string> m;
        m["mchid"] = mchid;
        m["method"] = method;
        m["charset"] = "utf-8";
        m["sign_type"] = "RSA2";
        m["timestamp"] = std::to_string((long long)std::time(nullptr));
        m["version"] = "1.0";
        m["biz_content"] = bizStr;
        // Sign
        std::string signContent = buildSignContent(m);
        m["sign"] = RsaUtils::signSha256(signContent, normalizePrivateKey(privateKey));

        // POST form
        std::string formBody = buildFormBody(m);
        auto resp = SyncHttp::postForm(apiurl + method, formBody);
        ar.body = resp.body;
        ar.success = resp.success;
        if (!resp.success) ar.errMsg = resp.errMsg;
        return ar;
    }

    // ksort, skip sign and empty values, concat k=v& (no trailing &)
    static std::string buildSignContent(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "sign" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string getTradeType(const std::string &payType, int apptype) {
        if (payType.find("wx") != std::string::npos) {
            if (apptype == 2) return "wechatPub";   // 微信公众号
            if (apptype == 3) return "wechatWap";   // 微信H5
            if (apptype == 4) return "wechatLite";  // 微信小程序
            return "wechatQr";                       // 微信扫码
        }
        if (payType == "qqpay") return "qqQr";
        if (payType == "bank" || payType == "bank_jsapi") return "unionQr";
        // alipay
        if (apptype == 2) return "alipayPc";
        if (apptype == 3) return "alipayWap";
        if (apptype == 4) return "alipayPub";
        return "alipayQr";
    }

    static std::string normalizePrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN RSA PRIVATE KEY-----\n" + b + "-----END RSA PRIVATE KEY-----\n";
    }
    static std::string normalizePublicKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN PUBLIC KEY-----\n" + b + "-----END PUBLIC KEY-----\n";
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(PasspayPlugin);
