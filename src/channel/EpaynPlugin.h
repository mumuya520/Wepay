#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class EpaynPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "epayn"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appurl", "接口地址", "input", "", "必须以 http:// 或 https:// 开头，以 / 结尾");
        add("appid", "商户ID", "input");
        add("appkey", "平台公钥", "textarea");
        add("appsecret", "商户私钥", "textarea");
        add("appswitch", "是否使用mapi接口", "select", "1", "0=否，跳转 api/pay/submit（旧版）；1=是，请求 api/pay/create（推荐）");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string base = normalizeBase(p.get("appurl", "").asString());
        std::string pid = p.get("appid", "").asString();
        std::string pri = p.get("appsecret", "").asString();
        if (base.empty() || pid.empty() || pri.empty()) {
            result.errMsg = "彩虹易支付V2参数不完整(appurl/appid/appsecret)";
            return result;
        }
        std::map<std::string, std::string> params;
        if (p.get("appswitch", "0").asString() == "1") params["method"] = "web";
        params["type"] = mapPayType(req.payType);
        params["device"] = p.get("device", "pc").asString();
        params["clientip"] = req.clientIp;
        params["notify_url"] = req.notifyUrl;
        params["return_url"] = req.returnUrl;
        params["out_trade_no"] = stripPrefix(req.orderId);  // 去 W 前缀
        params["name"] = req.subject.empty() ? "商品" : req.subject;
        params["money"] = fmtAmount(req.amount);
        addSign(params, pid, pri);
        std::string body = buildQuery(params);
        result.rawResponse = body;

        if (p.get("appswitch", "0").asString() != "1") {
            result.success = true;
            // 加 [POST] 标记，让收银台用 POST 表单提交（避免 GET 被上游拒绝）
            result.payUrl = "[POST]" + base + "api/pay/submit?" + body;
            return result;
        }
        auto resp = SyncHttp::postForm(base + "api/pay/create", body);
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { result.errMsg = "彩虹易支付V2响应解析失败"; return result; }
        if (j.get("code", -1).asInt() != 0) {
            result.errMsg = j.get("msg", "彩虹易支付V2下单失败").asString();
            return result;
        }
        result.success = true;
        std::string tradeNo = j.get("trade_no", "").asString();
        std::string payType = j.get("pay_type", "").asString();
        std::string payInfo = j.get("pay_info", "").asString();
        // 优先级：payInfo（jump/wxp） > 上游收银台跳转
        if (payType == "qrcode" && !payInfo.empty()) {
            // 直接用 wxp:// 这种 native 二维码 URL 时，仍优先跳转上游收银台
            if (!tradeNo.empty()) {
                result.payUrl = base + "Pay/console?trade_no=" + tradeNo;
            } else {
                result.qrCode = payInfo;
                result.payUrl = payInfo;
            }
        } else if (!payInfo.empty()) {
            result.payUrl = payInfo;
        } else if (!tradeNo.empty()) {
            result.payUrl = base + "Pay/console?trade_no=" + tradeNo;
        }
        result.channelOrderNo = tradeNo;
        result.extra["pay_type"] = payType;
        return result;
    }

    static std::string stripPrefix(const std::string &s) {
        size_t i = 0;
        while (i < s.size() && !std::isdigit((unsigned char)s[i])) ++i;
        return i == 0 ? s : s.substr(i);
    }

    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        std::map<std::string, std::string> params;
        params["trade_no"] = orderId;
        std::string raw, err;
        Json::Value j = execute(channelParams, "api/pay/query", params, raw, err);
        if (!err.empty()) return r;
        r.success = true;
        r.tradeState = j.get("status", 0).asInt() == 1 ? 1 : 0;
        r.channelOrderNo = j.get("trade_no", "").asString();
        try { r.paidAmount = std::stod(j.get("money", "0").asString()); } catch (...) {}
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";
        std::string pub = channelParams.get("appkey", "").asString();
        std::string sig = get(params, "sign");
        if (sig.empty() || pub.empty()) { r.verified = false; r.responseText = "fail"; return r; }
        try {
            long long ts = std::stoll(get(params, "timestamp"));
            if (std::llabs((long long)std::time(nullptr) - ts) > 300) { r.verified = false; r.responseText = "fail"; return r; }
        } catch (...) {
            r.verified = false; r.responseText = "fail"; return r;
        }
        r.verified = RsaUtils::verifySha256(signString(params), sig, normalizePublicKey(pub));
        if (!r.verified) { r.responseText = "fail"; return r; }
        r.paid = get(params, "trade_status") == "TRADE_SUCCESS";
        r.orderId = get(params, "out_trade_no");
        r.channelOrderNo = get(params, "trade_no");
        r.buyerId = get(params, "buyer");
        try { r.paidAmount = std::stod(get(params, "money")); } catch (...) {}
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        std::map<std::string, std::string> params;
        params["trade_no"] = req.channelOrderNo.empty() ? req.orderId : req.channelOrderNo;
        params["money"] = fmtAmount(req.refundAmount);
        params["out_refund_no"] = req.refundNo;
        std::string err;
        Json::Value j = execute(req.channelParams, "api/pay/refund", params, r.rawResponse, err);
        if (!err.empty()) { r.errMsg = err; return r; }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = j.get("refund_no", req.refundNo).asString();
        return r;
    }

