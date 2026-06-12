// WePay-Cpp — 微信支付 V3 Native(扫码) 通道插件
// 官方文档: https://pay.weixin.qq.com/wiki/doc/apiv3/apis/chapter3_4_1.shtml
// 签名: WECHATPAY2-SHA256-RSA2048，用商户私钥对规范化串做 SHA256-with-RSA
// 回调解密: AES-256-GCM，密钥为 APIv3 密钥(32字节)
//
// channelParams 必填:
//   appid        公众号/小程序 AppID
//   mchid        商户号
//   api_v3_key   APIv3 密钥(32字节)
//   serial_no    商户证书序列号
//   private_key  商户 API 私钥 PEM
#pragma once
#include "ChannelPlugin.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <random>
#include "../common/RsaUtils.h"
#include "../common/AesGcmUtils.h"
#include "../common/SyncHttp.h"

class WxpayNativePlugin : public ChannelPlugin {
public:
    std::string name() const override { return "wxpay_native"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid",       "AppID",          "input",    "", "微信公众号/小程序 AppID");
        add("mchid",       "商户号",         "input",    "", "微信支付商户号");
        add("api_v3_key",  "APIv3密钥",      "password", "", "微信支付 APIv3 密钥(32字节)");
        add("serial_no",   "证书序列号",     "input",    "", "商户API证书序列号");
        add("private_key", "商户私钥",       "textarea", "", "商户API私钥 PEM 格式(PKCS8)");
        add("secret",      "AppSecret",      "password", "", "公众号/小程序 Secret(获取openid用，可选)");
        return arr;
    }

    // 仅处理 wx_native (扫码), 其他 way_code 由 WxpayExtPlugin 处理
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;

        std::string appid     = p.get("appid", "").asString();
        std::string mchid     = p.get("mchid", "").asString();
        std::string apiV3Key  = p.get("api_v3_key", "").asString();
        std::string serialNo  = p.get("serial_no", "").asString();
        std::string priKey    = p.get("private_key", "").asString();

        if (appid.empty() || mchid.empty() || apiV3Key.empty() ||
            serialNo.empty() || priKey.empty()) {
            result.errMsg = "微信V3参数不完整(appid/mchid/api_v3_key/serial_no/private_key)";
            return result;
        }

