#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class KayixinPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "kayixin"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appurl", "接口域名", "input", "", "如 http://trade.kayixin.com");
        add("appid", "商户号", "input");
        add("appkey", "商户密钥", "input");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appurl", "").asString().empty() || p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "卡易信参数不完整(appurl/appid/appkey)";
            return r;
        }
        std::string gateway = p.get("appurl", "").asString();
        std::map<std::string, std::string> m;
        m["service"] = "alipay.pc";
        m["partner"] = p.get("appid", "").asString();
        m["notify_url"] = req.notifyUrl;
        m["return_url"] = req.returnUrl;
        m["out_trade_no"] = req.orderId;
        m["subject"] = req.subject.empty() ? "商品" : req.subject;
        m["body"] = req.subject.empty() ? "商品" : req.subject;
        m["total_fee"] = fmtAmount(req.amount);
        m["_input_charset"] = "utf-8";
        m["sign"] = md5Sign(m, p.get("appkey", "").asString());
        m["sign_type"] = "MD5";
        // Build form HTML
        std::string url = gateway;
        std::ostringstream html;
        html << "<form action=\"" << url << "\" method=\"post\" id=\"dopay\">";
        for (auto &kv : m) html << "<input type=\"hidden\" name=\"" << kv.first << "\" value=\"" << kv.second << "\" />";
        html << "<input type=\"submit\" value=\"正在跳转\"></form><script>document.getElementById(\"dopay\").submit();</script>";
        r.success = true;
        r.payUrl = url;
        r.rawResponse = html.str();
        r.extra["html"] = html.str();
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        std::string sign = get(params, "sign");
        std::string key = channelParams.get("appkey", "").asString();
        r.verified = md5Verify(params, sign, key);
        if (!r.verified) return r;
        std::string status = get(params, "trade_status");
        r.paid = (status == "TRADE_FINISHED" || status == "TRADE_SUCCESS");
        r.orderId = get(params, "out_trade_no");
        r.channelOrderNo = get(params, "trade_no");
        r.buyerId = get(params, "buyer_id");
        try { r.paidAmount = std::stod(get(params, "total_fee")); } catch (...) {}
        r.responseText = r.paid ? "success" : "fail";
        return r;
    }

private:
    static std::string md5Sign(const std::map<std::string, std::string> &m, const std::string &key) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return Md5Utils::md5(s + key);
    }

    static bool md5Verify(const std::map<std::string, std::string> &m, const std::string &sign, const std::string &key) {
        return md5Sign(m, key) == sign;
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
};

REGISTER_CHANNEL_PLUGIN(KayixinPlugin);
