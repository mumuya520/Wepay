// WePay-Cpp — 9家中小三方支付插件骨架
// 这些通道协议各家差异较大，本文件提供"通用 RSA + JSON" 骨架
// 用户在 plugin_store 安装后，可在通道管理填入参数运行；
// 真实 API 协议请按各家文档完善 createOrder 实际调用。
//
// 已支持: HkrtPay JlPay LcswPay LesPay PpPay SxfPay UmsPay XxPay YsePay
#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// ═══ 通用基类：RSA-SHA256 签名 + JSON POST ═══
class GenericRsaPlugin : public ChannelPlugin {
public:
    virtual std::string vendorName() const = 0;  // 厂商名(用于错误提示)
    virtual std::string defaultGateway() const = 0;

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("gateway", "接口网关", "input", defaultGateway(), "对应插件的接口地址/API URL");
        add("merchant_id", "商户号/应用ID", "input", "", "对应彩虹易 appid/mch_id/merchant_id");
        add("private_key", "商户私钥/签名密钥", "textarea", "", "RSA 私钥或上游要求的签名私钥");
        add("public_key", "平台公钥", "textarea", "", "验签使用，可为空");
        add("pay_method", "支付方式", "input", "qr", "qr/bar/jsapi/h5/app，具体以通道文档为准");
        add("auth_code", "付款码", "input", "", "条码支付时使用");
        add("openid", "用户标识", "input", "", "JSAPI/公众号支付时使用");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        std::string merId  = p.get("merchant_id", "").asString();
        std::string priKey = p.get("private_key", "").asString();
        std::string gateway = p.get("gateway", defaultGateway()).asString();
        std::string method = p.get("pay_method", "qr").asString();

        if (merId.empty() || priKey.empty()) {
            r.errMsg = vendorName() + " 参数不完整(merchant_id/private_key)"; return r;
        }

        Json::Value body;
        body["merchant_id"]  = merId;
        body["out_trade_no"] = req.orderId;
        body["total_amount"] = (int)std::round(req.amount * 100);
        body["subject"]      = req.subject.empty() ? "商品" : req.subject;
        body["notify_url"]   = req.notifyUrl;
        body["pay_method"]   = method;
        body["timestamp"]    = std::to_string(std::time(nullptr));
        body["nonce"]        = randomNonce();
        if (method == "bar") body["auth_code"] = p.get("auth_code", "").asString();
        if (method == "jsapi") body["openid"]  = p.get("openid", "").asString();

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);
        std::string sign = RsaUtils::signSha256(bodyStr, priKey);

        Json::Value envelope;
        envelope["data"] = body;
        envelope["sign"] = sign;
        std::string envStr = Json::writeString(wb, envelope);

        auto resp = SyncHttp::postJson(gateway, envStr);
        r.rawResponse = resp.body;
        if (!resp.success) {
            r.errMsg = vendorName() + " 请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }
        std::string code = j.get("code", j.get("ret_code", "").asString()).asString();
        if (code != "0" && code != "00" && code != "000000" && code != "SUCCESS") {
            r.errMsg = vendorName() + " 错误: " + code + " " +
                       j.get("msg", j.get("ret_msg", "").asString()).asString();
            return r;
        }

        // 通用解析
        if (method == "bar") {
            std::string status = j.get("trade_status", j.get("status", "").asString()).asString();
            r.success = true;
            r.extra["paid"] = (status == "SUCCESS" || status == "PAID");
            r.channelOrderNo = j.get("trade_no", "").asString();
        } else {
            std::string url = j.get("pay_url", j.get("qr_code", j.get("code_url", "").asString()).asString()).asString();
            r.success = !url.empty();
            r.payUrl = url; r.qrCode = url;
            r.channelOrderNo = j.get("trade_no", "").asString();
        }
        return r;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "SUCCESS";
        std::string pubKey = channelParams.get("public_key", "").asString();

        Json::Value body;
        if (Json::Reader().parse(rawBody, body)) {
            std::string sign = body.get("sign", "").asString();
            auto &data = body["data"];
            Json::StreamWriterBuilder wb; wb["indentation"] = "";
            std::string dataStr = Json::writeString(wb, data);

            if (!pubKey.empty() && !sign.empty()) {
                r.verified = RsaUtils::verifySha256(dataStr, sign, pubKey);
            } else r.verified = true;

            if (r.verified) {
                std::string status = data.get("trade_status", data.get("status", "").asString()).asString();
                r.paid = (status == "SUCCESS" || status == "PAID" || status == "TRADE_SUCCESS");
                r.orderId = data.get("out_trade_no", "").asString();
                r.channelOrderNo = data.get("trade_no", "").asString();
                try { r.paidAmount = data.get("total_amount", 0).asInt() / 100.0; } catch(...){}
            }
        }
        return r;
    }

private:
    static std::string randomNonce() {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < 16; ++i) s += cs[rng() % 36];
        return s;
    }
};

// ═══ 9 家中小三方支付插件骨架(继承 GenericRsaPlugin) ═══
// 注意: JlPayPlugin, YsePayPlugin, SxfPayPlugin 已单独实现完整版本

#define DECLARE_GENERIC_PLUGIN(ClsName, codeName, vendor, gw)                          \
    class ClsName : public GenericRsaPlugin {                                          \
    public:                                                                            \
        std::string name() const override { return codeName; }                         \
        std::string vendorName() const override { return vendor; }                     \
        std::string defaultGateway() const override { return gw; }                     \
    };                                                                                 \
    REGISTER_CHANNEL_PLUGIN(ClsName)

DECLARE_GENERIC_PLUGIN(HkrtPayPlugin,  "hkrtpay",  "海科融通", "https://api.hkrt.com/gateway");
// JlPayPlugin 已完整实现于 JlPayPlugin.h
DECLARE_GENERIC_PLUGIN(LcswPayPlugin,  "lcswpay",  "利楚扫呗", "https://api.lcsw.cn/lcsw/v3");
DECLARE_GENERIC_PLUGIN(LesPayPlugin,   "lespay",   "乐刷",     "https://api.leshuazf.com/gateway");
DECLARE_GENERIC_PLUGIN(PpPayPlugin,    "pppay",    "朋朋支付", "https://api.pppay.com/gateway");
// SxfPayPlugin 已完整实现于 SxfPayPlugin.h
DECLARE_GENERIC_PLUGIN(UmsPayPlugin,   "umspay",   "银联商务", "https://api.chinaums.com/gateway");
DECLARE_GENERIC_PLUGIN(XxPayPlugin,    "xxpay",    "汇付小付", "https://api.xxpay.com/gateway");
// YsePayPlugin 已完整实现于 YsePayPlugin.h (类名 YsePayPlugin)

#undef DECLARE_GENERIC_PLUGIN
