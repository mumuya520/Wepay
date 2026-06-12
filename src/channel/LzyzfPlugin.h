#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

// 浪子易支付(lzyzf) - 易支付兼容协议
// 签名: MD5, ksort拼接k=v&(skip sign/sign_type/空值), 去尾&, 直接追加key
// 下单: submit.php(页面跳转) / mapi.php(JSON API)
// 回调: GET参数验签, trade_status=TRADE_SUCCESS
// 退款: api.php?act=refund

class LzyzfPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "lzyzf"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appurl", "接口地址", "input", "", "必须以http://或https://开头，以/结尾");
        add("appid", "商户ID", "input");
        add("appkey", "商户密钥", "input");
        add("appswitch", "是否使用mapi接口", "select", "1", "0=否(页面跳转) 1=是(API接口，推荐)");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        std::string apiUrl = p.get("appurl", "").asString();
        if (!apiUrl.empty() && apiUrl.back() != '/') apiUrl += '/';
        std::string pid = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();
        if (apiUrl.empty() || pid.empty() || key.empty()) {
            r.errMsg = "浪子易支付参数不完整(appurl/appid/appkey)";
            return r;
        }
        bool useMapi = p.get("appswitch", "0").asInt() == 1;

        std::string payType = getPayType(req.payType);
        std::string outTradeNo = stripPrefix(req.orderId);  // 去 W 前缀

        if (useMapi) {
            // MAPI JSON API
            std::map<std::string, std::string> m;
            m["pid"] = pid;
            m["type"] = payType;
            m["device"] = "pc";
            m["clientip"] = req.clientIp;
            m["notify_url"] = req.notifyUrl;
            m["return_url"] = req.returnUrl;
            m["out_trade_no"] = outTradeNo;
            m["name"] = req.subject.empty() ? "商品" : req.subject;
            m["money"] = fmtAmount(req.amount);
            m["sign"] = makeSign(m, key);
            m["sign_type"] = "MD5";

            std::string formBody = buildFormBody(m);
            auto resp = SyncHttp::postForm(apiUrl + "mapi.php", formBody);
            r.rawResponse = resp.body;
            if (!resp.success) { r.errMsg = resp.errMsg; return r; }

            Json::Value result;
            if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
            if (result.get("code", -1).asInt() != 1) {
                r.errMsg = result.get("msg", "获取支付接口数据失败").asString();
                return r;
            }
            r.success = true;
            std::string tradeNo = result.get("trade_no", "").asString();
            std::string payurl = result.get("payurl", "").asString();
            std::string qrcode = result.get("qrcode", "").asString();
            std::string urlscheme = result.get("urlscheme", "").asString();
            // 优先级: payurl > 跳转上游收银台 > qrcode > urlscheme
            if (!payurl.empty()) {
                r.payUrl = payurl;
                r.qrCode = qrcode;
            } else if (!tradeNo.empty()) {
                r.payUrl = apiUrl + "Pay/console?trade_no=" + tradeNo;
            } else if (!qrcode.empty()) {
                r.payUrl = qrcode;
                r.qrCode = qrcode;
            } else if (!urlscheme.empty()) {
                r.payUrl = urlscheme;
            }
            r.channelOrderNo = tradeNo;
        } else {
            // Submit page redirect - build URL with sign
            std::map<std::string, std::string> m;
            m["pid"] = pid;
            m["type"] = payType;
            m["notify_url"] = req.notifyUrl;
            m["return_url"] = req.returnUrl;
            m["out_trade_no"] = outTradeNo;
            m["name"] = req.subject.empty() ? "商品" : req.subject;
            m["money"] = fmtAmount(req.amount);
            m["sign"] = makeSign(m, key);
            m["sign_type"] = "MD5";

            r.success = true;
            // 加 [POST] 标记，让收银台用自动提交表单方式跳转(避免 GET 不被支持)
            r.payUrl = "[POST]" + apiUrl + "submit.php?" + buildFormBody(m);
        }
        return r;
    }

    static std::string stripPrefix(const std::string &s) {
        size_t i = 0;
        while (i < s.size() && !std::isdigit((unsigned char)s[i])) ++i;
        return i == 0 ? s : s.substr(i);
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        std::string key = channelParams.get("appkey", "").asString();
        // Lzyzf notify uses GET params
        std::string calcSign = makeSign(params, key);
        auto signIt = params.find("sign");
        if (signIt == params.end()) { r.verified = false; return r; }
        r.verified = (calcSign == signIt->second);
        if (!r.verified) return r;

        auto statusIt = params.find("trade_status");
        r.paid = (statusIt != params.end() && statusIt->second == "TRADE_SUCCESS");
        auto orderIt = params.find("out_trade_no");
        if (orderIt != params.end()) r.orderId = orderIt->second;
        auto tradeIt = params.find("trade_no");
        if (tradeIt != params.end()) r.channelOrderNo = tradeIt->second;
        auto moneyIt = params.find("money");
        if (moneyIt != params.end()) try { r.paidAmount = std::stod(moneyIt->second); } catch (...) {}
        r.responseText = r.paid ? "success" : "fail";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string apiUrl = p.get("appurl", "").asString();
        std::string pid = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();

        // api.php?act=refund POST pid/key/trade_no/money
        std::string url = apiUrl + "api.php?act=refund";
        std::string formBody = "pid=" + pid + "&key=" + key + "&trade_no=" + req.channelOrderNo + "&money=" + fmtAmount(req.refundAmount);

        auto resp = SyncHttp::postForm(url, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { r.errMsg = "响应解析失败"; return r; }
        if (result.get("code", -1).asInt() == 0) {
            r.success = true;
            r.state = 1;
        } else {
            r.errMsg = result.get("msg", "退款失败").asString();
        }
        return r;
    }

private:
    // MD5 sign: ksort, k=v& (skip sign/sign_type/empty), remove last &, append key directly
    static std::string makeSign(const std::map<std::string, std::string> &m, const std::string &key) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            s += kv.first + "=" + kv.second + "&";
        }
        if (!s.empty()) s.pop_back(); // remove last &
        s += key; // append key directly (no &key=)
        return Md5Utils::md5(s);
    }

    static std::string getPayType(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "wxpay";
        if (payType.find("qq") != std::string::npos) return "qqpay";
        if (payType == "bank" || payType == "bank_jsapi") return "bank";
        if (payType.find("jd") != std::string::npos) return "jdpay";
        return "alipay";
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(LzyzfPlugin);
