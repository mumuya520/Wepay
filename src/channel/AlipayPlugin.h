// WePay-Cpp — 支付宝官方支付通道插件
#pragma once
#include "ChannelPlugin.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class AlipayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "应用APPID", "input", "", "支付宝开放平台应用 APPID");
        add("appkey", "支付宝公钥", "textarea", "", "彩虹易字段；填错可支付但无法回调，证书模式可留空");
        add("appsecret", "应用私钥", "textarea", "", "彩虹易字段；应用私钥 PEM(PKCS8 格式)");
        add("appmchid", "卖家支付宝用户ID", "input", "", "可留空，默认商户签约账号");
        add("private_key", "应用私钥(兼容)", "textarea", "", "兼容旧配置 private_key");
        add("alipay_public_key", "支付宝公钥(兼容)", "textarea", "", "兼容旧配置 alipay_public_key");
        add("gateway", "网关地址", "input", "https://openapi.alipay.com/gateway.do", "正式环境或沙箱网关");
        add("apptype", "可用接口", "input", "3", "彩虹易 apptype：1电脑网站 2手机网站 3当面付扫码 4当面付JS 5预授权 6APP 7JSAPI 8订单码，可逗号分隔");
        add("pay_method", "支付方式(兼容)", "select", "precreate", "precreate=扫码当面付 / wap=手机网页 / page=电脑网站 / app=APP");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string privateKey = getParam(p, "private_key", "appsecret");
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        std::string method;
        if (req.payType == "ali_qr" || req.payType == "alipay") method = "precreate";
        else if (req.payType == "ali_wap") method = "wap";
        else if (req.payType == "ali_page") method = "page";
        else if (req.payType == "ali_app") method = "app";
        if (method.empty()) method = chooseMethod(p.get("apptype", "").asString(), p.get("pay_method", "precreate").asString());

        if (appid.empty() || privateKey.empty()) {
            result.errMsg = "支付宝参数不完整(appid/appsecret)";
            return result;
        }

        std::map<std::string, std::string> params;
        params["app_id"] = appid;
        params["method"] = alipayMethod(method);
        params["format"] = "JSON";
        params["charset"] = "utf-8";
        params["sign_type"] = "RSA2";
        params["timestamp"] = currentTimestamp();
        params["version"] = "1.0";
        params["notify_url"] = req.notifyUrl;
        if (!req.returnUrl.empty() && (method == "wap" || method == "page")) params["return_url"] = req.returnUrl;

        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["total_amount"] = fmtAmount(req.amount);
        biz["subject"] = req.subject.empty() ? "商品" : req.subject;
        std::string sellerId = p.get("appmchid", "").asString();
        if (!sellerId.empty()) biz["seller_id"] = sellerId;
        if (method == "precreate") biz["product_code"] = "FACE_TO_FACE_PAYMENT";
        else if (method == "page") biz["product_code"] = "FAST_INSTANT_TRADE_PAY";
        else if (method == "app") biz["product_code"] = "QUICK_MSECURITY_PAY";
        else biz["product_code"] = "QUICK_WAP_WAY";
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);

        std::string signature = RsaUtils::signSha256(buildSignString(params), privateKey);
        if (signature.empty()) {
            result.errMsg = "支付宝 RSA2 签名失败，检查私钥格式";
            return result;
        }
        params["sign"] = signature;

        if (method == "wap" || method == "page" || method == "app") {
            std::string qs = buildQuery(params);
            result.payUrl = gateway + "?" + qs;
            result.success = true;
            result.rawResponse = qs;
            return result;
        }

        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        result.rawResponse = resp.body;
        if (!resp.success || resp.status != 200) {
            result.errMsg = "支付宝网关请求失败: " + resp.errMsg;
            return result;
        }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            result.errMsg = "支付宝响应解析失败";
            return result;
        }
        auto &rc = j["alipay_trade_precreate_response"];
        if (rc.get("code", "").asString() != "10000") {
            result.errMsg = "支付宝返回错误: " + rc.get("msg", "").asString() + " - " + rc.get("sub_msg", "").asString();
            return result;
        }
        result.qrCode = rc.get("qr_code", "").asString();
        result.payUrl = result.qrCode;
        result.channelOrderNo = rc.get("trade_no", "").asString();
        result.success = true;
        return result;
    }

    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        std::string appid = channelParams.get("appid", "").asString();
        std::string privateKey = getParam(channelParams, "private_key", "appsecret");
        std::string gateway = channelParams.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || privateKey.empty()) return r;
        std::map<std::string, std::string> params = baseParams(appid, "alipay.trade.query");
        Json::Value biz; biz["out_trade_no"] = orderId;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), privateKey);
        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        if (!resp.success) return r;
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) return r;
        auto &rc = j["alipay_trade_query_response"];
        if (rc.get("code", "").asString() != "10000") return r;
        std::string state = rc.get("trade_status", "").asString();
        if (state == "TRADE_SUCCESS" || state == "TRADE_FINISHED") r.tradeState = 1;
        else if (state == "TRADE_CLOSED") r.tradeState = -1;
        else r.tradeState = 0;
        r.channelOrderNo = rc.get("trade_no", "").asString();
        r.buyerId = rc.get("buyer_user_id", "").asString();
        try { r.paidAmount = std::stod(rc.get("total_amount", "0").asString()); } catch (...) {}
        r.success = true;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult result;
        result.responseText = "success";
        std::string pubKey = getParam(channelParams, "alipay_public_key", "appkey");
        auto signIt = params.find("sign");
        if (signIt == params.end()) return result;
        std::string signStr = buildSignString(params);
        result.verified = pubKey.empty() ? true : RsaUtils::verifySha256(signStr, signIt->second, pubKey);
        if (!result.verified) return result;
        std::string status = get(params, "trade_status");
        result.paid = (status == "TRADE_SUCCESS" || status == "TRADE_FINISHED");
        result.orderId = get(params, "out_trade_no");
        result.channelOrderNo = get(params, "trade_no");
        result.buyerId = get(params, "buyer_id");
        try { result.paidAmount = std::stod(get(params, "total_amount")); } catch (...) {}
        return result;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = getParam(p, "private_key", "appsecret");
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty()) { r.errMsg = "支付宝参数不完整"; return r; }
        std::map<std::string, std::string> params = baseParams(appid, "alipay.trade.refund");
        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["refund_amount"] = fmtAmount(req.refundAmount);
        biz["refund_reason"] = req.reason;
        biz["out_request_no"] = req.refundNo;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "响应解析失败"; return r; }
        auto &rc = j["alipay_trade_refund_response"];
        if (rc.get("code", "").asString() != "10000") {
            r.errMsg = rc.get("msg", "").asString() + " / " + rc.get("sub_msg", "").asString();
            return r;
        }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = rc.get("trade_no", "").asString();
        return r;
    }

    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = getParam(p, "private_key", "appsecret");
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty()) { r.errMsg = "支付宝参数不完整"; return r; }
        std::map<std::string, std::string> params = baseParams(appid, "alipay.trade.close");
        Json::Value biz; biz["out_trade_no"] = req.orderId;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "解析失败"; return r; }
        auto &rc = j["alipay_trade_close_response"];
        std::string code = rc.get("code", "").asString();
        if (code == "10000" || code == "40004") r.success = true;
        else r.errMsg = rc.get("msg", "").asString();
        return r;
    }

    ChannelTransferResult transfer(const ChannelTransferRequest &req) override {
        ChannelTransferResult r;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = getParam(p, "private_key", "appsecret");
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty()) { r.errMsg = "支付宝参数不完整"; return r; }
        std::map<std::string, std::string> params = baseParams(appid, "alipay.fund.trans.uni.transfer");
        Json::Value biz;
        biz["out_biz_no"] = req.transferNo;
        biz["trans_amount"] = fmtAmount(req.amount);
        biz["product_code"] = "TRANS_ACCOUNT_NO_PWD";
        biz["biz_scene"] = "DIRECT_TRANSFER";
        biz["order_title"] = req.remark.empty() ? "转账" : req.remark;
        Json::Value payee;
        payee["identity_type"] = "ALIPAY_LOGON_ID";
        payee["identity"] = req.accountNo;
        if (!req.accountName.empty()) payee["name"] = req.accountName;
        biz["payee_info"] = payee;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        params["biz_content"] = Json::writeString(wb, biz);
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "解析失败"; return r; }
        auto &rc = j["alipay_fund_trans_uni_transfer_response"];
        if (rc.get("code", "").asString() == "10000" && rc.get("status", "").asString() == "SUCCESS") {
            r.success = true; r.state = 1; r.channelTransferNo = rc.get("order_id", "").asString();
        } else r.errMsg = rc.get("msg", "").asString() + " / " + rc.get("sub_msg", "").asString();
        return r;
    }

    ChannelUserIdResult queryChannelUserId(const ChannelUserIdRequest &req) override {
        ChannelUserIdResult r;
        auto &p = req.channelParams;
        std::string appid = p.get("appid", "").asString();
        std::string priKey = getParam(p, "private_key", "appsecret");
        std::string gateway = p.get("gateway", "https://openapi.alipay.com/gateway.do").asString();
        if (appid.empty() || priKey.empty() || req.code.empty()) { r.errMsg = "appid/appsecret/code 必填"; return r; }
        std::map<std::string, std::string> params = baseParams(appid, "alipay.system.oauth.token");
        params["grant_type"] = "authorization_code";
        params["code"] = req.code;
        params["sign"] = RsaUtils::signSha256(buildSignString(params), priKey);
        auto resp = SyncHttp::postForm(gateway, buildQuery(params));
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "解析失败"; return r; }
        auto &rc = j["alipay_system_oauth_token_response"];
        std::string userId = rc.get("user_id", "").asString();
        if (userId.empty()) { r.errMsg = rc.get("msg", "获取 user_id 失败").asString(); return r; }
        r.success = true; r.userId = userId;
        return r;
    }

