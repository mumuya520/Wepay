#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <openssl/hmac.h>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

// Stripe - Bearer Token认证, form-urlencoded POST, Webhook HMAC-SHA256验签
// API: https://api.stripe.com
// 认证: Authorization: Bearer sk_live_xxx
// 下单模式: 0=Checkout Session(收银台), 1=PaymentIntent(直接支付)
// Webhook验签: Stripe-Signature header, t=timestamp,v1=HMAC-SHA256(timestamp.payload, whsec_xxx)

class StripePlugin : public ChannelPlugin {
public:
    std::string name() const override { return "stripe"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "API密钥", "textarea", "", "sk_live_开头的密钥");
        add("appkey", "Webhook密钥", "textarea", "", "whsec_开头的密钥");
        add("appswitch", "支付模式", "select", "0", "0=跳转收银台 1=直接支付(仅限支付宝/微信)");
        add("currency_code", "结算货币", "select", "CNY", "CNY/HKD/EUR/USD/AUD/CAD/GBP/JPY/SGD等");
        add("currency_rate", "货币汇率", "input", "1", "例如1元人民币兑换0.137USD则填0.137");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        std::string apiKey = p.get("appid", "").asString();
        if (apiKey.empty()) { r.errMsg = "Stripe参数不完整(appid)"; return r; }

        double rate = 1.0;
        try { rate = std::stod(p.get("currency_rate", "1").asString()); } catch (...) {}
        std::string currency = p.get("currency_code", "CNY").asString();
        std::transform(currency.begin(), currency.end(), currency.begin(), ::tolower);
        long long amount = (long long)std::llround(req.amount * rate * 100.0);

        int mode = p.get("appswitch", "0").asInt();
        std::string paymentMethod = getPaymentMethod(req.payType);

        if (mode == 1 && !paymentMethod.empty()) {
            // PaymentIntent 直接支付
            return paymentIntent(r, req, apiKey, amount, currency, paymentMethod);
        }
        // Checkout Session 收银台
        return checkoutSession(r, req, apiKey, amount, currency, paymentMethod);
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        // Stripe webhook: JSON body + Stripe-Signature header
        std::string webhookSecret = channelParams.get("appkey", "").asString();
        std::string sigHeader;
        auto sigIt = params.find("Stripe-Signature");
        if (sigIt != params.end()) sigHeader = sigIt->second;
        if (sigHeader.empty()) { auto it2 = params.find("stripe-signature"); if (it2 != params.end()) sigHeader = it2->second; }

        if (!webhookSecret.empty() && !sigHeader.empty()) {
            r.verified = verifyWebhookSignature(body, sigHeader, webhookSecret);
        } else {
            r.verified = true; // No webhook secret configured, skip verification
        }
        if (!r.verified) return r;

        Json::Value data;
        if (!Json::Reader().parse(body, data)) { r.verified = false; return r; }

        std::string eventType = data.get("type", "").asString();
        bool paid = false;
        std::string orderId, channelOrderNo;

        if (eventType == "checkout.session.completed") {
            auto obj = data["data"]["object"];
            if (obj.get("payment_status", "").asString() == "paid") {
                paid = true;
                orderId = obj.get("client_reference_id", "").asString();
                channelOrderNo = obj.get("payment_intent", "").asString();
            }
        } else if (eventType == "checkout.session.async_payment_succeeded") {
            auto obj = data["data"]["object"];
            paid = true;
            orderId = obj.get("client_reference_id", "").asString();
            channelOrderNo = obj.get("payment_intent", "").asString();
        } else if (eventType == "payment_intent.succeeded") {
            auto obj = data["data"]["object"];
            paid = true;
            orderId = obj.get("metadata", Json::Value()).get("order_id", "").asString();
            channelOrderNo = obj.get("id", "").asString();
        }

