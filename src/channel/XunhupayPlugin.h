// WePay-Cpp — 虎皮椒支付插件 (完整实现)
// 参考 PHP: mpay_v2_webman/app/common/payment/XunhupayApiPayment.php
// 参考 SDK: mpay_v2_webman/app/common/sdk/xunhupay/XunhupayClient.php
//
// 支付产品:
//   - alipay_h5: 支付宝 H5
//   - wechat_h5: 微信 H5
//   - alipay: 支付宝扫码
//   - wechat: 微信扫码
//
// 签名: MD5 签名 (ksort + hash)
#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#ifndef _WIN32
#include <openssl/md5.h>
#endif
#include "../common/SyncHttp.h"

class XunhupayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "xunhupay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid",     "商户ID",     "input",    "", "虎皮椒商户ID (appid)");
        add("api_key",   "API密钥",    "password", "", "虎皮椒 API 密钥");
        add("api_url",   "网关地址",   "input",    "https://api.xunhupay.com/payment/do.html", "留空使用虎皮椒默认网关");
        add("pay_method","支付方式",   "select",   "h5", "h5=H5跳转/jump=跳转/qrcode=扫码");
        return arr;
    }

    // ═══ 下单接口 ═══════════════════════════════════════════════════
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;

        std::string appId    = p.get("appid", "").asString();
        std::string apiKey   = p.get("api_key", "").asString();
        std::string apiUrl   = p.get("api_url", "https://api.xunhupay.com/payment/do.html").asString();
        std::string payMethod = p.get("pay_method", "h5").asString();
        std::string wayCode  = req.payType;

        if (appId.empty() || apiKey.empty()) {
            r.errMsg = "虎皮椒参数不完整(appid/api_key)";
            return r;
        }

        // 判断支付类型
        bool useH5 = (payMethod == "h5" || payMethod == "jump" || payMethod == "web" ||
                      wayCode == "ali_h5" || wayCode == "wx_h5" || wayCode == "alipay_h5" || wayCode == "wechat_h5");

        // 构造请求参数
        Json::Value body;
        body["appid"] = appId;
        body["version"] = "1.1";
        body["trade_order_id"] = req.orderId;
        body["payment"] = channelPayment(wayCode);
        body["total_fee"] = fmtAmount(req.amount);
        body["title"] = req.subject.empty() ? "商品" : req.subject;
        body["notify_url"] = req.notifyUrl;
        body["return_url"] = req.returnUrl;
        body["time"] = std::to_string(std::time(nullptr));
        body["nonce_str"] = randomNonce(16);

        // 微信 H5 需要额外参数
        std::string payment = body["payment"].asString();
        if (payment == "wechat" && useH5) {
            body["type"] = "WAP";
            // 从 return_url 提取 wap_url
            std::string returnUrl = req.returnUrl;
            size_t pos = returnUrl.find("://");
            if (pos != std::string::npos) {
                size_t end = returnUrl.find("/", pos + 3);
                if (end != std::string::npos) {
                    body["wap_url"] = returnUrl.substr(0, end);
                } else {
                    body["wap_url"] = returnUrl;
                }
            }
            body["wap_name"] = req.subject.empty() ? "商品" : req.subject;
        }

        // 生成签名
        std::string signData = buildSignData(body);
        std::string sign = md5(signData + apiKey);
        body["hash"] = sign;

        // 序列化 JSON
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string bodyStr = Json::writeString(wb, body);

        // 发送请求
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json; charset=utf-8"},
            {"Accept", "application/json"}
        };

        auto resp = SyncHttp::postJson(apiUrl, bodyStr, headers);
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "虎皮椒请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        // 检查错误码
        int errCode = j.get("errcode", -1).asInt();
        if (errCode != 0) {
            r.errMsg = j.get("errmsg", "下单失败").asString(); return r;
        }

        // 验签
        std::string respSign = j.get("hash", "").asString();
        Json::Value signBody = j;
        signBody.removeMember("hash");
        signBody["appid"] = appId;  // 验签需要包含 appid
        std::string signCheck = md5(buildSignData(signBody) + apiKey);
        if (respSign != signCheck) {
            r.errMsg = "响应验签失败"; return r;
        }

        r.success = true;
        r.channelOrderNo = j.get("trade_order_id", req.orderId).asString();

        if (useH5) {
            // H5 跳转支付
            std::string payUrl = j.get("url", "").asString();
            if (payUrl.empty()) {
                r.errMsg = "虎皮椒未返回跳转地址"; r.success = false; return r;
            }
            r.payUrl = payUrl;
            r.extra["url"] = payUrl;
        } else {
            // 扫码支付
            std::string qrcodeUrl = j.get("url_qrcode", "").asString();
            if (qrcodeUrl.empty()) {
                r.errMsg = "虎皮椒未返回二维码地址"; r.success = false; return r;
            }
            // 解析二维码内容
            std::string qrcode = parseQrcode(qrcodeUrl, apiKey, apiUrl);
            r.payUrl = qrcode;
            r.qrCode = qrcode;
            r.extra["qrcode"] = qrcode;
        }

        return r;
    }

    // ═══ 查单接口 ═══════════════════════════════════════════════════
    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        std::string appId  = channelParams.get("appid", "").asString();
        std::string apiKey = channelParams.get("api_key", "").asString();
        std::string apiUrl = channelParams.get("api_url", "https://api.xunhupay.com/payment/do.html").asString();

        if (appId.empty() || apiKey.empty()) {
            r.errMsg = "虎皮椒参数不完整"; return r;
        }

        // 替换为查询接口
        size_t pos = apiUrl.find("/payment/do.html");
        if (pos != std::string::npos) {
            apiUrl.replace(pos, 17, "/payment/query.html");
        } else {
            apiUrl = "https://api.xunhupay.com/payment/query.html";
        }

        Json::Value body;
        body["appid"] = appId;
        body["trade_order_id"] = orderId;
        body["time"] = std::to_string(std::time(nullptr));
        body["nonce_str"] = randomNonce(16);

        std::string signData = buildSignData(body);
        body["hash"] = md5(signData + apiKey);

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        auto resp = SyncHttp::postJson(apiUrl, Json::writeString(wb, body), {{"Content-Type", "application/json"}});
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "查询请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        int errCode = j.get("errcode", -1).asInt();
        if (errCode != 0) {
            r.errMsg = j.get("errmsg", "查询失败").asString(); return r;
        }

        std::string status = j.get("status", "").asString();
        // OD=已支付, WP=待支付, QR=二维码, Exp=已过期, CL=已取消
        if (status == "OD") r.tradeState = 1;
        else if (status == "CL" || status == "Exp") r.tradeState = -1;
        else r.tradeState = 0;

        r.success = true;
        r.channelOrderNo = j.get("trade_order_id", orderId).asString();
        r.channelTradeNo = j.get("open_order_id", "").asString();
        r.buyerId = j.get("open_id", "").asString();

        return r;
    }

    // ═══ 退款接口 ═══════════════════════════════════════════════════
    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string appId  = p.get("appid", "").asString();
        std::string apiKey = p.get("api_key", "").asString();
        std::string apiUrl = p.get("api_url", "https://api.xunhupay.com/payment/do.html").asString();

        if (appId.empty() || apiKey.empty()) {
            r.errMsg = "虎皮椒参数不完整"; return r;
        }
        if (req.channelOrderNo.empty()) {
            r.errMsg = "channelOrderNo(上游订单号)必填"; return r;
        }

        // 替换为退款接口
        size_t pos = apiUrl.find("/payment/do.html");
        if (pos != std::string::npos) {
            apiUrl.replace(pos, 17, "/payment/refund.html");
        } else {
            apiUrl = "https://api.xunhupay.com/payment/refund.html";
        }

        Json::Value body;
        body["appid"] = appId;
        body["open_order_id"] = req.channelOrderNo;  // 虎皮椒订单号
        body["time"] = std::to_string(std::time(nullptr));
        body["nonce_str"] = randomNonce(16);

        std::string signData = buildSignData(body);
        body["hash"] = md5(signData + apiKey);

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        auto resp = SyncHttp::postJson(apiUrl, Json::writeString(wb, body), {{"Content-Type", "application/json"}});
        r.rawResponse = resp.body;

        if (!resp.success) {
            r.errMsg = "退款请求失败: " + resp.errMsg; return r;
        }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) {
            r.errMsg = "响应解析失败"; return r;
        }

        int errCode = j.get("errcode", -1).asInt();
        if (errCode != 0) {
            r.errMsg = j.get("errmsg", "退款失败").asString(); return r;
        }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = j.get("transaction_id", req.refundNo).asString();
        r.refundAmount = req.refundAmount;

        return r;
    }

    // ═══ 关闭订单 (不支持) ═══════════════════════════════════════════
    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        r.success = false;
        r.errMsg = "虎皮椒插件暂不支持关单";
        return r;
    }

    // ═══ 回调验证 ═══════════════════════════════════════════════════
    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";

        std::string apiKey = channelParams.get("api_key", "").asString();
        if (apiKey.empty()) {
            r.verified = false; r.errMsg = "缺少 API 密钥"; return r;
        }

        // 从 params 构建签名数据
        std::string sign = getParam(params, "hash");
        if (sign.empty()) {
            r.verified = false; r.errMsg = "缺少签名"; return r;
        }

        // 构造验签数据
        std::map<std::string, std::string> signParams;
        for (auto &[k, v] : params) {
            if (k != "hash") signParams[k] = v;
        }
        std::string signData = buildSignData(signParams);
        std::string expectedSign = md5(signData + apiKey);

        r.verified = (sign == expectedSign);
        if (!r.verified) {
            r.errMsg = "验签失败"; return r;
        }

        std::string status = getParam(params, "status");
        r.paid = (status == "OD");  // OD = 已支付

        r.orderId = getParam(params, "trade_order_id");
        r.channelOrderNo = getParam(params, "open_order_id");
        r.buyerId = getParam(params, "open_id");

        try {
            std::string totalFee = getParam(params, "total_fee");
            if (!totalFee.empty()) {
                r.paidAmount = std::stod(totalFee);
            }
        } catch (...) {}

        return r;
    }

