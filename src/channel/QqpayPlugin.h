#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

// QQ钱包官方支付 - 类微信支付XML API协议
// 网关: https://qpay.qq.com/cgi-bin/
// 签名: MD5, ksort拼接k=v&追加key=, uppercase
// 请求/回调: XML格式

class QqpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "qqpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "QQ钱包商户号", "input");
        add("appkey", "QQ钱包API密钥", "input");
        add("appurl", "操作员账号", "input", "", "仅退款/企业付款时需要");
        add("appmchid", "操作员密码", "input", "", "仅退款/企业付款时需要");
        add("apptype", "支付方式", "select", "1", "1=扫码(含H5) 2=公众号");
        add("merchant_pem", "商户证书私钥(PEM)", "textarea", "", "退款需要SSL双向证书");
        add("merchant_cert", "商户证书(PEM)", "textarea", "", "退款需要SSL双向证书");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "QQ钱包参数不完整(appid/appkey)";
            return r;
        }
        int apptype = p.get("apptype", "1").asInt();
        if (apptype == 2) {
            return jsapiPay(r, req);
        }
        return nativePay(r, req);
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        // QQ钱包回调是XML格式
        auto m = parseXml(body);
        std::string key = channelParams.get("appkey", "").asString();
        auto signIt = m.find("sign");
        if (signIt == m.end() || signIt->second.empty()) { r.verified = false; return r; }
        std::string calcSign = makeSign(m, key);
        r.verified = (calcSign == signIt->second);
        if (!r.verified) return r;
        std::string tradeState = get(m, "trade_state");
        r.paid = (tradeState == "SUCCESS");
        r.orderId = get(m, "out_trade_no");
        r.channelOrderNo = get(m, "transaction_id");
        r.buyerId = get(m, "openid");
        try { r.paidAmount = std::stod(get(m, "total_fee")) / 100.0; } catch (...) {}
        // QQ钱包回调响应也是XML
        r.responseText = "<xml><return_code>SUCCESS</return_code><return_msg>OK</return_msg></xml>";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["transaction_id"] = req.channelOrderNo;
        m["out_refund_no"] = req.refundNo;
        m["refund_fee"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        m["op_user_id"] = p.get("appurl", "").asString();
        m["op_user_passwd"] = p.get("appmchid", "").asString();
        m["sign"] = makeSign(m, p.get("appkey", "").asString());

        std::string xml = buildXml(m);
        // Refund requires SSL mutual auth - stub if no cert configured
        if (p.get("merchant_pem", "").asString().empty()) {
            r.errMsg = "QQ钱包退款需要SSL双向证书(merchant_pem/merchant_cert)";
            r.rawResponse = xml;
            return r;
        }
        std::map<std::string, std::string> xmlHeaders; xmlHeaders["Content-Type"] = "application/xml";
        auto resp = SyncHttp::postJson("https://qpay.qq.com/cgi-bin/pay/qpay_refund.cgi", xml, xmlHeaders);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        auto result = parseXml(resp.body);
        if (get(result, "result_code") == "SUCCESS") {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = get(result, "refund_id");
        } else {
            r.errMsg = "[" + get(result, "err_code") + "]" + get(result, "err_code_des");
        }
        return r;
    }

    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["out_trade_no"] = req.orderId;
        m["sign"] = makeSign(m, p.get("appkey", "").asString());
        std::string xml = buildXml(m);
        std::map<std::string, std::string> xmlHeaders2; xmlHeaders2["Content-Type"] = "application/xml";
        auto resp = SyncHttp::postJson("https://qpay.qq.com/cgi-bin/pay/qpay_close_order.cgi", xml, xmlHeaders2);
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        auto result = parseXml(resp.body);
        r.success = (get(result, "result_code") == "SUCCESS");
        if (!r.success) r.errMsg = get(result, "err_code_des");
        return r;
    }

