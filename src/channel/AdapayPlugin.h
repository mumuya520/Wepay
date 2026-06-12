#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AdapayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "adapay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "应用 App_ID", "input");
        add("appkey", "prod 模式 API_KEY", "input");
        add("appsecret", "商户 RSA 私钥", "textarea", "", "可填写去掉 BEGIN/END 的裸私钥，按 AdaPay 后台配置");
        add("apptype", "支付模式", "input", "1", "支付宝:1扫码 2JS 3托管小程序；微信:1自有公众号/小程序 2动态二维码 3托管小程序；银行:1银联 2快捷 3网银");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string appId = p.get("appid", "").asString();
        std::string apiKey = p.get("appkey", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();
        if (appId.empty() || apiKey.empty() || privateKey.empty()) {
            result.errMsg = "AdaPay 参数不完整(appid/appkey/appsecret)";
            return result;
        }

        std::string payChannel = choosePayChannel(req.payType, p.get("apptype", "1").asString());
        if (payChannel.empty()) {
            result.errMsg = "AdaPay 未找到可用支付模式(apptype)";
            return result;
        }

        Json::Value body;
        body["app_id"] = appId;
        body["order_no"] = req.orderId;
        body["pay_channel"] = payChannel;
        body["pay_amt"] = fmtAmount(req.amount);
        body["goods_title"] = req.subject.empty() ? "商品" : req.subject;
        body["goods_desc"] = req.body.empty() ? body["goods_title"].asString() : req.body;
        body["currency"] = "cny";
        body["notify_url"] = req.notifyUrl;

        std::string endpoint = "/v1/payments";
        std::string host = "https://api.adapay.tech";
        std::string extraUser = p.get("channelUserId", "").asString();
        if ((payChannel == "wx_pub" || payChannel == "wx_lite") && !extraUser.empty()) {
            body["expend"]["openid"] = extraUser;
        } else if ((payChannel == "alipay_pub" || payChannel == "alipay_lite") && !extraUser.empty()) {
            body["expend"]["buyer_id"] = extraUser;
        }

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);
        auto resp = request(host, endpoint, bodyStr, apiKey, privateKey);
        result.rawResponse = resp.body;
        if (!resp.success) {
            result.errMsg = "AdaPay 请求失败: " + resp.errMsg;
            return result;
        }

        Json::Value outer;
        if (!Json::Reader().parse(resp.body, outer)) {
            result.errMsg = "AdaPay 响应解析失败";
            return result;
        }
        if (!outer.isMember("data")) {
            result.errMsg = outer.get("message", "AdaPay 未返回 data").asString();
            return result;
        }
        Json::Value data;
        std::string dataStr = outer["data"].asString();
        if (!Json::Reader().parse(dataStr, data)) {
            result.errMsg = "AdaPay data 解析失败";
            return result;
        }
        std::string status = data.get("status", "").asString();
        if (status != "succeeded" && status != "pending" && !data.isMember("expend")) {
            result.errMsg = "[" + data.get("error_code", "").asString() + "]" + data.get("error_msg", "AdaPay 下单失败").asString();
            return result;
        }

        Json::Value expend = data["expend"];
        std::string payData = pickPayData(expend, payChannel);
        if (payData.empty()) {
            result.errMsg = "AdaPay 未返回可用支付数据";
            return result;
        }
        result.success = true;
        result.channelOrderNo = data.get("id", "").asString();
        result.extra["pay_channel"] = payChannel;
        result.extra["status"] = status;
        result.rawResponse = resp.body;
        if (isJumpChannel(payChannel)) result.payUrl = payData;
        else result.qrCode = payData;
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &rawBody,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult result;
        result.responseText = "Ok";
        std::string sign = get(params, "sign");
        std::string dataStr = get(params, "data");
        if (dataStr.empty() && !rawBody.empty()) {
            Json::Value j;
            if (Json::Reader().parse(rawBody, j)) {
                sign = j.get("sign", sign).asString();
                dataStr = j.get("data", dataStr).asString();
            }
        }
        if (sign.empty() || dataStr.empty()) {
            result.responseText = "No";
            return result;
        }
        result.verified = RsaUtils::verifySha1(dataStr, sign, adapayPublicKey());
        if (!result.verified) {
            result.responseText = "No";
            return result;
        }
        Json::Value data;
        if (!Json::Reader().parse(dataStr, data)) {
            result.responseText = "No";
            return result;
        }
        result.paid = (data.get("status", "").asString() == "succeeded");
        result.orderId = data.get("order_no", "").asString();
        result.channelOrderNo = data.get("id", "").asString();
        result.buyerId = data["expend"].get("sub_open_id", "").asString();
        try { result.paidAmount = std::stod(data.get("pay_amt", "0").asString()); } catch (...) {}
        return result;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult result;
        auto &p = req.channelParams;
        std::string apiKey = p.get("appkey", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();
        if (apiKey.empty() || privateKey.empty() || req.channelOrderNo.empty()) {
            result.errMsg = "AdaPay 退款参数不完整(appkey/appsecret/channelOrderNo)";
            return result;
        }
        Json::Value body;
        body["payment_id"] = req.channelOrderNo;
        body["refund_order_no"] = req.refundNo;
        body["refund_amt"] = fmtAmount(req.refundAmount);
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string endpoint = "/v1/payments/" + req.channelOrderNo + "/refunds";
        auto resp = request("https://api.adapay.tech", endpoint, Json::writeString(wb, body), apiKey, privateKey);
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        Json::Value outer;
        if (!Json::Reader().parse(resp.body, outer) || !outer.isMember("data")) {
            result.errMsg = outer.get("message", "AdaPay 退款响应解析失败").asString();
            return result;
        }
        Json::Value data;
        if (!Json::Reader().parse(outer["data"].asString(), data)) {
            result.errMsg = "AdaPay 退款 data 解析失败";
            return result;
        }
        std::string status = data.get("status", "").asString();
        if (status == "succeeded" || status == "pending") {
            result.success = true;
            result.state = status == "succeeded" ? 1 : 0;
            result.channelRefundNo = data.get("id", "").asString();
        } else {
            result.errMsg = "[" + data.get("error_code", "").asString() + "]" + data.get("error_msg", "AdaPay 退款失败").asString();
        }
        return result;
    }

