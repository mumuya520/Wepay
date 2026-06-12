#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <regex>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 联动优势(umfpay) - RSA-SHA1签名, form POST, HTML META响应
// 网关: http://pay.soopay.net/spay/pay/payservice.do
// 签名: ksort拼接k=v&(skip sign/sign_type/空值), 去尾&, RSA-SHA1签名→Base64
// 验签: 同样拼接, 平台公钥RSA-SHA1验签
// 请求: charset/sign_type/res_format/version/amt_type/mer_id/.../sign → form POST
// 响应: HTML META name="MobilePayPlatform" content="k=v&...", 解析k=v
// 回调: GET参数验签, trade_state=TRADE_SUCCESS
// 退款: service=mer_refund
// 证书: 平台公钥cert.pem + 商户私钥key.pem (存为appkey/appsecret参数)

class UmfpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "umfpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户编号", "input");
        add("appkey", "平台公钥(cert.pem)", "textarea", "", "PEM格式平台公钥");
        add("appsecret", "商户私钥(key.pem)", "textarea", "", "PEM格式商户私钥");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=公众号");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appsecret", "").asString().empty()) {
            r.errMsg = "联动优势参数不完整(appid/appsecret)";
            return r;
        }
        int apptype = p.get("apptype", "1").asInt();
        std::string scancodeType = getScancodeType(req.payType);

        if (apptype == 2 && req.payType.find("wx") != std::string::npos) {
            // 微信公众号支付 - 构建跳转URL
            std::map<std::string, std::string> m;
            m["service"] = "publicnumber_and_verticalcode";
            m["notify_url"] = req.notifyUrl;
            m["ret_url"] = req.returnUrl;
            m["goods_inf"] = req.subject.empty() ? "商品" : req.subject;
            m["order_id"] = req.orderId;
            m["mer_date"] = getDateStr();
            m["amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
            m["user_ip"] = req.clientIp;
            m["is_public_number"] = "Y";

            auto signedParams = buildSignedParams(p, m);
            r.success = true;
            r.payUrl = std::string(GATEWAY_URL) + "?" + buildFormBody(signedParams);
            return r;
        }

        // 扫码支付
        std::map<std::string, std::string> m;
        m["service"] = "active_scancode_order_new";
        m["notify_url"] = req.notifyUrl;
        m["goods_inf"] = req.subject.empty() ? "商品" : req.subject;
        m["order_id"] = req.orderId;
        m["mer_date"] = getDateStr();
        m["amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        m["user_ip"] = req.clientIp;
        m["scancode_type"] = scancodeType;
        m["mer_flag"] = "KMER";
        // consumer_id: IP with dots removed
        std::string consumerId = req.clientIp;
        consumerId.erase(std::remove(consumerId.begin(), consumerId.end(), '.'), consumerId.end());
        m["consumer_id"] = consumerId;

        auto signedParams = buildSignedParams(p, m);
        std::string formBody = buildFormBody(signedParams);
        auto resp = SyncHttp::postForm(GATEWAY_URL, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        auto result = parseHtmlMeta(resp.body);
        if (result.empty()) { r.errMsg = "平台返回html解析失败"; return r; }
        std::string retCode = get(result, "ret_code");
        if (retCode != "0000") {
            r.errMsg = "[" + retCode + "]" + get(result, "ret_msg", "下单失败");
            return r;
        }
        r.success = true;
        // bank_payurl is base64 encoded
        std::string payUrl = get(result, "bank_payurl");
        if (!payUrl.empty()) {
            // Base64 decode
            try {
                auto decoded = base64Decode(payUrl);
                r.payUrl = std::string(decoded.begin(), decoded.end());
            } catch (...) {
                r.payUrl = payUrl; // fallback to raw
            }
        }
        r.channelOrderNo = get(result, "trade_no");
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        // UMF notify uses GET params
        std::string pubKey = channelParams.get("appkey", "").asString();
        auto signIt = params.find("sign");
        if (signIt == params.end() || signIt->second.empty()) { r.verified = false; return r; }

        std::string signContent = buildSignContent(params);
        r.verified = RsaUtils::verifySha1(signContent, signIt->second, pubKey);
        if (!r.verified) return r;

        auto stateIt = params.find("trade_state");
        r.paid = (stateIt != params.end() && stateIt->second == "TRADE_SUCCESS");
        auto orderIt = params.find("order_id");
        if (orderIt != params.end()) r.orderId = orderIt->second;
        auto tradeIt = params.find("trade_no");
        if (tradeIt != params.end()) r.channelOrderNo = tradeIt->second;
        auto custIt = params.find("mer_cust_id");
        if (custIt != params.end()) r.buyerId = custIt->second;
        try { r.paidAmount = std::stod(get(params, "amount")) / 100.0; } catch (...) {}

        // Build UMF response HTML
        std::string merId = channelParams.get("appid", "").asString();
        std::string retCode = r.paid ? "0000" : "0001";
        std::string retMsg = r.paid ? "success" : "trade_state" + get(params, "trade_state");
        std::map<std::string, std::string> respParams;
        respParams["sign_type"] = "RSA";
        respParams["version"] = "4.0";
        respParams["mer_id"] = merId;
        respParams["ret_code"] = retCode;
        respParams["ret_msg"] = retMsg;
        respParams["order_id"] = r.orderId;
        respParams["mer_date"] = get(params, "mer_date");
        respParams["sign"] = RsaUtils::signSha1(buildSignContent(respParams), channelParams.get("appsecret", "").asString());

        std::string respStr;
        for (auto &kv : respParams) {
            if (!respStr.empty()) respStr += "&";
            respStr += kv.first + "=" + kv.second;
        }
        r.responseText = "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"><html><head><META NAME=\"MobilePayPlatform\" CONTENT=\"" + respStr + "\"></head><body></body></html>";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["service"] = "mer_refund";
        m["refund_no"] = req.refundNo;
        m["order_id"] = req.orderId;
        m["mer_date"] = req.orderId.substr(0, 8); // first 8 chars of order_id
        m["org_amount"] = std::to_string((long long)std::llround(req.paidAmount * 100.0));
        m["refund_amount"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));

        auto signedParams = buildSignedParams(p, m);
        std::string formBody = buildFormBody(signedParams);
        auto resp = SyncHttp::postForm(GATEWAY_URL, formBody);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }

        auto result = parseHtmlMeta(resp.body);
        std::string retCode = get(result, "ret_code");
        if (retCode == "0000") {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = get(result, "order_id");
        } else {
            r.errMsg = "[" + retCode + "]" + get(result, "ret_msg", "退款失败");
        }
        return r;
    }

