// WePay-Cpp — 银盛支付插件 (完整实现)
// 参考 PHP: mpay_v2_webman/app/common/payment/YsepayApiPayment.php
// 参考 SDK: mpay_v2_webman/app/common/sdk/ysepay/YsepayClient.php
//
// 支付产品:
//   - ysepay.online.qrcodepay: 扫码支付 (alipay/wxpay/bank)
//   - ysepay.online.wap.directpay.createbyuser: 支付宝 H5
//   - ysepay.online.alijsapi.pay: 支付宝 JSAPI
//   - ysepay.online.weixin.pay: 微信 JSAPI/小程序
//   - ysepay.online.cupmulapp.qrcodepay: 银联 JSAPI
//
// 签名: RSA (PKCS1), biz_content 是 JSON 字符串
#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class YsePayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "ysepay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("partner_id",           "服务商商户号",   "input",    "", "银盛服务商商户号");
        add("seller_id",            "收款商户号",     "input",    "", "留空则使用 partner_id");
        add("business_code",        "业务代码",       "input",    "", "银盛分配的业务代码");
        add("private_key",          "商户私钥",       "textarea", "", "PEM 格式 RSA 私钥 (PKCS8)");
        add("platform_public_key",  "平台公钥",       "textarea", "", "银盛平台公钥 (用于验签)");
        add("private_cert_password","私钥证书密码",   "password", "", "私钥证书密码");
        add("pay_method",          "默认支付方式",   "select",   "qrcode", "qrcode=扫码/h5=H5/jsapi=JSAPI");
        return arr;
    }

    // ═══ 下单接口 ═══════════════════════════════════════════════════
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;

        std::string partnerId  = p.get("partner_id", "").asString();
        std::string sellerId   = p.get("seller_id", partnerId).asString();
        std::string businessCode = p.get("business_code", "").asString();
        std::string priKey     = p.get("private_key", "").asString();
        std::string payMethod  = p.get("pay_method", "qrcode").asString();

        if (partnerId.empty() || businessCode.empty() || priKey.empty()) {
            r.errMsg = "银盛参数不完整(partner_id/business_code/private_key)";
            return r;
        }

        std::string wayCode = req.payType;

        // 根据支付方式路由
        if (payMethod == "h5" || wayCode == "ali_h5" || wayCode == "alipay_h5") {
            return alipayH5Pay(req, p);
        } else if (payMethod == "jsapi" || wayCode == "ali_jsapi" || wayCode == "wx_jsapi") {
            return jsapiPay(req, p);
        } else {
            return qrcodePay(req, p);
        }
    }

    // ═══ 查单接口 (银盛不支持主动查单) ════════════════════════════════
    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        r.errMsg = "银盛插件暂不支持主动查单";
        r.success = false;
        return r;
    }

    // ═══ 退款接口 ═══════════════════════════════════════════════════
    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string partnerId = p.get("partner_id", "").asString();
        std::string priKey   = p.get("private_key", "").asString();

        if (partnerId.empty() || priKey.empty()) {
            r.errMsg = "银盛参数不完整"; return r;
        }

        // 构造退款请求
        Json::Value biz;
        biz["out_trade_no"] = req.orderId;  // 原订单号
        biz["shopdate"] = formatDate(std::time(nullptr));
        biz["trade_no"] = req.channelOrderNo;  // 银盛订单号
        biz["refund_amount"] = formatAmount(req.refundAmount);
        biz["refund_reason"] = req.reason.empty() ? "申请退款" : req.reason;
        biz["out_request_no"] = req.refundNo;

        auto resp = callYsepay("ysepay.online.trade.refund", biz, p);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "退款请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        auto &data = j["ysepay_online_trade_refund_response"];
        std::string code = data.get("code", "").asString();
        if (code != "10000") {
            r.errMsg = data.get("sub_msg", data.get("msg", "退款失败")).asString();
            return r;
        }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = data.get("trade_no", req.refundNo).asString();
        r.refundAmount = req.refundAmount;
        return r;
    }

    // ═══ 关闭订单 (银盛不支持关单) ════════════════════════════════════
    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        r.success = false;
        r.errMsg = "银盛插件暂不支持关单";
        return r;
    }

    // ═══ 回调验证 ═══════════════════════════════════════════════════
    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";

        std::string pubKey = channelParams.get("platform_public_key", "").asString();

        // 解析回调参数
        std::string sign = getParam(params, "sign");
        if (sign.empty()) {
            r.verified = false; r.errMsg = "缺少签名"; return r;
        }

        // 构造验签数据 (按 key=value& 排序, 不含 sign)
        std::string signData = buildSignData(params);
        if (!pubKey.empty()) {
            // RSA 验签 (银盛使用 RSA PKCS1)
            r.verified = RsaUtils::verifySha1(signData, sign, pubKey);
        } else {
            r.verified = true;  // 无公钥时跳过验签
        }

        if (!r.verified) {
            r.errMsg = "验签失败"; return r;
        }

        std::string tradeStatus = getParam(params, "trade_status");
        r.paid = (tradeStatus == "TRADE_SUCCESS");

        r.orderId = getParam(params, "out_trade_no");
        r.channelOrderNo = getParam(params, "trade_no");
        r.buyerId = getParam(params, "buyer_logon_id");

        try {
            r.paidAmount = std::stod(getParam(params, "total_amount"));
        } catch (...) {}

        return r;
    }

