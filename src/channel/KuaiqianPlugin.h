#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/err.h>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class KuaiqianPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "kuaiqian"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "快钱账户号", "input");
        add("appkey", "商户证书密码", "input", "", "PFX私钥密码");
        add("appsecret", "SSL客户端证书密码", "input", "", "SSL双向证书密码(当面付/API网关需要)");
        add("appmchid", "服务商-快钱子账户号", "input", "", "仅服务商需要填写");
        add("merchant_id", "当面付-商户号", "input", "", "仅当面付需要");
        add("terminal_id", "当面付-终端号", "input", "", "仅当面付需要");
        add("own_channel", "是否自有渠道", "select", "0", "0=否 1=是");
        add("apptype", "支付方式", "select", "1", "1=H5支付 2=当面付");
        add("merchant_pem", "商户RSA私钥(PEM)", "textarea", "", "从PFX导出的PEM格式私钥");
        add("kuaiqian_pubkey", "快钱公钥(PEM)", "textarea", "", "cert.cer导出的PEM格式公钥");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("merchant_pem", "").asString().empty()) {
            r.errMsg = "快钱参数不完整(appid/merchant_pem)";
            return r;
        }
        int apptype = p.get("apptype", "1").asInt();
        if (apptype == 2) {
            // 当面付 - 需要PKCS7/CMS支持
            return stubQrcode(r, req);
        }
        // H5/网银支付
        std::string payType = getPayType(req.payType, apptype);
        bool isMobile = (payType != "10" && payType != "21"); // 非网银即移动端
        std::string apiurl = isMobile
            ? "https://www.99bill.com/mobilegateway/recvMerchantInfoAction.htm"
            : "https://www.99bill.com/gateway/recvMerchantInfoAction.htm";
        std::string version = isMobile ? "mobile1.0" : "v2.0";
        std::map<std::string, std::string> m;
        m["inputCharset"] = "1";
        m["pageUrl"] = req.returnUrl;
        m["bgUrl"] = req.notifyUrl;
        m["version"] = version;
        m["language"] = "1";
        m["signType"] = "4";
        m["merchantAcctId"] = p.get("appid", "").asString() + "01";
        m["orderId"] = req.orderId;
        m["orderAmount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        m["orderTime"] = now();
        m["productName"] = req.subject.empty() ? "商品" : req.subject;
        m["payType"] = payType;
        // aggregatePay for wx_jsapi
        if (req.payType == "wx_jsapi" || req.payType == "wx_lite") {
            std::string appId = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            std::string openId = p.get("openid", p.get("sub_openid", "").asString()).asString();
            if (!appId.empty() && !openId.empty()) {
                m["aggregatePay"] = "appId=" + appId + ",openId=" + openId + ",limitPay=0";
            }
        }
        // own_channel
        if (p.get("own_channel", "0").asInt() == 1) {
            m["extDataType"] = "NB2";
            m["extDataContent"] = "<NB2>{\"customAuthNetInfo\":{\"own_channel\":\"1\"}}</NB2>";
        }
        // Sign
        std::string signStr = buildSignString(m);
        m["signMsg"] = rsaSha256Sign(signStr, p.get("merchant_pem", "").asString());
        m["terminalIp"] = req.clientIp;
        // Build form HTML
        std::ostringstream html;
        html << "<form action=\"" << apiurl << "\" method=\"post\" id=\"dopay\">";
        for (auto &kv : m) html << "<input type=\"hidden\" name=\"" << kv.first << "\" value=\"" << kv.second << "\" />";
        html << "<input type=\"submit\" value=\"正在跳转\"></form><script>document.getElementById(\"dopay\").submit();</script>";
        r.success = true;
        r.payUrl = apiurl;
        r.rawResponse = html.str();
        r.extra["html"] = html.str();
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "<result>0</result>";
        // Check if this is 当面付 JSON notify or RMB gateway GET notify
        if (!body.empty() && body[0] == '{') {
            // 当面付 JSON notify - needs PKCS7 unseal
            r.verified = false;
            r.responseText = "<result>0</result>";
            return r;
        }
        // RMB gateway callback (GET params)
        std::string signMsg = get(params, "signMsg");
        if (signMsg.empty()) return r;
        // Build sign string in fixed field order
        static const std::vector<std::string> fieldOrder = {
            "merchantAcctId","version","language","signType","payType","bankId",
            "orderId","orderTime","orderAmount","bindCard","bindMobile","dealId",
            "bankDealId","dealTime","payAmount","fee","ext1","ext2","payResult",
            "aggregatePay","errCode","period"
        };
        std::string signStr;
        for (auto &k : fieldOrder) {
            auto it = params.find(k);
            if (it != params.end() && !it->second.empty()) {
                if (!signStr.empty()) signStr += "&";
                signStr += k + "=" + it->second;
            }
        }
        std::string pubKey = channelParams.get("kuaiqian_pubkey", "").asString();
        if (pubKey.empty()) { r.verified = false; return r; }
        r.verified = RsaUtils::verifySha256(signStr, signMsg, normalizePublicKey(pubKey));
        if (!r.verified) return r;
        std::string payResult = get(params, "payResult");
        r.paid = (payResult == "10");
        r.orderId = get(params, "orderId");
        r.channelOrderNo = get(params, "dealId");
        r.buyerId = get(params, "ext1");
        try { r.paidAmount = std::stod(get(params, "payAmount")) / 100.0; } catch (...) {}
        r.responseText = "<result>1</result><redirecturl>" + get(params, "pageUrl", "") + "</redirecturl>";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        // 退款走API网关(F0001)，需要PKCS7/CMS
        r.errMsg = "快钱退款走API网关(messageType=F0001)，需要PKCS7/CMS加签加密支持，当前未实现";
        return r;
    }