private:
    // ── Native扫码支付 ──
    ChannelOrderResult nativePay(ChannelOrderResult &r, const ChannelOrderRequest &req) {
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["out_trade_no"] = req.orderId;
        m["body"] = req.subject.empty() ? "商品" : req.subject;
        m["fee_type"] = "CNY";
        m["notify_url"] = req.notifyUrl;
        m["spbill_create_ip"] = req.clientIp;
        m["total_fee"] = std::to_string((long long)std::llround(req.amount * 100.0));
        m["sign"] = makeSign(m, p.get("appkey", "").asString());

        std::string xml = buildXml(m);
        std::map<std::string, std::string> xmlHeaders3; xmlHeaders3["Content-Type"] = "application/xml";
        auto resp = SyncHttp::postJson("https://qpay.qq.com/cgi-bin/pay/qpay_unified_order.cgi", xml, xmlHeaders3);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        auto result = parseXml(resp.body);
        if (get(result, "result_code") == "SUCCESS") {
            r.success = true;
            r.payUrl = get(result, "code_url");
            r.channelOrderNo = get(result, "prepay_id");
        } else {
            r.errMsg = "[" + get(result, "err_code") + "]" + get(result, "err_code_des");
        }
        return r;
    }

    // ── JSAPI公众号支付 ──
    ChannelOrderResult jsapiPay(ChannelOrderResult &r, const ChannelOrderRequest &req) {
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["out_trade_no"] = req.orderId;
        m["body"] = req.subject.empty() ? "商品" : req.subject;
        m["fee_type"] = "CNY";
        m["notify_url"] = req.notifyUrl;
        m["spbill_create_ip"] = req.clientIp;
        m["total_fee"] = std::to_string((long long)std::llround(req.amount * 100.0));
        m["trade_type"] = "JSAPI";
        m["openid"] = p.get("openid", p.get("sub_openid", "").asString()).asString();
        m["sign"] = makeSign(m, p.get("appkey", "").asString());

        std::string xml = buildXml(m);
        std::map<std::string, std::string> xmlHeaders4; xmlHeaders4["Content-Type"] = "application/xml";
        auto resp = SyncHttp::postJson("https://qpay.qq.com/cgi-bin/pay/qpay_unified_order.cgi", xml, xmlHeaders4);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        auto result = parseXml(resp.body);
        if (get(result, "result_code") == "SUCCESS") {
            r.success = true;
            r.channelOrderNo = get(result, "prepay_id");
            // Build JSAPI params for QQ wallet
            Json::Value jsapi;
            jsapi["appId"] = p.get("sub_appid", "").asString();
            jsapi["timeStamp"] = std::to_string((long long)std::time(nullptr));
            jsapi["nonceStr"] = randomStr(16);
            jsapi["package"] = "prepay_id=" + get(result, "prepay_id");
            jsapi["signType"] = "MD5";
            // Sign JSAPI params
            std::map<std::string, std::string> jsMap;
            jsMap["appId"] = jsapi["appId"].asString();
            jsMap["timeStamp"] = jsapi["timeStamp"].asString();
            jsMap["nonceStr"] = jsapi["nonceStr"].asString();
            jsMap["package"] = jsMap["package"];
            jsMap["signType"] = "MD5";
            jsapi["paySign"] = makeSign(jsMap, p.get("appkey", "").asString());
            r.extra["jsapi"] = jsapi;
            r.payUrl = get(result, "code_url");
        } else {
            r.errMsg = "[" + get(result, "err_code") + "]" + get(result, "err_code_des");
        }
        return r;
    }

    // ── MD5签名: ksort, k=v& (skip sign/empty), append key=, MD5 uppercase ──
    static std::string makeSign(const std::map<std::string, std::string> &m, const std::string &key) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "sign" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        s += "&key=" + key;
        std::string md5 = Md5Utils::md5(s);
        std::transform(md5.begin(), md5.end(), md5.begin(), ::toupper);
        return md5;
    }

    // ── XML helpers ──
    static std::string buildXml(const std::map<std::string, std::string> &m) {
        std::string xml = "<xml>";
        for (auto &kv : m) {
            xml += "<" + kv.first + "><![CDATA[" + kv.second + "]]></" + kv.first + ">";
        }
        xml += "</xml>";
        return xml;
    }

    static std::map<std::string, std::string> parseXml(const std::string &xml) {
        std::map<std::string, std::string> m;
        // Simple XML parser for <key><![CDATA[value]]></key> or <key>value</key>
        size_t pos = 0;
        while (pos < xml.size()) {
            auto openStart = xml.find('<', pos);
            if (openStart == std::string::npos) break;
            auto openEnd = xml.find('>', openStart);
            if (openEnd == std::string::npos) break;
            std::string tag = xml.substr(openStart + 1, openEnd - openStart - 1);
            if (tag.empty() || tag[0] == '/' || tag[0] == '?') { pos = openEnd + 1; continue; }
            pos = openEnd + 1;
            // Look for closing tag
            std::string closeTag = "</" + tag + ">";
            auto closePos = xml.find(closeTag, pos);
            if (closePos == std::string::npos) continue;
            std::string value = xml.substr(pos, closePos - pos);
            // Strip CDATA
            if (value.find("<![CDATA[") == 0 && value.rfind("]]>") == value.size() - 3) {
                value = value.substr(9, value.size() - 12);
            }
            m[tag] = value;
            pos = closePos + closeTag.size();
        }
        return m;
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }

    static std::string randomStr(int len) {
        static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string s;
        unsigned char buf[32];
        RAND_bytes(buf, len < 32 ? len : 32);
        for (int i = 0; i < len; ++i) s += chars[buf[i % 32] % (sizeof(chars) - 1)];
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(QqpayPlugin);