private:
    // ═══ 扫码支付 ════════════════════════════════════════════════════
    ChannelOrderResult qrcodePay(const ChannelOrderRequest &req, const Json::Value &p) {
        ChannelOrderResult r;
        std::string partnerId   = p.get("partner_id", "").asString();
        std::string businessCode = p.get("business_code", "").asString();
        std::string sellerId     = p.get("seller_id", partnerId).asString();
        std::string wayCode      = req.payType;

        // 银行类型映射
        std::string bankType;
        if (wayCode == "wxpay" || wayCode == "wx_native") {
            bankType = "1902000";  // 微信
        } else if (wayCode == "bank" || wayCode == "unionpay") {
            bankType = "9001002";  // 银联
        } else {
            bankType = "1903000";  // 支付宝
        }

        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["shopdate"] = formatDate(std::time(nullptr));
        biz["subject"] = req.subject.empty() ? "商品" : req.subject;
        biz["total_amount"] = formatAmount(req.amount);
        biz["currency"] = "CNY";
        biz["seller_id"] = sellerId;
        biz["timeout_express"] = "2h";
        biz["business_code"] = businessCode;
        biz["bank_type"] = bankType;
        biz["submer_ip"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;

        auto resp = callYsepay("ysepay.online.qrcodepay", biz, p, req.notifyUrl);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "扫码请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        auto &data = j["ysepay_online_qrcodepay_response"];
        std::string code = data.get("code", "").asString();
        if (code != "10000") {
            r.errMsg = data.get("sub_msg", data.get("msg", "扫码下单失败")).asString();
            return r;
        }

        std::string qrCode = data.get("source_qr_code_url", "").asString();
        if (qrCode.empty()) {
            r.errMsg = "未返回二维码链接"; return r;
        }

        r.success = true;
        r.payUrl = qrCode;
        r.qrCode = qrCode;
        r.channelOrderNo = data.get("out_trade_no", req.orderId).asString();
        r.channelTradeNo = data.get("trade_no", "").asString();

        return r;
    }

    // ═══ 支付宝 H5 支付 ══════════════════════════════════════════════
    ChannelOrderResult alipayH5Pay(const ChannelOrderRequest &req, const Json::Value &p) {
        ChannelOrderResult r;
        std::string partnerId   = p.get("partner_id", "").asString();
        std::string businessCode = p.get("business_code", "").asString();
        std::string sellerId     = p.get("seller_id", partnerId).asString();

        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["shopdate"] = formatDate(std::time(nullptr));
        biz["subject"] = req.subject.empty() ? "商品" : req.subject;
        biz["total_amount"] = formatAmount(req.amount);
        biz["currency"] = "CNY";
        biz["seller_id"] = sellerId;
        biz["timeout_express"] = "2h";
        biz["business_code"] = businessCode;
        biz["pay_mode"] = "native";
        biz["bank_type"] = "1903000";

        std::map<std::string, std::string> urls;
        urls["notify_url"] = req.notifyUrl;
        if (!req.returnUrl.empty()) urls["return_url"] = req.returnUrl;

        // 银盛 H5 返回 HTML 表单
        auto resp = callYsepayPage("ysepay.online.wap.directpay.createbyuser", biz, p, urls);
        r.rawResponse = resp;

        if (resp.empty()) {
            r.errMsg = "H5 页面生成失败"; return r;
        }

        r.success = true;
        r.payUrl = resp;  // 返回自动提交表单
        r.extra["html"] = resp;
        r.channelOrderNo = req.orderId;

        return r;
    }

    // ═══ JSAPI 支付 ═══════════════════════════════════════════════════
    ChannelOrderResult jsapiPay(const ChannelOrderRequest &req, const Json::Value &p) {
        ChannelOrderResult r;
        std::string partnerId   = p.get("partner_id", "").asString();
        std::string businessCode = p.get("business_code", "").asString();
        std::string sellerId     = p.get("seller_id", partnerId).asString();
        std::string wayCode      = req.payType;

        Json::Value biz;
        biz["out_trade_no"] = req.orderId;
        biz["shopdate"] = formatDate(std::time(nullptr));
        biz["subject"] = req.subject.empty() ? "商品" : req.subject;
        biz["total_amount"] = formatAmount(req.amount);
        biz["currency"] = "CNY";
        biz["seller_id"] = sellerId;
        biz["timeout_express"] = "2h";
        biz["business_code"] = businessCode;
        biz["payer_ip"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;

        std::string method;
        std::string product;
        auto &extra = req.channelParams;

        if (wayCode == "wxpay" || wayCode == "wx_jsapi" || wayCode == "wx_mini") {
            // 微信 JSAPI/小程序
            method = "ysepay.online.weixin.pay";
            product = "weixin_jsapi";
            biz["appid"] = p.get("wx_appid", "").asString();
            biz["sub_openid"] = extra.get("mini_openid", extra.get("openid", "")).asString();
            biz["is_minipg"] = extra.get("mini_openid", "").asString().empty() ? "2" : "1";
        } else if (wayCode == "bank" || wayCode == "unionpay") {
            // 银联 JSAPI
            method = "ysepay.online.cupmulapp.qrcodepay";
            product = "cupmulapp";
            biz["spbill_create_ip"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;
            biz["bank_type"] = "9001002";
            biz["userId"] = extra.get("sub_openid", "").asString();
        } else {
            // 支付宝 JSAPI
            method = "ysepay.online.alijsapi.pay";
            product = "alipay_jsapi";
            biz["buyer_id"] = extra.get("buyer_id", extra.get("sub_openid", "")).asString();
        }

        auto resp = callYsepay(method, biz, p, req.notifyUrl);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "JSAPI请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        auto &data = j[methodToNode(method) + "_response"];
        std::string code = data.get("code", "").asString();
        if (code != "10000") {
            r.errMsg = data.get("sub_msg", data.get("msg", "JSAPI下单失败")).asString();
            return r;
        }

        r.success = true;
        r.channelOrderNo = data.get("out_trade_no", req.orderId).asString();
        r.channelTradeNo = data.get("trade_no", "").asString();

        // 支付宝返回 tradeNO, 微信返回调起参数
        std::string jsapiPayInfo = data.get("jsapi_pay_info", "").asString();
        if (!jsapiPayInfo.empty()) {
            Json::Value pi;
            if (Json::Reader().parse(jsapiPayInfo, pi)) {
                r.payUrl = pi.get("tradeNO", pi.get("url", "")).asString();
                r.extra["tradeNO"] = r.payUrl;
                r.extra["raw"] = jsapiPayInfo;
            }
        } else {
            r.payUrl = data.get("url", data.get("web_url", "")).asString();
        }

        return r;
    }

    // ═══ 调用银盛支付 API ═══════════════════════════════════════════
    SyncHttp::Response callYsepay(const std::string &method, const Json::Value &biz,
                                  const Json::Value &params, const std::string &notifyUrl = "") {
        std::string partnerId = params.get("partner_id", "").asString();
        std::string priKey    = params.get("private_key", "").asString();
        std::string certPwd  = params.get("private_cert_password", "").asString();

        // 构造请求参数
        Json::StreamWriterBuilder ww;
        ww["indentation"] = "";
        std::string bizContent = Json::writeString(ww, biz);

        std::map<std::string, std::string> payload;
        payload["method"] = method;
        payload["partner_id"] = partnerId;
        payload["timestamp"] = currentTimestamp();
        payload["charset"] = "UTF-8";
        payload["sign_type"] = "RSA";
        payload["version"] = "3.5";
        payload["biz_content"] = bizContent;
        if (!notifyUrl.empty()) payload["notify_url"] = notifyUrl;

        // 构造签名数据 (不含 sign, 按 key 排序)
        std::string signData = buildSignData(payload);
        std::string signature = RsaUtils::signSha1(signData, priKey);
        if (signature.empty()) {
            SyncHttp::Response r;
            r.success = false;
            r.errMsg = "签名失败";
            return r;
        }
        payload["sign"] = signature;

        // 构造 form-urlencoded 请求体
        std::string bodyStr = buildFormBody(payload);

        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"}
        };

        // 使用 SyncHttp 的 doRequestPublic 调用
        return SyncHttp::doRequestPublic("POST", "https://qrcode.ysepay.com/gateway.do", bodyStr, headers, 30);
    }

    // ═══ 银盛页面表单 (H5 支付) ════════════════════════════════════════
    std::string callYsepayPage(const std::string &method, const Json::Value &biz,
                                const Json::Value &params,
                                const std::map<std::string, std::string> &urls) {
        std::string partnerId = params.get("partner_id", "").asString();
        std::string priKey    = params.get("private_key", "").asString();

        Json::StreamWriterBuilder ww;
        ww["indentation"] = "";
        std::string bizContent = Json::writeString(ww, biz);

        std::map<std::string, std::string> payload;
        payload["method"] = method;
        payload["partner_id"] = partnerId;
        payload["timestamp"] = currentTimestamp();
        payload["charset"] = "UTF-8";
        payload["sign_type"] = "RSA";
        payload["version"] = "3.5";
        payload["biz_content"] = bizContent;
        for (auto &[k, v] : urls) {
            if (!v.empty()) payload[k] = v;
        }

        std::string signData = buildSignData(payload);
        std::string signature = RsaUtils::signSha1(signData, priKey);
        if (signature.empty()) return "";

        payload["sign"] = signature;

        // 生成 HTML 自动提交表单
        std::ostringstream html;
        html << "<form action=\"https://qrcode.ysepay.com/gateway.do\" method=\"post\" id=\"ysepayForm\">";
        for (auto &[k, v] : payload) {
            html << "<input type=\"hidden\" name=\"" << htmlEscape(k) << "\" value=\"" << htmlEscape(v) << "\">";
        }
        html << "</form><script>document.getElementById(\"ysepayForm\").submit();</script>";

        return html.str();
    }

    // ═══ 辅助方法 ═══════════════════════════════════════════════════

    // 构造银盛签名数据 (按 key 排序, 不含 sign)
    static std::string buildSignData(const std::map<std::string, std::string> &params) {
        std::vector<std::pair<std::string, std::string>> sorted;
        for (auto &[k, v] : params) {
            if (k == "sign" || v.empty()) continue;
            sorted.push_back({k, v});
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        std::ostringstream oss;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0) oss << "&";
            oss << sorted[i].first << "=" << sorted[i].second;
        }
        return oss.str();
    }

    // 构造 form-urlencoded 请求体
    static std::string buildFormBody(const std::map<std::string, std::string> &params) {
        std::ostringstream body;
        bool first = true;
        for (auto &[k, v] : params) {
            if (!first) body << "&";
            first = false;
            body << urlEncode(k) << "=" << urlEncode(v);
        }
        return body.str();
    }

    // 银盛方法名转响应节点名
    static std::string methodToNode(const std::string &method) {
        std::string node = method;
        for (char &c : node) {
            if (c == '.') c = '_';
        }
        return node;
    }

    // 格式化金额 (元, 2位小数)
    static std::string formatAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    // 格式化日期 (YYYYMMDD)
    static std::string formatDate(time_t t) {
        struct tm tt;
#ifdef _WIN32
        localtime_s(&tt, &t);
#else
        localtime_r(&t, &tt);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tt);
        return buf;
    }

    // 当前时间戳 (银盛格式: Y-m-d H:i:s)
    static std::string currentTimestamp() {
        time_t now = std::time(nullptr);
        struct tm tt;
#ifdef _WIN32
        localtime_s(&tt, &now);
#else
        localtime_r(&now, &tt);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tt);
        return buf;
    }

    // URL 编码
    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                oss << c;
            } else {
                oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
            }
        }
        return oss.str();
    }

    // HTML 转义
    static std::string htmlEscape(const std::string &s) {
        std::ostringstream oss;
        for (char c : s) {
            switch (c) {
                case '&': oss << "&amp;"; break;
                case '"': oss << "&quot;"; break;
                case '<': oss << "&lt;"; break;
                case '>': oss << "&gt;"; break;
                default: oss << c;
            }
        }
        return oss.str();
    }

    // 从 map 获取值
    static std::string getParam(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(YsePayPlugin);
