#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 盛付通 - RSA签名(默认SHA1) JSON API
// 网关: https://mchapi.shengpay.com
// 签名: ksort拼接k=v&(去sign/空值), RSA私钥签名, 平台公钥验签

class ShengpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "shengpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户号", "input");
        add("appkey", "商户私钥", "textarea", "", "RSA私钥(Base64编码)");
        add("appsecret", "盛付通公钥", "textarea", "", "RSA公钥(Base64编码)");
        add("appswitch", "收单接口类型", "select", "0", "0=线上 1=线下");
        add("appmchid", "子商户号", "input", "", "非代理商户可留空");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=PC网站 3=H5/手机 4=JSAPI/服务窗 5=聚合码");
        add("aeskey", "AES加密密钥", "input", "", "用于投诉事件回调解密");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "盛付通参数不完整(appid/appkey)";
            return r;
        }
        std::string tradeType = getTradeType(req.payType, p.get("apptype", "1").asInt());
        bool isOffline = p.get("appswitch", "0").asInt() == 1;
        std::string path = isOffline ? "/pay/unifiedorderOffline" : "/pay/unifiedorder";

        Json::Value params;
        params["outTradeNo"] = req.orderId;
        params["totalFee"] = (Json::Int64)std::llround(req.amount * 100.0);
        params["currency"] = "CNY";
        params["tradeType"] = tradeType;
        params["notifyUrl"] = req.notifyUrl;
        params["pageUrl"] = req.returnUrl;
        params["body"] = req.subject.empty() ? "商品" : req.subject;
        params["clientIp"] = req.clientIp;

        // JSAPI extra
        if (tradeType == "wx_jsapi" || tradeType == "wx_lite" || tradeType == "alipay_jsapi") {
            std::string subAppid = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
            Json::Value extra;
            extra["openId"] = openid;
            if (!subAppid.empty()) extra["appId"] = subAppid;
            Json::StreamWriterBuilder wb; wb["indentation"] = "";
            params["extra"] = Json::writeString(wb, extra);
        }

        std::string subMchId = p.get("appmchid", "").asString();
        if (!subMchId.empty()) params["subMchId"] = subMchId;

        auto resp = executeApi(p, path, params);
        r.rawResponse = resp.rawResponse;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        r.success = true;
        r.channelOrderNo = resp.data.get("transactionId", "").asString();
        r.payUrl = resp.data.get("payInfo", "").asString();
        r.extra["data"] = resp.data;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "SIGN FAIL";
        // ShengPay notify is JSON body
        Json::Value data;
        if (!Json::Reader().parse(body, data)) { r.verified = false; return r; }
        std::string sign = data.get("sign", "").asString();
        if (sign.empty()) { r.verified = false; return r; }
        // Build sign content from JSON keys (ksort, skip sign/empty)
        std::string signContent = buildSignContentFromJson(data);
        std::string pubKey = channelParams.get("appsecret", "").asString();
        if (pubKey.empty()) { r.verified = false; return r; }
        r.verified = RsaUtils::verifySha1(signContent, sign, normalizePublicKey(pubKey));
        if (!r.verified) return r;
        std::string status = data.get("status", "").asString();
        r.paid = (status == "PAY_SUCCESS");
        r.orderId = data.get("outTradeNo", "").asString();
        r.channelOrderNo = data.get("transactionId", "").asString();
        // payerInfo is JSON string
        std::string payerInfo = data.get("payerInfo", "").asString();
        if (!payerInfo.empty()) {
            Json::Value pi;
            if (Json::Reader().parse(payerInfo, pi)) {
                r.buyerId = pi.get("openid", "").asString();
            }
        }
        try { r.paidAmount = std::stod(data.get("totalFee", "0").asString()) / 100.0; } catch (...) {}
        r.responseText = r.paid ? "SUCCESS" : "FAIL";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        Json::Value params;
        params["outTradeNo"] = req.orderId;
        params["outRefundNo"] = req.refundNo;
        params["refundFee"] = (Json::Int64)std::llround(req.refundAmount * 100.0);
        params["notifyUrl"] = req.notifyUrl;

        auto resp = executeApi(p, "/refund/orderRefund", params);
        r.rawResponse = resp.rawResponse;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = resp.data.get("refundId", "").asString();
        return r;
    }

