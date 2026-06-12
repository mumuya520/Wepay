// WePay-Cpp — 免签监听通道插件
// 配合 /device/push 设备上报实现免签收款
// 不调用任何上游API，通过本地收款码 + 设备监听匹配订单
// channelParams: qrcode_url(固定收款码), lock_price(是否锁定唯一金额)
#pragma once
#include "ChannelPlugin.h"
#include "../common/ChannelService.h"
#include "../common/PayDb.h"
#include <sstream>
#include <iomanip>

class MonitorPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "monitor"; }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;

        std::string qrcodeUrl = p.get("qrcode_url", "").asString();
        bool lockPrice        = p.get("lock_price", true).asBool();

        // 从收款码表查找可用码
        if (qrcodeUrl.empty()) {
            int payTypeInt = 1; // 默认微信
            if (req.payType == "alipay") payTypeInt = 2;
            else if (req.payType == "qqpay") payTypeInt = 3;

            auto &db = PayDb::instance();
            auto qr = db.queryOne(
                "SELECT pay_url FROM pay_qrcode WHERE type=? AND state=1 "
                "ORDER BY RANDOM() LIMIT 1",
                {std::to_string(payTypeInt)});
            if (!qr.empty()) qrcodeUrl = qr["pay_url"];
        }

        if (qrcodeUrl.empty()) {
            result.errMsg = "没有可用的收款码";
            return result;
        }

        // 金额锁定(避免同金额多笔订单冲突)
        double realAmount = req.amount;
        if (lockPrice) {
            int payTypeInt = 1;
            if (req.payType == "alipay") payTypeInt = 2;
            else if (req.payType == "qqpay") payTypeInt = 3;
            realAmount = ChannelService::lockUniquePrice(payTypeInt, req.amount);

            // 记录临时金额锁
            PayDb::instance().exec(
                "INSERT INTO tmp_price(oid,type,price) VALUES(?,?,?)",
                {req.orderId, std::to_string(payTypeInt),
                 ChannelService::fmtAmount(realAmount)});
        }

        // 更新订单的真实金额
        if (std::abs(realAmount - req.amount) > 0.001) {
            PayDb::instance().exec(
                "UPDATE pay_order SET real_amount=? WHERE order_id=?",
                {ChannelService::fmtAmount(realAmount), req.orderId});
        }

        result.success = true;
        result.payUrl  = qrcodeUrl;
        result.qrCode  = qrcodeUrl;
        result.extra["real_amount"] = ChannelService::fmtAmount(realAmount);
        return result;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        // 免签模式的"通知"来自设备推送(/device/push)
        // 在 DeviceCtrl 中已直接处理，这里不需要额外验签
        ChannelNotifyResult result;
        result.verified = true;
        result.paid = true;
        result.responseText = "success";
        return result;
    }
};

REGISTER_CHANNEL_PLUGIN(MonitorPlugin);
