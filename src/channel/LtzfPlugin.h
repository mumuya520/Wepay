#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class LtzfPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "ltzf"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户号", "input");
        add("appkey", "商户密钥", "input");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=H5 3=公众号");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "蓝兔支付参数不完整(appid/appkey)";
            return r;
        }
        std::string path = getPath(req.payType, p.get("apptype", "1").asInt());
        // Build params
        std::map<std::string, std::string> m;
        m["mch_id"] = p.get("appid", "").asString();
        m["out_trade_no"] = req.orderId;
        m["total_fee"] = fmtAmount(req.amount);
        m["body"] = req.subject.empty() ? "商品" : req.subject;
        m["timestamp"] = std::to_string((long long)std::time(nullptr));
        m["notify_url"] = req.notifyUrl;
        m["return_url"] = req.returnUrl;
        m["quit_url"] = req.returnUrl;
        // Sign: specified fields only, ksort, concat k=v&, append key=xxx, MD5 uppercase
        std::vector<std::string> signFields = {"mch_id", "out_trade_no", "total_fee", "body", "timestamp", "notify_url"};
        m["sign"] = makeSign(m, signFields, p.get("appkey", "").asString());
        // POST form
        std::string formBody = buildFormBody(m);
        auto resp = SyncHttp::postForm("https://api.ltzf.cn" + path, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("code", -1).asInt() != 0) {
            r.errMsg = result.get("msg", "下单失败").asString();
            return r;
        }
        r.success = true;
        Json::Value data = result["data"];
        // Different paths return different fields
        if (data.isMember("code_url")) {
            r.payUrl = data["code_url"].asString(); // 微信扫码
        } else if (data.isMember("h5_url")) {
            r.payUrl = data["h5_url"].asString(); // 支付宝H5
        } else if (data.isMember("order_url")) {
            r.payUrl = data["order_url"].asString(); // 微信公众号/H5跳转
        } else if (data.isMember("code_img_url")) {
            r.payUrl = data["code_img_url"].asString(); // 支付宝扫码图片
        } else {
            r.payUrl = data.asString();
        }
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "FAIL";
        std::string key = channelParams.get("appkey", "").asString();
        std::vector<std::string> signFields = {"code", "timestamp", "mch_id", "order_no", "out_trade_no", "pay_no", "total_fee"};
        std::string sign = makeSign(params, signFields, key);
        auto signIt = params.find("sign");
        r.verified = (signIt != params.end() && signIt->second == sign);
        if (!r.verified) return r;
        auto codeIt = params.find("code");
        r.paid = (codeIt != params.end() && codeIt->second == "0");
        r.orderId = get(params, "out_trade_no");
        r.channelOrderNo = get(params, "order_no");
        r.buyerId = get(params, "openid");
        try { r.paidAmount = std::stod(get(params, "total_fee")); } catch (...) {}
        r.responseText = r.paid ? "SUCCESS" : "FAIL";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string path = "/api/alipay/refund_order";
        // Default to alipay refund path; could determine by pay type if available
        std::map<std::string, std::string> m;
        m["mch_id"] = p.get("appid", "").asString();
        m["out_trade_no"] = req.orderId;
        m["out_refund_no"] = req.refundNo;
        m["timestamp"] = std::to_string((long long)std::time(nullptr));
        m["refund_fee"] = fmtAmount(req.refundAmount);
        std::vector<std::string> signFields = {"mch_id", "out_trade_no", "out_refund_no", "timestamp", "refund_fee"};
        m["sign"] = makeSign(m, signFields, p.get("appkey", "").asString());
        std::string formBody = buildFormBody(m);
        auto resp = SyncHttp::postForm("https://api.ltzf.cn" + path, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("code", -1).asInt() == 0) {
            r.success = true;
            r.state = 1;
        } else {
            r.errMsg = result.get("msg", "退款失败").asString();
        }
        return r;
    }

private:
    static std::string makeSign(const std::map<std::string, std::string> &m, const std::vector<std::string> &fields, const std::string &key) {
        std::string s;
        for (auto &f : fields) {
            auto it = m.find(f);
            if (it != m.end() && !it->second.empty()) {
                s += f + "=" + it->second + "&";
            }
        }
        s += "key=" + key;
        // MD5 uppercase
        std::string md5 = Md5Utils::md5(s);
        std::transform(md5.begin(), md5.end(), md5.begin(), ::toupper);
        return md5;
    }

    static std::string getPath(const std::string &payType, int apptype) {
        if (payType.find("wx") != std::string::npos) {
            if (apptype == 3) return "/api/wxpay/jsapi_convenient"; // 公众号
            if (apptype == 2) return "/api/wxpay/jump_h5"; // H5
            return "/api/wxpay/native"; // 扫码
        }
        // alipay
        if (apptype == 2) return "/api/alipay/h5";
        return "/api/alipay/native";
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

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(LtzfPlugin);