private:
    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        Json::Value data;
    };

    static ApiResponse executeApi(const Json::Value &p, const std::string &path, const Json::Value &params) {
        ApiResponse ar;
        std::string mchId = p.get("appid", "").asString();
        std::string privateKey = p.get("appkey", "").asString();
        std::string pubKey = p.get("appsecret", "").asString();

        // Build request JSON with mchId, nonceStr, signType, sign
        Json::Value reqParams = params;
        reqParams["mchId"] = mchId;
        reqParams["nonceStr"] = Md5Utils::md5(std::to_string((long long)std::time(nullptr)) + std::to_string(rand()));
        reqParams["signType"] = "RSA";
        // Sign
        std::string signContent = buildSignContentFromJson(reqParams);
        reqParams["sign"] = RsaUtils::signSha1(signContent, normalizePrivateKey(privateKey));

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string body = Json::writeString(wb, reqParams);

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";

        auto resp = SyncHttp::postJson("https://mchapi.shengpay.com" + path, body, headers);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { ar.errMsg = "响应解析失败"; return ar; }
        std::string returnCode = result.get("returnCode", "").asString();
        if (returnCode != "SUCCESS") {
            ar.errMsg = result.get("returnMsg", "请求失败").asString();
            return ar;
        }
        std::string resultCode = result.get("resultCode", "").asString();
        if (resultCode != "SUCCESS") {
            ar.errMsg = "[" + result.get("errorCode", "").asString() + "]" + result.get("errorCodeDes", "").asString();
            return ar;
        }
        // Verify response sign
        if (result.isMember("sign") && !result["sign"].asString().empty()) {
            std::string respSignContent = buildSignContentFromJson(result);
            if (!RsaUtils::verifySha1(respSignContent, result["sign"].asString(), normalizePublicKey(pubKey))) {
                ar.errMsg = "返回数据验签失败";
                return ar;
            }
        }
        ar.success = true;
        ar.data = result;
        return ar;
    }

    // ksort JSON keys, skip sign and empty values, concat k=v&
    static std::string buildSignContentFromJson(const Json::Value &j) {
        // Collect keys and sort
        std::vector<std::string> keys;
        for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.name());
        std::sort(keys.begin(), keys.end());
        std::string s;
        for (auto &k : keys) {
            if (k == "sign") continue;
            std::string v;
            if (j[k].isString()) v = j[k].asString();
            else if (j[k].isInt() || j[k].isInt64()) v = std::to_string(j[k].asInt64());
            else if (j[k].isDouble()) v = fmtAmount(j[k].asDouble());
            else { Json::StreamWriterBuilder wb; wb["indentation"] = ""; v = Json::writeString(wb, j[k]); }
            if (v.empty()) continue;
            if (!s.empty()) s += "&";
            s += k + "=" + v;
        }
        return s;
    }

    static std::string getTradeType(const std::string &payType, int apptype) {
        if (payType.find("wx") != std::string::npos) {
            if (apptype == 1) return "wx_jsapi";
            if (apptype == 2) return "wx_native";
            if (apptype == 3) return "wx_wap";
            if (apptype == 4) return "wx_lite";
            if (apptype == 5) return "shengpay_aggre";
            return "wx_native";
        }
        if (payType == "bank" || payType == "bank_jsapi") return "upacp_qr";
        // alipay
        if (apptype == 2) return "alipay_pc";
        if (apptype == 3) return "alipay_wap";
        if (apptype == 4) return "alipay_jsapi";
        return "alipay_qr";
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
};

REGISTER_CHANNEL_PLUGIN(ShengpayPlugin);
