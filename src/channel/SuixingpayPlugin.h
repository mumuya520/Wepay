#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 随行付 - RSA-SHA1签名, JSON API
// 网关: https://openapi.tianquetech.com
// 签名: ksort拼接k=v&(skip sign/null/空, 数组JSON编码), 去尾&, RSA私钥签名→Base64
// 验签: 同样拼接, 平台公钥RSA-SHA1验签
// 请求: orgId/reqId/reqData/timestamp/version/signType/sign → JSON POST
// 响应: code=0000, 验签, 取respData

class SuixingpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "suixingpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "机构编号", "input");
        add("appkey", "平台公钥", "textarea", "", "RSA公钥(Base64编码)");
        add("appsecret", "商户私钥", "textarea", "", "RSA私钥(Base64编码)");
        add("appmchid", "商户编号", "input");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=JSAPI/公众号");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appsecret", "").asString().empty()) {
            r.errMsg = "随行付参数不完整(appid/appsecret)";
            return r;
        }
        int apptype = p.get("apptype", "1").asInt();
        std::string payType = getPayType(req.payType);

        if (apptype == 2) {
            return jsapiPay(r, req, payType);
        }
        return qrcodePay(r, req, payType);
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "{\"code\":\"fail\",\"msg\":\"签名错误\"}";
        Json::Value data;
        if (!Json::Reader().parse(body, data)) { r.verified = false; return r; }

        std::string sign = data.get("sign", "").asString();
        if (sign.empty()) { r.verified = false; return r; }

        std::string pubKey = channelParams.get("appkey", "").asString();
        std::string signContent = buildSignContentFromJson(data);
        r.verified = RsaUtils::verifySha1(signContent, sign, normalizePublicKey(pubKey));
        if (!r.verified) return r;

        std::string bizCode = data.get("bizCode", "").asString();
        r.paid = (bizCode == "0000");
        r.orderId = data.get("ordNo", "").asString();
        r.channelOrderNo = data.get("sxfUuid", "").asString();
        r.buyerId = data.get("buyerId", "").asString();
        try { r.paidAmount = std::stod(data.get("amt", "0").asString()); } catch (...) {}
        r.responseText = r.paid ? "{\"code\":\"success\",\"msg\":\"成功\"}" : "{\"code\":\"fail\",\"msg\":\"状态错误\"}";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        Json::Value reqData;
        reqData["mno"] = p.get("appmchid", "").asString();
        reqData["ordNo"] = req.refundNo;
        reqData["origOrderNo"] = req.orderId;
        reqData["amt"] = fmtAmount(req.refundAmount);

        auto resp = executeApi(p, "/order/refund", reqData);
        r.rawResponse = resp.rawResponse;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        if (resp.data.get("bizCode", "").asString() == "0000") {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = resp.data.get("origOrderNo", "").asString();
        } else {
            r.errMsg = "[" + resp.data.get("bizCode", "").asString() + "]" + resp.data.get("bizMsg", "").asString();
        }
        return r;
    }