        // 请求体
        Json::Value body;
        body["appid"]        = appid;
        body["mchid"]        = mchid;
        body["description"]  = req.subject.empty() ? "商品" : req.subject;
        body["out_trade_no"] = req.orderId;
        body["notify_url"]   = req.notifyUrl;
        Json::Value amount;
        amount["total"]    = (int)std::round(req.amount * 100);
        amount["currency"] = "CNY";
        body["amount"] = amount;

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);

        std::string urlPath = "/v3/pay/transactions/native";
        std::string fullUrl = "https://api.mch.weixin.qq.com" + urlPath;

        // 签名串: HTTP方法\nURL路径\n时间戳\n随机串\n请求体\n
        std::string nonce = randomString(32);
        std::string ts = std::to_string(std::time(nullptr));
        std::string signStr = "POST\n" + urlPath + "\n" + ts + "\n" + nonce + "\n" + bodyStr + "\n";

        std::string signature = RsaUtils::signSha256(signStr, priKey);
        if (signature.empty()) {
            result.errMsg = "签名失败: 检查私钥格式"; return result;
        }

        std::string authHeader = "WECHATPAY2-SHA256-RSA2048 "
            "mchid=\"" + mchid + "\","
            "nonce_str=\"" + nonce + "\","
            "timestamp=\"" + ts + "\","
            "serial_no=\"" + serialNo + "\","
            "signature=\"" + signature + "\"";

        std::map<std::string, std::string> headers = {
            {"Authorization", authHeader},
            {"Content-Type", "application/json"},
            {"Accept",       "application/json"}
        };

        auto resp = SyncHttp::postJson(fullUrl, bodyStr, headers);
        result.rawResponse = resp.body;
        if (!resp.success) {
            result.errMsg = "HTTP请求失败: " + resp.errMsg; return result;
        }

        Json::Value respJson;
        Json::Reader().parse(resp.body, respJson);

        if (resp.status != 200) {
            result.errMsg = "微信返回错误 code=" + std::to_string(resp.status) +
                ": " + respJson.get("message", "").asString();
            return result;
        }

        std::string codeUrl = respJson.get("code_url", "").asString();
        if (codeUrl.empty()) {
            result.errMsg = "微信返回为空: " + resp.body; return result;
        }

        result.success = true;
        result.payUrl = codeUrl;
        result.qrCode = codeUrl;
        return result;
    }

    ChannelQueryResult queryOrder(const std::string &orderId,
                                   const Json::Value &channelParams) override {
        ChannelQueryResult r;
        std::string mchid    = channelParams.get("mchid", "").asString();
        std::string serialNo = channelParams.get("serial_no", "").asString();
        std::string priKey   = channelParams.get("private_key", "").asString();
        if (mchid.empty() || priKey.empty()) return r;

        std::string urlPath = "/v3/pay/transactions/out-trade-no/" + orderId +
                              "?mchid=" + mchid;
        std::string fullUrl = "https://api.mch.weixin.qq.com" + urlPath;
        std::string nonce = randomString(32);
        std::string ts = std::to_string(std::time(nullptr));
        std::string signStr = "GET\n" + urlPath + "\n" + ts + "\n" + nonce + "\n\n";
        std::string signature = RsaUtils::signSha256(signStr, priKey);

        std::string authHeader = "WECHATPAY2-SHA256-RSA2048 "
            "mchid=\"" + mchid + "\","
            "nonce_str=\"" + nonce + "\","
            "timestamp=\"" + ts + "\","
            "serial_no=\"" + serialNo + "\","
            "signature=\"" + signature + "\"";

        auto resp = SyncHttp::get(fullUrl, {{"Authorization", authHeader}, {"Accept", "application/json"}});
        if (!resp.success || resp.status != 200) return r;

        Json::Value j;
        Json::Reader().parse(resp.body, j);
        std::string state = j.get("trade_state", "").asString();
        if (state == "SUCCESS") r.tradeState = 1;
        else if (state == "CLOSED" || state == "REVOKED") r.tradeState = -1;
        else r.tradeState = 0;

        r.channelOrderNo = j.get("transaction_id", "").asString();
        if (j.isMember("amount")) {
            int cents = j["amount"].get("total", 0).asInt();
            r.paidAmount = cents / 100.0;
        }
        if (j.isMember("payer")) r.buyerId = j["payer"].get("openid", "").asString();
        r.success = true;
        return r;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {

        ChannelNotifyResult result;
        result.responseText = "{\"code\":\"SUCCESS\",\"message\":\"成功\"}";

        std::string apiV3Key = channelParams.get("api_v3_key", "").asString();

        Json::Value body;
        Json::Reader reader;
        if (!reader.parse(rawBody, body)) { result.verified = false; return result; }

        // 注意: 生产环境应用微信支付平台证书验签 HTTP 头 Wechatpay-Signature
        // 简化: 信任 HTTPS + AES-GCM 解密成功即视为合法
        std::string eventType = body.get("event_type", "").asString();
        if (eventType != "TRANSACTION.SUCCESS") {
            result.verified = true; result.paid = false; return result;
        }

        auto &res = body["resource"];
        std::string ct    = res.get("ciphertext", "").asString();
        std::string nonce = res.get("nonce", "").asString();
        std::string aad   = res.get("associated_data", "").asString();

        std::string plain = AesGcmUtils::decryptWxV3(ct, nonce, aad, apiV3Key);
        if (plain.empty()) { result.verified = false; return result; }

        Json::Value inner;
        if (!reader.parse(plain, inner)) { result.verified = false; return result; }

        result.verified = true;
        std::string tradeState = inner.get("trade_state", "").asString();
        result.paid = (tradeState == "SUCCESS");
        result.orderId = inner.get("out_trade_no", "").asString();
        result.channelOrderNo = inner.get("transaction_id", "").asString();
        if (inner.isMember("amount")) {
            int cents = inner["amount"].get("total", 0).asInt();
            result.paidAmount = cents / 100.0;
        }
        if (inner.isMember("payer")) {
            result.buyerId = inner["payer"].get("openid", "").asString();
        }
        return result;
    }

    // ── 退款 ────────────────────────────────────────────
    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string mchid    = p.get("mchid", "").asString();
        std::string serialNo = p.get("serial_no", "").asString();
        std::string priKey   = p.get("private_key", "").asString();
        if (mchid.empty() || priKey.empty()) {
            r.errMsg = "微信参数不完整"; return r;
        }
        if (req.channelOrderNo.empty()) {
            r.errMsg = "channelOrderNo(transaction_id) 必填"; return r;
        }

        Json::Value body;
        body["transaction_id"] = req.channelOrderNo;
        body["out_refund_no"]  = req.refundNo;
        body["reason"]         = req.reason;
        body["notify_url"]     = req.notifyUrl;
        Json::Value amount;
        amount["refund"]   = (int)std::round(req.refundAmount * 100);
        amount["total"]    = (int)std::round(req.paidAmount * 100);
        amount["currency"] = "CNY";
        body["amount"] = amount;

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);

        std::string urlPath = "/v3/refund/domestic/refunds";
        std::string fullUrl = "https://api.mch.weixin.qq.com" + urlPath;
        std::string nonce = randomString(32);
        std::string ts = std::to_string(std::time(nullptr));
        std::string signStr = "POST\n" + urlPath + "\n" + ts + "\n" + nonce + "\n" + bodyStr + "\n";
        std::string signature = RsaUtils::signSha256(signStr, priKey);

        std::string authHeader = "WECHATPAY2-SHA256-RSA2048 mchid=\"" + mchid +
            "\",nonce_str=\"" + nonce + "\",timestamp=\"" + ts +
            "\",serial_no=\"" + serialNo + "\",signature=\"" + signature + "\"";

        auto resp = SyncHttp::postJson(fullUrl, bodyStr,
            {{"Authorization", authHeader}, {"Content-Type", "application/json"},
             {"Accept", "application/json"}});
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = "HTTP失败: " + resp.errMsg; return r; }

        Json::Value j; Json::Reader().parse(resp.body, j);
        if (resp.status != 200) {
            r.errMsg = "微信退款失败 " + std::to_string(resp.status) + ": " +
                       j.get("message", "").asString();
            return r;
        }
        r.success = true;
        std::string state = j.get("status", "").asString();
        // SUCCESS / CLOSED / PROCESSING / ABNORMAL
        if (state == "SUCCESS") r.state = 1;
        else if (state == "CLOSED" || state == "ABNORMAL") r.state = -1;
        else r.state = 0;
        r.channelRefundNo = j.get("refund_id", "").asString();
        return r;
    }

    // 退款回调验签 (与支付回调相同的 V3 签名机制)
    ChannelNotifyResult verifyRefundNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult result;
        result.responseText = "{\"code\":\"SUCCESS\",\"message\":\"成功\"}";
        std::string apiV3Key = channelParams.get("api_v3_key", "").asString();

        Json::Value body;
        if (!Json::Reader().parse(rawBody, body)) { result.verified = false; return result; }

        std::string et = body.get("event_type", "").asString();
        if (et != "REFUND.SUCCESS" && et != "REFUND.ABNORMAL" && et != "REFUND.CLOSED") {
            result.verified = true; result.paid = false; return result;
        }

        auto &res = body["resource"];
        std::string ct    = res.get("ciphertext", "").asString();
        std::string nonce = res.get("nonce", "").asString();
        std::string aad   = res.get("associated_data", "").asString();
        std::string plain = AesGcmUtils::decryptWxV3(ct, nonce, aad, apiV3Key);
        if (plain.empty()) { result.verified = false; return result; }

        Json::Value inner;
        if (!Json::Reader().parse(plain, inner)) { result.verified = false; return result; }
        result.verified = true;
        result.paid = (et == "REFUND.SUCCESS");
        result.orderId = inner.get("out_refund_no", "").asString();
        result.channelOrderNo = inner.get("refund_id", "").asString();
        if (inner.isMember("amount")) {
            int cents = inner["amount"].get("refund", 0).asInt();
            result.paidAmount = cents / 100.0;
        }
        return result;
    }

    // ── 关闭订单 ────────────────────────────────────────
    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        auto &p = req.channelParams;
        std::string mchid    = p.get("mchid", "").asString();
        std::string serialNo = p.get("serial_no", "").asString();
        std::string priKey   = p.get("private_key", "").asString();
        if (mchid.empty() || priKey.empty() || req.orderId.empty()) {
            r.errMsg = "参数不完整"; return r;
        }

        std::string urlPath = "/v3/pay/transactions/out-trade-no/" + req.orderId + "/close";
        std::string fullUrl = "https://api.mch.weixin.qq.com" + urlPath;
        Json::Value body; body["mchid"] = mchid;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);

        std::string nonce = randomString(32);
        std::string ts = std::to_string(std::time(nullptr));
        std::string signStr = "POST\n" + urlPath + "\n" + ts + "\n" + nonce + "\n" + bodyStr + "\n";
        std::string signature = RsaUtils::signSha256(signStr, priKey);
        std::string authHeader = "WECHATPAY2-SHA256-RSA2048 mchid=\"" + mchid +
            "\",nonce_str=\"" + nonce + "\",timestamp=\"" + ts +
            "\",serial_no=\"" + serialNo + "\",signature=\"" + signature + "\"";

        auto resp = SyncHttp::postJson(fullUrl, bodyStr,
            {{"Authorization", authHeader}, {"Content-Type", "application/json"}});
        // 关闭成功返回 204
        if (resp.success && (resp.status == 204 || resp.status == 200)) {
            r.success = true;
        } else {
            r.errMsg = "关闭失败 " + std::to_string(resp.status) + ": " + resp.body;
        }
        return r;
    }

    // ── 获取 openid (微信公众号 OAuth2) ─────────────────
    ChannelUserIdResult queryChannelUserId(const ChannelUserIdRequest &req) override {
        ChannelUserIdResult r;
        auto &p = req.channelParams;
        std::string appid  = p.get("appid", "").asString();
        std::string secret = p.get("secret", "").asString();
        if (appid.empty() || secret.empty() || req.code.empty()) {
            r.errMsg = "appid/secret/code 必填"; return r;
        }
        std::string url = "https://api.weixin.qq.com/sns/oauth2/access_token"
                          "?appid=" + appid + "&secret=" + secret +
                          "&code=" + req.code + "&grant_type=authorization_code";
        auto resp = SyncHttp::get(url);
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "响应解析失败"; return r; }
        std::string openid = j.get("openid", "").asString();
        if (openid.empty()) {
            r.errMsg = "微信返回: " + j.get("errmsg", resp.body).asString(); return r;
        }
        r.success = true;
        r.userId = openid;
        return r;
    }

private:
    static std::string randomString(int len) {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < len; ++i) s += cs[rng() % 36];
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(WxpayNativePlugin);
