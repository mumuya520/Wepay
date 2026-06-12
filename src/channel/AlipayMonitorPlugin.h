// WePay-Cpp — 支付宝免签 (AliMonitor)
// 与 vmq 等价但只处理 alipay (type=2) 通道, 订单池按 plugin='alipay_monitor' 隔离。
#pragma once
#include "VmqPlugin.h"

class AlipayMonitorPlugin : public VmqPlugin {
public:
    std::string name() const override { return "alipay_monitor"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("key", "监听密钥", "password", "");
        add("require_online", "要求监听在线才下单", "switch", "false");
        add("lock_price", "金额浮动锁", "switch", "true");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderRequest r = req;
        // 强制 pay_type=alipay
        r.payType = "alipay";
        if (!r.channelParams.isMember("plugin_code") ||
            r.channelParams["plugin_code"].asString().empty())
            r.channelParams["plugin_code"] = "alipay_monitor";
        return VmqPlugin::createOrder(r);
    }
};
REGISTER_CHANNEL_PLUGIN(AlipayMonitorPlugin);
