#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 威富通RSA - RSA_1_256签名, XML POST/响应
// 网关: https://pay.swiftpass.cn/pay/gateway (可自定义)
// 签名: ksort拼接k=v&(skip sign/空值/trim空), 去尾&, RSA-SHA256签名→Base64
// 验签: 同样拼接, 平台公钥RSA-SHA256验签
// 请求: mch_id/version/sign_type/nonce_str/service/.../sign → XML POST
// 响应: XML, status=0, result_code=0

class SwiftpassPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "swiftpass"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户号", "input");
        add("appkey", "平台RSA公钥", "textarea", "", "RSA公钥(Base64编码)");
        add("appsecret", "商户RSA私钥", "textarea", "", "RSA私钥(Base64编码)");
        add("appurl", "自定义网关URL", "input", "", "可不填,默认https://pay.swiftpass.cn/pay/gateway");
        add("appswitch", "微信是否支持H5", "select", "0", "0=否 1=是");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=JSAPI/公众号 3=H5");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appsecret", "").asString().empty()) {
            r.errMsg = "威富通参数不完整(appid/appsecret)";
            return r;
        }
        int apptype = p.get("apptype", "1").asInt();
        std::string service = getService(req.payType, apptype);

        std::map<std::string, std::string> m;
        m["service"] = service;
        m["body"] = req.subject.empty() ? "商品" : req.subject;
        m["total_fee"] = std::to_string((long long)std::llround(req.amount * 100.0));
        m["mch_create_ip"] = req.clientIp;
        m["out_trade_no"] = req.orderId;
        m["notify_url"] = req.notifyUrl;

        // JSAPI extend
        if (service == "pay.weixin.jspay") {
            std::string subAppid = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
            bool isMini = req.payType.find("lite") != std::string::npos || req.payType.find("mini") != std::string::npos;
            m["is_raw"] = "1";
            m["is_minipg"] = isMini ? "1" : "0";
            m["sub_appid"] = subAppid;
            m["sub_openid"] = openid;
            m["device_info"] = "AND_WAP";
        }

        // H5 extend
        if (service == "pay.weixin.wappay") {
            m["device_info"] = "AND_WAP";
            m["callback_url"] = req.returnUrl;
        }

        auto result = requestApi(p, m);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        if (service == "pay.weixin.jspay") {
            r.payUrl = get(result.data, "pay_info"); // JSAPI payInfo JSON
        } else if (service == "pay.weixin.wappay") {
            r.payUrl = get(result.data, "pay_info"); // H5 pay_info is URL
        } else {
            r.payUrl = get(result.data, "code_url"); // Native QR code URL
        }
        r.channelOrderNo = get(result.data, "transaction_id");
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "failure";
        // Swiftpass notify is XML body
        auto m = parseXml(body);
        std::string pubKey = channelParams.get("appkey", "").asString();
        auto signIt = m.find("sign");
        if (signIt == m.end() || signIt->second.empty()) { r.verified = false; return r; }

        std::string signContent = buildSignContent(m);
        r.verified = RsaUtils::verifySha256(signContent, signIt->second, normalizePublicKey(pubKey));
        if (!r.verified) return r;

        std::string status = get(m, "status");
        std::string resultCode = get(m, "result_code");
        r.paid = (status == "0" && resultCode == "0");
        r.orderId = get(m, "out_trade_no");
        r.channelOrderNo = get(m, "transaction_id");
        r.buyerId = get(m, "openid");
        try { r.paidAmount = std::stod(get(m, "total_fee")) / 100.0; } catch (...) {}
        r.responseText = r.paid ? "success" : "failure";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["service"] = "unified.trade.refund";
        m["transaction_id"] = req.channelOrderNo;
        m["out_refund_no"] = req.refundNo;
        m["total_fee"] = std::to_string((long long)std::llround(req.paidAmount * 100.0));
        m["refund_fee"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        m["op_user_id"] = p.get("appid", "").asString();

        auto result = requestApi(p, m);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = get(result.data, "refund_id");
        return r;
    }

