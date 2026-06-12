#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class JeepayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "jeepay"; }

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
        add("appmchid", "商户号", "input");
        add("appid", "应用 AppId", "input");
        add("appkey", "私钥 AppSecret", "textarea");
        add("apptype", "支付模式", "input", "1", "彩虹易 apptype：1扫码 2/H5/PC 3/JSAPI/WAP 4小程序 5聚合扫码 6WEB收银台 7APP，可填逗号分隔");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string appurl = normalizeBase(p.get("appurl", "").asString());
        std::string mchNo = p.get("appmchid", "").asString();
        std::string appId = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();
        if (appurl.empty() || mchNo.empty() || appId.empty() || key.empty()) {
            result.errMsg = "Jeepay 参数不完整(appurl/appmchid/appid/appkey)";
            return result;
        }

        std::string wayCode = chooseWayCode(req.payType, p.get("apptype", "1").asString());
        if (wayCode.empty()) {
            result.errMsg = "Jeepay 未找到可用支付模式(apptype)";
            return result;
        }

        std::map<std::string, std::string> params;
        params["mchNo"] = mchNo;
        params["appId"] = appId;
        params["mchOrderNo"] = req.orderId;
        params["wayCode"] = wayCode;
        params["amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        params["currency"] = "cny";
        params["clientIp"] = req.clientIp;
        params["subject"] = req.subject.empty() ? "商品" : req.subject;
        params["body"] = req.body.empty() ? params["subject"] : req.body;
        params["notifyUrl"] = req.notifyUrl;
        params["returnUrl"] = req.returnUrl;
        params["reqTime"] = std::to_string(nowMillis());
        params["version"] = "1.0";
        params["signType"] = "MD5";
        std::string channelExtra = p.get("channelExtra", "").asString();
        if (!channelExtra.empty()) params["channelExtra"] = channelExtra;
        params["sign"] = makeSign(params, key);

        Json::Value body;
        for (auto &kv : params) body[kv.first] = kv.second;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);

        auto resp = SyncHttp::postJson(appurl + "api/pay/unifiedOrder", bodyStr);
        result.rawResponse = resp.body;
        if (!resp.success) {
            result.errMsg = "Jeepay 请求失败: " + resp.errMsg;
            return result;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            result.errMsg = "Jeepay 响应解析失败";
            return result;
        }
        if (j.get("code", -1).asInt() != 0) {
            result.errMsg = j.get("msg", "Jeepay 下单失败").asString();
            return result;
        }
        Json::Value data = j["data"];
        std::string errMsg = data.get("errMsg", "").asString();
        if (!errMsg.empty()) {
            result.errMsg = "[" + data.get("errCode", "").asString() + "]" + errMsg;
            return result;
        }
        std::string error = data.get("error", "").asString();
        if (!error.empty()) {
            result.errMsg = error;
            return result;
        }

        std::string payDataType = lower(data.get("payDataType", "").asString());
        std::string payData = data.get("payData", "").asString();
        if (payData.empty()) {
            result.errMsg = "Jeepay 未返回 payData";
            return result;
        }

        result.success = true;
        result.channelOrderNo = data.get("payOrderId", "").asString();
        result.extra["payDataType"] = payDataType;
        result.extra["wayCode"] = wayCode;
        if (payDataType == "payurl" || payDataType == "form") result.payUrl = payData;
        else result.qrCode = payData;
        if (result.payUrl.empty() && result.qrCode.empty()) result.payUrl = payData;
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &rawBody,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult result;
        result.responseText = "success";
        std::map<std::string, std::string> data = params;
        if (data.empty() && !rawBody.empty()) {
            Json::Value j;
            if (Json::Reader().parse(rawBody, j)) {
                for (auto it = j.begin(); it != j.end(); ++it) data[it.name()] = (*it).asString();
            }
        }
        std::string key = channelParams.get("appkey", "").asString();
        auto signIt = data.find("sign");
        if (key.empty() || signIt == data.end()) return result;
        result.verified = (makeSign(data, key) == signIt->second);
        if (!result.verified) return result;
        result.paid = (get(data, "state") == "2");
        result.orderId = get(data, "mchOrderNo");
        result.channelOrderNo = get(data, "payOrderId");
        try { result.paidAmount = std::stod(get(data, "amount")) / 100.0; } catch (...) {}
        return result;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult result;
        auto &p = req.channelParams;
        std::string appurl = normalizeBase(p.get("appurl", "").asString());
        std::string mchNo = p.get("appmchid", "").asString();
        std::string appId = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();
        if (appurl.empty() || mchNo.empty() || appId.empty() || key.empty()) {
            result.errMsg = "Jeepay 参数不完整(appurl/appmchid/appid/appkey)";
            return result;
        }
        std::map<std::string, std::string> params;
        params["mchNo"] = mchNo;
        params["appId"] = appId;
        params["payOrderId"] = req.channelOrderNo;
        params["mchRefundNo"] = req.refundNo;
        params["refundAmount"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        params["currency"] = "cny";
        params["refundReason"] = req.reason.empty() ? "申请退款" : req.reason;
        params["reqTime"] = std::to_string(nowMillis());
        params["version"] = "1.0";
        params["signType"] = "MD5";
        params["sign"] = makeSign(params, key);
        Json::Value body;
        for (auto &kv : params) body[kv.first] = kv.second;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        auto resp = SyncHttp::postJson(appurl + "api/refund/refundOrder", Json::writeString(wb, body));
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j) || j.get("code", -1).asInt() != 0) {
            result.errMsg = j.get("msg", "Jeepay 退款失败").asString();
            return result;
        }
        Json::Value data = j["data"];
        std::string errMsg = data.get("errMsg", "").asString();
        if (!errMsg.empty()) { result.errMsg = "[" + data.get("errCode", "").asString() + "]" + errMsg; return result; }
        result.success = true;
        result.state = 0;
        result.channelRefundNo = data.get("refundOrderId", "").asString();
        return result;
    }

