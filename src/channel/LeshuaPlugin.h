#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

// 乐刷聚合支付 - MD5签名, form POST请求, XML响应
// 网关: https://paygate.leshuazf.com/cgi-bin/lepos_pay_gateway.cgi
// 签名: ksort拼接k=v&(skip sign/error_code), 追加key=, MD5 uppercase
// 下单用appkey签名, 回调用appsecret验签(小写比较)

class LeshuaPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "leshua"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户号", "input");
        add("appkey", "交易密钥", "input", "", "下单/退款签名用");
        add("appsecret", "异步通知密钥", "input", "", "回调验签用");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=JSAPI/公众号");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "乐刷参数不完整(appid/appkey)";
            return r;
        }
        int apptype = p.get("apptype", "1").asInt();
        std::string payWay = getPayWay(req.payType);
        std::string jspayFlag = getJspayFlag(req.payType, apptype);

        std::map<std::string, std::string> m;
        m["service"] = "get_tdcode";
        m["jspay_flag"] = jspayFlag;
        m["pay_way"] = payWay;
        m["merchant_id"] = p.get("appid", "").asString();
        m["third_order_id"] = req.orderId;
        m["amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        m["body"] = req.subject.empty() ? "商品" : req.subject;
        m["notify_url"] = req.notifyUrl;
        m["client_ip"] = req.clientIp;
        m["nonce_str"] = randomStr(16);

        // JSAPI extend
        if (jspayFlag == "1" || jspayFlag == "3") {
            std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
            std::string subAppid = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            if (!openid.empty()) m["sub_openid"] = openid;
            if (!subAppid.empty()) m["appid"] = subAppid;
        }

        m["sign"] = makeSign(m, p.get("appkey", "").asString());

        std::string formBody = buildFormBody(m);
        auto resp = SyncHttp::postForm(API_URL, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        auto result = parseXml(resp.body);
        if (get(result, "resp_code") != "0") {
            r.errMsg = get(result, "resp_msg", "返回数据解析失败");
            return r;
        }
        if (get(result, "result_code") != "0") {
            r.errMsg = get(result, "error_msg", "下单失败");
            return r;
        }
        r.success = true;
        r.channelOrderNo = get(result, "leshua_order_id");

        // Different response fields per jspay_flag
        if (jspayFlag == "0") {
            r.payUrl = get(result, "td_code"); // Native扫码URL
        } else if (jspayFlag == "2") {
            r.payUrl = get(result, "jspay_url"); // 微信Native(jspay_url)
        } else if (jspayFlag == "1" || jspayFlag == "3") {
            r.payUrl = get(result, "jspay_info"); // JSAPI payInfo JSON
        }
        r.extra["data"] = mapToJson(result);
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        // Leshua notify is XML body
        auto m = parseXml(body);
        std::string notifyKey = channelParams.get("appsecret", "").asString();
        // Sign verification uses lowercase comparison
        std::string calcSign = makeSign(m, notifyKey);
        std::transform(calcSign.begin(), calcSign.end(), calcSign.begin(), ::tolower);
        std::string recvSign = get(m, "sign");
        std::transform(recvSign.begin(), recvSign.end(), recvSign.begin(), ::tolower);
        r.verified = (calcSign == recvSign);
        if (!r.verified) return r;

        std::string status = get(m, "status");
        r.paid = (status == "2");
        r.orderId = get(m, "third_order_id");
        r.channelOrderNo = get(m, "leshua_order_id");
        r.buyerId = get(m, "sub_openid");
        try { r.paidAmount = std::stod(get(m, "account")) / 100.0; } catch (...) {}
        r.responseText = r.paid ? "000000" : "fail";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["service"] = "unified_refund";
        m["merchant_id"] = p.get("appid", "").asString();
        m["leshua_order_id"] = req.channelOrderNo;
        m["merchant_refund_id"] = req.refundNo;
        m["refund_amount"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        m["nonce_str"] = randomStr(16);
        m["sign"] = makeSign(m, p.get("appkey", "").asString());

        std::string formBody = buildFormBody(m);
        auto resp = SyncHttp::postForm(API_URL, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        auto result = parseXml(resp.body);
        if (get(result, "resp_code") != "0") {
            r.errMsg = get(result, "resp_msg", "返回数据解析失败");
            return r;
        }
        if (get(result, "result_code") != "0") {
            r.errMsg = get(result, "error_msg", "退款失败");
            return r;
        }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = get(result, "leshua_refund_id");
        return r;
    }

private:
    static constexpr const char* API_URL = "https://paygate.leshuazf.com/cgi-bin/lepos_pay_gateway.cgi";

    // MD5 uppercase sign: ksort, k=v& (skip sign/error_code, arrays→empty), append key=
    static std::string makeSign(const std::map<std::string, std::string> &m, const std::string &key) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "sign" || kv.first == "error_code") continue;
            if (kv.second.empty()) continue;
            s += kv.first + "=" + kv.second + "&";
        }
        s += "key=" + key;
        std::string md5 = Md5Utils::md5(s);
        std::transform(md5.begin(), md5.end(), md5.begin(), ::toupper);
        return md5;
    }

    // Overload for XML-parsed map (may have non-string values handled already)
    static std::string makeSign(const std::vector<std::pair<std::string,std::string>> &v, const std::string &key) {
        std::map<std::string,std::string> m(v.begin(), v.end());
        return makeSign(m, key);
    }

    static std::string getPayWay(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "WXZF";
        if (payType == "bank" || payType == "bank_jsapi") return "UPSMZF";
        return "ZFBZF"; // 支付宝
    }

    static std::string getJspayFlag(const std::string &payType, int apptype) {
        if (apptype == 2) {
            // JSAPI/公众号
            if (payType.find("wx") != std::string::npos) return "1";
            return "1"; // 支付宝JSAPI
        }
        // 扫码
        if (payType.find("wx") != std::string::npos) return "2"; // 微信Native用jspay_flag=2
        return "0"; // 支付宝/银联Native
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    // Simple XML parser
    static std::vector<std::pair<std::string,std::string>> parseXml(const std::string &xml) {
        std::vector<std::pair<std::string,std::string>> result;
        size_t pos = 0;
        while (pos < xml.size()) {
            auto openStart = xml.find('<', pos);
            if (openStart == std::string::npos) break;
            auto openEnd = xml.find('>', openStart);
            if (openEnd == std::string::npos) break;
            std::string tag = xml.substr(openStart + 1, openEnd - openStart - 1);
            if (tag.empty() || tag[0] == '/' || tag[0] == '?') { pos = openEnd + 1; continue; }
            pos = openEnd + 1;
            std::string closeTag = "</" + tag + ">";
            auto closePos = xml.find(closeTag, pos);
            if (closePos == std::string::npos) continue;
            std::string value = xml.substr(pos, closePos - pos);
            if (value.find("<![CDATA[") == 0 && value.rfind("]]>") == value.size() - 3) {
                value = value.substr(9, value.size() - 12);
            }
            result.push_back({tag, value});
            pos = closePos + closeTag.size();
        }
        return result;
    }

    static std::string get(const std::vector<std::pair<std::string,std::string>> &v, const std::string &k, const std::string &dflt = "") {
        for (auto &p : v) if (p.first == k) return p.second;
        return dflt;
    }

    static Json::Value mapToJson(const std::vector<std::pair<std::string,std::string>> &v) {
        Json::Value j;
        for (auto &p : v) j[p.first] = p.second;
        return j;
    }

    static std::string randomStr(int len) {
        static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string s;
        unsigned char buf[32];
        RAND_bytes(buf, len < 32 ? len : 32);
        for (int i = 0; i < len; ++i) s += chars[buf[i % 32] % (sizeof(chars) - 1)];
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(LeshuaPlugin);
