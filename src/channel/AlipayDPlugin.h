#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AlipayDPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipayd"; }

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
        add("appkey", "支付宝公钥", "textarea", "", "填错可支付但无法回调；证书模式可留空");
        add("appsecret", "应用私钥", "textarea");
        add("appmchid", "子商户SMID", "input", "", "多个 SMID 可用英文逗号分隔");
        add("gateway", "网关地址", "input", "https://openapi.alipay.com/gateway.do");
        add("apptype", "可用接口", "input", "3", "1电脑网站 2手机网站 3当面付扫码 4当面付JS 5预授权 6APP 7JSAPI 8订单码，可逗号分隔");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = p.get("appsecret", p.get("private_key", "").asString()).asString();
        std::string smid = chooseSmid(p.get("appmchid", "").asString());
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty() || smid.empty()) {
            result.errMsg = "支付宝直付通参数不完整(appid/appsecret/appmchid)";
            return result;
        }

        std::string method = chooseMethod(req.payType, p.get("apptype", "3").asString());
        std::map<std::string, std::string> params = baseParams(appid, alipayMethod(method));
        params["notify_url"] = req.notifyUrl;
        if (!req.returnUrl.empty() && (method == "wap" || method == "page")) params["return_url"] = req.returnUrl;

        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["total_amount"] = fmtAmount(req.amount);
        biz["subject"] = req.subject.empty() ? "商品" : req.subject;
        if (method == "precreate") biz["product_code"] = "FACE_TO_FACE_PAYMENT";
        else if (method == "page") biz["product_code"] = "FAST_INSTANT_TRADE_PAY";
        else if (method == "app") biz["product_code"] = "QUICK_MSECURITY_PAY";
        else biz["product_code"] = "QUICK_WAP_WAY";
        biz["sub_merchant"]["merchant_id"] = smid;
        biz["settle_info"]["settle_period_time"] = "1d";
        Json::Value detail;
        detail["trans_in_type"] = "defaultSettle";
        detail["amount"] = fmtAmount(req.amount);
        biz["settle_info"]["settle_detail_infos"].append(detail);
        if (!req.clientIp.empty()) biz["business_params"]["mc_create_trade_ip"] = req.clientIp;

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        if (params["sign"].empty()) {
            result.errMsg = "支付宝直付通 RSA2 签名失败";
            return result;
        }

        if (method == "wap" || method == "page" || method == "app") {
            std::string qs = buildQuery(params);
            result.payUrl = gateway + "?" + qs;
            result.rawResponse = qs;
            result.success = true;
            return result;
        }

        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        result.rawResponse = resp.body;
        if (!resp.success || resp.status != 200) {
            result.errMsg = "支付宝直付通网关请求失败: " + resp.errMsg;
            return result;
        }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            result.errMsg = "支付宝直付通响应解析失败";
            return result;
        }
        auto &rc = j["alipay_trade_precreate_response"];
        if (rc.get("code", "").asString() != "10000") {
            result.errMsg = rc.get("msg", "").asString() + " / " + rc.get("sub_msg", "").asString();
            return result;
        }
        result.qrCode = rc.get("qr_code", "").asString();
        result.payUrl = result.qrCode;
        result.channelOrderNo = rc.get("trade_no", "").asString();
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
        if (signIt == params.end()) return result;
        result.verified = pubKey.empty() ? true : RsaUtils::verifySha256(buildSignString(params), signIt->second, pubKey);
        if (!result.verified) return result;
        std::string status = get(params, "trade_status");
        result.paid = (status == "TRADE_SUCCESS" || status == "TRADE_FINISHED");
        result.orderId = get(params, "out_trade_no");
        result.channelOrderNo = get(params, "trade_no");
        result.buyerId = get(params, "buyer_id");
        try { result.paidAmount = std::stod(get(params, "total_amount")); } catch (...) {}
        return result;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = p.get("appsecret", p.get("private_key", "").asString()).asString();
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty()) { r.errMsg = "支付宝直付通参数不完整"; return r; }
        std::map<std::string, std::string> params = baseParams(appid, "alipay.trade.refund");
        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["refund_amount"] = fmtAmount(req.refundAmount);
        biz["refund_reason"] = req.reason.empty() ? "退款" : req.reason;
        biz["out_request_no"] = req.refundNo;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "响应解析失败"; return r; }
        auto &rc = j["alipay_trade_refund_response"];
        if (rc.get("code", "").asString() != "10000") {
            r.errMsg = rc.get("msg", "").asString() + " / " + rc.get("sub_msg", "").asString();
            return r;
        }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = rc.get("trade_no", "").asString();
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

    static std::string chooseSmid(const std::string &smids) {
        auto pos = smids.find(',');
        return pos == std::string::npos ? smids : smids.substr(0, pos);
    }

    static std::string chooseMethod(const std::string &payType, const std::string &apptype) {
        if (payType == "ali_wap") return "wap";
        if (payType == "ali_page") return "page";
        if (payType == "ali_app") return "app";
        auto has = [&](const std::string &x) { return ("," + apptype + ",").find("," + x + ",") != std::string::npos; };
        if (has("3") || has("4") || has("8")) return "precreate";
        if (has("2")) return "wap";
        if (has("1")) return "page";
        if (has("6")) return "app";
        return "precreate";
    }

    static std::string alipayMethod(const std::string &method) {
        if (method == "wap") return "alipay.trade.wap.pay";
        if (method == "page") return "alipay.trade.page.pay";
        if (method == "app") return "alipay.trade.app.pay";
        return "alipay.trade.precreate";
    }

    static std::string currentTimestamp() {
        auto now = std::time(nullptr);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        return buf;
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string buildSignString(const std::map<std::string, std::string> &params) {
        std::string result;
        for (auto &[k, v] : params) {
            if (k == "sign" || k == "sign_type" || v.empty()) continue;
            if (!result.empty()) result += "&";
            result += k + "=" + v;
        }
        return result;
    }

    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string body;
        for (auto &[k, v] : params) {
            if (!body.empty()) body += "&";
            body += k + "=" + urlEncode(v);
        }
        return body;
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

REGISTER_CHANNEL_PLUGIN(AlipayDPlugin);
