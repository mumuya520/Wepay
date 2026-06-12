#pragma once
#include "ChannelPlugin.h"
#include <iomanip>
#include <sstream>

class AlipayCodePlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipaycode"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "应用APPID", "input", "", "兼容彩虹易字段，账单查询/授权时使用");
        add("appkey", "支付宝公钥", "textarea", "", "兼容彩虹易字段");
        add("appsecret", "应用私钥", "textarea", "", "兼容彩虹易字段");
        add("appmchid", "支付宝UID", "input", "", "2088开头的支付宝用户ID，普通转账收款账号");
        add("apptoken", "商户授权token", "input", "", "第三方应用授权 token，非第三方应用留空");
        add("appswitch", "支付类型", "select", "0", "0=普通转账，1=转账确认单");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string uid = p.get("appmchid", "").asString();
        if (uid.empty()) {
            result.errMsg = "支付宝UID(appmchid)不能为空";
            return result;
        }

        std::string amount = fmtAmount(req.amount);
        std::string remark = "请勿添加备注-" + req.orderId;
        std::string mode = p.get("appswitch", "0").asString();

        if (mode == "1") {
            Json::Value params;
            params["productCode"] = "TRANSFER_TO_ALIPAY_ACCOUNT";
            params["bizScene"] = "YUEBAO";
            params["transAmount"] = amount;
            params["remark"] = req.orderId;
            params["businessParams"]["returnUrl"] = "alipays://platformapi/startapp?appId=2021001167654035&nbupdate=syncforce";
            params["payeeInfo"]["identity"] = uid;
            params["payeeInfo"]["identityType"] = "ALIPAY_USER_ID";
            Json::StreamWriterBuilder wb; wb["indentation"] = "";
            std::string formData = Json::writeString(wb, params);
            result.payUrl = "https://render.alipay.com/p/yuyan/180020010001206672/rent-index.html?formData=" + urlEncode(formData);
            result.success = true;
            result.rawResponse = formData;
            return result;
        }

        std::string scanParams = "actionType=scan&u=" + urlEncode(uid) +
                                 "&a=" + urlEncode(amount) +
                                 "&m=" + urlEncode(remark);
        result.payUrl = "alipays://platformapi/startapp?appId=20000123&" + scanParams;
        result.qrCode = result.payUrl;
        result.success = true;
        result.rawResponse = scanParams;
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &) override {
        ChannelNotifyResult result;
        result.responseText = "success";
        auto orderIt = params.find("trade_no");
        auto amountIt = params.find("money");
        if (orderIt == params.end()) orderIt = params.find("out_trade_no");
        if (amountIt == params.end()) amountIt = params.find("amount");
        result.verified = true;
        result.paid = true;
        if (orderIt != params.end()) result.orderId = orderIt->second;
        auto chIt = params.find("alipay_order_no");
        if (chIt != params.end()) result.channelOrderNo = chIt->second;
        if (amountIt != params.end()) {
            try { result.paidAmount = std::stod(amountIt->second); } catch (...) {}
        }
        return result;
    }

private:
    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(AlipayCodePlugin);