private:
    static std::map<std::string, std::string> baseParams(const std::string &appid, const std::string &method) {
        std::map<std::string, std::string> p;
        p["app_id"] = appid;
        p["method"] = method;
        p["format"] = "JSON";
        p["charset"] = "utf-8";
        p["sign_type"] = "RSA2";
        p["timestamp"] = currentTimestamp();
        p["version"] = "1.0";
        return p;
    }
    static std::string currentTimestamp() {
        auto now = std::time(nullptr);
        struct tm t;
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
    static std::string getParam(const Json::Value &p, const std::string &primary, const std::string &fallback) {
        std::string v = p.get(primary, "").asString();
        return v.empty() ? p.get(fallback, "").asString() : v;
    }
    static std::string chooseMethod(const std::string &apptype, const std::string &fallback) {
        auto has = [&](const std::string &x) { return ("," + apptype + ",").find("," + x + ",") != std::string::npos; };
        if (has("3") || has("4") || has("8")) return "precreate";
        if (has("2")) return "wap";
        if (has("1")) return "page";
        if (has("6")) return "app";
        if (has("7")) return "wap";
        return fallback;
    }
    static std::string alipayMethod(const std::string &method) {
        if (method == "wap") return "alipay.trade.wap.pay";
        if (method == "page") return "alipay.trade.page.pay";
        if (method == "app") return "alipay.trade.app.pay";
        return "alipay.trade.precreate";
    }
    static std::string buildSignString(const std::map<std::string, std::string> &params) {
        std::string result;
        for (auto &[k, v] : params) {
            if (k == "sign" || k == "sign_type" || v.empty()) continue;
            if (!result.empty()) result += "&";
            result += k + "=" + v;
        }
        return result;
    }
    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string body;
        for (auto &[k, v] : params) {
            if (!body.empty()) body += "&";
            body += k + "=" + urlEncode(v);
        }
        return body;
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

REGISTER_CHANNEL_PLUGIN(AlipayPlugin);