private:
    // ═══ 支付方式映射 ══════════════════════════════════════════════
    static std::string channelPayment(const std::string &wayCode) {
        if (wayCode == "wxpay" || wayCode == "wx_native" || wayCode == "wx_h5" ||
            wayCode == "wechat" || wayCode == "wechat_h5") {
            return "wechat";
        }
        return "alipay";
    }

    // ═══ 构造签名数据 (map版) ═════════════════════════════════════════
    static std::string buildSignData(const std::map<std::string, std::string> &params) {
        std::vector<std::pair<std::string, std::string>> items;
        for (auto &[k, v] : params) {
            if (k == "hash" || k == "sign") continue;
            if (!v.empty()) {
                items.push_back({k, v});
            }
        }
        std::sort(items.begin(), items.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        std::ostringstream oss;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) oss << "&";
            oss << items[i].first << "=" << items[i].second;
        }
        return oss.str();
    }

    // ═══ 构造签名数据 (Json::Value版) ═══════════════════════════════
    static std::string buildSignData(const Json::Value &body) {
        std::vector<std::pair<std::string, std::string>> items;
        for (auto it = body.begin(); it != body.end(); ++it) {
            std::string key = it.key().asString();
            if (key == "hash" || key == "sign") continue;
            if (!it->isNull() && !it->asString().empty()) {
                items.push_back({key, it->asString()});
            }
        }
        std::sort(items.begin(), items.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        std::ostringstream oss;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) oss << "&";
            oss << items[i].first << "=" << items[i].second;
        }
        return oss.str();
    }

    // ═══ MD5 签名 ═════════════════════════════════════════════════
    static std::string md5(const std::string &input) {
        unsigned char digest[16];
#ifdef _WIN32
       HCRYPTPROV hProv;
       HCRYPTHASH hHash;
        CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, 0);
        CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash);
        CryptHashData(hHash, (BYTE*)input.c_str(), input.length(), 0);
        CryptGetHashParam(hHash, HP_HASHVAL, digest, NULL, 0);
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
#else
        MD5((unsigned char*)input.c_str(), input.length(), digest);
