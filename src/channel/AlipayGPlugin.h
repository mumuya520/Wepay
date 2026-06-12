#pragma once
#include "ChannelPlugin.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AlipayGPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipayg"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "应用Client ID", "input");
        add("appkey", "Antom公钥", "textarea", "", "用于回调/响应验签");
        add("appsecret", "应用私钥", "textarea", "", "RSA 私钥，兼容裸 PKCS1 私钥");
        add("appswitch", "网关区域", "select", "0", "0亚洲 1北美 2欧洲");
        add("currency_code", "结算货币", "input", "CNY", "CNY/HKD/EUR/USD 等");
        add("currency_rate", "货币汇率", "input", "1", "例如 1 元人民币兑换 0.137 USD，则填 0.137");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string clientId = p.get("appid", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();
        if (clientId.empty() || privateKey.empty()) {
            result.errMsg = "支付宝国际版参数不完整(appid/appsecret)";
            return result;
        }
        std::string currency = p.get("currency_code", "CNY").asString();
        double rate = 1.0;
        try { rate = std::stod(p.get("currency_rate", "1").asString()); } catch (...) {}
        long long amount = (long long)std::llround(req.amount * rate * 100.0);

        Json::Value body;
        body["env"]["terminalType"] = "WEB";
        body["env"]["osType"] = "";
        body["order"]["orderAmount"]["currency"] = currency;
        body["order"]["orderAmount"]["value"] = (Json::Int64)amount;
        body["order"]["referenceOrderId"] = req.orderId;
        body["order"]["orderDescription"] = req.subject.empty() ? "商品" : req.subject;
        body["paymentRequestId"] = req.orderId;
        body["paymentAmount"]["currency"] = currency;
        body["paymentAmount"]["value"] = (Json::Int64)amount;
        body["settlementStrategy"]["settlementCurrency"] = currency;
        body["paymentMethod"]["paymentMethodType"] = "ALIPAY_CN";
        body["paymentNotifyUrl"] = req.notifyUrl;
        body["paymentRedirectUrl"] = req.returnUrl;
        body["productCode"] = "CASHIER_PAYMENT";

        auto resp = execute(p, "/v1/payments/pay", body);
        result.rawResponse = resp.body;
        if (!resp.success) {
            result.errMsg = "支付宝国际版请求失败: " + resp.errMsg;
            return result;
        }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j) || !j.isMember("result")) {
            result.errMsg = "支付宝国际版响应解析失败";
            return result;
        }
        std::string status = j["result"].get("resultStatus", "").asString();
        std::string code = j["result"].get("resultCode", "").asString();
        if (!(status == "S" || (status == "U" && code == "PAYMENT_IN_PROCESS"))) {
            result.errMsg = "[" + code + "]" + j["result"].get("resultMessage", "支付宝国际版下单失败").asString();
            return result;
        }
        std::string payUrl = j.get("normalUrl", "").asString();
        if (payUrl.empty()) {
            result.errMsg = "支付宝国际版未返回 normalUrl";
            return result;
        }
        result.success = true;
        result.payUrl = payUrl;
        result.channelOrderNo = j.get("paymentId", "").asString();
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &rawBody,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult result;
        result.responseText = "{\"result\":{\"resultCode\":\"SUCCESS\",\"resultStatus\":\"S\",\"resultMessage\":\"success\"}}";
        Json::Value j;
        if (!Json::Reader().parse(rawBody, j)) {
            result.responseText = "{\"result\":{\"resultCode\":\"FAIL\",\"resultStatus\":\"F\",\"resultMessage\":\"data error\"}}";
            return result;
        }
        std::string signature = get(params, "signature");
        std::string requestTime = get(params, "request-time");
        std::string path = get(params, "request-uri");
        std::string pubKey = channelParams.get("appkey", "").asString();
        if (!signature.empty() && !requestTime.empty() && !path.empty() && !pubKey.empty()) {
            result.verified = verifySignature("POST", path, requestTime, rawBody, signature, pubKey);
        } else {
            result.verified = true;
        }
        if (!result.verified) {
            result.responseText = "{\"result\":{\"resultCode\":\"FAIL\",\"resultStatus\":\"F\",\"resultMessage\":\"sign error\"}}";
            return result;
        }
        result.paid = (j["result"].get("resultStatus", "").asString() == "S");
        result.orderId = j.get("paymentRequestId", "").asString();
        result.channelOrderNo = j.get("paymentId", "").asString();
        result.buyerId = j["pspCustomerInfo"].get("pspCustomerId", "").asString();
        try { result.paidAmount = std::stod(j["paymentAmount"].get("value", "0").asString()) / 100.0; } catch (...) {}
        return result;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult result;
        auto &p = req.channelParams;
        std::string currency = p.get("currency_code", "CNY").asString();
        double rate = 1.0;
        try { rate = std::stod(p.get("currency_rate", "1").asString()); } catch (...) {}
        long long amount = (long long)std::llround(req.refundAmount * rate * 100.0);
        Json::Value body;
        body["refundRequestId"] = req.refundNo;
        body["paymentId"] = req.channelOrderNo;
        body["refundAmount"]["currency"] = currency;
        body["refundAmount"]["value"] = (Json::Int64)amount;
        auto resp = execute(p, "/v1/payments/refund", body);
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j) || !j.isMember("result")) {
            result.errMsg = "支付宝国际版退款响应解析失败";
            return result;
        }
        std::string status = j["result"].get("resultStatus", "").asString();
        if (status == "S" || status == "U") {
            result.success = true;
            result.state = status == "S" ? 1 : 0;
            result.channelRefundNo = j.get("refundId", "").asString();
        } else {
            result.errMsg = "[" + j["result"].get("resultCode", "").asString() + "]" + j["result"].get("resultMessage", "退款失败").asString();
        }
        return result;
    }

    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult result;
        Json::Value body;
        body["paymentRequestId"] = req.orderId;
        auto resp = execute(req.channelParams, "/v1/payments/cancel", body);
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        result.success = true;
        return result;
    }

