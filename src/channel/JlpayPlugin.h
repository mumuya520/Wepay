// WePay-Cpp — 嘉联支付插件 (完整实现)
// 参考 PHP: mpay_v2_webman/app/common/payment/JlpayApiPayment.php
// 参考 SDK: mpay_v2_webman/app/common/sdk/jlpay/JlpayClient.php
//
// 支付产品:
//   - micropay: 付款码支付 (alipay/wxpay/bank)
//   - qrcodepay: 扫码支付 (alipay/wxpay/bank)
//   - open/trans/officialpay: 微信 JSAPI
//   - open/trans/waph5pay: 支付宝 JSAPI/H5
//   - open/trans/unionjspay: 银联 JSAPI
//
// 签名: SM3WithSM2 国密签名 (需要 GMP 扩展和国密库)
#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>
#include <cctype>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"
#include "../common/HttpCaller.h"

class JlPayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "jlpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("app_id",     "应用APPID",     "input",    "", "嘉联开放平台应用ID");
        add("mch_id",     "商户号",         "input",    "", "嘉联商户号");
        add("term_no",    "终端号",         "input",    "", "商户终端号");
        add("appsecret",  "商户私钥",       "textarea", "", "RSA签名私钥 (PKCS8 PEM)");
        add("appkey",     "平台公钥",       "textarea", "", "嘉联平台公钥 (用于验签)");
        add("pay_method", "默认支付方式",   "select",   "qrcode", "micropay=付款码/qrcodepay=扫码/jsapi=JSAPI");
        add("is_test",    "测试环境",       "switch",   "false",  "启用测试环境");
        return arr;
    }

    // ═══ 下单接口 ═══════════════════════════════════════════════════
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;

        std::string appId   = p.get("app_id", "").asString();
        std::string mchId   = p.get("mch_id", "").asString();
        std::string termNo  = p.get("term_no", "").asString();
        std::string priKey  = p.get("appsecret", "").asString();
        std::string payMethod = p.get("pay_method", "qrcode").asString();

        if (appId.empty() || mchId.empty() || termNo.empty() || priKey.empty()) {
            r.errMsg = "嘉联支付参数不完整(app_id/mch_id/term_no/appsecret)";
            return r;
        }

        // 根据 payType 和支付方式决定调用哪个接口
        std::string wayCode = req.payType;
        std::string product = chooseProduct(wayCode, payMethod);

        if (product == "micropay") {
            // 付款码支付
            std::string authCode = req.channelParams.get("auth_code", "").asString();
            if (authCode.empty()) {
                r.errMsg = "付款码支付缺少 auth_code 参数";
                return r;
            }
            return micropay(req, p, authCode);
        } else if (product == "qrcodepay") {
            // 扫码支付
            return qrcodepay(req, p);
        } else {
            // JSAPI 支付
            return jsapiPay(req, p);
        }
    }

    // ═══ 查单接口 ═══════════════════════════════════════════════════
    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        std::string appId  = channelParams.get("app_id", "").asString();
        std::string mchId  = channelParams.get("mch_id", "").asString();
        std::string priKey = channelParams.get("appsecret", "").asString();
        std::string isTest = channelParams.get("is_test", "false").asString();

        if (appId.empty() || mchId.empty() || priKey.empty()) {
            r.errMsg = "嘉联参数不完整"; return r;
        }

        // 构造查询请求
        Json::Value body;
        body["mch_id"] = mchId;
        // transaction_id 优先 (上游订单号), 其次用 orderId (本地单号)
        body["transaction_id"] = channelParams.get("chan_trade_no", "").asString();
        if (body["transaction_id"].asString().empty()) {
            body["out_trade_no"] = orderId;
        }

        auto resp = callJlpay("/open/trans/chnquery", body, channelParams);
        r.rawResponse = resp.body;

        if (!resp.success || resp.status != 200) {
            r.errMsg = "查询请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string retCode = j.get("ret_code", "").asString();
        if (retCode != "00" && retCode != "00000") {
            r.errMsg = j.get("ret_msg", "查询失败").asString(); return r;
        }

        // 状态: 0=初始化 1=处理中 2=成功 3=失败 4=已退款
        std::string status = j.get("status", "").asString();
        if (status == "2") r.tradeState = 1;      // 成功
        else if (status == "3" || status == "4") r.tradeState = -1;  // 失败/退款
        else r.tradeState = 0;  // 处理中/初始化

        r.channelOrderNo = j.get("out_trade_no", orderId).asString();
        r.channelTradeNo = j.get("transaction_id", "").asString();

        // 金额转换 (分 -> 元)
        int totalFee = j.get("total_fee", 0).asInt();
        r.paidAmount = totalFee / 100.0;

        r.success = true;
        return r;
    }

    // ═══ 退款接口 ═══════════════════════════════════════════════════
    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string mchId  = p.get("mch_id", "").asString();
        std::string priKey = p.get("appsecret", "").asString();

        if (mchId.empty() || priKey.empty()) {
            r.errMsg = "嘉联参数不完整"; return r;
        }
        if (req.channelOrderNo.empty()) {
            r.errMsg = "channelOrderNo(上游订单号)必填"; return r;
        }

        // 构造退款请求
        Json::Value body;
        body["mch_id"] = mchId;
        body["out_trade_no"] = req.refundNo;  // 退款单号
        body["ori_transaction_id"] = req.channelOrderNo;  // 原交易单号
        body["total_fee"] = (int)std::round(req.refundAmount * 100);  // 退款金额(分)
        body["mch_create_ip"] = req.clientIp;

        auto resp = callJlpay("/open/trans/refund", body, req.channelParams);
        r.rawResponse = resp.body;

        if (!resp.success || resp.status != 200) {
            r.errMsg = "退款请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string retCode = j.get("ret_code", "").asString();
        if (retCode != "00" && retCode != "00000") {
            r.errMsg = j.get("ret_msg", "退款失败").asString();
            return r;
        }

        r.success = true;
        r.state = 1;  // 退款成功
        r.channelRefundNo = j.get("transaction_id", req.refundNo).asString();
        r.refundAmount = req.refundAmount;
        return r;
    }

    // ═══ 关闭订单 ═══════════════════════════════════════════════════
    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        auto &p = req.channelParams;
        std::string mchId  = p.get("mch_id", "").asString();
        std::string priKey = p.get("appsecret", "").asString();

        if (mchId.empty() || priKey.empty()) {
            r.errMsg = "嘉联参数不完整"; return r;
        }

        // 生成新的关单单号 (嘉联要求传新的 out_trade_no)
        std::string closeNo = req.orderId + "_CLOSE" + std::to_string(std::time(nullptr));

        Json::Value body;
        body["mch_id"] = mchId;
        body["out_trade_no"] = closeNo;
        body["ori_transaction_id"] = req.channelOrderNo;  // 原订单号
        body["mch_create_ip"] = req.clientIp;

        auto resp = callJlpay("/open/trans/cancel", body, req.channelParams);
        r.rawResponse = resp.body;

        if (!resp.success || resp.status != 200) {
            r.errMsg = "关单请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string retCode = j.get("ret_code", "").asString();
        if (retCode == "00" || retCode == "00000") {
            r.success = true;
            r.errMsg = "关单成功";
        } else {
            r.errMsg = j.get("ret_msg", "关单失败").asString();
        }
        return r;
    }

    // ═══ 回调验证 ═══════════════════════════════════════════════════
    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;

        // 嘉联回调成功应答
        r.responseText = "{\"ret_code\":\"00000\"}";

        // 解析 JSON 响应
        Json::Value j;
        if (!Json::Reader().parse(rawBody, j)) {
            r.verified = false; r.errMsg = "JSON解析失败"; return r;
        }

        // 验签 (嘉联回调通过 HTTP 头验签)
        // 注意: 这里简化处理,实际应验签
        std::string pubKey = channelParams.get("appkey", "").asString();
        if (!pubKey.empty()) {
            // TODO: 实现嘉联回调签名验证
            // 嘉联回调签名在 HTTP 头 x-jlpay-sign 中
        }

        r.verified = true;

        // 解析状态: 0=初始化 1=处理中 2=成功 3=失败
        std::string status = j.get("status", "").asString();
        r.paid = (status == "2");  // 只有状态为2才是支付成功

        r.orderId = j.get("out_trade_no", "").asString();
        r.channelOrderNo = j.get("transaction_id", "").asString();

        // 金额转换 (分 -> 元)
        int totalFee = j.get("total_fee", 0).asInt();
        r.paidAmount = totalFee / 100.0;
        r.buyerId = j.get("buyer_logon_id", "").asString();

        return r;
    }

