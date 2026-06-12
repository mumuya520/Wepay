#pragma once
#include "ChannelPlugin.h"
#include <cmath>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AllinPayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "allinpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appmchid", "商户号", "input");
        add("appid", "应用ID", "input");
        add("appkey", "通联公钥", "textarea");
        add("appsecret", "商户私钥", "textarea");
        add("orgid", "代理商商户号", "input", "", "仅代理商需要填写");
        add("apptype", "支付方式", "input", "1", "1=扫码支付 2=JS支付/公众号/小程序");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string cusid = p.get("appmchid", "").asString();
        std::string appid = p.get("appid", "").asString();
        std::string priKey = p.get("appsecret", "").asString();
        if (cusid.empty() || appid.empty() || priKey.empty()) {
            result.errMsg = "通联参数不完整(appmchid/appid/appsecret)";
            return result;
        }

        std::map<std::string, std::string> params = publicParams(p, "11");
        params["trxamt"] = std::to_string((long long)std::llround(req.amount * 100.0));
        params["reqsn"] = req.orderId;
        params["paytype"] = mapPayType(req.payType, p.get("apptype", "1").asString());
        params["body"] = req.subject.empty() ? "商品" : req.subject;
        params["validtime"] = "30";
        params["notify_url"] = req.notifyUrl;
        params["cusip"] = req.clientIp;
        std::string subAppid = p.get("sub_appid", "").asString();
        std::string acct = p.get("acct", "").asString();
        if (!subAppid.empty()) params["sub_appid"] = subAppid;
        if (!acct.empty()) {
            params["acct"] = acct;
            params["front_url"] = req.returnUrl;
        }
        params["sign"] = RsaUtils::signSha1(buildSignString(params), normalizePrivateKey(priKey));

        auto resp = SyncHttp::postForm("https://vsp.allinpay.com/apiweb/unitorder/pay", buildQuery(params));
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = "通联请求失败: " + resp.errMsg; return result; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { result.errMsg = "通联响应解析失败"; return result; }
        if (j.get("retcode", "").asString() != "SUCCESS") {
            result.errMsg = j.get("retmsg", "通联返回失败").asString();
            return result;
        }
        if (j.get("trxstatus", "").asString() != "0000") {
            result.errMsg = j.get("errmsg", "通联下单失败").asString();
            return result;
        }
        std::string payinfo = j.get("payinfo", "").asString();
        result.success = true;
        result.channelOrderNo = j.get("trxid", "").asString();
        result.rawResponse = resp.body;
        if (payinfo.size() > 0 && (payinfo[0] == '{' || payinfo[0] == '[')) result.extra["payinfo"] = payinfo;
        else { result.payUrl = payinfo; result.qrCode = payinfo; }
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";
        std::string pubKey = channelParams.get("appkey", "").asString();
        auto signIt = params.find("sign");
        if (!pubKey.empty() && signIt != params.end()) {
            r.verified = RsaUtils::verifySha1(buildSignString(params), signIt->second, normalizePublicKey(pubKey));
        } else {
            r.verified = (signIt != params.end());
        }
        if (!r.verified) { r.responseText = "fail"; return r; }
        r.paid = (get(params, "trxstatus") == "0000");
        r.orderId = get(params, "cusorderid");
        r.channelOrderNo = get(params, "trxid");
        r.buyerId = get(params, "acct");
        try { r.paidAmount = std::stod(get(params, "initamt")) / 100.0; } catch (...) {}
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string priKey = p.get("appsecret", "").asString();
        if (priKey.empty() || req.channelOrderNo.empty()) { r.errMsg = "通联退款参数不完整"; return r; }
        std::map<std::string, std::string> params = publicParams(p, "11");
        params["trxamt"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        params["reqsn"] = req.refundNo;
        params["oldtrxid"] = req.channelOrderNo;
        params["sign"] = RsaUtils::signSha1(buildSignString(params), normalizePrivateKey(priKey));
        auto resp = SyncHttp::postForm("https://vsp.allinpay.com/apiweb/tranx/refund", buildQuery(params));
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "通联退款响应解析失败"; return r; }
        if (j.get("retcode", "").asString() != "SUCCESS") {
            r.errMsg = j.get("retmsg", "通联退款失败").asString();
            return r;
        }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = j.get("trxid", "").asString();
        return r;
    }

private:
    static std::map<std::string, std::string> publicParams(const Json::Value &p, const std::string &version) {
        std::map<std::string, std::string> m;
        std::string orgid = p.get("orgid", "").asString();
        if (!orgid.empty()) m["orgid"] = orgid;
        m["appid"] = p.get("appid", "").asString();
        m["cusid"] = p.get("appmchid", "").asString();
        m["version"] = version;
        m["randomstr"] = randomStr(16);
        m["signtype"] = "RSA";
        return m;
    }

    static std::string mapPayType(const std::string &payType, const std::string &apptype) {
        bool js = ("," + apptype + ",").find(",2,") != std::string::npos;
        if (payType == "wxpay" || payType == "wx_qr") return js ? "W02" : "W01";
        if (payType == "qqpay") return "Q01";
        if (payType == "bank" || payType == "ysf_qr") return js ? "U02" : "U01";
        return js ? "A02" : "A01";
    }

    static std::string buildSignString(const std::map<std::string, std::string> &params) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string q;
        for (auto &kv : params) {
            if (!q.empty()) q += "&";
            q += kv.first + "=" + urlEncode(kv.second);
        }
        return q;
    }

    static std::string normalizePrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string body;
        for (size_t i = 0; i < key.size(); i += 64) body += key.substr(i, 64) + "\n";
        return "-----BEGIN RSA PRIVATE KEY-----\n" + body + "-----END RSA PRIVATE KEY-----\n";
    }

    static std::string normalizePublicKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string body;
        for (size_t i = 0; i < key.size(); i += 64) body += key.substr(i, 64) + "\n";
        return "-----BEGIN PUBLIC KEY-----\n" + body + "-----END PUBLIC KEY-----\n";
    }

    static std::string randomStr(int len) {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < len; ++i) s += cs[rng() % (sizeof(cs) - 1)];
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

REGISTER_CHANNEL_PLUGIN(AllinPayPlugin);