private:
    static SyncHttp::Response execute(const Json::Value &p, const std::string &apiPath, const Json::Value &body) {
        std::string host = gateway(p.get("appswitch", "0").asString());
        std::string path = isSandbox(p.get("appid", "").asString()) ? "/ams/sandbox/api" + apiPath : "/ams/api" + apiPath;
        std::string url = host + path;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);
        std::string reqTime = std::to_string(nowMillis());
        std::string clientId = p.get("appid", "").asString();
        std::string signContent = "POST " + path + "\n" + clientId + "." + reqTime + "." + bodyStr;
        std::string sign = RsaUtils::signSha256(signContent, normalizeRsaPrivateKey(p.get("appsecret", "").asString()));
        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=UTF-8";
        headers["User-Agent"] = "global-alipay-sdk-cpp";
        headers["Request-Time"] = reqTime;
        headers["client-id"] = clientId;
        headers["Signature"] = "algorithm=RSA256,keyVersion=1,signature=" + urlEncode(sign);
        return SyncHttp::postJson(url, bodyStr, headers);
    }

    static bool verifySignature(const std::string &method, const std::string &path, const std::string &time,
                                const std::string &body, std::string signature, const std::string &publicKey) {
        auto pos = signature.rfind("signature=");
        if (pos != std::string::npos) signature = signature.substr(pos + 10);
        std::string content = method + " " + path + "\n." + time + "." + body;
        return RsaUtils::verifySha256(content, signature, publicKey);
    }

    static std::string gateway(const std::string &code) {
        if (code == "2") return "https://open-de-global.alipay.com";
        if (code == "1") return "https://open-na-global.alipay.com";
        return "https://open-sea-global.alipay.com";
    }

    static bool isSandbox(const std::string &clientId) {
        return clientId.rfind("SANDBOX_", 0) == 0;
    }

    static long long nowMillis() {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    static std::string normalizeRsaPrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string body;
        for (size_t i = 0; i < key.size(); i += 64) body += key.substr(i, 64) + "\n";
        return "-----BEGIN RSA PRIVATE KEY-----\n" + body + "-----END RSA PRIVATE KEY-----\n";
    }

    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(AlipayGPlugin);
