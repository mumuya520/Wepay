#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class LakalaPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "lakala"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "APPID", "input");
        add("appmchid", "商户号", "input");
        add("appkey", "终端号", "input");
        add("appselect", "接口类型", "select", "0", "0=聚合扫码 1=聚合收银台");
        add("appswitch", "环境选择", "select", "0", "0=生产环境 1=测试环境");
        add("serial_no", "商户证书序列号", "input", "", "从api_cert.cer提取的序列号");
        add("merchant_pem", "商户RSA私钥(PEM)", "textarea", "", "api_private_key.pem内容");
        add("lakala_pubkey", "拉卡拉平台公钥(PEM)", "textarea", "", "lkl-apigw-v1.cer导出的PEM公钥");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("merchant_pem", "").asString().empty()) {
            r.errMsg = "拉卡拉参数不完整(appid/merchant_pem)";
            return r;
        }
        int appselect = p.get("appselect", "0").asInt();
        if (appselect == 1) {
            return createCashier(r, req);
        }
        return createPreorder(r, req);
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        // Verify Authorization header signature
        auto authIt = params.find("Authorization");
        if (authIt == params.end()) authIt = params.find("authorization");
        if (authIt == params.end()) { r.verified = false; return r; }

        std::string pubKey = channelParams.get("lakala_pubkey", "").asString();
        if (pubKey.empty()) { r.verified = false; return r; }

        // Parse Authorization: LKLAPI-SHA256withRSA appid="xxx",serial_no="xxx",timestamp="xxx",nonce_str="xxx",signature="xxx"
        std::string auth = authIt->second;
        std::string prefix = "LKLAPI-SHA256withRSA ";
        auto pos = auth.find(prefix);
        if (pos == std::string::npos) { r.verified = false; return r; }
        std::string kvStr = auth.substr(pos + prefix.size());

        auto fields = parseAuthFields(kvStr);
        std::string timestamp = fields["timestamp"];
        std::string nonceStr = fields["nonce_str"];
        std::string signature = fields["signature"];

        // Verify message: timestamp\nnonce_str\nbody\n
        std::string message = timestamp + "\n" + nonceStr + "\n" + body + "\n";
        r.verified = RsaUtils::verifySha256(message, signature, normalizePublicKey(pubKey));
        if (!r.verified) return r;

        // Parse body
        Json::Value data;
        if (Json::Reader().parse(body, data)) {
            std::string tradeStatus = data.get("trade_status", "").asString();
            std::string orderStatus = data.get("order_status", "").asString();
            r.paid = (tradeStatus == "SUCCESS" || orderStatus == "2");
            // Preorder notify fields
            r.orderId = data.get("out_trade_no", data.get("out_order_no", "")).asString();
            r.channelOrderNo = data.get("trade_no", "").asString();
            if (data.isMember("order_trade_info")) {
                r.channelOrderNo = data["order_trade_info"].get("trade_no", r.channelOrderNo).asString();
                r.buyerId = data["order_trade_info"].get("user_id2", "").asString();
            }
            r.buyerId = data.get("user_id2", r.buyerId).asString();
            try { r.paidAmount = std::stod(data.get("total_amount", "0").asString()) / 100.0; } catch (...) {}
        }
        r.responseText = r.paid ? "success" : "fail";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        Json::Value reqData;
        reqData["merchant_no"] = p.get("appmchid", "").asString();
        reqData["term_no"] = p.get("appkey", "").asString();
        reqData["out_trade_no"] = req.refundNo;
        reqData["refund_amount"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        reqData["origin_out_trade_no"] = req.orderId;
        reqData["origin_trade_no"] = req.channelOrderNo;

        auto resp = executeApi(p, "/api/v3/labs/relation/refund", reqData);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (Json::Reader().parse(resp.body, result) && result.get("code", "").asString() == "BBS00000") {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = result["resp_data"].get("trade_no", "").asString();
        } else {
            r.errMsg = "[" + result.get("code", "").asString() + "]" + result.get("msg", "").asString();
        }
        return r;
    }

