#pragma once
#include "ChannelPlugin.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include "../common/Md5Utils.h"
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class ChinaUmsPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "chinaums"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "AppId", "input");
        add("appkey", "AppKey", "input");
        add("appmchid", "商户号mid", "input");
        add("appurl", "终端号tid", "input");
        add("appsecret", "通讯密钥", "input");
        add("msgsrcid", "来源编号", "input", "", "4位来源编号，拼接到订单号前");
        add("appswitch", "环境选择", "select", "0", "0=生产环境 1=测试环境");
        add("apptype", "支付方式", "input", "1", "支付宝/微信：1扫码 2H5；微信：3H5转小程序");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() ||
            p.get("appmchid", "").asString().empty() || p.get("appurl", "").asString().empty()) {
            result.errMsg = "银联商务参数不完整(appid/appkey/appmchid/appurl)";
            return result;
        }
        std::string payKind = choosePayKind(req.payType, p.get("apptype", "1").asString());
        std::string path = choosePath(payKind);
        bool isGetPay = (payKind == "alipay_h5" || payKind == "wx_h5" || payKind == "wx_minipay");
        Json::Value body = buildOrderBody(req, p, isGetPay);
        if (isGetPay) {
            body["instMid"] = "H5DEFAULT";
            body["merOrderId"] = p.get("msgsrcid", "").asString() + req.orderId;
            body["orderDesc"] = req.subject.empty() ? "商品" : req.subject;
            if (payKind == "wx_h5") {
                body["sceneType"] = "AND_WAP";
                body["merAppName"] = "WePay";
                body["merAppId"] = req.returnUrl;
            }
            std::string url = buildGetUrl(p, path, body);
            result.success = true;
            result.payUrl = url;
            result.rawResponse = url;
            return result;
        }

        body["instMid"] = "QRPAYDEFAULT";
        body["billNo"] = p.get("msgsrcid", "").asString() + req.orderId;
        body["billDate"] = currentDate();
        body["billDesc"] = req.subject.empty() ? "商品" : req.subject;
        auto resp = post(p, path, body);
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { result.errMsg = "银联商务响应解析失败"; return result; }
        if (j.get("errCode", "").asString() != "SUCCESS") {
            result.errMsg = j.get("errMsg", j.get("errInfo", "银联商务下单失败").asString()).asString();
            return result;
        }
        result.success = true;
        result.qrCode = j.get("billQRCode", "").asString();
        result.payUrl = result.qrCode;
        result.channelOrderNo = j.get("billNo", "").asString();
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "SUCCESS";
        std::string key = channelParams.get("appsecret", "").asString();
        auto signIt = params.find("sign");
        if (key.empty() || signIt == params.end()) return r;
        std::string signType = get(params, "signType");
        std::string expected = callbackSign(params, key, signType);
        r.verified = (expected == signIt->second);
        if (!r.verified) { r.responseText = "FAIL"; return r; }
        std::string status = get(params, "status");
        r.paid = (status == "TRADE_SUCCESS" || status == "SUCCESS" || status == "PAID");
        r.orderId = stripPrefix(get(params, "merOrderId"), channelParams.get("msgsrcid", "").asString());
        if (r.orderId.empty()) r.orderId = stripPrefix(get(params, "billNo"), channelParams.get("msgsrcid", "").asString());
        r.channelOrderNo = get(params, "targetOrderId");
        if (r.channelOrderNo.empty()) r.channelOrderNo = get(params, "seqId");
        try { r.paidAmount = std::stod(get(params, "totalAmount")) / 100.0; } catch (...) {}
        return r;
    }

