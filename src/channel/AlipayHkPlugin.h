#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class AlipayHkPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipayhk"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "Partner ID", "input");
        add("appkey", "MD5 Key", "input");
        add("appswitch", "支付时选择钱包类型", "select", "0", "0=默认Alipay，1=允许选择Alipay/AlipayHK");
        add("apptype", "支付方式", "input", "1", "1=PC支付 2=WAP支付 3=APP支付");
        add("gateway", "网关地址", "input", "https://intlmapi.alipay.com/gateway.do");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string partner = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();
        std::string gateway = p.get("gateway", "https://intlmapi.alipay.com/gateway.do").asString();
        if (partner.empty() || key.empty()) {
            result.errMsg = "AlipayHK 参数不完整(appid/appkey)";
            return result;
        }
        std::string method = chooseMethod(req.payType, p.get("apptype", "1").asString());
        std::map<std::string, std::string> params;
        params["service"] = serviceName(method);
        params["partner"] = partner;
        params["notify_url"] = req.notifyUrl;
        params["return_url"] = req.returnUrl;
        params["out_trade_no"] = req.orderId;
        params["subject"] = req.subject.empty() ? "商品" : req.subject;
        params["currency"] = "HKD";
        params["rmb_fee"] = fmtAmount(req.amount);
        params["refer_url"] = req.returnUrl.empty() ? req.notifyUrl : req.returnUrl;
        params["product_code"] = "NEW_WAP_OVERSEAS_SELLER";
        params["trade_information"] = "{\"business_type\":\"5\",\"other_business_type\":\"在线充值\"}";
        params["_input_charset"] = "utf-8";
        if (method == "pc") {
            params["qr_pay_mode"] = "4";
            params["qrcode_width"] = "230";
        }
        if (method == "app") {
            params["payment_type"] = "1";
            params["seller_id"] = partner;
            params["forex_biz"] = "FP";
        }
        std::string wallet = p.get("payment_inst", "").asString();
        if (!wallet.empty()) params["payment_inst"] = wallet;
        params["sign"] = makeSign(params, key);
        params["sign_type"] = "MD5";

        std::string query = buildQuery(params);
        result.success = true;
        result.rawResponse = query;
        if (method == "app") {
            result.payUrl = "alipays://platformapi/startApp?appId=20000125&orderSuffix=" + urlEncode(query) + "#Intent;scheme=alipays;package=com.eg.android.AlipayGphone;end";
        } else {
            result.payUrl = gateway + "?_input_charset=utf-8&" + query;
        }
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult result;
        result.responseText = "success";
        std::string key = channelParams.get("appkey", "").asString();
        auto signIt = params.find("sign");
        if (key.empty() || signIt == params.end()) { result.responseText = "fail"; return result; }
        result.verified = (makeSign(params, key) == signIt->second);
        if (!result.verified) { result.responseText = "fail"; return result; }
        std::string status = get(params, "trade_status");
        result.paid = (status == "TRADE_FINISHED" || status == "TRADE_SUCCESS");
        result.orderId = get(params, "out_trade_no");
        result.channelOrderNo = get(params, "trade_no");
        result.buyerId = get(params, "buyer_id");
        try { result.paidAmount = std::stod(get(params, "total_fee")); } catch (...) {}
        return result;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult result;
        auto &p = req.channelParams;
        std::string partner = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();
        std::string gateway = p.get("gateway", "https://intlmapi.alipay.com/gateway.do").asString();
        if (partner.empty() || key.empty()) { result.errMsg = "AlipayHK 参数不完整"; return result; }
        std::map<std::string, std::string> params;
        params["service"] = "forex_refund";
        params["partner"] = partner;
        params["out_return_no"] = req.refundNo;
        params["out_trade_no"] = req.orderId;
        params["return_rmb_amount"] = fmtAmount(req.refundAmount);
        params["currency"] = "HKD";
        params["gmt_return"] = currentTimestamp();
        params["_input_charset"] = "utf-8";
        params["sign"] = makeSign(params, key);
        params["sign_type"] = "MD5";
        auto resp = SyncHttp::postForm(gateway + "?_input_charset=utf-8", buildQuery(params));
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        if (resp.body.find("<is_success>T</is_success>") != std::string::npos || resp.body.find("<is_success><![CDATA[T]]></is_success>") != std::string::npos) {
            result.success = true;
            result.state = 1;
        } else {
            result.errMsg = resp.body.empty() ? "AlipayHK 退款失败" : resp.body;
        }
        return result;
    }

private:
    static std::string chooseMethod(const std::string &payType, const std::string &apptype) {
        if (payType == "ali_wap") return "wap";
        if (payType == "ali_app") return "app";
        if (("," + apptype + ",").find(",2,") != std::string::npos) return "wap";
        if (("," + apptype + ",").find(",3,") != std::string::npos) return "app";
        return "pc";
    }

    static std::string serviceName(const std::string &method) {
        if (method == "wap") return "create_forex_trade_wap";
        if (method == "app") return "mobile.securitypay.pay";
        return "create_forex_trade";
    }

    static std::string makeSign(const std::map<std::string, std::string> &params, const std::string &key) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return Md5Utils::md5(s + key);
    }

    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string q;
        for (auto &kv : params) {
            if (!q.empty()) q += "&";
            q += kv.first + "=" + urlEncode(kv.second);
        }
        return q;
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
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

REGISTER_CHANNEL_PLUGIN(AlipayHkPlugin);