private:
    // ── Preorder (聚合扫码) ──
    ChannelOrderResult createPreorder(ChannelOrderResult &r, const ChannelOrderRequest &req) {
        auto &p = req.channelParams;
        std::string accountType = getAccountType(req.payType);
        std::string transType = getTransType(req.payType);
        Json::Value reqData;
        reqData["merchant_no"] = p.get("appmchid", "").asString();
        reqData["term_no"] = p.get("appkey", "").asString();
        reqData["out_trade_no"] = req.orderId;
        reqData["account_type"] = accountType;
        reqData["trans_type"] = transType;
        reqData["total_amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        Json::Value locInfo;
        locInfo["request_ip"] = req.clientIp;
        reqData["location_info"] = locInfo;
        reqData["subject"] = req.subject.empty() ? "商品" : req.subject;
        reqData["notify_url"] = req.notifyUrl;
        // JSAPI extend
        if (req.payType == "wx_jsapi" || req.payType == "wx_lite") {
            Json::Value ext;
            ext["sub_appid"] = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            ext["user_id"] = p.get("openid", p.get("sub_openid", "").asString()).asString();
            reqData["acc_busi_fields"] = ext;
        } else if (req.payType == "ali_jsapi") {
            Json::Value ext;
            ext["user_id"] = p.get("buyer_id", p.get("openid", "").asString()).asString();
            reqData["acc_busi_fields"] = ext;
        } else if (req.payType == "bank_jsapi") {
            Json::Value ext;
            ext["user_id"] = p.get("openid", "").asString();
            reqData["acc_busi_fields"] = ext;
        }

        auto resp = executeApi(p, "/api/v3/labs/trans/preorder", reqData);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("code", "").asString() != "BBS00000") {
            r.errMsg = "[" + result.get("code", "").asString() + "]" + result.get("msg", "").asString();
            return r;
        }
        r.success = true;
        Json::Value respData = result["resp_data"];
        r.channelOrderNo = respData.get("trade_no", "").asString();
        // acc_resp_fields contains payment info
        Json::Value accResp = respData["acc_resp_fields"];
        if (accResp.isMember("code")) {
            r.payUrl = accResp["code"].asString(); // QR code URL
        } else if (accResp.isMember("prepay_id")) {
            r.payUrl = accResp["prepay_id"].asString(); // JSAPI prepay_id
        } else if (accResp.isMember("redirect_url")) {
            r.payUrl = accResp["redirect_url"].asString();
        }
        r.extra["acc_resp_fields"] = accResp;
        return r;
    }

    // ── Cashier (收银台) ──
    ChannelOrderResult createCashier(ChannelOrderResult &r, const ChannelOrderRequest &req) {
        auto &p = req.channelParams;
        std::string payMode = "ALIPAY";
        if (req.payType.find("wx") != std::string::npos) payMode = "WECHAT";
        else if (req.payType == "bank" || req.payType == "bank_jsapi") payMode = "UNION";

        Json::Value reqData;
        reqData["out_order_no"] = req.orderId;
        reqData["merchant_no"] = p.get("appmchid", "").asString();
        reqData["total_amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        reqData["order_efficient_time"] = nowPlus(1200);
        reqData["notify_url"] = req.notifyUrl;
        reqData["support_refund"] = 1;
        reqData["callback_url"] = req.returnUrl;
        reqData["order_info"] = req.subject.empty() ? "商品" : req.subject;
        Json::Value counterParam;
        counterParam["pay_mode"] = payMode;
        reqData["counter_param"] = Json::writeString(Json::StreamWriterBuilder(), counterParam);

        auto resp = executeCashierApi(p, "/api/v3/ccss/counter/order/special_create", reqData);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("code", "").asString() != "000000") {
            r.errMsg = "[" + result.get("code", "").asString() + "]" + result.get("msg", "").asString();
            return r;
        }
        r.success = true;
        r.payUrl = result["resp_data"].get("counter_url", "").asString();
        r.channelOrderNo = result["resp_data"].get("pay_order_no", "").asString();
        return r;
    }

    // ── Execute API with Authorization header ──
    struct ApiResponse {
        bool success = false;
        std::string body;
        std::string errMsg;
    };

    static ApiResponse executeApi(const Json::Value &p, const std::string &path, const Json::Value &reqData) {
        ApiResponse ar;
        bool isTest = p.get("appswitch", "0").asInt() == 1;
        std::string baseUrl = isTest ? "https://test.wsmsd.cn/sit" : "https://s2.lakala.com";

        Json::Value publicParams;
        publicParams["req_time"] = now();
        publicParams["version"] = "3.0";
        publicParams["req_data"] = reqData;

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string body = Json::writeString(wb, publicParams);

        std::string auth = buildAuthorization(p, body);
        if (auth.empty()) { ar.errMsg = "签名失败"; return ar; }

        std::map<std::string, std::string> headers;
        headers["Authorization"] = auth;
        headers["Content-Type"] = "application/json; charset=utf-8";
        headers["Accept"] = "application/json";

        auto resp = SyncHttp::postJson(baseUrl + path, body, headers);
        ar.body = resp.body;
        ar.success = resp.success;
        if (!resp.success) ar.errMsg = resp.errMsg;
        return ar;
    }

    static ApiResponse executeCashierApi(const Json::Value &p, const std::string &path, const Json::Value &reqData) {
        ApiResponse ar;
        bool isTest = p.get("appswitch", "0").asInt() == 1;
        std::string baseUrl = isTest ? "https://test.wsmsd.cn/sit" : "https://s2.lakala.com";

        Json::Value publicParams;
        publicParams["req_time"] = now();
        publicParams["version"] = "1.0";
        publicParams["req_data"] = reqData;

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string body = Json::writeString(wb, publicParams);

        std::string auth = buildAuthorization(p, body);
        if (auth.empty()) { ar.errMsg = "签名失败"; return ar; }

        std::map<std::string, std::string> headers;
        headers["Authorization"] = auth;
        headers["Content-Type"] = "application/json; charset=utf-8";
        headers["Accept"] = "application/json";

        auto resp = SyncHttp::postJson(baseUrl + path, body, headers);
        ar.body = resp.body;
        ar.success = resp.success;
        if (!resp.success) ar.errMsg = resp.errMsg;
        return ar;
    }

    // ── Build Authorization header ──
    static std::string buildAuthorization(const Json::Value &p, const std::string &body) {
        std::string appid = p.get("appid", "").asString();
        std::string serialNo = p.get("serial_no", "").asString();
        std::string privateKey = p.get("merchant_pem", "").asString();
        if (appid.empty() || privateKey.empty()) return "";

        std::string nonceStr = randomStr(12);
        std::string timestamp = std::to_string(std::time(nullptr));

        // Message: appid\nserial_no\ntimestamp\nnonce_str\nbody\n
        std::string message = appid + "\n" + serialNo + "\n" + timestamp + "\n" + nonceStr + "\n" + body + "\n";
        std::string signature = RsaUtils::signSha256(message, normalizePrivateKey(privateKey));
        if (signature.empty()) return "";

        return "LKLAPI-SHA256withRSA appid=\"" + appid + "\",serial_no=\"" + serialNo
            + "\",timestamp=\"" + timestamp + "\",nonce_str=\"" + nonceStr
            + "\",signature=\"" + signature + "\"";
    }

    // ── Parse Authorization fields ──
    static std::map<std::string, std::string> parseAuthFields(const std::string &kvStr) {
        std::map<std::string, std::string> m;
        // Split by comma, but signature may contain = and base64 chars
        // Format: key="value",key="value",signature="base64..."
        std::string remaining = kvStr;
        while (!remaining.empty()) {
            auto eqPos = remaining.find('=');
            if (eqPos == std::string::npos) break;
            std::string key = remaining.substr(0, eqPos);
            // trim key
            while (!key.empty() && (key.back() == ',' || key.back() == ' ')) key.pop_back();
            // skip leading spaces in key
            auto ks = key.find_first_not_of(' ');
            if (ks != std::string::npos) key = key.substr(ks);

            remaining = remaining.substr(eqPos + 1);
            if (remaining.empty() || remaining[0] != '"') break;
            remaining = remaining.substr(1); // skip opening quote
            auto endQuote = remaining.find('"');
            if (endQuote == std::string::npos) break;
            std::string value = remaining.substr(0, endQuote);
            m[key] = value;
            remaining = remaining.substr(endQuote + 1);
            if (!remaining.empty() && remaining[0] == ',') remaining = remaining.substr(1);
        }
        return m;
    }

    // ── Account type / trans type mapping ──
    static std::string getAccountType(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "WECHAT";
        if (payType.find("ali") != std::string::npos) return "ALIPAY";
        return "UQRCODEPAY"; // 银联/云闪付
    }
    static std::string getTransType(const std::string &payType) {
        if (payType == "wx_jsapi" || payType == "wx_lite") return "51"; // JSAPI
        if (payType == "ali_jsapi") return "51";
        if (payType == "bank_jsapi") return "51";
        return "41"; // 扫码
    }

    // ── Helpers ──
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

    static std::string randomStr(int len) {
        static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string s;
        unsigned char buf[32];
        RAND_bytes(buf, len < 32 ? len : 32);
        for (int i = 0; i < len; ++i) s += chars[buf[i % 32] % (sizeof(chars) - 1)];
        return s;
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

    static std::string nowPlus(int seconds) {
        auto t = std::time(nullptr) + seconds;
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[16]; std::strftime(b, sizeof(b), "%Y%m%d%H%M%S", &tmv); return b;
    }
};

REGISTER_CHANNEL_PLUGIN(LakalaPlugin);
