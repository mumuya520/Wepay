// WePay-Cpp — 支付宝沙箱插件
// 复用 AlipayPlugin 全部逻辑，网关默认指向沙箱环境
// 沙箱文档: https://opendocs.alipay.com/common/02kkv7
//
// 使用步骤:
//   1. 登录 https://open.alipay.com/develop/sandbox/app 获取沙箱 AppID
//   2. 下载沙箱支付宝 App 或使用沙箱钱包
//   3. 在插件配置填入沙箱 AppID / 沙箱应用私钥 / 沙箱支付宝公钥
//   4. 下单时使用沙箱买家账号扫码支付
#pragma once
#include "AlipayPlugin.h"

class AlipaySandboxPlugin : public AlipayPlugin {
public:
    std::string name() const override { return "alipay_sandbox"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid",             "沙箱AppID",      "input",    "", "沙箱应用 APPID (open.alipay.com/develop/sandbox/app)");
        add("private_key",       "沙箱应用私钥",   "textarea", "", "沙箱应用私钥 PEM(PKCS8)");
        add("alipay_public_key", "沙箱支付宝公钥", "textarea", "", "沙箱支付宝公钥 PEM");
        add("gateway",           "沙箱网关",       "input",    "https://openapi-sandbox.dl.alipaydev.com/gateway.do", "沙箱网关地址(一般不需修改)");
        add("pay_method",        "支付方式",       "select",   "precreate", "precreate=扫码 / wap=手机网页");
        return arr;
    }

    // 重写 createOrder，强制使用沙箱网关
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderRequest sandboxReq = req;
        if (!sandboxReq.channelParams.isMember("gateway") ||
            sandboxReq.channelParams["gateway"].asString().empty()) {
            sandboxReq.channelParams["gateway"] = "https://openapi-sandbox.dl.alipaydev.com/gateway.do";
        }
        return AlipayPlugin::createOrder(sandboxReq);
    }
};

REGISTER_CHANNEL_PLUGIN(AlipaySandboxPlugin);
