#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class HuolianPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "huolian"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "对接商授权编号", "input");
        add("appkey", "对接商MD5加密盐", "input");
        add("appmchid", "商户ID", "input");
        add("appurl", "收银员手机号", "input");
        add("appsecret", "退款密码（管理密码）", "input", "", "如不需要退款功能可留空");
        add("apptype", "支付方式", "input", "1", "微信：1聚合支付 2H5预下单");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() || p.get("appmchid", "").asString().empty() || p.get("appurl", "").asString().empty()) {
            r.errMsg = "火脸支付参数不完整(appid/appkey/appmchid/appurl)";
            return r;
        }
        Json::Value params;
        params["businessOrderNo"] = req.orderId;
        params["payAmount"] = fmtAmount(req.amount);
        params["merchantNo"] = p.get("appmchid", "").asString();
        params["operatorAccount"] = p.get("appurl", "").asString();
        params["notifyUrl"] = req.notifyUrl;
        params["subject"] = req.subject.empty() ? "商品" : req.subject;
        params["payWay"] = payWay(req.payType);
        params["clientIp"] = req.clientIp;
        std::string resource = "api.hl.order.pay.unified";
        if (req.payType == "wx_wap" || (req.payType == "wxpay" && p.get("apptype", "1").asString() == "2")) {
            resource = "api.hl.order.pay.h5";
            params["pageNotifyUrl"] = req.returnUrl;
        } else if (req.payType == "wx_lite" || req.payType == "wx_jsapi") {
            resource = "api.hl.order.pay.applet";
            params["appId"] = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            params["openId"] = p.get("openid", p.get("sub_openid", "").asString()).asString();
        }
        Json::Value data = execute(p, resource, params, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.channelOrderNo = data.get("orderNo", "").asString();
        if (data.isMember("payUrl")) r.payUrl = data["payUrl"].asString();
        else if (data.isMember("qrCodeUrl")) r.payUrl = data["qrCodeUrl"].asString();
        else if (data.isMember("payPath")) r.payUrl = data["payPath"].asString();
        r.qrCode = r.payUrl;
        r.extra = data;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "FAIL";
        Json::Value j;
        if (!body.empty()) Json::Reader().parse(body, j);
        if (!j.isObject()) {
            for (auto &kv : params) j[kv.first] = kv.second;
        }
        if (!j.isObject() || !j.isMember("respBody")) return r;
        r.verified = verify(j, channelParams.get("appkey", "").asString());
        if (!r.verified) return r;
        Json::Value data;
        if (!Json::Reader().parse(j["respBody"].asString(), data)) return r;
        r.orderId = data.get("businessOrderNo", "").asString();
        r.channelOrderNo = data.get("orderNo", "").asString();
        r.buyerId = data.get("userId", "").asString();
        r.paid = data.get("orderStatus", 0).asInt() == 2;
        try { r.paidAmount = std::stod(data.get("payAmount", "0").asString()); } catch (...) {}
        r.responseText = r.paid ? "SUCCESS" : ("status=" + data.get("orderStatus", "").asString());
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        Json::Value params;
        params["orderNo"] = req.channelOrderNo.empty() ? req.orderId : req.channelOrderNo;
        params["businessRefundNo"] = req.refundNo;
        params["refundAmount"] = fmtAmount(req.refundAmount);
        params["refundPassword"] = req.channelParams.get("appsecret", "").asString();
        params["merchantNo"] = req.channelParams.get("appmchid", "").asString();
        params["operatorAccount"] = req.channelParams.get("appurl", "").asString();
        Json::Value data = execute(req.channelParams, "api.hl.order.refund.operation", params, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.state = 1;
        r.channelRefundNo = data.get("refundNo", req.refundNo).asString();
        return r;
    }

private:
    static Json::Value execute(const Json::Value &p, const std::string &resource, const Json::Value &params, std::string &raw, std::string &err) {
        Json::Value common;
        common["authCode"] = p.get("appid", "").asString();
        common["requestTime"] = now();
        common["resource"] = resource;
        common["versionNo"] = "1";
        Json::Value signData = common;
        for (auto &n : params.getMemberNames()) if (!params[n].isNull()) signData[n] = params[n];
        common["sign"] = makeSign(signData, p.get("appkey", "").asString());
        common["params"] = jsonCompact(params);
        auto resp = SyncHttp::postJson("https://open.lianok.com/open/v1/api/biz/do", jsonCompact(common), {{"Content-Type", "application/json; charset=utf-8"}});
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "火脸响应解析失败"; return Json::Value(); }
        if (j.get("code", -1).asInt() == 0 && j.get("status", 0).asInt() == 200) return j["data"];
        err = j.get("message", "火脸接口返回失败").asString();
        return Json::Value();
    }

    static bool verify(Json::Value j, const std::string &salt) {
        std::string sign = j.get("sign", "").asString();
        if (sign.empty()) return false;
        j.removeMember("code");
        j.removeMember("message");
        return makeSign(j, salt) == sign;
    }

    static std::string makeSign(const Json::Value &v, const std::string &salt) {
        auto names = v.getMemberNames();
        std::sort(names.begin(), names.end());
        std::string s;
        for (auto &n : names) {
            if (n == "sign" || v[n].isNull()) continue;
            s += n + "=" + valueToString(v[n]) + "&";
        }
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return Md5Utils::md5(s + salt);
    }

    static std::string valueToString(const Json::Value &v) {
        if (v.isString()) return v.asString();
        if (v.isDouble()) return fmtAmount(v.asDouble());
        if (v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt()) return v.asString();
        if (v.isBool()) return v.asBool() ? "true" : "false";
        return jsonCompact(v);
    }

    static std::string jsonCompact(const Json::Value &v) { Json::StreamWriterBuilder wb; wb["indentation"] = ""; return Json::writeString(wb, v); }
    static std::string payWay(const std::string &payType) { if (payType == "wxpay" || payType == "wx_wap" || payType == "wx_jsapi" || payType == "wx_lite") return "wechat"; if (payType == "bank") return "cloud"; return "alipay"; }
    static std::string fmtAmount(double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); }
    static std::string now() {
        auto t = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[16]; std::strftime(b, sizeof(b), "%Y%m%d%H%M%S", &tmv); return b;
    }
};

REGISTER_CHANNEL_PLUGIN(HuolianPlugin);