private:
    static std::string makeSign(const std::map<std::string, std::string> &params, const std::string &key) {
        std::string signStr;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.second.empty()) continue;
            signStr += kv.first + "=" + kv.second + "&";
        }
        signStr += "key=" + key;
        return upper(Md5Utils::md5(signStr));
    }

    static std::string chooseWayCode(const std::string &payType, const std::string &apptype) {
        auto has = [&](const std::string &x) { return ("," + apptype + ",").find("," + x + ",") != std::string::npos; };
        if (payType == "alipay" || payType == "ali_qr") {
            if (has("1")) return "ALI_QR";
            if (has("2")) return "ALI_PC";
            if (has("3")) return "ALI_WAP";
            if (has("7")) return "ALI_APP";
            if (has("5")) return "QR_CASHIER";
            if (has("6")) return "WEB_CASHIER";
        }
        if (payType == "wxpay" || payType == "wx_qr") {
            if (has("1")) return "WX_NATIVE";
            if (has("2")) return "WX_H5";
            if (has("7")) return "WX_APP";
            if (has("3")) return "WX_JSAPI";
            if (has("4")) return "WX_LITE";
            if (has("5")) return "QR_CASHIER";
            if (has("6")) return "WEB_CASHIER";
        }
        if (payType == "bank" || payType == "ysf_qr") {
            if (has("1")) return "YSF_NATIVE";
            if (has("5")) return "QR_CASHIER";
            if (has("6")) return "WEB_CASHIER";
        }
        return "";
    }

    static long long nowMillis() {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    static std::string normalizeBase(std::string s) {
        if (!s.empty() && s.back() != '/') s += '/';
        return s;
    }

    static std::string upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::toupper(c); });
        return s;
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(JeepayPlugin);