private:
    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        Json::Value data;
    };

    static ApiResponse executeApi(const Json::Value &p, const std::string &path, const Json::Value &reqData) {
        ApiResponse ar;
        std::string orgId = p.get("appid", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();
        std::string pubKey = p.get("appkey", "").asString();

        // Build request JSON
        Json::Value params;
        params["orgId"] = orgId;
        params["reqId"] = getMillisecond();
        params["reqData"] = reqData;
        params["timestamp"] = getTimestamp();
        params["version"] = "1.0";
        params["signType"] = "RSA";

        std::string signContent = buildSignContentFromJson(params);
        params["sign"] = RsaUtils::signSha1(signContent, normalizePrivateKey(privateKey));

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string body = Json::writeString(wb, params);

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";

        auto resp = SyncHttp::postJson("https://openapi.tianquetech.com" + path, body, headers);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { ar.errMsg = "响应解析失败"; return ar; }
        std::string code = result.get("code", "").asString();
        if (code != "0000") {
            ar.errMsg = result.get("msg", "请求失败").asString();
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
        ar.data = result["respData"];
        return ar;
    }

    // ── 扫码支付 ──
    ChannelOrderResult qrcodePay(ChannelOrderResult &r, const ChannelOrderRequest &req, const std::string &payType) {
        auto &p = req.channelParams;
        Json::Value reqData;
        reqData["mno"] = p.get("appmchid", "").asString();
        reqData["ordNo"] = req.orderId;
        reqData["amt"] = fmtAmount(req.amount);
        reqData["payType"] = payType;
        reqData["subject"] = req.subject.empty() ? "商品" : req.subject;
        reqData["trmIp"] = req.clientIp;
        reqData["notifyUrl"] = req.notifyUrl;

        auto resp = executeApi(p, "/order/activeScan", reqData);
        r.rawResponse = resp.rawResponse;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        if (resp.data.get("bizCode", "").asString() == "0000") {
            r.success = true;
            r.payUrl = resp.data.get("payUrl", "").asString();
        } else {
            r.errMsg = "[" + resp.data.get("bizCode", "").asString() + "]" + resp.data.get("bizMsg", "").asString();
        }
        return r;
    }

    // ── JSAPI支付 ──
    ChannelOrderResult jsapiPay(ChannelOrderResult &r, const ChannelOrderRequest &req, const std::string &payType) {
        auto &p = req.channelParams;
        std::string subAppid = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
        std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
        bool isMini = req.payType.find("lite") != std::string::npos || req.payType.find("mini") != std::string::npos;
        std::string payWay = (payType == "WECHAT" && isMini) ? "03" : "02";

        Json::Value reqData;
        reqData["mno"] = p.get("appmchid", "").asString();
        reqData["ordNo"] = req.orderId;
        reqData["amt"] = fmtAmount(req.amount);
        reqData["payType"] = payType;
        reqData["payWay"] = payWay;
        reqData["subject"] = req.subject.empty() ? "商品" : req.subject;
        reqData["trmIp"] = req.clientIp;
        reqData["subAppid"] = subAppid;
        reqData["userId"] = openid;
        reqData["notifyUrl"] = req.notifyUrl;

        auto resp = executeApi(p, "/order/jsapiScan", reqData);
        r.rawResponse = resp.rawResponse;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        if (resp.data.get("bizCode", "").asString() == "0000") {
            r.success = true;
            if (payType == "WECHAT") {
                // WeChat JSAPI params
                Json::Value jsapi;
                jsapi["appId"] = resp.data.get("payAppId", "").asString();
                jsapi["timeStamp"] = resp.data.get("payTimeStamp", "").asString();
                jsapi["nonceStr"] = resp.data.get("paynonceStr", "").asString();
                jsapi["package"] = resp.data.get("payPackage", "").asString();
                jsapi["signType"] = resp.data.get("paySignType", "").asString();
                jsapi["paySign"] = resp.data.get("paySign", "").asString();
                Json::StreamWriterBuilder wb; wb["indentation"] = "";
                r.payUrl = Json::writeString(wb, jsapi);
                r.extra["jsapi"] = jsapi;
            } else if (payType == "ALIPAY") {
                r.payUrl = resp.data.get("source", "").asString();
            } else {
                r.payUrl = resp.data.get("redirectUrl", "").asString();
            }
        } else {
            r.errMsg = "[" + resp.data.get("bizCode", "").asString() + "]" + resp.data.get("bizMsg", "").asString();
        }
        return r;
    }

    // ksort JSON keys, skip sign/null/empty, arrays→JSON, concat k=v&, remove last &
    static std::string buildSignContentFromJson(const Json::Value &j) {
        std::vector<std::string> keys;
        for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.name());
        std::sort(keys.begin(), keys.end());
        std::string s;
        for (auto &k : keys) {
            if (k == "sign") continue;
            const Json::Value &v = j[k];
            if (v.isNull() || (v.isString() && v.asString().empty())) continue;
            std::string val;
            if (v.isObject() || v.isArray()) {
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                val = Json::writeString(wb, v);
            } else if (v.isString()) {
                val = v.asString();
            } else if (v.isInt() || v.isInt64()) {
                val = std::to_string(v.asInt64());
            } else if (v.isDouble()) {
                val = fmtAmount(v.asDouble());
            } else {
                continue;
            }
            if (!s.empty()) s += "&";
            s += k + "=" + val;
        }
        return s;
    }

    static std::string getPayType(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "WECHAT";
        if (payType == "bank" || payType == "bank_jsapi") return "UNIONPAY";
        return "ALIPAY";
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

    static std::string getMillisecond() {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return std::to_string(ms);
    }

    static std::string getTimestamp() {
        auto t = std::time(nullptr);
        std::tm *tm = std::localtime(&t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y%m%d%H%M%S");
        return oss.str();
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(SuixingpayPlugin);