private:
    static SyncHttp::Response request(const std::string &host, const std::string &endpoint,
                                      const std::string &body, const std::string &apiKey,
                                      const std::string &privateKey) {
        std::string url = host + endpoint;
        std::string sign = RsaUtils::signSha1(url + body, privateKey);
        std::map<std::string, std::string> headers;
        headers["Authorization"] = apiKey;
        headers["Signature"] = sign;
        headers["sdk_version"] = "v1.0.0";
        headers["Content-Type"] = "application/json";
        return SyncHttp::postJson(url, body, headers);
    }

    static std::string choosePayChannel(const std::string &payType, const std::string &apptype) {
        auto has = [&](const std::string &x) { return ("," + apptype + ",").find("," + x + ",") != std::string::npos; };
        if (payType == "alipay" || payType == "ali_qr") {
            if (has("1") || apptype.empty()) return "alipay_qr";
            if (has("2")) return "alipay_pub";
            if (has("3")) return "alipay_lite";
        }
        if (payType == "wxpay" || payType == "wx_qr") {
            if (has("2")) return "";
            if (has("3")) return "wx_lite";
            return "wx_pub";
        }
        if (payType == "bank" || payType == "ysf_qr") {
            if (has("2")) return "fast_pay";
            if (has("3")) return "online_pay";
            return "union_qr";
        }
        return "";
    }

    static std::string pickPayData(const Json::Value &expend, const std::string &payChannel) {
        if (payChannel == "alipay_qr" || payChannel == "union_qr") return expend.get("qrcode_url", "").asString();
        if (payChannel == "alipay_lite") return expend.get("ali_h5_pay_url", "").asString();
        if (payChannel == "wx_lite") return expend.get("scheme_code", "").asString();
        if (payChannel == "fast_pay" || payChannel == "online_pay") return expend.get("pay_url", "").asString();
        if (payChannel == "wx_pub" || payChannel == "alipay_pub") return expend.get("pay_info", "").asString();
        return expend.get("qrcode_url", expend.get("pay_url", expend.get("pay_info", "").asString()).asString()).asString();
    }

    static bool isJumpChannel(const std::string &payChannel) {
        return payChannel == "alipay_lite" || payChannel == "wx_lite" || payChannel == "fast_pay" || payChannel == "online_pay";
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }

    static std::string adapayPublicKey() {
        return "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCwN6xgd6Ad8v2hIIsQVnbt8a3JituR8o4Tc3B5WlcFR55bz4OMqrG/356Ur3cPbc2Fe8ArNd/0gZbC9q56Eb16JTkVNA/fye4SXznWxdyBPR7+guuJZHc/VW2fKH2lfZ2P3Tt0QkKZZoawYOGSMdIvO+WqK44updyax0ikK6JlNQIDAQAB";
    }
};

REGISTER_CHANNEL_PLUGIN(AdapayPlugin);
