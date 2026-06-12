// WePay-Cpp — WePay 原生监控插件（增强版）
// 专属协议: /api/wepay/heart, /api/wepay/push, /api/wepay/pending
// 安全: HMAC-SHA256 签名 / 时间戳防重放 / Nonce 去重 / 设备绑定
// 匹配: 订单级精确匹配（优先）+ 金额模糊兜底
// 同时兼容旧版 /api/monitor/* 和 V免签 /appHeart /appPush 端点
#pragma once
#include "VmqPlugin.h"

class WepayPlugin : public VmqPlugin {
public:
    std::string name() const override { return "wepay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("key", "通讯密钥", "password", "",
            "HMAC-SHA256 共享密钥，留空使用平台密钥");
        add("require_online", "要求监控端在线", "switch", "false",
            "开启后监控端离线时自动拒单（基于 wepay 心跳状态）");
        add("require_device", "要求设备已注册", "switch", "true",
            "仅允许管理端已注册的设备进行推送");
        add("replay_window", "防重放窗口(秒)", "number", "60",
            "时间戳偏差超过此值则拒绝请求");
        add("lock_price", "金额浮动锁", "switch", "true",
            "并发同金额订单时自动浮动 ±0.01 避免冲突");
        add("payQf", "浮动方向", "select", "1",
            "1=递增 2=递减");
        add("max_devices", "最大设备数", "number", "5",
            "允许同时注册的最大监控设备数量");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderRequest r = req;
        if (!r.channelParams.isMember("plugin_code") ||
            r.channelParams["plugin_code"].asString().empty())
            r.channelParams["plugin_code"] = "wepay";

        // wepay 专属在线检测: 用 wepay_jkstate 而不是 vmq 的 jkstate
        Json::Value p = r.channelParams;
        bool requireOnline = p.get("require_online", false).asBool();
        if (requireOnline) {
            auto &db = PayDb::instance();
            std::string st = db.getSetting("wepay_jkstate", "0");
            if (st != "1") {
                ChannelOrderResult fail;
                fail.errMsg = "WePay 监控端离线";
                return fail;
            }
            // 检查最后心跳是否超过 3 分钟
            long long lh = 0;
            try { lh = std::stoll(db.getSetting("wepay_lastheart", "0")); } catch (...) {}
            if (std::time(nullptr) - lh > 180) {
                db.setSetting("wepay_jkstate", "0");
                ChannelOrderResult fail;
                fail.errMsg = "WePay 监控端心跳超时";
                return fail;
            }
        }
        return VmqPlugin::createOrder(r);
    }
};

REGISTER_CHANNEL_PLUGIN(WepayPlugin);
