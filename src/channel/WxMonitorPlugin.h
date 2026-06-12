// WePay-Cpp — 微信免签 (WxMonitor)
// 仅处理 wxpay (type=1) 通道, 订单池 plugin='wx_monitor' 隔离。
#pragma once
#include "VmqPlugin.h"

class WxMonitorPlugin : public VmqPlugin {
public:
    std::string name() const override { return "wx_monitor"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            arr.append(v);
        };
        add("key", "监听密钥", "password", "");
        add("require_online", "要求监听在线", "switch", "false");
        add("lock_price", "金额浮动锁", "switch", "true");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderRequest r = req;
        r.payType = "wxpay";
        if (!r.channelParams.isMember("plugin_code") ||
            r.channelParams["plugin_code"].asString().empty())
            r.channelParams["plugin_code"] = "wx_monitor";
        return VmqPlugin::createOrder(r);
    }
};
REGISTER_CHANNEL_PLUGIN(WxMonitorPlugin);