private:
    static Json::Value buildOrderBody(const ChannelOrderRequest &req, const Json::Value &p, bool h5) {
        Json::Value body;
        body["msgId"] = randomHex();
        body["requestTimestamp"] = currentDateTime();
        body["mid"] = p.get("appmchid", "").asString();
        body["tid"] = p.get("appurl", "").asString();
        body["totalAmount"] = (Json::Int64)std::llround(req.amount * 100.0);
        body["notifyUrl"] = req.notifyUrl;
        body["returnUrl"] = req.returnUrl;
        body["clientIp"] = req.clientIp;
        if (!h5) body["returnUrl"] = req.returnUrl;
        return body;
    }

    static SyncHttp::Response post(const Json::Value &p, const std::string &path, const Json::Value &body) {
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string json = Json::writeString(wb, body);
        long long ts = std::time(nullptr);
        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";
        headers["Authorization"] = authorization(p, json, ts);
        return SyncHttp::postJson(gateway(p) + path, json, headers);
    }

    static std::string buildGetUrl(const Json::Value &p, const std::string &path, const Json::Value &body) {
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string content = Json::writeString(wb, body);
        long long ts = std::time(nullptr);
        std::string timestamp = formatTs(ts, "%Y%m%d%H%M%S");
        std::string nonce = randomHex();
        std::string sig = signature(p.get("appid", "").asString(), p.get("appkey", "").asString(), timestamp, nonce, content);
        return gateway(p) + path + "?authorization=OPEN-FORM-PARAM&appId=" + urlEncode(p.get("appid", "").asString()) +
               "&timestamp=" + timestamp + "&nonce=" + nonce + "&content=" + urlEncode(content) + "&signature=" + urlEncode(sig);
    }

    static std::string authorization(const Json::Value &p, const std::string &body, long long t) {
        std::string timestamp = formatTs(t, "%Y%m%d%H%M%S");
        std::string nonce = randomHex();
        std::string sig = signature(p.get("appid", "").asString(), p.get("appkey", "").asString(), timestamp, nonce, body);
        return "OPEN-BODY-SIG AppId=\"" + p.get("appid", "").asString() + "\", Timestamp=\"" + timestamp +
               "\", Nonce=\"" + nonce + "\", Signature=\"" + sig + "\"";
    }

    static std::string signature(const std::string &appid, const std::string &key, const std::string &timestamp,
                                 const std::string &nonce, const std::string &body) {
        std::string bodyHash = sha256Hex(body);
        std::string data = appid + timestamp + nonce + bodyHash;
        unsigned int len = 0;
        unsigned char out[EVP_MAX_MD_SIZE];
        HMAC(EVP_sha256(), key.data(), (int)key.size(), reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len);
        return RsaUtils::base64Encode(out, len);
    }

    static std::string callbackSign(const std::map<std::string, std::string> &params, const std::string &key, const std::string &signType) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        s += key;
        if (signType == "SHA256") return upper(sha256Hex(s));
        return upper(Md5Utils::md5(s));
    }

    static std::string choosePayKind(const std::string &payType, const std::string &apptype) {
        bool h5 = ("," + apptype + ",").find(",2,") != std::string::npos;
        bool mini = ("," + apptype + ",").find(",3,") != std::string::npos;
        if (payType == "wxpay" || payType == "wx_qr") return mini ? "wx_minipay" : (h5 ? "wx_h5" : "qrcode");
        if (payType == "bank" || payType == "ysf_qr") return "qrcode";
        return h5 ? "alipay_h5" : "qrcode";
    }

    static std::string choosePath(const std::string &kind) {
        if (kind == "alipay_h5") return "/v1/netpay/trade/h5-pay";
        if (kind == "wx_h5") return "/v1/netpay/wxpay/h5-pay";
        if (kind == "wx_minipay") return "/v1/netpay/wxpay/h5-to-minipay";
        return "/v1/netpay/bills/get-qrcode";
    }

    static std::string gateway(const Json::Value &p) {
        return p.get("appswitch", "0").asString() == "1" ? "https://test-api-open.chinaums.com" : "https://api-mop.chinaums.com";
    }

    static std::string currentDateTime() { return formatTs(std::time(nullptr), "%Y-%m-%d %H:%M:%S"); }
    static std::string currentDate() { return formatTs(std::time(nullptr), "%Y-%m-%d"); }

    static std::string formatTs(long long t, const char *fmt) {
        std::time_t tt = (std::time_t)t;
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), fmt, &tmv);
        return buf;
    }

    static std::string sha256Hex(const std::string &s) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
        std::ostringstream oss;
        for (unsigned char c : hash) oss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        return oss.str();
    }

    static std::string randomHex() {
        static const char cs[] = "0123456789abcdef";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < 32; ++i) s += cs[rng() % 16];
        return s;
    }

    static std::string stripPrefix(const std::string &s, const std::string &prefix) {
        if (!prefix.empty() && s.rfind(prefix, 0) == 0) return s.substr(prefix.size());
        return s;
    }

    static std::string upper(std::string s) {
        for (char &c : s) c = (char)std::toupper((unsigned char)c);
        return s;
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

REGISTER_CHANNEL_PLUGIN(ChinaUmsPlugin);
