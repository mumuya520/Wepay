// WePay-Cpp — xpay-go 原生支付通道插件
// 支持: 支付宝当面付 (xpay_alipay) / 微信 Native (xpay_wxpay)
// 原理: 通过 XpayBridge HTTP 代理调用 xpay-go 的 precreate/query 接口
// 订单 ID: 创建时将 wepay-cpp orderId 传给 xpay-go 作为其 id，查询时直接复用
// 轮询: wepay-cpp 后台任务定期调用 queryOrder 确认支付状态
#pragma once
#include "ChannelPlugin.h"
#include "../common/XpayBridge.h"
#include "../common/PayDb.h"
#include <sstream>

// ── 支付宝当面付 (xpay_alipay) ─────────────────────────────────
class XpayAlipayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "xpay_alipay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl,
                       const std::string &t, const std::string &dflt = "",
                       const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("notify_email", "收款通知邮箱", "text", "",
            "支付成功后 xpay-go 发送邮件通知的地址（需与 xpay-go config.yaml 中 mail.receiver 一致）");
        add("min_amount", "最小金额(元)", "number", "10.00",
            "xpay-go 当面付最小金额限制，默认 10.00");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &bridge = XpayBridge::instance();
        if (!bridge.ready()) {
            result.errMsg = "xpay-go 服务未就绪";
            return result;
        }

        Json::Value p = req.channelParams;
        std::string email = p.get("notify_email", "").asString();
        if (email.empty()) {
            // 尝试从 wepay-cpp 系统设置获取管理员邮箱
            email = PayDb::instance().getSetting("admin_email", "admin@example.com");
        }

        // 构建 form 参数（id 指定为 wepay-cpp orderId，queryOrder 直接复用）
        std::string money = formatMoney(req.amount);
        std::string body = "id=" + urlEncode(req.orderId)
                         + "&money=" + money
                         + "&email=" + urlEncode(email)
                         + "&payType=Alipay"
                         + "&nickName=" + urlEncode(req.mchOrderNo)
                         + "&info="  + urlEncode(req.subject);

        auto resp = bridge.request("POST", "/alipay/precreate", "", body,
                                   {{"Content-Type","application/x-www-form-urlencoded"}});
        if (!resp.ok()) {
            result.errMsg = "调用 xpay-go 失败 (HTTP " + std::to_string(resp.status) + ")";
            return result;
        }

        Json::Value jv;
        Json::CharReaderBuilder rb;
        std::istringstream ss(resp.body);
        std::string err;
        if (!Json::parseFromStream(rb, ss, &jv, &err)) {
            result.errMsg = "xpay-go 响应解析失败";
            return result;
        }
        if (!jv.get("success", false).asBool()) {
            result.errMsg = jv.get("message", "xpay-go 拒绝请求").asString();
            return result;
        }

        auto &r = jv["result"];
        std::string xpayId = r.get("id", "").asString();
        std::string qrCode = r.get("qrCode", "").asString();

        result.success        = true;
        result.payUrl         = qrCode;   // alipay:// 深链，前端生成二维码
        result.channelOrderNo = xpayId;   // xpay-go 订单号 = wepay-cpp orderId
        return result;
    }

    ChannelQueryResult queryOrder(const std::string &orderId,
                                  const Json::Value &) override {
        ChannelQueryResult r;
        auto &bridge = XpayBridge::instance();
        if (!bridge.ready()) return r;

        auto resp = bridge.request("GET", "/alipay/query/" + orderId, "", "", {});
        if (!resp.ok()) return r;

        Json::Value jv;
        Json::Reader().parse(resp.body, jv);
        r.success = true;
        // result=1 表示已支付
        r.tradeState = (jv.get("success", false).asBool() && jv["result"].asInt() == 1) ? 1 : 0;
        return r;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &,
        const std::string &,
        const Json::Value &) override {
        // xpay-go 自己处理 Alipay notify（邮件通知）
        // wepay-cpp 通过 queryOrder 轮询确认，无需独立 notify 端点
        ChannelNotifyResult r;
        r.verified = false;
        return r;
    }

private:
    static std::string formatMoney(double amount) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", amount);
        return buf;
    }
    static std::string urlEncode(const std::string &s) {
        std::string r;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                r += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                r += buf;
            }
        }
        return r;
    }
};

REGISTER_CHANNEL_PLUGIN(XpayAlipayPlugin);

// ── 微信 Native 支付 (xpay_wxpay) ──────────────────────────────
class XpayWxpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "xpay_wxpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        Json::Value v;
        v["key"] = "notify_email"; v["label"] = "收款通知邮箱";
        v["type"] = "text"; v["default"] = "";
        v["help"] = "支付成功后 xpay-go 发送邮件通知的地址";
        arr.append(v);
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &bridge = XpayBridge::instance();
        if (!bridge.ready()) {
            result.errMsg = "xpay-go 服务未就绪";
            return result;
        }

        Json::Value p = req.channelParams;
        std::string email = p.get("notify_email", "").asString();
        if (email.empty())
            email = PayDb::instance().getSetting("admin_email", "admin@example.com");

        std::string money = formatMoney(req.amount);
        std::string body = "id=" + urlEncode(req.orderId)
                         + "&money=" + money
                         + "&email=" + urlEncode(email)
                         + "&payType=Wechat"
                         + "&nickName=" + urlEncode(req.mchOrderNo)
                         + "&info="  + urlEncode(req.subject);

        auto resp = bridge.request("POST", "/wechat/precreate", "", body,
                                   {{"Content-Type","application/x-www-form-urlencoded"}});
        if (!resp.ok()) {
            result.errMsg = "调用 xpay-go 失败 (HTTP " + std::to_string(resp.status) + ")";
            return result;
        }

        Json::Value jv;
        Json::CharReaderBuilder rb;
        std::istringstream ss(resp.body);
        std::string err;
        if (!Json::parseFromStream(rb, ss, &jv, &err)) {
            result.errMsg = "xpay-go 响应解析失败";
            return result;
        }
        if (!jv.get("success", false).asBool()) {
            result.errMsg = jv.get("message", "xpay-go 拒绝请求").asString();
            return result;
        }

        auto &r = jv["result"];
        std::string xpayId = r.get("id", "").asString();
        std::string qrCode = r.get("qrCode", "").asString();

        result.success        = true;
        result.payUrl         = qrCode;   // weixin://wxpay/... 前端生成二维码
        result.channelOrderNo = xpayId;
        return result;
    }

    ChannelQueryResult queryOrder(const std::string &orderId,
                                  const Json::Value &) override {
        ChannelQueryResult r;
        auto &bridge = XpayBridge::instance();
        if (!bridge.ready()) return r;

        auto resp = bridge.request("GET", "/wechat/query/" + orderId, "", "", {});
        if (!resp.ok()) return r;

        Json::Value jv;
        Json::Reader().parse(resp.body, jv);
        r.success = true;
        r.tradeState = (jv.get("success", false).asBool() && jv["result"].asInt() == 1) ? 1 : 0;
        return r;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &,
        const std::string &,
        const Json::Value &) override {
        ChannelNotifyResult r;
        r.verified = false;
        return r;
    }

private:
    static std::string formatMoney(double amount) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", amount);
        return buf;
    }
    static std::string urlEncode(const std::string &s) {
        std::string r;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                r += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                r += buf;
            }
        }
        return r;
    }
};

REGISTER_CHANNEL_PLUGIN(XpayWxpayPlugin);