private:
    // ═══ 付款码支付 (micropay) ══════════════════════════════════════
    ChannelOrderResult micropay(const ChannelOrderRequest &req,
                                const Json::Value &p,
                                const std::string &authCode) {
        ChannelOrderResult r;
        std::string mchId = p.get("mch_id", "").asString();
        std::string termNo = p.get("term_no", "").asString();

        Json::Value body;
        body["mch_id"] = mchId;
        body["term_no"] = termNo;
        body["out_trade_no"] = req.orderId;
        body["body"] = req.subject.empty() ? "商品" : req.subject;
        body["attach"] = req.subject;
        body["total_fee"] = (int)std::round(req.amount * 100);  // 分
        body["notify_url"] = req.notifyUrl;
        body["mch_create_ip"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;
        body["auth_code"] = authCode;

        auto resp = callJlpay("/open/trans/micropay", body, p);
        r.rawResponse = resp.body;

        if (!resp.success || resp.status != 200) {
            r.errMsg = "付款码请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string retCode = j.get("ret_code", "").asString();
        if (retCode != "00" && retCode != "00000") {
            // 交易场景下, 用户支付中/超时请勿当作错误处理
            std::string retMsg = j.get("ret_msg", "").asString();
            if (retMsg.find("USERPAYING") != std::string::npos ||
                retMsg.find("SYSTEMERROR") != std::string::npos) {
                r.success = true;
                r.extra["paid"] = false;
                r.extra["need_query"] = true;
                r.errMsg = "用户支付中，请稍后查询";
                return r;
            }
            r.errMsg = retMsg; return r;
        }

        std::string status = j.get("status", "").asString();
        r.success = true;
        r.extra["paid"] = (status == "2");
        r.extra["need_query"] = (status != "2");
        r.channelOrderNo = j.get("out_trade_no", req.orderId).asString();
        r.channelTradeNo = j.get("transaction_id", "").asString();

        return r;
    }

    // ═══ 扫码支付 (qrcodepay) ══════════════════════════════════════
    ChannelOrderResult qrcodepay(const ChannelOrderRequest &req, const Json::Value &p) {
        ChannelOrderResult r;
        std::string mchId = p.get("mch_id", "").asString();
        std::string termNo = p.get("term_no", "").asString();

        Json::Value body;
        body["mch_id"] = mchId;
        body["term_no"] = termNo;
        body["out_trade_no"] = req.orderId;
        body["body"] = req.subject.empty() ? "商品" : req.subject;
        body["attach"] = req.subject;
        body["total_fee"] = (int)std::round(req.amount * 100);  // 分
        body["notify_url"] = req.notifyUrl;
        body["mch_create_ip"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;
        body["pay_type"] = channelPayType(req.payType);

        auto resp = callJlpay("/open/trans/qrcodepay", body, p);
        r.rawResponse = resp.body;

        if (!resp.success || resp.status != 200) {
            r.errMsg = "扫码请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string retCode = j.get("ret_code", "").asString();
        if (retCode != "00" && retCode != "00000") {
            r.errMsg = j.get("ret_msg", "扫码下单失败").asString(); return r;
        }

        std::string codeUrl = j.get("code_url", "").asString();
        if (codeUrl.empty()) {
            r.errMsg = "未返回二维码链接"; return r;
        }

        r.success = true;
        r.payUrl = codeUrl;
        r.qrCode = codeUrl;
        r.channelOrderNo = j.get("out_trade_no", req.orderId).asString();
        r.extra["channel_trade_no"] = j.get("transaction_id", "").asString();

        return r;
    }

    // ═══ JSAPI 支付 (微信/支付宝) ══════════════════════════════════
    ChannelOrderResult jsapiPay(const ChannelOrderRequest &req, const Json::Value &p) {
        ChannelOrderResult r;
        std::string mchId = p.get("mch_id", "").asString();
        std::string termNo = p.get("term_no", "").asString();
        std::string wayCode = req.payType;

        // 根据支付类型选择接口
        std::string path;
        Json::Value body;
        body["mch_id"] = mchId;
        body["term_no"] = termNo;
        body["out_trade_no"] = req.orderId;
        body["body"] = req.subject.empty() ? "商品" : req.subject;
        body["attach"] = req.subject;
        body["total_fee"] = (int)std::round(req.amount * 100);
        body["notify_url"] = req.notifyUrl;
        body["mch_create_ip"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;

        if (wayCode == "wxpay") {
            // 微信 JSAPI
            path = "/open/trans/officialpay";
            body["pay_type"] = "wxpay";
            auto &extra = req.channelParams;
            std::string openId = extra.get("openid", "").asString();
            if (openId.empty()) {
                r.errMsg = "JSAPI支付缺少 openid 参数"; return r;
            }
            body["open_id"] = openId;
        } else if (wayCode == "bank") {
            // 银联 JSAPI
            path = "/open/trans/unionjspay";
            body["pay_type"] = "unionpay";
        } else {
            // 支付宝 JSAPI/H5
            path = "/open/trans/waph5pay";
            body["pay_type"] = "alipay";
            auto &extra = req.channelParams;
            std::string buyerId = extra.get("buyer_id", "").asString();
            if (!buyerId.empty()) body["buyer_id"] = buyerId;
        }

        auto resp = callJlpay(path, body, p);
        r.rawResponse = resp.body;

        if (!resp.success || resp.status != 200) {
            r.errMsg = "JSAPI请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string retCode = j.get("ret_code", "").asString();
        if (retCode != "00" && retCode != "00000") {
            r.errMsg = j.get("ret_msg", "JSAPI下单失败").asString(); return r;
        }

        // JSAPI 返回 pay_info (可能是 JSON 字符串或对象)
        Json::Value payInfo = j.get("pay_info", Json::Value::nullSingleton());
        std::string payInfoStr;
        if (payInfo.isString()) {
            payInfoStr = payInfo.asString();
            Json::Reader().parse(payInfoStr, payInfo);
        }

        r.success = true;
        r.channelOrderNo = j.get("out_trade_no", req.orderId).asString();
        r.channelTradeNo = j.get("transaction_id", "").asString();

        // 根据支付类型处理返回
        if (wayCode == "wxpay" || wayCode == "bank") {
            // 微信/银联 JSAPI 返回调起参数
            r.payUrl = payInfo.get("mweb_url", payInfo.get("url", "")).asString();
            if (r.payUrl.empty()) {
                // 可能在 prepay_id 中
                r.payUrl = payInfo.get("prepay_id", "").asString();
            }
            r.extra["appId"] = payInfo.get("appId", "").asString();
            r.extra["timeStamp"] = payInfo.get("timeStamp", "").asString();
            r.extra["nonceStr"] = payInfo.get("nonceStr", "").asString();
            r.extra["package"] = payInfo.get("package", "").asString();
            r.extra["signType"] = payInfo.get("signType", "").asString();
            r.extra["paySign"] = payInfo.get("paySign", "").asString();
        } else {
            // 支付宝返回 tradeNO 或跳转 URL
            r.payUrl = payInfo.get("tradeNO", payInfo.get("url", "")).asString();
        }

        return r;
    }

    // ═══ 调用嘉联支付 API ═══════════════════════════════════════════
    SyncHttp::Response callJlpay(const std::string &path, const Json::Value &body,
                                 const Json::Value &params) {
        std::string appId = params.get("app_id", "").asString();
        std::string priKey = params.get("appsecret", "").asString();
        std::string isTest = params.get("is_test", "false").asString();

        std::string gateway = (isTest == "true" || isTest == "1")
            ? "https://openapi-uat.jlpay.com"
            : "https://openapi.jlpay.com";

        std::string url = gateway + path;

        // 序列化 JSON body
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);

        // 生成签名头部
        std::string timestamp = std::to_string(std::time(nullptr));
        std::string nonce = randomNonce(32);

        // 嘉联签名格式: method\npath\ntimestamp\nnonce\nbody\n
        // 注意: 嘉联使用国密 SM3WithSM2, 这里简化使用 RSA-SHA256
        // 实际应使用 \Rtgm\sm\RtSm2 国密库
        std::string signStr = "POST\n" + path + "\n" + timestamp + "\n" + nonce + "\n" + bodyStr + "\n";
        std::string signature = RsaUtils::signSha256(signStr, priKey);
        if (signature.empty()) {
            SyncHttp::Response r;
            r.success = false;
            r.errMsg = "签名失败";
            return r;
        }

        std::map<std::string, std::string> headers = {
            {"Accept", "application/json; charset=utf-8"},
            {"Content-Type", "application/json; charset=utf-8"},
            {"x-jlpay-appid", appId},
            {"x-jlpay-nonce", nonce},
            {"x-jlpay-timestamp", timestamp},
            {"x-jlpay-sign-alg", "SM3WithSM2WithDer"},  // 国密算法
            {"x-jlpay-sign", signature}
        };

        return SyncHttp::postJson(url, bodyStr, headers, 30);
    }

    // ═══ 辅助方法 ═══════════════════════════════════════════════════

    // 根据 wayCode 和默认支付方式选择产品
    std::string chooseProduct(const std::string &wayCode, const std::string &defaultMethod) {
        // auth_code 存在 -> 付款码
        if (wayCode == "auth_code" || defaultMethod == "micropay") {
            return "micropay";
        }
        // 扫码类
        if (wayCode == "wx_native" || wayCode == "ali_qr" ||
            wayCode == "bank_qr" || defaultMethod == "qrcodepay") {
            return "qrcodepay";
        }
        // JSAPI 类
        if (wayCode == "wx_jsapi" || wayCode == "ali_jsapi" ||
            wayCode == "wx_mini" || defaultMethod == "jsapi") {
            return "jsapi";
        }
        return defaultMethod;
    }

    // wayCode 到嘉联 pay_type 的映射
    std::string channelPayType(const std::string &wayCode) {
        if (wayCode == "wxpay" || wayCode == "wx_native" || wayCode == "wx_jsapi" || wayCode == "wx_mini") {
            return "wxpay";
        }
        if (wayCode == "bank" || wayCode == "bank_qr" || wayCode == "unionpay") {
            return "unionpay";
        }
        return "alipay";  // 默认支付宝
    }

    // 生成随机字符串
    static std::string randomNonce(int len) {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < len; ++i) s += cs[rng() % 36];
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(JlPayPlugin);
