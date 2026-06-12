#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include <iomanip>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 银盛e企通(yseqt) - RSA证书签名, ksort拼接, form POST, 支持转账
// 网关: https://eqt.ysepay.com/api/trade
// 签名: ksort, skip sign/null/@, k=v&, 去尾&, RSA签名(商户私钥), Base64
// 请求: requestId/srcMerchantNo/version/charset/serviceNo/signType/bizReqJson(JSON)/sign → form POST
// 响应: code=SYS000成功, bizResponseJson包含数据, 验签(平台公钥)
// 回调: POST bizResponseJson, 验签(平台公钥)
// 扫码: scanPay, bank_type=1903000(支付宝)/1902000(微信)/9001002(云闪付)
// 收银台: cashierPay, pay_mode=29h5(微信H5)/29UrlScheme(微信scheme)/28(微信JSAPI)/29(微信APP)/26(支付宝JSAPI)
// JSAPI: jsPay, bankType+payMode+openid/appid
// 退款: refund, 支持分账退款
// 转账: paymentRequest, transferNotify回调
// 查询: paymentQuery, 余额查询

class YseqtPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "yseqt"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "服务商商户号", "input");
        add("appkey", "私钥证书密码", "input");
        add("appmchid", "收款商户号", "input");
        add("appsecret", "商户私钥", "textarea", "", "RSA私钥(PKCS#8 PEM格式)");
        add("productkey", "平台公钥", "textarea", "", "RSA公钥(PEM格式)");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=小程序H5 3=JS支付");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appsecret", "").asString().empty() || p.get("productkey", "").asString().empty()) {
            r.errMsg = "银盛e企通参数不完整(appid/appsecret/productkey)";
            return r;
        }

        int apptype = p.get("apptype", "1").asInt();
        std::string merchantNo = p.get("appmchid", "").asString();
        std::string bankType = getBankType(req.payType);

        // Determine service and biz content
        std::string serviceNo;
        Json::Value bizContent;

        if (apptype == 2) {
            // 小程序H5
            serviceNo = "cashierPay";
            bizContent["requestNo"] = req.orderId;
            bizContent["payeeMerchantNo"] = merchantNo;
            bizContent["orderDesc"] = req.subject.empty() ? "商品" : req.subject;
            bizContent["amount"] = fmtAmount(req.amount);
            bizContent["payMode"] = (req.payType.find("wx") != std::string::npos) ? "29h5" : "";
            bizContent["notifyUrl"] = req.notifyUrl;
            bizContent["isFastPay"] = "Y";
        } else if (apptype == 3) {
            // JS支付
            serviceNo = "jsPay";
            bizContent["requestNo"] = req.orderId;
            bizContent["payeeMerchantNo"] = merchantNo;
            bizContent["orderDesc"] = req.subject.empty() ? "商品" : req.subject;
            bizContent["amount"] = fmtAmount(req.amount);
            bizContent["bankType"] = bankType;
            bizContent["notifyUrl"] = req.notifyUrl;

            std::string payMode = getPayMode(req.payType);
            bizContent["payMode"] = payMode;
            std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
            std::string appId = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            if (payMode == "28" || payMode == "29") {
                if (!appId.empty()) bizContent["wxAppId"] = appId;
                if (!openid.empty()) bizContent["wxOpenId"] = openid;
            } else if (payMode == "26") {
                if (!openid.empty()) bizContent["alipayId"] = openid;
            } else if (payMode == "30") {
                if (!openid.empty()) bizContent["unionUserId"] = openid;
            }
        } else {
            // 扫码
            serviceNo = "scanPay";
            bizContent["requestNo"] = req.orderId;
            bizContent["payeeMerchantNo"] = merchantNo;
            bizContent["orderDesc"] = req.subject.empty() ? "商品" : req.subject;
            bizContent["amount"] = fmtAmount(req.amount);
            bizContent["bankType"] = bankType;
            bizContent["notifyUrl"] = req.notifyUrl;
        }

        auto result = executeRequest(p, serviceNo, bizContent);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        if (serviceNo == "scanPay") {
            r.payUrl = result.data.get("qrCode", "").asString();
        } else if (serviceNo == "cashierPay") {
            r.payUrl = result.data.get("payData", "").asString();
        } else if (serviceNo == "jsPay") {
            r.payUrl = result.data.get("payData", "").asString();
        }
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        auto signIt = params.find("sign");
        auto bizJsonIt = params.find("bizResponseJson");
        if (signIt == params.end() || bizJsonIt == params.end()) return r;

        std::string publicKey = channelParams.get("productkey", "").asString();
        std::string signContent = buildSignContent(params);
        bool verified = RsaUtils::verifySha1(signContent, signIt->second, normalizePublicKey(publicKey));
        r.verified = verified;
        if (!verified) return r;

        Json::Value bizJson;
        if (!Json::Reader().parse(bizJsonIt->second, bizJson)) return r;

        std::string state = bizJson.get("state", "").asString();
        r.paid = (state == "SUCCESS");
        r.orderId = bizJson.get("requestNo", "").asString();
        r.channelOrderNo = bizJson.get("tradeSn", "").asString();
        r.buyerId = bizJson.get("openId", bizJson.get("userId", "")).asString();
        try { r.paidAmount = std::stod(bizJson.get("amount", "0").asString()); } catch (...) {}
        r.responseText = r.paid ? "success" : "fail";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string merchantNo = p.get("appmchid", "").asString();

        Json::Value bizContent;
        bizContent["requestNo"] = req.refundNo;
        bizContent["origRequestNo"] = req.orderId;
        bizContent["origTradeSn"] = req.channelOrderNo;
        bizContent["amount"] = fmtAmount(req.refundAmount);
        bizContent["reason"] = "申请退款";
        bizContent["isDivision"] = "N";

        auto result = executeRequest(p, "refund", bizContent);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = result.data.get("refundSn", "").asString();
        return r;
    }

    ChannelTransferResult transfer(const ChannelTransferRequest &req) override {
        ChannelTransferResult r;
        auto &p = req.channelParams;
        std::string merchantNo = p.get("appmchid", "").asString();

        Json::Value bizContent;
        bizContent["requestNo"] = req.transferNo;
        bizContent["merchantNo"] = merchantNo;
        bizContent["amount"] = fmtAmount(req.amount);
        bizContent["orderNote"] = req.remark.empty() ? "转账" : req.remark;
        bizContent["bankAccountNo"] = req.accountNo;
        bizContent["bankAccountName"] = req.accountName;
        bizContent["notifyUrl"] = req.notifyUrl;

        auto result = executeRequest(p, "paymentRequest", bizContent);
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.state = 0; // Processing
        r.channelTransferNo = result.data.get("tradeSn", "").asString();
        return r;
    }

