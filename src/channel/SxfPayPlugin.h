// WePay-Cpp — 随行付 OpenAPI 支付插件 (完整实现)
// 参考 PHP: mpay_v2_webman/app/common/payment/SuixingpayApiPayment.php
// 参考 SDK: mpay_v2_webman/app/common/sdk/suixingpay/SuixingpayClient.php
//
// 支付产品:
//   - ALIPAY: 支付宝扫码/JSAPI
//   - WECHAT: 微信扫码/JSAPI
//   - UNIONPAY: 云闪付扫码
//
// 签名: RSA-SHA1, reqData 嵌套在报文中
#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class SxfPayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "sxfpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("org_id",       "机构编号",       "input",    "", "随行付机构编号");
        add("merchant_no",  "商户编号",       "input",    "", "随行付商户编号");
        add("appsecret",    "商户私钥",       "textarea", "", "RSA 商户私钥 (PKCS8 PEM)");
        add("appkey",       "平台公钥",       "textarea", "", "随行付平台公钥");
        add("pay_method",   "默认支付方式",   "select",   "qrcode", "qrcode=扫码/jsapi=JSAPI");
        add("is_test",      "测试环境",       "switch",   "false",  "启用测试环境");
        return arr;
    }

    // ═══ 下单接口 ═══════════════════════════════════════════════════
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;

        std::string orgId     = p.get("org_id", "").asString();
        std::string merchantNo = p.get("merchant_no", "").asString();
        std::string priKey    = p.get("appsecret", "").asString();
        std::string payMethod = p.get("pay_method", "qrcode").asString();
        std::string wayCode   = req.payType;

        if (orgId.empty() || merchantNo.empty() || priKey.empty()) {
            r.errMsg = "随行付参数不完整(org_id/merchant_no/appsecret)";
            return r;
        }

        if (payMethod == "jsapi" || wayCode == "ali_jsapi" || wayCode == "wx_jsapi") {
            return jsapiPay(req, p);
        } else {
            return scanPay(req, p);
        }
    }

    // ═══ 查单接口 ═══════════════════════════════════════════════════
    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        std::string orgId     = channelParams.get("org_id", "").asString();
        std::string merchantNo = channelParams.get("merchant_no", "").asString();
        std::string priKey    = channelParams.get("appsecret", "").asString();

        if (orgId.empty() || merchantNo.empty() || priKey.empty()) {
            r.errMsg = "随行付参数不完整"; return r;
        }

        Json::Value reqData;
        reqData["mno"] = merchantNo;
        reqData["ordNo"] = orderId;

        auto resp = callSxfpay("/query/tradeQuery", reqData, channelParams);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "查询请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string code = j.get("code", "").asString();
        if (code != "0000") {
            r.errMsg = j.get("msg", "查询失败").asString(); return r;
        }

        auto &data = j["respData"];
        // 状态: 0=初始化 1=处理中 2=成功 3=失败 4=已退款
        std::string status = data.get("ordSts", "").asString();
        if (status == "SUCCESS") r.tradeState = 1;
        else if (status == "FAIL" || status == "CLOSE") r.tradeState = -1;
        else r.tradeState = 0;

        r.success = true;
        r.channelOrderNo = data.get("ordNo", orderId).asString();
        r.channelTradeNo = data.get("txnNo", "").asString();

        return r;
    }

    // ═══ 退款接口 ═══════════════════════════════════════════════════
    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string merchantNo = p.get("merchant_no", "").asString();

        if (merchantNo.empty()) {
            r.errMsg = "随行付参数不完整"; return r;
        }

        Json::Value reqData;
        reqData["mno"] = merchantNo;
        reqData["ordNo"] = req.orderId;
        reqData["refundAmount"] = (int)std::round(req.refundAmount * 100);  // 分
        reqData["refundReqNo"] = req.refundNo;

        auto resp = callSxfpay("/trade/refund", reqData, req.channelParams);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "退款请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string code = j.get("code", "").asString();
        if (code != "0000") {
            r.errMsg = j.get("msg", "退款失败").asString(); return r;
        }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = j["respData"].get("refundNo", req.refundNo).asString();

        return r;
    }

    // ═══ 关闭订单 ═══════════════════════════════════════════════════
    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        auto &p = req.channelParams;
        std::string merchantNo = p.get("merchant_no", "").asString();

        if (merchantNo.empty()) {
            r.errMsg = "随行付参数不完整"; return r;
        }

        Json::Value reqData;
        reqData["mno"] = merchantNo;
        reqData["ordNo"] = req.orderId;

        auto resp = callSxfpay("/trade/close", reqData, req.channelParams);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "关单请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string code = j.get("code", "").asString();
        if (code == "0000") {
            r.success = true;
            r.errMsg = "关单成功";
        } else {
            r.errMsg = j.get("msg", "关单失败").asString();
        }

        return r;
    }

    // ═══ 回调验证 ═══════════════════════════════════════════════════
    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";

        std::string pubKey = channelParams.get("appkey", "").asString();
        if (pubKey.empty()) {
            r.verified = false; r.errMsg = "缺少平台公钥"; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(rawBody, j)) {
            r.verified = false; r.errMsg = "JSON解析失败"; return r;
        }

        // 验签
        std::string sign = j.get("sign", "").asString();
        if (!sign.empty()) {
            std::string signData = buildSignContent(j);
            r.verified = RsaUtils::verifySha1(signData, sign, pubKey);
        } else {
            r.verified = true;  // 无签名时跳过
        }

        if (!r.verified) {
            r.errMsg = "验签失败"; return r;
        }

        auto &data = j["respData"];
        std::string status = data.get("ordSts", "").asString();
        r.paid = (status == "SUCCESS");

        r.orderId = data.get("ordNo", "").asString();
        r.channelOrderNo = data.get("txnNo", "").asString();

        int amount = data.get("txnAmt", 0).asInt();
        r.paidAmount = amount / 100.0;

        return r;
    }