private:
    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        std::map<std::string, std::string> data;
    };

    static ApiResponse requestApi(const Json::Value &p, const std::map<std::string, std::string> &bizParams) {
        ApiResponse ar;
        std::string mchId = p.get("appid", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();
        std::string pubKey = p.get("appkey", "").asString();
        std::string gatewayUrl = p.get("appurl", "").asString();
        if (gatewayUrl.empty()) gatewayUrl = "https://pay.swiftpass.cn/pay/gateway";

        // Build full params with public params
        std::map<std::string, std::string> m;
        m["mch_id"] = mchId;
        m["version"] = "2.0";
        m["sign_type"] = "RSA_1_256";
        m["nonce_str"] = randomStr(32);
        for (auto &kv : bizParams) m[kv.first] = kv.second;

        // Sign
        std::string signContent = buildSignContent(m);
        m["sign"] = RsaUtils::signSha256(signContent, normalizePrivateKey(privateKey));

        // Build XML and POST
        std::string xml = buildXml(m);
        std::map<std::string, std::string> xmlHeaders;
        xmlHeaders["Content-Type"] = "application/xml";
        auto resp = SyncHttp::postJson(gatewayUrl, xml, xmlHeaders);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        auto result = parseXml(resp.body);
        std::string status = get(result, "status");
        if (status != "0") {
            ar.errMsg = get(result, "message", "请求失败");
            return ar;
        }
        // Verify response sign
        auto signIt = result.find("sign");
        if (signIt != result.end() && !signIt->second.empty()) {
            std::string respSignContent = buildSignContent(result);
            if (!RsaUtils::verifySha256(respSignContent, signIt->second, normalizePublicKey(pubKey))) {
                ar.errMsg = "返回数据签名校验失败";
                return ar;
            }
        }
        std::string resultCode = get(result, "result_code");
        if (resultCode != "0") {
            ar.errMsg = "[" + get(result, "err_code") + "]" + get(result, "err_msg");
            return ar;
        }
        ar.success = true;
        ar.data = result;
        return ar;
    }

    // ksort, skip sign and empty/trim-empty values, concat k=v&, remove last &
    static std::string buildSignContent(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "sign") continue;
            std::string v = kv.second;
            // trim
            size_t start = v.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue; // empty after trim
            v = v.substr(start, v.find_last_not_of(" \t\r\n") - start + 1);
            if (v.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + v;
        }
        return s;
    }

    static std::string getService(const std::string &payType, int apptype) {
        if (payType.find("wx") != std::string::npos) {
            if (apptype == 2) return "pay.weixin.jspay";
            if (apptype == 3) return "pay.weixin.wappay";
            return "pay.weixin.native";
        }
        if (payType.find("qq") != std::string::npos) return "pay.tenpay.native";
        if (payType.find("jd") != std::string::npos) return "pay.jdpay.native";
        if (payType == "bank" || payType == "bank_jsapi") return "pay.unionpay.native";
        return "pay.alipay.native";
    }

    static std::string buildXml(const std::map<std::string, std::string> &m) {
        std::string xml = "<xml>";
        for (auto &kv : m) {
            // Numeric values: no CDATA; string values: CDATA
            bool isNumeric = !kv.second.empty() && kv.second.find_first_not_of("0123456789") == std::string::npos;
            if (isNumeric) {
                xml += "<" + kv.first + ">" + kv.second + "</" + kv.first + ">";
            } else {
                xml += "<" + kv.first + "><![CDATA[" + kv.second + "]]></" + kv.first + ">";
            }
        }
        xml += "</xml>";
        return xml;
    }

    static std::map<std::string, std::string> parseXml(const std::string &xml) {
        std::map<std::string, std::string> m;
        size_t pos = 0;
        while (pos < xml.size()) {
            auto openStart = xml.find('<', pos);
            if (openStart == std::string::npos) break;
            auto openEnd = xml.find('>', openStart);
            if (openEnd == std::string::npos) break;
            std::string tag = xml.substr(openStart + 1, openEnd - openStart - 1);
            if (tag.empty() || tag[0] == '/' || tag[0] == '?') { pos = openEnd + 1; continue; }
            pos = openEnd + 1;
            std::string closeTag = "</" + tag + ">";
            auto closePos = xml.find(closeTag, pos);
            if (closePos == std::string::npos) continue;
            std::string value = xml.substr(pos, closePos - pos);
            if (value.find("<![CDATA[") == 0 && value.rfind("]]>") == value.size() - 3) {
                value = value.substr(9, value.size() - 12);
            }
            m[tag] = value;
            pos = closePos + closeTag.size();
        }
        return m;
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k, const std::string &dflt = "") {
        auto it = m.find(k);
        return it == m.end() ? dflt : it->second;
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

    static std::string randomStr(int len) {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::string s;
        unsigned char buf[32];
        RAND_bytes(buf, len < 32 ? len : 32);
        for (int i = 0; i < len; ++i) s += chars[buf[i % 32] % (sizeof(chars) - 1)];
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(SwiftpassPlugin);