private:
    static constexpr const char* GATEWAY_URL = "https://eqt.ysepay.com/api/trade";

    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        Json::Value data;
    };

    static ApiResponse executeRequest(const Json::Value &p, const std::string &serviceNo, const Json::Value &bizContent) {
        ApiResponse ar;
        std::string srcMerchantNo = p.get("appid", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();
        std::string publicKey = p.get("productkey", "").asString();

        // Build params
        std::map<std::string, std::string> params;
        params["requestId"] = generateRequestId();
        params["srcMerchantNo"] = srcMerchantNo;
        params["version"] = "v2.0.0";
        params["charset"] = "UTF-8";
        params["serviceNo"] = serviceNo;
        params["signType"] = "RSA";

        Json::FastWriter fw;
        params["bizReqJson"] = fw.write(bizContent);

        // Sign
        std::string signContent = buildSignContent(params);
        params["sign"] = RsaUtils::signSha1(signContent, normalizePrivateKey(privateKey));

        // Form POST
        std::string formBody = buildFormBody(params);
        auto resp = SyncHttp::postForm(GATEWAY_URL, formBody);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { ar.errMsg = "响应解析失败"; return ar; }

        std::string code = result.get("code", "").asString();
        if (code == "SYS000") {
            // Verify response sign
            if (result.isMember("sign")) {
                std::string signData = buildSignContent(result);
                std::string sign = result.get("sign", "").asString();
                bool verified = RsaUtils::verifySha1(signData, sign, normalizePublicKey(publicKey));
                if (!verified) { ar.errMsg = "返回数据验签失败"; return ar; }
            }
            ar.success = true;
            ar.data = result["bizResponseJson"];
        } else {
            ar.errMsg = result.get("msg", "系统异常").asString();
        }
        return ar;
    }

    static std::string buildSignContent(const Json::Value &v) {
        std::map<std::string, std::string> m;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it->isString()) m[it.name()] = it->asString();
            else if (it->isInt()) m[it.name()] = std::to_string(it->asInt());
            else if (it->isInt64()) m[it.name()] = std::to_string(it->asInt64());
            else if (it->isDouble()) m[it.name()] = fmtAmount(it->asDouble());
            else if (it->isBool()) m[it.name()] = it->asBool() ? "1" : "0";
            else if (it->isObject()) {
                Json::FastWriter fw;
                m[it.name()] = fw.write(*it);
            }
        }
        return buildSignContent(m);
    }

    static std::string buildSignContent(const std::map<std::string, std::string> &params) {
        std::vector<std::string> keys;
        for (auto &kv : params) {
            if (kv.first == "sign") continue;
            if (kv.second.empty()) continue;
            if (kv.second[0] == '@') continue;
            keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());

        std::string s;
        for (auto &k : keys) {
            auto it = params.find(k);
            if (it != params.end()) {
                s += k + "=" + it->second + "&";
            }
        }
        if (!s.empty()) s = s.substr(0, s.length() - 1);
        return s;
    }

    static std::string getBankType(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "1902000";
        if (payType == "bank" || payType == "bank_jsapi") return "9001002";
        return "1903000"; // alipay
    }

    static std::string getPayMode(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) {
            if (payType.find("lite") != std::string::npos || payType.find("mini") != std::string::npos) return "29";
            return "28"; // 公众号JSAPI
        }
        if (payType == "bank" || payType == "bank_jsapi") return "30";
        return "26"; // alipay JSAPI
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string generateRequestId() {
        unsigned char buf[16];
        RAND_bytes(buf, 16);
        std::ostringstream oss;
        for (int i = 0; i < 16; i++) oss << std::hex << std::setfill('0') << std::setw(2) << (int)buf[i];
        return oss.str();
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + urlEncode(kv.second);
        }
        return s;
    }

    static std::string urlEncode(const std::string &s) {
        std::string result;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                result += buf;
            }
        }
        return result;
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
};

REGISTER_CHANNEL_PLUGIN(YseqtPlugin);
