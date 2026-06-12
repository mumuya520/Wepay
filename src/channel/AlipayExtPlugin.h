// WePay-Cpp — 支付宝扩展插件：付款码 / JSAPI / WAP / APP
// 根据 params.pay_method 分发到不同 method:
//   "precreate"(default): alipay.trade.precreate       扫码
//   "bar":                alipay.trade.pay              条码支付(当面付)
//   "jsapi":              alipay.trade.create           创建订单用于JSAPI
//   "wap":                alipay.trade.wap.pay          手机网页
//   "page":               alipay.trade.page.pay         PC网页
//
// channelParams:
//   appid / private_key / alipay_public_key [/ gateway]
#pragma once
#include "ChannelPlugin.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AlipayExtPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipay_ext"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid",             "AppID",        "input",    "", "支付宝开放平台应用 APPID");
        add("private_key",       "应用私钥",     "textarea", "", "应用私钥 PEM(PKCS8)");
        add("alipay_public_key", "支付宝公钥",   "textarea", "", "支付宝公钥 PEM(回调验签)");
        add("gateway",           "网关地址",     "input",    "https://openapi.alipay.com/gateway.do", "正式/沙箱网关");
        add("pay_method",        "支付方式",     "select",   "precreate", "precreate=扫码 / bar=付款码 / jsapi=JSAPI / wap=手机网页 / page=PC网页");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;

        std::string appid      = p.get("appid", "").asString();
        std::string privateKey = p.get("private_key", "").asString();
        std::string gateway    = p.get("gateway",
            "https://openapi.alipay.com/gateway.do").asString();
        // 优先从 way_code 自动映射，其次从 channelParams 读
        std::string payMethod = wayCodeToMethod(req.payType);
        if (payMethod.empty()) payMethod = p.get("pay_method", "precreate").asString();

        if (appid.empty() || privateKey.empty()) {
            result.errMsg = "支付宝参数不完整"; return result;
        }

        std::map<std::string, std::string> params;
        params["app_id"]    = appid;
        params["format"]    = "JSON";
        params["charset"]   = "utf-8";
        params["sign_type"] = "RSA2";
        params["timestamp"] = currentTs();
        params["version"]   = "1.0";
        params["notify_url"]= req.notifyUrl;
        if (!req.returnUrl.empty()) params["return_url"] = req.returnUrl;

        // method + biz_content + product_code
        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["total_amount"] = fmtAmount(req.amount);
        biz["subject"]      = req.subject.empty() ? "商品" : req.subject;

        std::string method;
        std::string productCode;

        if (payMethod == "bar") {
            method = "alipay.trade.pay";
            productCode = "FACE_TO_FACE_PAYMENT";
            std::string authCode = p.get("auth_code", "").asString();
            if (authCode.empty()) { result.errMsg = "付款码支付缺少 auth_code"; return result; }
            biz["auth_code"] = authCode;
            biz["scene"] = "bar_code";
        } else if (payMethod == "jsapi") {
            method = "alipay.trade.create";
            productCode = "JSAPI_PAY";
            std::string buyerId = p.get("buyer_id", "").asString();
            if (buyerId.empty()) { result.errMsg = "JSAPI支付缺少 buyer_id"; return result; }
            biz["buyer_id"] = buyerId;
        } else if (payMethod == "wap") {
            method = "alipay.trade.wap.pay";
            productCode = "QUICK_WAP_WAY";
        } else if (payMethod == "page") {
            method = "alipay.trade.page.pay";
            productCode = "FAST_INSTANT_TRADE_PAY";
        } else {
            method = "alipay.trade.precreate";
            productCode = "FACE_TO_FACE_PAYMENT";
        }
        params["method"] = method;
        biz["product_code"] = productCode;

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);

        std::string signStr = buildSign(params);
        std::string signature = RsaUtils::signSha256(signStr, privateKey);
        if (signature.empty()) { result.errMsg = "RSA2签名失败"; return result; }
        params["sign"] = signature;

        // wap / page: 构造跳转 URL
        if (payMethod == "wap" || payMethod == "page") {
            std::string qs;
            for (auto &[k, v] : params) {
                if (!qs.empty()) qs += "&";
                qs += k + "=" + urlEncode(v);
            }
            result.payUrl = gateway + "?" + qs;
            result.success = true;
            return result;
        }

        // precreate / bar / jsapi: POST form
        std::string body;
        for (auto &[k, v] : params) {
            if (!body.empty()) body += "&";
            body += k + "=" + urlEncode(v);
        }
        auto resp = SyncHttp::postForm(gateway, body);
        result.rawResponse = resp.body;
        if (!resp.success || resp.status != 200) {
            result.errMsg = "支付宝请求失败: " + resp.errMsg; return result;
        }

        Json::Value j;
        Json::Reader().parse(resp.body, j);

        std::string respKey;
        if (payMethod == "bar")    respKey = "alipay_trade_pay_response";
        else if (payMethod == "jsapi") respKey = "alipay_trade_create_response";
        else                            respKey = "alipay_trade_precreate_response";

        auto &rc = j[respKey];
        std::string code = rc.get("code", "").asString();
        if (code != "10000") {
            // 用户支付中 code=10003
            if (code == "10003") {
                result.success = true;
                result.extra["paid"] = false;
                result.extra["need_query"] = true;
                result.errMsg = "用户支付中";
                return result;
            }
            result.errMsg = "支付宝返回: " + rc.get("msg", "").asString() + " / " +
                            rc.get("sub_msg", "").asString();
            return result;
        }

        if (payMethod == "bar") {
            result.success = true;
            result.extra["paid"] = true;
            result.channelOrderNo = rc.get("trade_no", "").asString();
            result.extra["buyer_user_id"] = rc.get("buyer_user_id", "").asString();
        } else if (payMethod == "jsapi") {
            result.success = true;
            result.channelOrderNo = rc.get("trade_no", "").asString();
            result.extra["trade_no"] = result.channelOrderNo;
        } else {
            // precreate
            std::string qr = rc.get("qr_code", "").asString();
            result.success = !qr.empty();
            result.payUrl = qr;
            result.qrCode = qr;
        }
        return result;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";
        std::string pubKey = channelParams.get("alipay_public_key", "").asString();

        auto signIt = params.find("sign");
        if (signIt == params.end()) { r.verified = false; return r; }

        std::string signStr = buildSign(params);
        if (!pubKey.empty()) {
            r.verified = RsaUtils::verifySha256(signStr, signIt->second, pubKey);
        } else r.verified = true;
        if (!r.verified) return r;

        auto statusIt = params.find("trade_status");
        if (statusIt != params.end() &&
            (statusIt->second == "TRADE_SUCCESS" || statusIt->second == "TRADE_FINISHED"))
            r.paid = true;
        auto it1 = params.find("out_trade_no"); if (it1 != params.end()) r.orderId = it1->second;
        auto it2 = params.find("trade_no");     if (it2 != params.end()) r.channelOrderNo = it2->second;
        auto it3 = params.find("buyer_id");     if (it3 != params.end()) r.buyerId = it3->second;
        auto it4 = params.find("total_amount");
        if (it4 != params.end()) {
            try { r.paidAmount = std::stod(it4->second); } catch(...) {}
        }
        return r;
    }

private:
    // way_code → 支付宝接口方法映射
    static std::string wayCodeToMethod(const std::string &wayCode) {
        if (wayCode == "ali_qr")     return "precreate";
        if (wayCode == "ali_bar")    return "bar";
        if (wayCode == "ali_jsapi")  return "jsapi";
        if (wayCode == "ali_wap")    return "wap";
        if (wayCode == "ali_page")   return "page";
        return "";  // 未知，回退到 channelParams.pay_method
    }

    static std::string currentTs() {
        auto now = std::time(nullptr); struct tm t;
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
    static std::string buildSign(const std::map<std::string, std::string> &ps) {
        std::string r;
        for (auto &[k, v] : ps) {
            if (k == "sign" || k == "sign_type" || v.empty()) continue;
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
};

REGISTER_CHANNEL_PLUGIN(AlipayExtPlugin);