private:
    static Json::Value execute(const Json::Value &p, const std::string &path, std::map<std::string, std::string> params,
                               std::string &raw, std::string &err) {
        std::string base = normalizeBase(p.get("appurl", "").asString());
        std::string pid = p.get("appid", "").asString();
        std::string pri = p.get("appsecret", "").asString();
        std::string pub = p.get("appkey", "").asString();
        if (base.empty() || pid.empty() || pri.empty()) { err = "彩虹易支付V2参数不完整"; return Json::Value(); }
        addSign(params, pid, pri);
        auto resp = SyncHttp::postForm(base + path, buildQuery(params));
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "彩虹易支付V2响应解析失败"; return Json::Value(); }
        if (j.get("code", -1).asInt() != 0) { err = j.get("msg", "彩虹易支付V2请求失败").asString(); return Json::Value(); }
        if (!pub.empty() && !RsaUtils::verifySha256(signString(jsonToMap(j)), j.get("sign", "").asString(), normalizePublicKey(pub))) {
            err = "彩虹易支付V2返回验签失败";
            return Json::Value();
        }
        return j;
    }

    static void addSign(std::map<std::string, std::string> &params, const std::string &pid, const std::string &pri) {
        params["pid"] = pid;
        params["timestamp"] = std::to_string(std::time(nullptr));
        params["sign"] = RsaUtils::signSha256(signString(params), normalizePrivateKey(pri));
        params["sign_type"] = "RSA";
    }

    static std::string signString(const std::map<std::string, std::string> &params) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::map<std::string, std::string> jsonToMap(const Json::Value &j) {
        std::map<std::string, std::string> m;
        for (auto &k : j.getMemberNames()) {
            if (j[k].isString()) m[k] = j[k].asString();
            else if (j[k].isNumeric()) m[k] = j[k].asString();
        }
        return m;
    }

    static std::string mapPayType(const std::string &payType) {
        if (payType == "wxpay" || payType == "wx_qr") return "wxpay";
        if (payType == "qqpay") return "qqpay";
        if (payType == "bank" || payType == "ysf_qr") return "bank";
        if (payType == "jdpay") return "jdpay";
        return "alipay";
    }

    static std::string normalizeBase(std::string s) { if (!s.empty() && s.back() != '/') s += '/'; return s; }
    static std::string normalizePrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string body; for (size_t i = 0; i < key.size(); i += 64) body += key.substr(i, 64) + "\n";
        return "-----BEGIN PRIVATE KEY-----\n" + body + "-----END PRIVATE KEY-----\n";
    }
    static std::string normalizePublicKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string body; for (size_t i = 0; i < key.size(); i += 64) body += key.substr(i, 64) + "\n";
        return "-----BEGIN PUBLIC KEY-----\n" + body + "-----END PUBLIC KEY-----\n";
    }
    static std::string fmtAmount(double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); }
    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string q; for (auto &kv : params) { if (!q.empty()) q += "&"; q += kv.first + "=" + urlEncode(kv.second); } return q;
    }
    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss; for (unsigned char c : s) { if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c; else oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c; } return oss.str();
    }
    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) { auto it = m.find(k); return it == m.end() ? "" : it->second; }
};

REGISTER_CHANNEL_PLUGIN(EpaynPlugin);