#endif
        char hex[33];
        for (int i = 0; i < 16; ++i) {
            sprintf(hex + i * 2, "%02x", digest[i]);
        }
        hex[32] = '\0';
        return std::string(hex);
    }

    // ═══ 解析二维码内容 ════════════════════════════════════════════
    std::string parseQrcode(const std::string &qrcodeUrl, const std::string &apiKey,
                           const std::string &baseUrl) {
        // 虎皮椒二维码可能需要跳转解析
        // 先尝试直接返回 URL，如果包含 data= 参数则解码
        size_t dataPos = qrcodeUrl.find("data=");
        if (dataPos != std::string::npos) {
            std::string encoded = qrcodeUrl.substr(dataPos + 5);
            // URL decode
            std::string decoded;
            for (size_t i = 0; i < encoded.length(); ++i) {
                if (encoded[i] == '%' && i + 2 < encoded.length()) {
                    std::string hex = encoded.substr(i + 1, 2);
                    char c = (char)std::stoi(hex, nullptr, 16);
                    decoded += c;
                    i += 2;
                } else if (encoded[i] == '+') {
                    decoded += ' ';
                } else {
                    decoded += encoded[i];
                }
            }
            return decoded;
        }
        return qrcodeUrl;
    }

    // ═══ 格式化金额 ════════════════════════════════════════════════
    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    // ═══ 生成随机字符串 ════════════════════════════════════════════
    static std::string randomNonce(int len) {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < len; ++i) s += cs[rng() % 36];
        return s;
    }

    // ═══ 从 map 获取值 ════════════════════════════════════════════
    static std::string getParam(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(XunhupayPlugin);
