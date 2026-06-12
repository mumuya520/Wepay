#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 新生易(xsy) - RSA-SHA1签名, JSON POST
// 网关: https://gateway-hpx.hnapay.com/order (生产) / gateway-hpxtest1.hnapay.com/order (测试)
// 签名: ksort拼接k=v&(skip sign/空值/null), reqData/respData先JSON编码再拼接, 去尾&
//       RSA-SHA1签名→Base64
// 验签: 同样拼接, 平台公钥RSA-SHA1验签
// 请求: reqId/orgNo/reqData/signType/timestamp/version/sign → JSON POST
// 响应: code=0000/0001, respData
// 扫码: /trade/activeScan, payType=ALIPAY/WECHAT/UNIONPAY
// JSAPI: /trade/jsapiScan, payWay=02(公众号)/03(小程序)
// 被扫: /trade/reverseScan
// 回调: JSON POST, verifySign, respData.orderNo/outOrderNo/buyerId
// 退款: /trade/refund

class XsyPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "xsy"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "机构代码", "input");
        add("appkey", "平台公钥", "textarea", "", "RSA公钥(Base64编码)");
        add("appsecret", "商户私钥", "textarea", "", "RSA私钥(Base64编码)");
        add("appmchid", "商户编号", "input");
        add("appswitch", "环境选择", "select", "0", "0=生产环境 1=测试环境");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=JSAPI");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appsecret", "").asString().empty() || p.get("appmchid", "").asString().empty()) {
            r.errMsg = "新生易参数不完整(appid/appsecret/appmchid)";
            return r;
        }
        int apptype = p.get("apptype", "1").asInt();
        std::string payType = getPayType(req.payType);
        bool isTest = p.get("appswitch", "0").asString() == "1";

        if (apptype == 2) {
            // JSAPI mode
            std::string payWay = "02"; // 公众号
            if (req.payType.find("lite") != std::string::npos || req.payType.find("mini") != std::string::npos) payWay = "03";
            std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
            std::string subAppId = p.get("sub_appid", p.get("app_appid", "").asString()).asString();

            Json::Value reqData = buildJsapiReqData(p, req, payType, payWay, openid, subAppId);
            auto result = requestApi(p, "/trade/jsapiScan", reqData, isTest);
            r.rawResponse = result.rawResponse;
            if (!result.success) { r.errMsg = result.errMsg; return r; }

            r.success = true;
            if (payType == "WECHAT") {
                // Build WeChat JSAPI payInfo
                Json::Value payInfo;
                payInfo["appId"] = result.data.get("payAppId", "");
                payInfo["timeStamp"] = result.data.get("payTimeStamp", "");
                payInfo["nonceStr"] = result.data.get("paynonceStr", "");
                payInfo["package"] = result.data.get("payPackage", "");
                payInfo["signType"] = result.data.get("paySignType", "");
                payInfo["paySign"] = result.data.get("paySign", "");
                Json::FastWriter fw;
                r.payUrl = fw.write(payInfo);
            } else if (payType == "ALIPAY") {
                r.payUrl = result.data.get("source", "").asString();
            } else if (payType == "UNIONPAY") {
                r.payUrl = result.data.get("redirectUrl", "").asString();
            }
            r.channelOrderNo = result.data.get("outOrderNo", "").asString();
        } else {
            // 扫码模式
            Json::Value reqData = buildScanReqData(p, req, payType);
            auto result = requestApi(p, "/trade/activeScan", reqData, isTest);
            r.rawResponse = result.rawResponse;
            if (!result.success) { r.errMsg = result.errMsg; return r; }

            r.success = true;
            std::string payUrl = result.data.get("payUrl", "").asString();
            // Extract qrContent if present
            auto qrPos = payUrl.find("qrContent=");
            if (qrPos != std::string::npos) {
                auto signPos = payUrl.find("&sign=", qrPos);
                if (signPos != std::string::npos) {
                    payUrl = payUrl.substr(qrPos + 10, signPos - qrPos - 10);
                } else {
                    payUrl = payUrl.substr(qrPos + 10);
                }
            }
            r.payUrl = payUrl;
            r.channelOrderNo = result.data.get("outOrderNo", "").asString();
        }
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = R"({"code":"fail"})";
        std::string pubKey = channelParams.get("appkey", "").asString();
        std::string privateKey = channelParams.get("appsecret", "").asString();
        bool isTest = channelParams.get("appswitch", "0").asString() == "1";

        Json::Value arr;
        if (!Json::Reader().parse(body, arr)) return r;
        if (!arr.isMember("sign")) return r;

        // Verify sign
        std::string signContent = buildNotifySignContent(arr, body);
        std::string sign = arr.get("sign", "").asString();
        bool verified = RsaUtils::verifySha1(signContent, sign, normalizePublicKey(pubKey));
        r.verified = verified;
        if (!verified) return r;

        const Json::Value &respData = arr["respData"];
        std::string tranSts = respData.get("tranSts", "").asString();
        r.paid = (tranSts == "SUCCESS");
        r.orderId = respData.get("orderNo", "").asString();
        r.channelOrderNo = respData.get("outOrderNo", "").asString();
        r.buyerId = respData.get("buyerId", "").asString();
        try { r.paidAmount = std::stod(respData.get("amt", "0").asString()) / 100.0; } catch (...) {}
        r.responseText = R"({"code":"success"})";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        bool isTest = p.get("appswitch", "0").asString() == "1";

        Json::Value reqData;
        reqData["merchantNo"] = p.get("appmchid", "").asString();
        reqData["orderNo"] = req.refundNo;
        reqData["origOrderNo"] = req.orderId;
        reqData["amt"] = (Json::Int64)std::llround(req.refundAmount * 100.0);

        auto result = requestApi(p, "/trade/refund", reqData, isTest);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = result.data.get("orderNo", "").asString();
        return r;
    }