private:
    static ChannelOrderResult stubQrcode(ChannelOrderResult &r, const ChannelOrderRequest &req) {
        r.errMsg = "快钱当面付(messageType=A7007)需要PKCS7/CMS加签加密+SSL双向认证支持，当前未实现";
        Json::Value data;
        data["messageType"] = "A7007";
        data["orderId"] = req.orderId;
        data["amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        r.rawResponse = Json::writeString(wb, data);
        return r;
    }

    static std::string getPayType(const std::string &payType, int apptype) {
        if (payType == "wxpay" || payType == "wx_jsapi" || payType == "wx_lite") {
            if (apptype == 1) return "26-2"; // 微信H5
            return "26-1"; // 微信公众号
        }
        if (payType == "alipay" || payType == "ali_jsapi") {
            return "27-3"; // 支付宝H5
        }
        if (payType == "bank") return "10"; // 网银
        if (payType == "bank_jsapi") return "21"; // 快捷支付
        return "00"; // 默认
    }

    // Build sign string: iterate map, skip signMsg and empty values, concat k=v&
    static std::string buildSignString(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "signMsg" || kv.first == "terminalIp" || kv.first == "tdpformName" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    // RSA-SHA256 sign with PEM private key → Base64
    static std::string rsaSha256Sign(const std::string &data, const std::string &pem) {
        return RsaUtils::signSha256(data, normalizePrivateKey(pem));
    }

    static std::string normalizePrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN PRIVATE KEY-----\n" + b + "-----END PRIVATE KEY-----\n";
    }
    static std::string normalizePublicKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN PUBLIC KEY-----\n" + b + "-----END PUBLIC KEY-----\n";
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k, const std::string &dflt = "") {
        auto it = m.find(k);
        return it == m.end() ? dflt : it->second;
    }

    static std::string now() {
        auto t = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[16]; std::strftime(b, sizeof(b), "%Y%m%d%H%M%S", &tmv); return b;
    }
};

REGISTER_CHANNEL_PLUGIN(KuaiqianPlugin);
