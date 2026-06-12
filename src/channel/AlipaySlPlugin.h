#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AlipaySlPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipaysl"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "应用APPID", "input", "", "必须使用第三方应用");
        add("appkey", "支付宝公钥", "textarea", "", "公钥证书模式可留空");
        add("appsecret", "应用私钥", "textarea");
        add("appmchid", "商户授权token", "input", "", "app_auth_token");
        add("apptype", "支付方式", "input", "3", "1=电脑网站 2=手机网站 3=扫码 4=当面付JS 6=APP 7=JSAPI 8=订单码");
        add("gateway", "支付宝网关", "input", "https://openapi.alipay.com/gateway.do");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appsecret", "").asString().empty() || p.get("appmchid", "").asString().empty()) {
            r.errMsg = "支付宝服务商版参数不完整(appid/appsecret/appmchid)";
            return r;
        }
        std::string apptype = p.get("apptype", "3").asString();
        std::string method = chooseMethod(req.payType, apptype);
        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["total_amount"] = fmtAmount(req.amount);
        biz["subject"] = req.subject.empty() ? "商品" : req.subject;
        if (!req.clientIp.empty()) biz["business_params"]["mc_create_trade_ip"] = req.clientIp;
        if (method == "alipay.trade.precreate") {
        } else if (method == "alipay.trade.wap.pay") {
            biz["product_code"] = "QUICK_WAP_WAY";
        } else if (method == "alipay.trade.page.pay") {
            biz["product_code"] = "FAST_INSTANT_TRADE_PAY";
        } else if (method == "alipay.trade.app.pay") {
            biz["product_code"] = "QUICK_MSECURITY_PAY";
        } else if (method == "alipay.trade.create") {
            std::string buyerId = p.get("buyer_id", p.get("openid", "").asString()).asString();
            if (!buyerId.empty()) biz["buyer_id"] = buyerId;
        } else if (method == "alipay.trade.pay") {
            biz["scene"] = "bar_code";
            biz["auth_code"] = p.get("auth_code", "").asString();
        }
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::map<std::string, std::string> params = baseParams(p, method);
        params["notify_url"] = req.notifyUrl;
        if (!req.returnUrl.empty()) params["return_url"] = req.returnUrl;
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(signString(params), normalizePrivateKey(p.get("appsecret", "").asString()));
        std::string body = buildQuery(params);
        r.rawResponse = body;
        if (method == "alipay.trade.page.pay" || method == "alipay.trade.wap.pay") {
            r.success = true;
            r.payUrl = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString() + "?" + body;
            return r;
        }
        auto resp = SyncHttp::postForm(p.get("gateway", "https://openapi.alipay.com/gateway.do").asString(), body);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "支付宝响应解析失败"; return r; }
        Json::Value node = responseNode(j, method);
        if (node.get("code", "").asString() != "10000") {
            r.errMsg = node.get("sub_msg", node.get("msg", "支付宝下单失败").asString()).asString();
            return r;
        }
        r.success = true;
        r.channelOrderNo = node.get("trade_no", "").asString();
        r.qrCode = node.get("qr_code", "").asString();
        r.payUrl = r.qrCode.empty() ? node.get("trade_no", "").asString() : r.qrCode;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";
        std::string sign = get(params, "sign");
        std::string pub = channelParams.get("appkey", "").asString();
        if (sign.empty() || pub.empty()) { r.verified = false; r.responseText = "fail"; return r; }
        r.verified = RsaUtils::verifySha256(signString(params), sign, normalizePublicKey(pub));
        if (!r.verified) { r.responseText = "fail"; return r; }
        r.paid = get(params, "trade_status") == "TRADE_SUCCESS" || get(params, "trade_status") == "TRADE_FINISHED";
        r.orderId = get(params, "out_trade_no");
        r.channelOrderNo = get(params, "trade_no");
        r.buyerId = get(params, "buyer_id").empty() ? get(params, "buyer_open_id") : get(params, "buyer_id");
        try { r.paidAmount = std::stod(get(params, "total_amount")); } catch (...) {}
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        Json::Value biz;
        if (!req.channelOrderNo.empty()) biz["trade_no"] = req.channelOrderNo;
        else biz["out_trade_no"] = req.orderId;
        biz["refund_amount"] = fmtAmount(req.refundAmount);
        biz["out_request_no"] = req.refundNo;
        std::string raw, err;
        Json::Value node = call(p, "alipay.trade.refund", biz, raw, err);
        r.rawResponse = raw;
        if (!err.empty()) { r.errMsg = err; return r; }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = node.get("trade_no", "").asString();
        return r;
    }

    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        std::string raw, err;
        Json::Value node = call(req.channelParams, "alipay.trade.close", biz, raw, err);
        if (!err.empty()) { r.errMsg = err; return r; }
        r.success = true;
        return r;
    }

