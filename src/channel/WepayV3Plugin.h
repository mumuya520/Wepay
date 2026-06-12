// WePay-Cpp — WePay V3 原生监控插件
// 专属协议: /api/wepay/v3/heart  /api/wepay/v3/push
//           /api/wepay/v3/pending /api/wepay/v3/ocr
//           ws://host/api/wepay/v3/ws
// 安全升级: 签名改为 sorted-query-string HMAC-SHA256
// 新增: 多商户绑定 / 设备状态上报 / OCR截图 / WebSocket推单 / MinIO凭证
#pragma once
#include "WepayPlugin.h"

class WepayV3Plugin : public WepayPlugin {
public:
    std::string name() const override { return "wepay_v3"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("key",            "通讯密钥",           "password", "",
            "HMAC-SHA256 共享密钥，留空使用平台密钥");
        add("require_online", "要求监控端在线",      "switch",   "false",
            "开启后监控端离线时自动拒单（基于 wepay_v3 心跳状态）");
        add("require_device", "要求设备已注册",      "switch",   "true",
            "仅允许管理端已注册的设备进行推送");
        add("replay_window",  "防重放窗口(秒)",      "number",   "60",
            "时间戳偏差超过此值则拒绝请求");
        add("lock_price",     "金额浮动锁",          "switch",   "true",
            "并发同金额订单时自动浮动 ±0.01 避免冲突");
        add("payQf",          "浮动方向",            "select",   "1",
            "1=递增 2=递减");
        add("max_devices",    "最大设备数",          "number",   "10",
            "允许同时注册的最大 V3 监控设备数量");
        add("ocr_enabled",    "允许截图OCR上传",     "switch",   "true",
            "开启后设备可上传收款截图由服务端识别金额匹配订单");
        add("ws_push",        "WebSocket订单推送",   "switch",   "true",
            "开启后新订单创建时服务端主动推送给在线设备，无需轮询");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderRequest r = req;
        if (!r.channelParams.isMember("plugin_code") ||
            r.channelParams["plugin_code"].asString().empty())
            r.channelParams["plugin_code"] = "wepay_v3";

        // V3 专属在线检测
        bool requireOnline = r.channelParams.get("require_online", false).asBool();
        if (requireOnline) {
            auto &db = PayDb::instance();
            std::string st = db.getSetting("wepay_v3_jkstate", "0");
            if (st != "1") {
                ChannelOrderResult fail;
                fail.errMsg = "WePay V3 监控端离线";
                return fail;
            }
            long long lh = 0;
            try { lh = std::stoll(db.getSetting("wepay_v3_lastheart", "0")); } catch (...) {}
            if (std::time(nullptr) - lh > 180) {
                db.setSetting("wepay_v3_jkstate", "0");
                ChannelOrderResult fail;
                fail.errMsg = "WePay V3 监控端心跳超时";
                return fail;
            }
        }
        // 创建订单后推送 WS（由 WepayV3MonitorCtrl 负责 WsBus 推送）
        return VmqPlugin::createOrder(r);
    }
};

REGISTER_CHANNEL_PLUGIN(WepayV3Plugin);
