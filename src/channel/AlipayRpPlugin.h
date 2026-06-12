#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AlipayRpPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipayrp"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "应用APPID", "input");
        add("appsecret", "应用私钥", "textarea");
        add("appkey", "支付宝公钥", "textarea", "", "用于资金单据状态变更通知验签");
        add("appmchid", "收款方支付宝UID", "input", "", "留空则需业务侧提供 payee_user_id");
        add("gateway", "网关地址", "input", "https://openapi.alipay.com/gateway.do");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = p.get("appsecret", p.get("private_key", "").asString()).asString();
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty()) {
            result.errMsg = "支付宝现金红包参数不完整(appid/appsecret)";
            return result;
        }
        std::string payerUid = p.get("payer_user_id", "").asString();
        Json::Value biz;
        biz["out_biz_no"] = req.orderId;
        biz["trans_amount"] = fmtAmount(req.amount);
        biz["product_code"] = "STD_RED_PACKET";
        biz["biz_scene"] = "PERSONAL_PAY";
        biz["order_title"] = req.subject.empty() ? "红包支付" : req.subject;
        Json::Value business;
        business["sub_biz_scene"] = "REDPACKET";
        if (!payerUid.empty()) business["payer_binded_alipay_uid"] = payerUid;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        biz["business_params"] = Json::writeString(wb, business);

        std::map<std::string, std::string> params = baseParams(appid, "alipay.fund.trans.app.pay");
        params["notify_url"] = req.notifyUrl;
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        if (params["sign"].empty()) {
            result.errMsg = "支付宝现金红包 RSA2 签名失败";
            return result;
        }
        std::string orderSuffix = buildQuery(params);
        result.payUrl = "alipays://platformapi/startApp?appId=20000125&orderSuffix=" + urlEncode(orderSuffix) + "#Intent;scheme=alipays;package=com.eg.android.AlipayGphone;end";
        result.rawResponse = orderSuffix;
        result.success = true;
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult result;
        result.responseText = "success";
        std::string pubKey = channelParams.get("appkey", channelParams.get("alipay_public_key", "").asString()).asString();
        auto signIt = params.find("sign");
        if (signIt != params.end() && !pubKey.empty()) {
            result.verified = RsaUtils::verifySha256(buildSignString(params), signIt->second, pubKey);
        } else {
            result.verified = true;
        }
        if (!result.verified) { result.responseText = "fail"; return result; }
        Json::Value biz;
        std::string bizContent = get(params, "biz_content");
        if (!bizContent.empty()) Json::Reader().parse(bizContent, biz);
        if (get(params, "msg_method") == "alipay.fund.trans.order.changed" && biz.isObject()) {
            result.orderId = biz.get("out_biz_no", "").asString();
            result.channelOrderNo = biz.get("order_id", "").asString();
            result.paid = (biz.get("status", "").asString() == "SUCCESS" &&
                           biz.get("product_code", "").asString() == "STD_RED_PACKET" &&
                           biz.get("biz_scene", "").asString() == "PERSONAL_PAY");
            try { result.paidAmount = std::stod(biz.get("trans_amount", "0").asString()); } catch (...) {}
        }
        return result;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = p.get("appsecret", p.get("private_key", "").asString()).asString();
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty() || req.channelOrderNo.empty()) {
            r.errMsg = "支付宝现金红包退款参数不完整";
            return r;
        }
        Json::Value biz;
        biz["out_request_no"] = req.refundNo;
        biz["order_id"] = req.channelOrderNo;
        biz["refund_amount"] = fmtAmount(req.refundAmount);
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::map<std::string, std::string> params = baseParams(appid, "alipay.fund.trans.refund");
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "响应解析失败"; return r; }
        auto &rc = j["alipay_fund_trans_refund_response"];
        if (rc.get("code", "").asString() != "10000") {
            r.errMsg = rc.get("msg", "").asString() + " / " + rc.get("sub_msg", "").asString();
            return r;
        }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = rc.get("refund_order_id", "").asString();
        return r;
    }

private:
    static std::map<std::string, std::string> baseParams(const std::string &appid, const std::string &method) {
        std::map<std::string, std::string> p;
        p["app_id"] = appid;
        p["method"] = method;
        p["format"] = "JSON";
        p["charset"] = "utf-8";
        p["sign_type"] = "RSA2";
        p["timestamp"] = currentTimestamp();
        p["version"] = "1.0";
        return p;
    }

    static std::string currentTimestamp() {
        auto t = std::time(nullptr);
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

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string buildSignString(const std::map<std::string, std::string> &params) {
        std::string result;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            if (!result.empty()) result += "&";
            result += kv.first + "=" + kv.second;
        }
        return result;
    }

    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string q;
        for (auto &kv : params) {
            if (!q.empty()) q += "&";
            q += kv.first + "=" + urlEncode(kv.second);
        }
        return q;
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

REGISTER_CHANNEL_PLUGIN(AlipayRpPlugin);