private:
    // ═══ 扫码支付 ════════════════════════════════════════════════════
    ChannelOrderResult scanPay(const ChannelOrderRequest &req, const Json::Value &p) {
        ChannelOrderResult r;
        std::string merchantNo = p.get("merchant_no", "").asString();
        std::string wayCode = req.payType;

        Json::Value reqData;
        reqData["mno"] = merchantNo;
        reqData["ordNo"] = req.orderId;
        reqData["txnAmt"] = (int)std::round(req.amount * 100);  // 分
        reqData["orderSubject"] = req.subject.empty() ? "商品" : req.subject;
        reqData["txnTime"] = formatTimestamp(std::time(nullptr));
        reqData["payType"] = channelPayType(wayCode);
        reqData["notifyUrl"] = req.notifyUrl;
        reqData["clientIp"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;

        auto resp = callSxfpay("/trade/h5pay", reqData, p);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "扫码请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string code = j.get("code", "").asString();
        if (code != "0000") {
            r.errMsg = j.get("msg", "扫码下单失败").asString(); return r;
        }

        auto &data = j["respData"];

        // 支付宝返回 code_url, 微信返回 mweb_url
        std::string codeUrl = data.get("codeUrl", data.get("mwebUrl", "").asString()).asString();
        if (codeUrl.empty()) {
            r.errMsg = "未返回支付链接"; return r;
        }

        r.success = true;
        r.payUrl = codeUrl;
        r.qrCode = codeUrl;
        r.channelOrderNo = data.get("ordNo", req.orderId).asString();
        r.channelTradeNo = data.get("txnNo", "").asString();

        return r;
    }

    // ═══ JSAPI 支付 ═══════════════════════════════════════════════════
    ChannelOrderResult jsapiPay(const ChannelOrderRequest &req, const Json::Value &p) {
        ChannelOrderResult r;
        std::string merchantNo = p.get("merchant_no", "").asString();
        std::string wayCode = req.payType;

        Json::Value reqData;
        reqData["mno"] = merchantNo;
        reqData["ordNo"] = req.orderId;
        reqData["txnAmt"] = (int)std::round(req.amount * 100);
        reqData["orderSubject"] = req.subject.empty() ? "商品" : req.subject;
        reqData["txnTime"] = formatTimestamp(std::time(nullptr));
        reqData["notifyUrl"] = req.notifyUrl;
        reqData["clientIp"] = req.clientIp.empty() ? "127.0.0.1" : req.clientIp;

        std::string payType = channelPayType(wayCode);
        reqData["payType"] = payType;

        if (wayCode == "wxpay" || wayCode == "wx_jsapi") {
            // 微信 JSAPI 需要 openid
            std::string openId = req.channelParams.get("openid", "").asString();
            if (!openId.empty()) {
                reqData["subOpenId"] = openId;
            }
        } else {
            // 支付宝 JSAPI 需要 buyerId
            std::string buyerId = req.channelParams.get("buyer_id", "").asString();
            if (!buyerId.empty()) {
                reqData["buyerId"] = buyerId;
            }
        }

        auto resp = callSxfpay("/trade/jsapi", reqData, p);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "JSAPI请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        std::string code = j.get("code", "").asString();
        if (code != "0000") {
            r.errMsg = j.get("msg", "JSAPI下单失败").asString(); return r;
        }

        auto &data = j["respData"];
        r.success = true;
        r.channelOrderNo = data.get("ordNo", req.orderId).asString();
        r.channelTradeNo = data.get("txnNo", "").asString();

        // JSAPI 返回调起参数
        r.payUrl = data.get("payInfo", "").asString();
        if (r.payUrl.empty()) {
            r.payUrl = data.get("mwebUrl", "").asString();
        }
        r.extra["appId"] = data.get("appId", "").asString();
        r.extra["timeStamp"] = data.get("timeStamp", "").asString();
        r.extra["nonceStr"] = data.get("nonceStr", "").asString();
        r.extra["package"] = data.get("package", "").asString();
        r.extra["signType"] = data.get("signType", "").asString();
        r.extra["paySign"] = data.get("paySign", "").asString();

        return r;
    }

    // ═══ 调用随行付 API ═══════════════════════════════════════════
    SyncHttp::Response callSxfpay(const std::string &path, const Json::Value &reqData,
                                  const Json::Value &params) {
        std::string orgId = params.get("org_id", "").asString();
        std::string priKey = params.get("appsecret", "").asString();
        std::string isTest = params.get("is_test", "false").asString();

        std::string gateway;
        if (isTest == "true" || isTest == "1") {
            gateway = "https://openapi-test.tianquetech.com";
        } else {
            gateway = "https://openapi.tianquetech.com";
        }
        std::string url = gateway + path;

        // 构造请求报文
        Json::Value payload;
        payload["orgId"] = orgId;
        payload["reqId"] = randomReqId();
        payload["reqData"] = reqData;
        payload["timestamp"] = formatTimestampFull(std::time(nullptr));
        payload["version"] = "1.0";
        payload["signType"] = "RSA";

        // 生成签名
        std::string signData = buildSignContent(payload);
        std::string sign = RsaUtils::signSha1(signData, priKey);
        if (sign.empty()) {
            SyncHttp::Response r;
            r.success = false;
            r.errMsg = "签名失败";
            return r;
        }
        payload["sign"] = sign;

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, payload);

        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json; charset=utf-8"},
            {"Accept", "application/json"}
        };

        return SyncHttp::postJson(url, bodyStr, headers);
    }

    // ═══ 辅助方法 ═══════════════════════════════════════════════════

    // 支付方式映射
    static std::string channelPayType(const std::string &wayCode) {
        if (wayCode == "wxpay" || wayCode == "wx_native" || wayCode == "wx_jsapi" || wayCode == "wx_mini") {
            return "WECHAT";
        }
        if (wayCode == "bank" || wayCode == "unionpay") {
            return "UNIONPAY";
        }
        return "ALIPAY";
    }

    // 构造签名数据 (按 key 排序, 不含 sign)
    static std::string buildSignContent(const Json::Value &payload) {
        std::vector<std::pair<std::string, std::string>> items;
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            std::string key = it.key().asString();
            if (key == "sign") continue;

            std::string value;
            if (it->isObject() || it->isArray()) {
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                value = Json::writeString(wb, *it);
            } else {
                value = it->asString();
            }
            items.push_back({key, value});
        }
        std::sort(items.begin(), items.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        std::ostringstream oss;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) oss << "&";
            oss << items[i].first << "=" << items[i].second;
        }
        return oss.str();
    }

    // 生成唯一请求 ID
    static std::string randomReqId() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        std::ostringstream oss;
        oss << ms << (std::rand() % 10000);
        return oss.str();
    }

    // 格式化时间戳 (YYYYMMDDHHmmss)
    static std::string formatTimestampFull(time_t t) {
        struct tm tt;
#ifdef _WIN32
        localtime_s(&tt, &t);
#else
        localtime_r(&t, &tt);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tt);
        return buf;
    }

    // 格式化时间戳 (YYYYMMDD)
    static std::string formatTimestamp(time_t t) {
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
};

REGISTER_CHANNEL_PLUGIN(SxfPayPlugin);
