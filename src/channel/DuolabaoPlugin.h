#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include <openssl/sha.h>
#include "../common/SyncHttp.h"

class DuolabaoPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "duolabao"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("agentNum", "代理商编号", "input", "", "非代理商不需要填写");
        add("customerNum", "商户编号", "input");
        add("shopNum", "店铺编号", "input", "", "可留空");
        add("accessKey", "公钥", "input");
        add("secretKey", "私钥", "input");
        add("apptype", "支付方式", "input", "1", "支付宝/微信：1扫码 2JS");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        if (p.get("accessKey", "").asString().empty() || p.get("secretKey", "").asString().empty() ||
            p.get("customerNum", "").asString().empty()) {
            result.errMsg = "哆啦宝参数不完整(accessKey/secretKey/customerNum)";
            return result;
        }
        bool js = shouldJs(req.payType, p.get("apptype", "1").asString());
        Json::Value body = baseOrder(req, p);
        std::string path = "/api/generateQRCodeUrl";
        if (js) {
            path = "/api/createPayWithCheck";
            body["bankType"] = mapBankType(req.payType);
            body["paySource"] = body["bankType"];
            body["authCode"] = p.get("authCode", p.get("openid", "").asString()).asString();
            body["payType"] = "ACTIVE";
            std::string appid = p.get("sub_appid", "").asString();
            if (!appid.empty()) { body["appId"] = appid; body["subAppId"] = appid; }
        }
        auto resp = submit(p, path, body);
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { result.errMsg = "哆啦宝响应解析失败"; return result; }
        bool ok = (j.get("success", false).asBool() || j.get("result", false).asBool() || j.get("result", "").asString() == "success");
        if (!ok) {
            result.errMsg = j.get("errorMsg", j.get("msg", j.get("message", "哆啦宝下单失败").asString()).asString()).asString();
            return result;
        }
        result.success = true;
        result.channelOrderNo = j.get("orderNum", "").asString();
        if (js) {
            result.extra["payinfo"] = Json::writeString(Json::StreamWriterBuilder(), j["bankRequest"]);
            if (j["bankRequest"].isMember("TRADENO")) result.channelOrderNo = j["bankRequest"]["TRADENO"].asString();
        } else {
            std::string url = j.get("url", j["data"].get("url", "").asString()).asString();
            result.payUrl = url;
            result.qrCode = url;
        }
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &rawBody,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "error";
        std::string timestamp = get(params, "timestamp");
        std::string token = get(params, "token");
        if (timestamp.empty()) timestamp = get(params, "Timestamp");
        if (token.empty()) token = get(params, "Token");
        std::string expected = makeToken(channelParams.get("secretKey", "").asString(), timestamp, "", rawBody);
        r.verified = (!timestamp.empty() && !token.empty() && expected == token);
        if (!r.verified) return r;
        Json::Value j;
        if (!Json::Reader().parse(rawBody, j)) return r;
        r.paid = (j.get("status", "").asString() == "SUCCESS");
        r.orderId = j.get("requestNum", "").asString();
        r.channelOrderNo = j.get("orderNum", "").asString();
        r.buyerId = j.get("subOpenId", "").asString();
        try { r.paidAmount = std::stod(j.get("orderAmount", "0").asString()); } catch (...) {}
        r.responseText = r.paid ? "success" : "error";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        Json::Value body;
        body["requestVersion"] = "V4.0";
        body["agentNum"] = p.get("agentNum", "").asString();
        body["customerNum"] = p.get("customerNum", "").asString();
        body["shopNum"] = p.get("shopNum", "").asString();
        body["requestNum"] = req.orderId;
        body["refundPartAmount"] = fmtAmount(req.refundAmount);
        body["refundRequestNum"] = req.refundNo;
        body["extMap"]["refund_status_type"] = "1";
        auto resp = submit(p, "/api/refundByRequestNum", body);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "哆啦宝退款响应解析失败"; return r; }
        bool ok = (j.get("success", false).asBool() || j.get("result", false).asBool());
        if (!ok) { r.errMsg = j.get("errorMsg", j.get("msg", "哆啦宝退款失败").asString()).asString(); return r; }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = j.get("orderNum", "").asString();
        return r;
    }

private:
    static Json::Value baseOrder(const ChannelOrderRequest &req, const Json::Value &p) {
        Json::Value body;
        body["version"] = "V4.0";
        body["agentNum"] = p.get("agentNum", "").asString();
        body["customerNum"] = p.get("customerNum", "").asString();
        body["shopNum"] = p.get("shopNum", "").asString();
        body["requestNum"] = req.orderId;
        body["orderAmount"] = fmtAmount(req.amount);
        body["subOrderType"] = "NORMAL";
        body["orderType"] = "SALES";
        body["timeExpire"] = expireTime();
        body["businessType"] = "QRCODE_TRAD";
        body["payModel"] = "ONCE";
        body["source"] = "API";
        body["callbackUrl"] = req.notifyUrl;
        body["completeUrl"] = req.returnUrl;
        body["clientIp"] = req.clientIp;
        return body;
    }

    static SyncHttp::Response submit(const Json::Value &p, const std::string &path, const Json::Value &body) {
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);
        std::string ts = std::to_string(std::time(nullptr));
        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json";
        headers["accessKey"] = p.get("accessKey", "").asString();
        headers["timestamp"] = ts;
        headers["token"] = makeToken(p.get("secretKey", "").asString(), ts, path, bodyStr);
        return SyncHttp::postJson("https://openapi.duolabao.com" + path, bodyStr, headers);
    }

    static std::string makeToken(const std::string &secret, const std::string &timestamp,
                                 const std::string &path, const std::string &body) {
        std::string s = "secretKey=" + secret + "&timestamp=" + timestamp;
        if (!path.empty()) s += "&path=" + path;
        if (!body.empty()) s += "&body=" + body;
        return upper(sha1Hex(s));
    }

    static bool shouldJs(const std::string &payType, const std::string &apptype) {
        return (payType == "ali_jsapi" || payType == "wx_jsapi" || ("," + apptype + ",").find(",2,") != std::string::npos);
    }

    static std::string mapBankType(const std::string &payType) {
        if (payType == "wxpay" || payType == "wx_jsapi") return "WX";
        if (payType == "qqpay") return "QQ";
        if (payType == "bank") return "UNIONPAY";
        if (payType == "jdpay") return "JD";
        return "ALIPAY";
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string expireTime() {
        std::time_t t = std::time(nullptr) + 7200;
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        return buf;
    }

    static std::string sha1Hex(const std::string &s) {
        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
        std::ostringstream oss;
        for (unsigned char c : hash) oss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        return oss.str();
    }

    static std::string upper(std::string s) {
        for (char &c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(DuolabaoPlugin);
