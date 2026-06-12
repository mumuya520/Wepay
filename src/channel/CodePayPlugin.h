#pragma once
#include "VmqPlugin.h"

// 码支付插件(mpay 兼容)
// 与 VMQ 的区别:
//   - 订单池按 plugin_code=codepay 隔离, 不和 VMQ 抢单
//   - 配合 CodePayCtrl 暴露 /checkOrder/{pid}/{sign} 让监听端主动拉单
//   - 配合 CodePayCtrl 暴露 /mpayNotify 接收 SmsForwarder/mpayPC 直推
//   - 复用 VmqPlugin 的金额浮动、选码、订单匹配静态工具
class CodePayPlugin : public VmqPlugin {
public:
    std::string name() const override { return "codepay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("lock_price", "金额浮动锁", "switch", "true",
            "并发同金额订单浮动 0.01 避免冲突 (码支付协议要求)");
        add("payQf", "金额浮动方向", "select", "1",
            "1=递增 2=递减");
        add("require_online", "要求监听端在线才允许下单", "switch", "false");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderRequest r = req;
        if (!r.channelParams.isMember("plugin_code") ||
            r.channelParams["plugin_code"].asString().empty())
            r.channelParams["plugin_code"] = "codepay";
        return VmqPlugin::createOrder(r);
    }

    // 插件层异步通知(mpay 直推走 CodePayCtrl, 这里兜底处理 /notify/channel/codepay)
    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &) override {
        ChannelNotifyResult r;
        std::string payway = get(params, "payway");
        if (payway.empty()) payway = get(params, "type");
        std::string price = get(params, "price");
        if (price.empty()) price = get(params, "money");
        if (payway.empty() || price.empty()) return r;
        auto order = findPendingCodepayOrder(payway, fmtPrice(price));
        if (order.empty()) return r;
        r.verified = true;
        r.paid = true;
        r.orderId = order["order_id"];
        r.channelOrderNo = order["order_id"];
        r.paidAmount = toDouble(order.count("real_amount") ? order.at("real_amount") : price);
        r.responseText = "success";
        markOrderPaid(order, payTypeNormalize(payway), "");
        return r;
    }

    // 匹配监听类插件(codepay/vmq/monitor)创建的订单
    static PayDb::Row findPendingCodepayOrder(const std::string &payway,
                                              const std::string &price,
                                              const std::string &mchId = "") {
        std::string type = payTypeNormalize(payway);
        std::string sql = "SELECT po.* FROM pay_order po "
                          "LEFT JOIN pay_channel pc ON pc.id=po.channel_id "
                          "WHERE po.state=0 AND LOWER(po.pay_type)=LOWER(?) "
                          "AND (ABS(CAST(po.real_amount AS REAL)-?)<0.001 OR ABS(CAST(po.amount AS REAL)-?)<0.001) "
                          "AND (pc.plugin IN ('codepay','vmq','wepay','wepay_v3','monitor','wx_monitor','alipay_monitor','qq_monitor') "
                          "     OR po.channel_id=0 OR pc.plugin IS NULL)";
        std::vector<std::string> params{type, price, price};
        if (!mchId.empty()) {
            sql += " AND po.mch_id=?";
            params.push_back(mchId);
        }
        sql += " ORDER BY po.created_at ASC LIMIT 1";
        return PayDb::instance().queryOne(sql, params);
    }

    static std::string payTypeNormalize(const std::string &s) {
        std::string v = s;
        for (auto &c : v) c = std::tolower((unsigned char)c);
        // 原版码支付用数字: 1=微信 2=支付宝 3=QQ
        if (v == "1" || v == "wxpay" || v == "weixin" || v == "wechat" || v == "wx") return "wxpay";
        if (v == "2" || v == "alipay" || v == "zfb") return "alipay";
        if (v == "3" || v == "qqpay" || v == "qq") return "qqpay";
        return v;
    }

    // 返回 codepay 通道的待支付订单 (mpay 协议格式)
    // 可选 mchId 过滤指定商户。
    static Json::Value pendingCodepayOrders(int mchId = 0) {
        std::string sql =
            "SELECT po.id,po.order_id,po.mch_id,po.pay_type,po.real_amount,po.channel_id "
            "FROM pay_order po "
            "LEFT JOIN pay_channel pc ON pc.id=po.channel_id "
            "WHERE po.state=0 AND (pc.plugin IN ('codepay','vmq','wepay','wepay_v3','monitor','wx_monitor','alipay_monitor','qq_monitor') "
            "OR po.channel_id=0 OR pc.plugin IS NULL)";
        std::vector<std::string> params;
        if (mchId > 0) { sql += " AND po.mch_id=?"; params.push_back(std::to_string(mchId)); }
        sql += " ORDER BY po.created_at ASC LIMIT 50";
        auto rows = PayDb::instance().query(sql, params);
        Json::Value arr(Json::arrayValue);
        for (auto &o : rows) {
            Json::Value item;
            // 与 mpay 字段对齐: id/pid/cid/aid/patt/type/really_price
            item["id"] = o.count("id") ? o["id"] : "0";
            item["pid"] = o.count("mch_id") ? o["mch_id"] : "0";
            item["cid"] = o.count("channel_id") ? o["channel_id"] : "0";
            item["aid"] = "0";
            item["patt"] = 1; // 默认连续监听
            // 类型回写为 wxpay/alipay
            std::string pt = o.count("pay_type") ? o["pay_type"] : "";
            item["type"] = pt;
            item["really_price"] = o.count("real_amount") ? o["real_amount"] : "0";
            item["order_id"] = o.count("order_id") ? o["order_id"] : "";
            arr.append(item);
        }
        return arr;
    }

    // mpay 风格的推送匹配: 按 payway+price+(可选 mchId) 仅匹配 codepay 订单
    // 不要求 t/sign (兼容 SmsForwarder 等不带签名的直推)
    static int handleCodepayPush(const std::string &payway, const std::string &price,
                                 const std::string &mchIdStr = "") {
        std::string normalType = payTypeNormalize(payway);
        std::string normalPrice = fmtPrice(price);
        LOG_INFO << "[codepay] push payway=" << payway << " normalized=" << normalType
                 << " price=" << price << " fmtPrice=" << normalPrice
                 << " mchId=" << mchIdStr;
        auto order = findPendingCodepayOrder(normalType, normalPrice, mchIdStr);
        if (order.empty()) {
            LOG_WARN << "[codepay] no pending order matched type=" << normalType
                     << " price=" << normalPrice << " mch=" << mchIdStr;
            return 0;
        }
        LOG_INFO << "[codepay] matched order_id=" << order["order_id"]
                 << " notify_url=" << (order.count("notify_url") ? order["notify_url"] : "(empty)");
        // 回调签名需用订单所属商户的 mch_key，而非空串
        auto &db = PayDb::instance();
        std::string key;
        std::string mid = order.count("mch_id") ? order.at("mch_id") : "";
        if (!mid.empty() && mid != "0") {
            auto m = db.queryOne("SELECT mch_key FROM merchant WHERE id=?", {mid});
            if (!m.empty()) key = m["mch_key"];
        }
        if (key.empty()) key = db.getSetting("key", "");
        markOrderPaid(order, normalType, key);
        LOG_INFO << "[codepay] markOrderPaid done for " << order["order_id"];
        return 1;
    }
};

REGISTER_CHANNEL_PLUGIN(CodePayPlugin);
