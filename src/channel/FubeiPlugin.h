#pragma once
#include "ChannelPlugin.h"
#include <iomanip>
#include <random>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class FubeiPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "fubei"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "开放平台ID", "input");
        add("appkey", "接口密钥", "input");
        add("appmchid", "门店ID", "input");
        add("mchid", "商户ID", "input");
        add("apptype", "支付宝支付方式", "input", "1", "1=生活号支付 2=H5支付");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() ||
            p.get("appmchid", "").asString().empty() || p.get("mchid", "").asString().empty()) {
            result.errMsg = "付呗参数不完整(appid/appkey/appmchid/mchid)";
            return result;
        }
        std::string method = "fbpay.order.create";
        Json::Value biz;
        biz["merchant_id"] = p.get("mchid", "").asString();
        biz["merchant_order_sn"] = req.orderId;
        biz["store_id"] = p.get("appmchid", "").asString();
        biz["total_amount"] = fmtAmount(req.amount);
        biz["body"] = req.subject.empty() ? "商品" : req.subject;
        biz["notify_url"] = req.notifyUrl;
        std::string payType = mapPayType(req.payType);
        if (req.payType == "ali_wap" || (req.payType == "alipay" && p.get("apptype", "1").asString() == "2")) {
            method = "fbpay.order.wap.create";
            biz["user_ip"] = req.clientIp;
            biz["return_url"] = req.returnUrl;
        } else {
            biz["pay_type"] = payType;
            biz["user_id"] = p.get("openid", p.get("user_id", "").asString()).asString();
            std::string subAppid = p.get("sub_appid", "").asString();
            if (!subAppid.empty()) biz["sub_appid"] = subAppid;
        }
        Json::Value resp = execute(p, method, biz, result.rawResponse, result.errMsg);
        if (!result.errMsg.empty()) return result;
        result.success = true;
        result.channelOrderNo = resp.get("order_sn", "").asString();
        if (method == "fbpay.order.wap.create") {
            result.payUrl = resp.get("html", "").asString();
            result.extra["html"] = result.payUrl;
        } else if (payType == "alipay") {
            result.payUrl = resp.get("prepay_id", "").asString();
            result.extra["prepay_id"] = result.payUrl;
        } else if (payType == "wxpay") {
            Json::StreamWriterBuilder wb; wb["indentation"] = "";
            result.extra["sign_package"] = Json::writeString(wb, resp["sign_package"]);
        } else {
            result.payUrl = resp.get("qrcode", resp.get("pay_url", "").asString()).asString();
            result.qrCode = result.payUrl;
        }
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        std::string signGot = get(params, "sign");
        if (signGot.empty() || sign(params, channelParams.get("appkey", "").asString()) != signGot) return r;
        r.verified = true;
        Json::Value data;
        std::string rawData = get(params, "data");
        if (!Json::Reader().parse(rawData, data)) return r;
        r.paid = data.get("order_status", "").asString() == "SUCCESS";
        r.orderId = data.get("merchant_order_sn", "").asString();
        r.channelOrderNo = data.get("order_sn", "").asString();
        r.buyerId = data.get("user_id", "").asString();
        try { r.paidAmount = std::stod(data.get("total_amount", "0").asString()); } catch (...) {}
        r.responseText = r.paid ? "success" : ("status=" + data.get("order_status", "").asString());
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        Json::Value biz;
        biz["order_sn"] = req.channelOrderNo;
        biz["merchant_refund_sn"] = req.refundNo;
        biz["refund_amount"] = fmtAmount(req.refundAmount);
        Json::Value resp = execute(req.channelParams, "fbpay.order.refund", biz, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.state = 1;
        r.channelRefundNo = resp.get("merchant_order_sn", req.refundNo).asString();
        return r;
    }

private:
    static Json::Value execute(const Json::Value &p, const std::string &method, const Json::Value &biz,
                               std::string &raw, std::string &err) {
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        Json::Value req;
        req["app_id"] = p.get("appid", "").asString();
        req["method"] = method;
        req["format"] = "json";
        req["sign_method"] = "md5";
        req["nonce"] = randomStr(12);
        req["version"] = "1.0";
        req["biz_content"] = Json::writeString(wb, biz);
        req["sign"] = sign(jsonToMap(req), p.get("appkey", "").asString());
        std::string body = Json::writeString(wb, req);
        auto resp = SyncHttp::postJson("https://shq-api.51fubei.com/gateway/agent", body, {{"Content-Type", "application/json; charset=utf-8"}});
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "付呗响应解析失败"; return Json::Value(); }
        if (j.get("result_code", 0).asInt() != 200) {
            err = j.get("result_message", "付呗请求失败").asString();
            return Json::Value();
        }
        return j["data"];
    }

    static std::string sign(const std::map<std::string, std::string> &params, const std::string &secret) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return upper(Md5Utils::md5(s + secret));
    }

    static std::map<std::string, std::string> jsonToMap(const Json::Value &j) {
        std::map<std::string, std::string> m;
        for (auto &k : j.getMemberNames()) m[k] = j[k].asString();
        return m;
    }

    static std::string mapPayType(const std::string &payType) {
        if (payType == "wxpay" || payType == "wx_jsapi") return "wxpay";
        if (payType == "bank" || payType == "ysf_qr") return "unionpay";
        return "alipay";
    }

    static std::string fmtAmount(double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); }
    static std::string randomStr(int n) {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s; for (int i = 0; i < n; ++i) s += cs[rng() % (sizeof(cs) - 1)]; return s;
    }
    static std::string upper(std::string s) { for (char &c : s) c = (char)std::toupper((unsigned char)c); return s; }
    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) { auto it = m.find(k); return it == m.end() ? "" : it->second; }
};

REGISTER_CHANNEL_PLUGIN(FubeiPlugin);
