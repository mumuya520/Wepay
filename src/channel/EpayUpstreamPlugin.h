// WePay-Cpp — 易支付上游通道插件
// 对接任意易支付协议的上游平台(如彩虹易/独角兽等)
// channelParams: upstream_url, pid, key
#pragma once
#include "ChannelPlugin.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "../common/Md5Utils.h"

class EpayUpstreamPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "epay_upstream"; }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;

        std::string upstreamUrl = p.get("upstream_url", "").asString();
        std::string pid         = p.get("pid", "").asString();
        std::string key         = p.get("key", "").asString();

        if (upstreamUrl.empty() || pid.empty() || key.empty()) {
            result.errMsg = "易支付上游参数不完整(upstream_url/pid/key)";
            return result;
        }

        // 构建参数（部分上游严格要求 out_trade_no 为纯数字，去 W 前缀）
        std::map<std::string, std::string> params;
        params["pid"]          = pid;
        params["type"]         = mapPayType(req.payType);
        params["out_trade_no"] = stripPrefix(req.orderId);
        params["notify_url"]   = req.notifyUrl;
        params["return_url"]   = req.returnUrl;
        params["name"]         = req.subject.empty() ? "商品" : req.subject;
        params["money"]        = fmtAmount(req.amount);

        // MD5签名
        std::string signStr = buildSignString(params) + key;
        params["sign"]      = Md5Utils::md5(signStr);
        params["sign_type"] = "MD5";

        // 构建跳转URL
        std::string queryStr;
        for (auto &[k, v] : params) {
            if (!queryStr.empty()) queryStr += "&";
            queryStr += k + "=" + urlEncode(v);
        }

        // 移除尾部斜杠
        std::string baseUrl = upstreamUrl;
        if (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();

        // [POST] 标记让收银台用自动提交表单方式跳转（避免 GET 不被支持）
        result.payUrl = "[POST]" + baseUrl + "/submit.php?" + queryStr;
        result.success = true;
        result.rawResponse = queryStr;

        // 也可以用 /mapi.php 获取 JSON 响应
        // TODO: 实际调用 POST /mapi.php 获取支付链接
        return result;
    }

    static std::string stripPrefix(const std::string &s) {
        size_t i = 0;
        while (i < s.size() && !std::isdigit((unsigned char)s[i])) ++i;
        return i == 0 ? s : s.substr(i);
    }

    ChannelQueryResult queryOrder(const std::string &orderId,
                                   const Json::Value &channelParams) override {
        ChannelQueryResult result;
        // TODO: GET upstream_url/api.php?act=order&pid=&out_trade_no=&key=
        result.success = false;
        return result;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {

        ChannelNotifyResult result;
        result.responseText = "success";

        std::string key = channelParams.get("key", "").asString();

        // 提取签名
        auto signIt = params.find("sign");
        if (signIt == params.end()) {
            result.verified = false;
            return result;
        }

        // MD5 验签
        std::string signStr = buildSignString(params) + key;
        std::string expected = Md5Utils::md5(signStr);

        if (expected != signIt->second) {
            result.verified = false;
            return result;
        }

        result.verified = true;

        auto statusIt = params.find("trade_status");
        if (statusIt != params.end() && statusIt->second == "TRADE_SUCCESS") {
            result.paid = true;
        }

        auto it1 = params.find("out_trade_no");
        if (it1 != params.end()) result.orderId = it1->second;

        auto it2 = params.find("trade_no");
        if (it2 != params.end()) result.channelOrderNo = it2->second;

        auto it3 = params.find("money");
        if (it3 != params.end()) {
            try { result.paidAmount = std::stod(it3->second); } catch (...) {}
        }

        return result;
    }

private:
    static std::string mapPayType(const std::string &payType) {
        if (payType == "wxpay" || payType == "wechat") return "wxpay";
        if (payType == "alipay" || payType == "zfb") return "alipay";
        if (payType == "qqpay") return "qqpay";
        return payType;
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string buildSignString(const std::map<std::string, std::string> &params) {
        std::string result;
        for (auto &[k, v] : params) {
            if (k == "sign" || k == "sign_type" || v.empty()) continue;
            if (!result.empty()) result += "&";
            result += k + "=" + v;
        }
        return result;
    }

    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else oss << '%' << std::uppercase << std::hex
                     << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(EpayUpstreamPlugin);