private:
    static constexpr const char* GATEWAY_URL = "http://pay.soopay.net/spay/pay/payservice.do";

    static std::map<std::string, std::string> buildSignedParams(const Json::Value &p, const std::map<std::string, std::string> &bizParams) {
        std::string merId = p.get("appid", "").asString();
        std::string privateKey = p.get("appsecret", "").asString();

        std::map<std::string, std::string> m;
        // Public params
        m["charset"] = "UTF-8";
        m["sign_type"] = "RSA";
        m["res_format"] = "HTML";
        m["version"] = "4.0";
        m["amt_type"] = "RMB";
        m["mer_id"] = merId;
        // Biz params
        for (auto &kv : bizParams) m[kv.first] = kv.second;

        // Sign
        std::string signContent = buildSignContent(m);
        m["sign"] = RsaUtils::signSha1(signContent, privateKey);
        return m;
    }

    // ksort, skip sign/sign_type/empty, concat k=v&, remove last &
    static std::string buildSignContent(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            s += kv.first + "=" + kv.second + "&";
        }
        if (!s.empty()) s.pop_back();
        return s;
    }

    // Parse HTML META MobilePayPlatform content="k=v&..."
    static std::map<std::string, std::string> parseHtmlMeta(const std::string &html) {
        std::map<std::string, std::string> m;
        // Match <META name="MobilePayPlatform" content="...">
        std::regex metaRegex(R"(<META\s+name\s*=\s*["']MobilePayPlatform["']\s+content\s*=\s*["']([^"']*)["'])", std::regex::icase);
        std::smatch match;
        if (!std::regex_search(html, match, metaRegex) || match.size() < 2) return m;

        std::string content = match[1].str();
        // Split by &
        std::istringstream iss(content);
        std::string item;
        while (std::getline(iss, item, '&')) {
            auto eqPos = item.find('=');
            if (eqPos != std::string::npos) {
                m[item.substr(0, eqPos)] = item.substr(eqPos + 1);
            }
        }
        return m;
    }

    static std::string getScancodeType(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "WECHAT";
        if (payType == "bank" || payType == "bank_jsapi") return "UNION";
        return "ALIPAY";
    }

    static std::string getDateStr() {
        auto t = std::time(nullptr);
        std::tm *tm = std::localtime(&t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y%m%d");
        return oss.str();
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k, const std::string &dflt = "") {
        auto it = m.find(k);
        return it == m.end() ? dflt : it->second;
    }

    static std::vector<unsigned char> base64Decode(const std::string &encoded) {
        static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<unsigned char> result;
        std::vector<int> decoding(256, -1);
        for (int i = 0; i < 64; i++) decoding[base64_chars[i]] = i;
        int val = 0, valb = -8;
        for (unsigned char c : encoded) {
            if (decoding[c] == -1) break;
            val = (val << 6) + decoding[c];
            valb += 6;
            if (valb >= 0) {
                result.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return result;
    }
};

REGISTER_CHANNEL_PLUGIN(UmfpayPlugin);
