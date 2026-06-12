// WePay-Cpp — 云闪付通道插件(银联标准接入)
// 标准线提标/获取订单 (5.1.0 协议)
// 签名: RSA (SHA256WithRSA)
// 文档: https://open.unionpay.com/
//
// channelParams 必填:
//   mer_id         商户号(15位)
//   cert_id        签名证书ID
//   private_key    商户签名私钥 PEM
//   public_key     银联公钥 PEM(验签)
//   gateway        网关地址，默认 https://gateway.95516.com/gateway/api
//   back_url       后台通知 URL (可覆盖系统默认)
#pragma once
#include "ChannelPlugin.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class UnionPayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "unionpay"; }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;

        std::string merId      = p.get("mer_id", "").asString();
        std::string certId     = p.get("cert_id", "").asString();
        std::string privateKey = p.get("private_key", "").asString();
        std::string gateway    = p.get("gateway",
            "https://gateway.95516.com/gateway/api/appTransReq.do").asString();

        if (merId.empty() || privateKey.empty()) {
            result.errMsg = "云闪付参数不完整(mer_id/private_key)"; return result;
        }

        // 银联标准参数(form 表单提交)
        std::map<std::string, std::string> params;
        params["version"]     = "5.1.0";
        params["encoding"]    = "UTF-8";
        params["signMethod"]  = "01";         // 01=RSA
        params["txnType"]     = "01";         // 01=消费
        params["txnSubType"]  = "01";         // 01=自助消费
        params["bizType"]     = "000201";     // 000201=B2C网关支付
        params["channelType"] = "07";         // 07=互联网
        params["accessType"]  = "0";          // 0=商户直连
        params["merId"]       = merId;
        params["orderId"]     = req.orderId;
        params["txnTime"]     = currentTxnTime();
        params["currencyCode"]= "156";        // 人民币
        params["txnAmt"]      = std::to_string((long)std::round(req.amount * 100));  // 分
        params["backUrl"]     = req.notifyUrl;
        if (!req.returnUrl.empty()) params["frontUrl"] = req.returnUrl;
        if (!certId.empty()) params["certId"] = certId;

        // 签名: 排序 + SHA256 摘要的 base64 十六进制再 RSA-SHA256 签名
        // 银联简化版: 直接对 key=value&key=value 做 SHA256WithRSA 签名
        std::string signStr = buildSignString(params);
        std::string signature = RsaUtils::signSha256(signStr, privateKey);
        if (signature.empty()) {
            result.errMsg = "云闪付签名失败"; return result;
        }
        params["signature"] = signature;

        std::string postBody;
        for (auto &[k, v] : params) {
            if (!postBody.empty()) postBody += "&";
            postBody += k + "=" + urlEncode(v);
        }

        auto resp = SyncHttp::postForm(gateway, postBody);
        result.rawResponse = resp.body;
        if (!resp.success) {
            result.errMsg = "云闪付请求失败: " + resp.errMsg; return result;
        }

        // 银联返回 key=value&key=value 格式
        auto m = parseKv(resp.body);
        std::string respCode = m["respCode"];
        if (respCode != "00") {
            result.errMsg = "云闪付返回: " + respCode + " " + m["respMsg"];
            return result;
        }

        // tn 字段是支付 token，客户端用此调起云闪付 App
        std::string tn = m["tn"];
        if (tn.empty()) { result.errMsg = "云闪付返回无 tn"; return result; }

        result.success = true;
        result.payUrl  = tn;             // 客户端使用 tn 调起
        result.qrCode  = tn;
        result.channelOrderNo = m["queryId"];
        result.extra["tn"] = tn;
        return result;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {

        ChannelNotifyResult result;
        result.responseText = "ok";

        std::string pubKey = channelParams.get("public_key", "").asString();
        auto signIt = params.find("signature");
        if (signIt == params.end()) { result.verified = false; return result; }

        // 构建签名串(排除 signature)
        std::string signStr;
        std::map<std::string, std::string> filtered;
        for (auto &[k, v] : params) {
            if (k == "signature" || v.empty()) continue;
            filtered[k] = v;
        }
        for (auto &[k, v] : filtered) {
            if (!signStr.empty()) signStr += "&";
            signStr += k + "=" + v;
        }

        if (!pubKey.empty()) {
            result.verified = RsaUtils::verifySha256(signStr, signIt->second, pubKey);
        } else {
            result.verified = true;  // 未配置公钥跳过验签
        }
        if (!result.verified) return result;

        // 云闪付 respCode=00 表示支付成功
        auto itResp = params.find("respCode");
        if (itResp != params.end() && itResp->second == "00") result.paid = true;

        auto it1 = params.find("orderId");
        if (it1 != params.end()) result.orderId = it1->second;
        auto it2 = params.find("queryId");
        if (it2 != params.end()) result.channelOrderNo = it2->second;
        auto it3 = params.find("txnAmt");
        if (it3 != params.end()) {
            try { result.paidAmount = std::stol(it3->second) / 100.0; } catch (...) {}
        }
        return result;
    }

private:
    static std::string currentTxnTime() {
        auto now = std::time(nullptr);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char buf[20];
        std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &t);
        return buf;
    }
    static std::string buildSignString(const std::map<std::string, std::string> &params) {
        std::string r;
        for (auto &[k, v] : params) {
            if (k == "signature" || v.empty()) continue;
            if (!r.empty()) r += "&";
            r += k + "=" + v;
        }
        return r;
    }
    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else oss << '%' << std::uppercase << std::hex
                     << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }
    // 解析 key=value&key=value 格式
    static std::map<std::string, std::string> parseKv(const std::string &s) {
        std::map<std::string, std::string> m;
        size_t i = 0;
        while (i < s.size()) {
            size_t eq = s.find('=', i);
            if (eq == std::string::npos) break;
            size_t amp = s.find('&', eq + 1);
            if (amp == std::string::npos) amp = s.size();
            std::string k = s.substr(i, eq - i);
            std::string v = s.substr(eq + 1, amp - eq - 1);
            m[k] = urlDecode(v);
            i = amp + 1;
        }
        return m;
    }
    static std::string urlDecode(const std::string &s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                int v = 0;
                std::istringstream(s.substr(i + 1, 2)) >> std::hex >> v;
                out += (char)v; i += 2;
            } else if (s[i] == '+') out += ' ';
            else out += s[i];
        }
        return out;
    }
};

REGISTER_CHANNEL_PLUGIN(UnionPayPlugin);
