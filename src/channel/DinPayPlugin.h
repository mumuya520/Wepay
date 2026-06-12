#pragma once
#include "ChannelPlugin.h"

class DinPayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "dinpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k;
            v["label"] = lbl;
            v["type"] = t;
            v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "商户号", "input");
        add("appsecret", "商户私钥", "textarea", "", "SM2-Hex 格式");
        add("appkey", "平台公钥", "textarea", "", "SM2-Hex 格式");
        add("appmchid", "子商户号", "input", "", "可留空；不为空且不是 JSON 数组时会作为实际商户号");
        add("reportid", "渠道商户报备ID", "input", "", "可留空，多个报备ID可用英文逗号分隔");
        add("apptype", "支付方式", "input", "1", "支付宝/微信：1扫码 2H5 3JS");
        add("appswitch", "环境", "select", "0", "0=生产 https://payment.dinpay.com/trx；1=测试 https://paymenttest.dinpay.com/trx");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &) override {
        ChannelOrderResult r;
        r.errMsg = "智付 dinpay 需要 SM2/SM4 国密完整实现：SM2 加密随机 SM4 密钥、SM4-CBC 加密业务 JSON、SM3withSM2 签名后调用 /api/appPay/*。当前已注册参数与插件入口，但未启用真实下单。";
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &,
                                     const std::string &,
                                     const Json::Value &) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        r.verified = false;
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &) override {
        ChannelRefundResult r;
        r.errMsg = "智付 dinpay 退款同样依赖 SM2/SM4 国密实现，当前未启用。";
        return r;
    }
};

REGISTER_CHANNEL_PLUGIN(DinPayPlugin);