private:
    static Json::Value call(const Json::Value &p, const std::string &method, const Json::Value &biz, std::string &raw, std::string &err) {
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        auto params = baseParams(p, method);
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(signString(params), normalizePrivateKey(p.get("appsecret", "").asString()));
        auto resp = SyncHttp::postForm(p.get("gateway", "https://openapi.alipay.com/gateway.do").asString(), buildQuery(params));
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "支付宝响应解析失败"; return Json::Value(); }
        Json::Value node = responseNode(j, method);
        if (node.get("code", "").asString() != "10000") err = node.get("sub_msg", node.get("msg", "支付宝请求失败").asString()).asString();
        return node;
    }

    static std::map<std::string, std::string> baseParams(const Json::Value &p, const std::string &method) {
        return {{"app_id", p.get("appid", "").asString()}, {"method", method}, {"format", "JSON"}, {"charset", "utf-8"}, {"sign_type", "RSA2"}, {"timestamp", now()}, {"version", "1.0"}, {"app_auth_token", p.get("appmchid", "").asString()}};
    }

    static Json::Value responseNode(const Json::Value &j, const std::string &method) {
        std::string key = method;
        for (char &c : key) if (c == '.') c = '_';
        key += "_response";
        return j[key];
    }

    static std::string chooseMethod(const std::string &payType, const std::string &apptype) {
        if (payType == "ali_wap" || apptype == "2") return "alipay.trade.wap.pay";
        if (payType == "ali_app" || apptype == "6") return "alipay.trade.app.pay";
        if (payType == "ali_jsapi" || apptype == "4" || apptype == "7") return "alipay.trade.create";
        if (payType == "scan" || apptype == "5") return "alipay.trade.pay";
        if (apptype == "1") return "alipay.trade.page.pay";
        return "alipay.trade.precreate";
    }

    static std::string signString(const std::map<std::string, std::string> &params) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string q;
        for (auto &kv : params) { if (!q.empty()) q += "&"; q += kv.first + "=" + urlEncode(kv.second); }
        return q;
    }

    static std::string normalizePrivateKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b+=key.substr(i,64)+"\n"; return "-----BEGIN RSA PRIVATE KEY-----\n"+b+"-----END RSA PRIVATE KEY-----\n"; }
    static std::string normalizePublicKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b+=key.substr(i,64)+"\n"; return "-----BEGIN PUBLIC KEY-----\n"+b+"-----END PUBLIC KEY-----\n"; }
    static std::string fmtAmount(double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); }
    static std::string now() {
        auto t = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[32];
        std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
        return b;
    }
    static std::string urlEncode(const std::string &s) { std::ostringstream oss; for(unsigned char c:s){ if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') oss<<c; else oss<<'%'<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)c; } return oss.str(); }
    static std::string get(const std::map<std::string,std::string> &m,const std::string &k){auto it=m.find(k);return it==m.end()?"":it->second;}
};

REGISTER_CHANNEL_PLUGIN(AlipaySlPlugin);
