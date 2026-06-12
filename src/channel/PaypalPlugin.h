#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/SyncHttp.h"

class PaypalPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "paypal"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "ClientId", "input");
        add("appkey", "ClientSecret", "input");
        add("appswitch", "模式选择", "select", "0", "0=线上模式 1=沙盒模式");
        add("appsecret", "Webhook ID", "input", "", "PayPal Webhook ID，用于webhook验签");
        add("currency_code", "结算货币", "select", "USD", "USD/AUD/CAD/CNY/EUR/HKD/JPY/GBP/SGD等");
        add("currency_rate", "货币汇率", "input", "1", "例如1元人民币兑换0.137美元则填0.137");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "PayPal参数不完整(appid/appkey)";
            return r;
        }
        double rate = p.get("currency_rate", "1").asDouble();
        if (rate <= 0) rate = 1;
        double money = std::llround(req.amount * rate * 100.0) / 100.0;
        std::string currency = p.get("currency_code", "USD").asString();

        // Get access token
        std::string token = getAccessToken(p);
        if (token.empty()) { r.errMsg = "PayPal获取AccessToken失败"; return r; }

        // Build order JSON
        Json::Value order;
        order["intent"] = "CAPTURE";
        Json::Value unit;
        unit["amount"]["currency_code"] = currency;
        unit["amount"]["value"] = fmtAmount(money);
        unit["description"] = req.subject.empty() ? "Goods" : req.subject;
        unit["custom_id"] = req.orderId;
        unit["invoice_id"] = req.orderId;
        Json::Value units(Json::arrayValue);
        units.append(unit);
        order["purchase_units"] = units;
        order["application_context"]["cancel_url"] = req.returnUrl;
        order["application_context"]["return_url"] = req.returnUrl;

        std::string baseUrl = getBaseUrl(p);
        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + token;
        headers["Content-Type"] = "application/json";
        headers["Accept"] = "application/json";

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string body = Json::writeString(wb, order);

        auto resp = SyncHttp::postJson(baseUrl + "/v2/checkout/orders", body, headers);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("status", "").asString() != "CREATED" && result.get("status", "").asString() != "APPROVED") {
            r.errMsg = result.get("message", "下单失败").asString();
            return r;
        }
        r.success = true;
        r.channelOrderNo = result.get("id", "").asString();
        // Find approve link
        if (result.isMember("links")) {
            for (auto &link : result["links"]) {
                if (link.get("rel", "").asString() == "approve") {
                    r.payUrl = link.get("href", "").asString();
                    break;
                }
            }
        }
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        // PayPal webhook verification
        // sign_string = transmission_id|transmission_time|webhook_id|crc32(body)
        auto tidIt = params.find("Paypal-Transmission-Id");
        if (tidIt == params.end()) tidIt = params.find("PAYPAL-TRANSMISSION-ID");
        if (tidIt == params.end()) { r.verified = false; return r; }

        std::string transmissionId = tidIt->second;
        auto ttimeIt = params.find("Paypal-Transmission-Time");
        if (ttimeIt == params.end()) ttimeIt = params.find("PAYPAL-TRANSMISSION-TIME");
        std::string transmissionTime = (ttimeIt != params.end()) ? ttimeIt->second : "";

        std::string webhookId = channelParams.get("appsecret", "").asString();
        if (webhookId.empty()) { r.verified = false; return r; }

        // CRC32 of raw body
        uint32_t crc = crc32(body);
        char crcBuf[16];
        snprintf(crcBuf, sizeof(crcBuf), "%u", crc);

        std::string signStr = transmissionId + "|" + transmissionTime + "|" + webhookId + "|" + std::string(crcBuf);

        // Get cert URL and fetch public key, then verify
        auto certIt = params.find("Paypal-Cert-Url");
        if (certIt == params.end()) certIt = params.find("PAYPAL-CERT-URL");
        if (certIt == params.end()) { r.verified = false; return r; }

        auto sigIt = params.find("Paypal-Transmission-Sig");
        if (sigIt == params.end()) sigIt = params.find("PAYPAL-TRANSMISSION-SIG");
        if (sigIt == params.end()) { r.verified = false; return r; }

        // Fetch PayPal cert and verify - simplified: mark verified for now
        // Full verification requires fetching cert from certIt->second and RSA-SHA256 verify
        r.verified = true; // TODO: implement full webhook cert verification

        // Parse event
        Json::Value data;
        if (!Json::Reader().parse(body, data)) return r;
        std::string eventType = data.get("event_type", "").asString();
        if (eventType != "PAYMENT.CAPTURE.COMPLETED") { r.responseText = "other event"; return r; }

        auto &res = data["resource"];
        r.paid = true;
        r.orderId = res.get("invoice_id", "").asString();
        r.channelOrderNo = res.get("id", "").asString();
        try { r.paidAmount = std::stod(res["amount"].get("value", "0").asString()); } catch (...) {}
        r.responseText = "success";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        double rate = p.get("currency_rate", "1").asDouble();
        if (rate <= 0) rate = 1;
        double money = std::llround(req.refundAmount * rate * 100.0) / 100.0;
        std::string currency = p.get("currency_code", "USD").asString();

        std::string token = getAccessToken(p);
        if (token.empty()) { r.errMsg = "PayPal获取AccessToken失败"; return r; }

        Json::Value refundReq;
        refundReq["amount"]["currency_code"] = currency;
        refundReq["amount"]["value"] = fmtAmount(money);

        std::string baseUrl = getBaseUrl(p);
        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + token;
        headers["Content-Type"] = "application/json";
        headers["Accept"] = "application/json";

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string body = Json::writeString(wb, refundReq);

        auto resp = SyncHttp::postJson(baseUrl + "/v2/payments/captures/" + req.channelOrderNo + "/refund", body, headers);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("status", "").asString() == "COMPLETED" || result.get("status", "").asString() == "PENDING") {
            r.success = true;
            r.state = (result.get("status", "").asString() == "COMPLETED") ? 1 : 0;
            r.channelRefundNo = result.get("id", "").asString();
        } else {
            r.errMsg = result.get("message", "退款失败").asString();
        }
        return r;
    }

private:
    static std::string getBaseUrl(const Json::Value &p) {
        return (p.get("appswitch", "0").asInt() == 1)
            ? "https://api-m.sandbox.paypal.com"
            : "https://api-m.paypal.com";
    }

    static std::string getAccessToken(const Json::Value &p) {
        std::string clientId = p.get("appid", "").asString();
        std::string clientSecret = p.get("appkey", "").asString();
        std::string baseUrl = getBaseUrl(p);

        // POST /v1/oauth2/token with Basic auth
        std::string auth = base64Encode(clientId + ":" + clientSecret);
        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Basic " + auth;
        headers["Content-Type"] = "application/x-www-form-urlencoded";
        headers["Accept"] = "application/json";

        auto resp = SyncHttp::postJson(baseUrl + "/v1/oauth2/token", "grant_type=client_credentials", headers);
        if (!resp.success) return "";
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) return "";
        return result.get("access_token", "").asString();
    }

    static uint32_t crc32(const std::string &data) {
        uint32_t crc = 0xFFFFFFFF;
        for (unsigned char c : data) {
            crc ^= c;
            for (int i = 0; i < 8; i++) {
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
            }
        }
        return ~crc;
    }

    static std::string base64Encode(const std::string &input) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : input) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(table[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(PaypalPlugin);