private:
    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        std::string resCode;
        Json::Value data;
    };

    static ApiResponse requestApi(const Json::Value &p, const std::string &path, const Json::Value &reqData, bool isTest) {
        ApiResponse ar;
        std::string appId = p.get("appid", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();
        std::string pubKey = p.get("appkey", "").asString();
        std::string gatewayUrl = isTest ? "https://gateway-hpxtest1.hnapay.com/order" : "https://gateway-hpx.hnapay.com/order";

        // Build request param
        Json::Value param;
        param["reqId"] = randomStr(60);
        param["orgNo"] = appId;
        param["reqData"] = reqData;
        param["signType"] = "RSA";
        param["timestamp"] = (Json::Int64)getMillisecond();
        param["version"] = "1.0";

        // Sign
        std::string signContent = buildRequestSignContent(param);
        param["sign"] = RsaUtils::signSha1(signContent, normalizePrivateKey(privateKey));

        Json::FastWriter fw;
        std::string jsonBody = fw.write(param);

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";
        auto resp = SyncHttp::postJson(gatewayUrl + path, jsonBody, headers);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { ar.errMsg = "响应解析失败"; return ar; }

        std::string code = result.get("code", "").asString();
        ar.resCode = code;
        if (code == "0000" || code == "0001") {
            ar.success = true;
            ar.data = result["respData"];
        } else {
            ar.errMsg = result.get("msg", "返回数据解析失败").asString();
        }
        return ar;
    }

    // Build sign content: ksort, reqData→JSON string, skip sign/empty/null, k=v&, remove last &
    static std::string buildRequestSignContent(const Json::Value &param) {
        // First convert reqData to JSON string
        Json::Value p = param;
        if (p.isMember("reqData") && p["reqData"].isObject()) {
            Json::FastWriter fw;
            p["reqData"] = fw.write(p["reqData"]);
            // Remove trailing newline from FastWriter
            std::string s = p["reqData"].asString();
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            p["reqData"] = s;
        }
        return ksortConcat(p);
    }

    // Build notify sign content: respData handling
    static std::string buildNotifySignContent(const Json::Value &param, const std::string &rawBody) {
        Json::Value p = param;
        if (p.isMember("respData") && p["respData"].isObject()) {
            // Try extracting raw respData from body
            auto start = rawBody.find("\"respData\":");
            if (start != std::string::npos) {
                auto signStart = rawBody.find(",\"sign\"");
                if (signStart != std::string::npos) {
                    // Extract from after "respData": to before ,"sign"
                    std::string afterKey = rawBody.substr(start + 11);
                    std::string beforeSign = rawBody.substr(0, signStart);
                    // Use raw substring approach
                    auto valStart = rawBody.find("\"respData\":") + 11;
                    auto valEnd = rawBody.rfind(",\"sign\"");
                    if (valEnd != std::string::npos && valEnd > valStart) {
                        p["respData"] = rawBody.substr(valStart, valEnd - valStart);
                    }
                }
            }
            if (p["respData"].isObject()) {
                // Remove empty/null values then JSON encode
                Json::Value cleaned;
                for (auto it = p["respData"].begin(); it != p["respData"].end(); ++it) {
                    if (!it->isNull() && (!it->isString() || !it->asString().empty())) {
                        cleaned[it.name()] = *it;
                    }
                }
                Json::FastWriter fw;
                p["respData"] = fw.write(cleaned);
                std::string s = p["respData"].asString();
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                p["respData"] = s;
            }
        }
        return ksortConcat(p);
    }

    static std::string ksortConcat(const Json::Value &p) {
        // Collect keys and sort
        std::vector<std::string> keys;
        for (auto it = p.begin(); it != p.end(); ++it) keys.push_back(it.name());
        std::sort(keys.begin(), keys.end());

        std::string s;
        for (auto &k : keys) {
            if (k == "sign") continue;
            const Json::Value &v = p[k];
            if (v.isNull()) continue;
            if (v.isString() && v.asString().empty()) continue;
            std::string val;
            if (v.isString()) val = v.asString();
            else if (v.isInt()) val = std::to_string(v.asInt());
            else if (v.isInt64()) val = std::to_string(v.asInt64());
            else if (v.isDouble()) val = std::to_string(v.asDouble());
            else if (v.isBool()) val = v.asBool() ? "1" : "0";
            else continue;
            if (!s.empty()) s += "&";
            s += k + "=" + val;
        }
        return s;
    }

    static Json::Value buildScanReqData(const Json::Value &p, const ChannelOrderRequest &req, const std::string &payType) {
        Json::Value d;
        d["merchantNo"] = p.get("appmchid", "").asString();
        d["orderNo"] = req.orderId;
        d["amt"] = (Json::Int64)std::llround(req.amount * 100.0);
        d["payType"] = payType;
        d["subject"] = req.subject.empty() ? "商品" : req.subject;
        d["trmIp"] = req.clientIp;
        d["customerIp"] = req.clientIp;
        d["notifyUrl"] = req.notifyUrl;
        return d;
    }

    static Json::Value buildJsapiReqData(const Json::Value &p, const ChannelOrderRequest &req, const std::string &payType, const std::string &payWay, const std::string &openid, const std::string &subAppId) {
        Json::Value d;
        d["merchantNo"] = p.get("appmchid", "").asString();
        d["orderNo"] = req.orderId;
        d["amt"] = (Json::Int64)std::llround(req.amount * 100.0);
        d["payType"] = payType;
        d["payWay"] = payWay;
        if (!subAppId.empty()) d["subAppId"] = subAppId;
        d["userId"] = openid;
        d["subject"] = req.subject.empty() ? "商品" : req.subject;
        d["trmIp"] = req.clientIp;
        d["customerIp"] = req.clientIp;
        d["notifyUrl"] = req.notifyUrl;
        return d;
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

    static long long getMillisecond() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
        return ms.count();
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

REGISTER_CHANNEL_PLUGIN(XsyPlugin);