        r.paid = paid;
        if (!orderId.empty()) r.orderId = orderId;
        if (!channelOrderNo.empty()) r.channelOrderNo = channelOrderNo;
        r.responseText = "success";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string apiKey = p.get("appid", "").asString();
        double rate = 1.0;
        try { rate = std::stod(p.get("currency_rate", "1").asString()); } catch (...) {}
        long long amount = (long long)std::llround(req.refundAmount * rate * 100.0);

        std::map<std::string, std::string> formParams;
        formParams["payment_intent"] = req.channelOrderNo;
        formParams["amount"] = std::to_string(amount);

        std::string formBody = buildFormBody(formParams);
        auto resp = stripePost(apiKey, "https://api.stripe.com/v1/refunds", formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.isMember("error")) {
            r.errMsg = result["error"].get("message", "退款失败").asString();
            return r;
        }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = result.get("id", "").asString();
        return r;
    }

private:
    // ── Checkout Session 收银台模式 ──
    ChannelOrderResult checkoutSession(ChannelOrderResult &r, const ChannelOrderRequest &req,
                                        const std::string &apiKey, long long amount,
                                        const std::string &currency, const std::string &paymentMethod) {
        std::map<std::string, std::string> formParams;
        formParams["success_url"] = req.returnUrl;
        formParams["cancel_url"] = req.returnUrl;
        formParams["client_reference_id"] = req.orderId;
        formParams["line_items[0][price_data][currency]"] = currency;
        formParams["line_items[0][price_data][product_data][name]"] = req.subject.empty() ? "Product" : req.subject;
        formParams["line_items[0][price_data][unit_amount]"] = std::to_string(amount);
        formParams["line_items[0][quantity]"] = "1";
        formParams["mode"] = "payment";
        if (!paymentMethod.empty()) {
            formParams["payment_method_types[0]"] = paymentMethod;
            if (paymentMethod == "wechat_pay") {
                formParams["payment_method_options[wechat_pay][client]"] = "web";
            }
        }

        std::string formBody = buildFormBody(formParams);
        auto resp = stripePost(apiKey, "https://api.stripe.com/v1/checkout/sessions", formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.isMember("error")) {
            r.errMsg = result["error"].get("message", "下单失败").asString();
            return r;
        }
        r.success = true;
        r.payUrl = result.get("url", "").asString();
        r.channelOrderNo = result.get("id", "").asString();
        return r;
    }

    // ── PaymentIntent 直接支付模式 ──
    ChannelOrderResult paymentIntent(ChannelOrderResult &r, const ChannelOrderRequest &req,
                                      const std::string &apiKey, long long amount,
                                      const std::string &currency, const std::string &paymentMethod) {
        // Step 1: Create PaymentMethod
        std::map<std::string, std::string> pmParams;
        pmParams["type"] = paymentMethod;
        std::string pmBody = buildFormBody(pmParams);
        auto pmResp = stripePost(apiKey, "https://api.stripe.com/v1/payment_methods", pmBody);
        if (!pmResp.success) { r.errMsg = pmResp.errMsg; return r; }

        Json::Value pmResult;
        if (!Json::Reader().parse(pmResp.body, pmResult) || pmResult.isMember("error")) {
            r.errMsg = "创建支付方式失败:" + (pmResult.isMember("error") ? pmResult["error"].get("message","").asString() : "解析失败");
            return r;
        }
        std::string pmId = pmResult.get("id", "").asString();

        // Step 2: Create PaymentIntent
        std::map<std::string, std::string> piParams;
        piParams["amount"] = std::to_string(amount);
        piParams["currency"] = currency;
        piParams["confirm"] = "true";
        piParams["payment_method"] = pmId;
        piParams["payment_method_types[0]"] = paymentMethod;
        piParams["description"] = req.subject.empty() ? "Product" : req.subject;
        piParams["metadata[order_id]"] = req.orderId;
        piParams["return_url"] = req.returnUrl;
        if (paymentMethod == "wechat_pay") {
            piParams["payment_method_options[wechat_pay][client]"] = "web";
        }

        std::string piBody = buildFormBody(piParams);
        auto piResp = stripePost(apiKey, "https://api.stripe.com/v1/payment_intents", piBody);
        r.rawResponse = piResp.body;
        if (!piResp.success) { r.errMsg = piResp.errMsg; return r; }

        Json::Value piResult;
        if (!Json::Reader().parse(piResp.body, piResult) || piResult.isMember("error")) {
            r.errMsg = "下单失败:" + (piResult.isMember("error") ? piResult["error"].get("message","").asString() : "解析失败");
            return r;
        }
        r.success = true;
        r.channelOrderNo = piResult.get("id", "").asString();

        // Extract redirect URL from next_action
        if (piResult.isMember("next_action")) {
            auto na = piResult["next_action"];
            if (paymentMethod == "alipay" && na.isMember("alipay_handle_redirect")) {
                r.payUrl = na["alipay_handle_redirect"].get("url", "").asString();
            } else if (paymentMethod == "wechat_pay" && na.isMember("wechat_pay_display_qr_code")) {
                r.payUrl = na["wechat_pay_display_qr_code"].get("data", "").asString();
            } else if (na.isMember("redirect_to_url")) {
                r.payUrl = na["redirect_to_url"].get("url", "").asString();
            }
        }
        return r;
    }

    // ── Stripe API POST (Bearer Token + form-urlencoded) ──
    static SyncHttp::Response stripePost(const std::string &apiKey, const std::string &url, const std::string &formBody) {
        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + apiKey;
        headers["Content-Type"] = "application/x-www-form-urlencoded";
        return SyncHttp::postJson(url, formBody, headers);
    }

    // ── Webhook HMAC-SHA256 验签 ──
    // Stripe-Signature: t=timestamp,v1=signature[,v1=signature...]
    static bool verifyWebhookSignature(const std::string &payload, const std::string &sigHeader, const std::string &secret) {
        // Parse timestamp
        long long timestamp = 0;
        std::vector<std::string> v1Sigs;
        // Split by comma
        std::istringstream iss(sigHeader);
        std::string item;
        while (std::getline(iss, item, ',')) {
            auto eqPos = item.find('=');
            if (eqPos == std::string::npos) continue;
            std::string key = item.substr(0, eqPos);
            std::string val = item.substr(eqPos + 1);
            // Trim
            size_t start = key.find_first_not_of(" \t");
            size_t end = key.find_last_not_of(" \t");
            if (start != std::string::npos) key = key.substr(start, end - start + 1);
            if (key == "t") {
                try { timestamp = std::stoll(val); } catch (...) {}
            } else if (key == "v1") {
                v1Sigs.push_back(val);
            }
        }
        if (timestamp == 0 || v1Sigs.empty()) return false;

        // Check tolerance (300s)
        long long now = (long long)std::time(nullptr);
        if (std::abs(now - timestamp) > 300) return false;

        // Compute expected signature: HMAC-SHA256(timestamp.payload, secret)
        std::string signedPayload = std::to_string(timestamp) + "." + payload;
        std::string expected = hmacSha256(signedPayload, secret);

        for (auto &sig : v1Sigs) {
            if (sig == expected) return true;
        }
        return false;
    }

    static std::string hmacSha256(const std::string &data, const std::string &key) {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), key.c_str(), (int)key.size(),
             (const unsigned char*)data.c_str(), data.size(), result, &len);
        // Return hex string
        std::string hex;
        const char *digits = "0123456789abcdef";
        for (unsigned int i = 0; i < len; i++) {
            hex += digits[result[i] >> 4];
            hex += digits[result[i] & 0x0f];
        }
        return hex;
    }

    static std::string getPaymentMethod(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "wechat_pay";
        if (payType.find("alipay") != std::string::npos) return "alipay";
        if (payType.find("paypal") != std::string::npos) return "paypal";
        return ""; // bank/card → empty = all methods in checkout
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(StripePlugin);
