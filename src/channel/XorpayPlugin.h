#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

// XorPay - MD5拼接签名, form POST, JSON响应
// API: https://xorpay.com/api/pay/{appid} (扫码), /api/cashier/{appid} (收银台JSAPI)
// 签名(下单): MD5(name + pay_type + price + order_id + notify_url + appkey)
// 签名(回调): MD5(aoid + order_id + pay_price + pay_time + appkey)
// 签名(退款): MD5(price + appkey)
// 回调: POST参数, aoid/order_id/pay_price/pay_time/detail/sign
// 退款: POST https://xorpay.com/api/refund/{aoid}

class XorpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "xorpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "AppId", "input");
        add("appkey", "AppSecret", "input");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=收银台JSAPI");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        std::string appId = p.get("appid", "").asString();
        std::string appKey = p.get("appkey", "").asString();
        if (appId.empty() || appKey.empty()) { r.errMsg = "XorPay参数不完整(appid/appkey)"; return r; }

        int apptype = p.get("apptype", "1").asInt();
        std::string payType = getPayType(req.payType);

        if (apptype == 2 && req.payType.find("wx") != std::string::npos) {
            // 收银台JSAPI模式 - 构建form跳转
            std::string apiUrl = "https://xorpay.com/api/cashier/" + appId;
            std::string name = req.subject.empty() ? "商品" : req.subject;
            std::string price = fmtAmount(req.amount);
            std::string sign = Md5Utils::md5(name + payType + price + req.orderId + req.notifyUrl + appKey);

            std::map<std::string, std::string> formParams;
            formParams["name"] = name;
            formParams["pay_type"] = payType;
            formParams["price"] = price;
            formParams["order_id"] = req.orderId;
            formParams["notify_url"] = req.notifyUrl;
            formParams["return_url"] = req.returnUrl;
            formParams["sign"] = sign;

            r.success = true;
            // Build form HTML for redirect
            std::string html = "<form action=\"" + apiUrl + "\" method=\"post\" id=\"dopay\">";
            for (auto &kv : formParams) {
                html += "<input type=\"hidden\" name=\"" + kv.first + "\" value=\"" + kv.second + "\" />";
            }
            html += "<input type=\"submit\" value=\"正在跳转\"></form><script>document.getElementById('dopay').submit();</script>";
            r.payUrl = "data:text/html," + html;
            r.extra["form_html"] = html;
            r.extra["form_action"] = apiUrl;
            r.extra["form_params"] = Json::Value(Json::objectValue);
            for (auto &kv : formParams) r.extra["form_params"][kv.first] = kv.second;
            return r;
        }

        // 扫码模式
        std::string apiUrl = "https://xorpay.com/api/pay/" + appId;
        std::string name = req.subject.empty() ? "商品" : req.subject;
        std::string price = fmtAmount(req.amount);
        std::string sign = Md5Utils::md5(name + payType + price + req.orderId + req.notifyUrl + appKey);

        std::map<std::string, std::string> formParams;
        formParams["name"] = name;
        formParams["pay_type"] = payType;
        formParams["price"] = price;
        formParams["order_id"] = req.orderId;
        formParams["notify_url"] = req.notifyUrl;
        formParams["sign"] = sign;

        std::string formBody = buildFormBody(formParams);
        auto resp = SyncHttp::postForm(apiUrl, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        std::string status = result.get("status", "").asString();
        if (status != "ok") {
            r.errMsg = status.empty() ? "返回数据解析失败" : status;
            return r;
        }
        r.success = true;
        r.payUrl = result["info"].get("qr", "").asString();
        r.channelOrderNo = result["info"].get("aoid", "").asString();
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        std::string appKey = channelParams.get("appkey", "").asString();
        auto aoidIt = params.find("aoid");
        if (aoidIt == params.end()) { r.verified = false; return r; }

        // Sign: MD5(aoid + order_id + pay_price + pay_time + appkey)
        std::string signSrc = get(params, "aoid") + get(params, "order_id") + get(params, "pay_price") + get(params, "pay_time") + appKey;
        std::string calcSign = Md5Utils::md5(signSrc);

        auto signIt = params.find("sign");
        r.verified = (signIt != params.end() && calcSign == signIt->second);
        if (!r.verified) return r;

        r.paid = true;
        r.orderId = get(params, "order_id");
        r.channelOrderNo = get(params, "aoid");
        try { r.paidAmount = std::stod(get(params, "pay_price")); } catch (...) {}
        // Parse buyer from detail JSON
        std::string detail = get(params, "detail");
        Json::Value detailJson;
        if (Json::Reader().parse(detail, detailJson)) {
            r.buyerId = detailJson.get("buyer", "").asString();
        }
        r.responseText = "success";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string appKey = p.get("appkey", "").asString();
        std::string price = fmtAmount(req.refundAmount);

        // Sign: MD5(price + appkey)
        std::string sign = Md5Utils::md5(price + appKey);

        std::string apiUrl = "https://xorpay.com/api/refund/" + req.channelOrderNo;
        std::map<std::string, std::string> formParams;
        formParams["price"] = price;
        formParams["sign"] = sign;

        std::string formBody = buildFormBody(formParams);
        auto resp = SyncHttp::postForm(apiUrl, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        std::string status = result.get("status", "").asString();
        if (status == "ok") {
            r.success = true;
            r.state = 1;
        } else {
            r.errMsg = result.get("info", "退款失败").asString();
        }
        return r;
    }

private:
    static std::string getPayType(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "native";
        return "alipay";
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k, const std::string &dflt = "") {
        auto it = m.find(k);
        return it == m.end() ? dflt : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(XorpayPlugin);
