#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include <iomanip>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"
#include <openssl/des.h>

// 银盈通支付(yinyingtong) - MD5签名(仅指定字段), JSON POST, DES-ECB回调解密
// 网关: https://gc-gw.gomepay.com/gpayCashApi
// 签名: 仅req_no/app_id/sign_type/charset/format/version/data/timestamp/method, ksort, k=v&, key=secret, MD5大写
// 请求: app_id/method/format/charset/sign_type/timestamp/version/client_ip/data(JSON)/req_no/terminal_type/browser_brand → JSON POST
// 响应: code=000000/900888/900889/900001, data为JSON字符串, op_ret_code=000/701成功
// 回调: dstbdata/dstbdatasign, 前107字符header, 后4字节长度, DES-ECB解密(productkey), 分割取第二部分JSON
// 扫码: gepos.pre.pay, pay_type=01(支付宝)/02(微信), bank_service_type=16(微信扫码)/22(微信H5)
// JSAPI: gepos.public.number.order(公众号)/gepos.mini.program.pay(小程序)
// 快捷: gcash.trade.precreate, scene=14
// 退款: gepos.refund, scene=0615(T61)/0606(其他)

class YinyingtongPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "yinyingtong"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "应用ID", "input");
        add("appkey", "应用KEY", "input", "", "同时是私钥证书密码");
        add("productkey", "产品密钥", "input", "", "用于支付回调数据解密");
        add("appmchid", "交易商户企业号", "input");
        add("trade_platform_no", "平台商企业号(参考号)", "input");
        add("channel_merch_no", "渠道商户号", "input", "", "多个用逗号分隔");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=小程序 3=自有公众号");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() || p.get("appmchid", "").asString().empty()) {
            r.errMsg = "银盈通参数不完整(appid/appkey/appmchid)";
            return r;
        }

        int apptype = p.get("apptype", "1").asInt();
        std::string merchantNo = p.get("appmchid", "").asString();
        std::string refNo = p.get("trade_platform_no", "").asString();
        std::string channelMerchNo = p.get("channel_merch_no", "").asString();
        if (!channelMerchNo.empty()) {
            // Randomly select if comma-separated
            auto pos = channelMerchNo.find(',');
            if (pos != std::string::npos) {
                std::vector<std::string> nos;
                std::stringstream ss(channelMerchNo);
                std::string tok;
                while (std::getline(ss, tok, ',')) nos.push_back(tok);
                if (!nos.empty()) channelMerchNo = nos[rand() % nos.size()];
            }
        }

        // Determine API method
        std::string method, payType, bankServiceType;
        bool isQuickpay = (req.payType == "bank" || req.payType == "bank_jsapi");

        if (isQuickpay) {
            method = "gcash.trade.precreate";
        } else if (apptype == 2 || apptype == 3) {
            // JSAPI or mini program
            method = (req.payType.find("lite") != std::string::npos || req.payType.find("mini") != std::string::npos) 
                     ? "gepos.mini.program.pay" : "gepos.public.number.order";
        } else {
            method = "gepos.pre.pay";
        }

        // Build biz data
        Json::Value bizData;
        bizData["merchant_number"] = merchantNo;
        bizData["order_number"] = req.orderId;
        bizData["currency"] = "CNY";
        bizData["order_title"] = req.subject.empty() ? "商品" : req.subject;
        bizData["channel_code"] = payType;
        bizData["async_notification_addr"] = req.notifyUrl;
        bizData["notify_key_mode"] = "03";
        if (!refNo.empty()) bizData["ref_no"] = refNo;
        if (!channelMerchNo.empty()) bizData["bank_mch_id"] = channelMerchNo;

        if (isQuickpay) {
            bizData["scene"] = "14";
            bizData["good_desc"] = req.subject.empty() ? "商品" : req.subject;
            bizData["total_amount"] = fmtAmount(req.amount);
            bizData["notify_url"] = req.notifyUrl;
            bizData["return_url"] = req.returnUrl;
            bizData["user_id"] = generateUserId(); // Random user ID
        } else {
            bizData["amount"] = fmtAmount(req.amount);
            bizData["pay_type"] = payType;
            if (apptype == 2 || apptype == 3) {
                std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
                std::string subAppid = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
                if (!openid.empty()) bizData["open_id"] = openid;
                if (!subAppid.empty()) bizData["sub_appid"] = subAppid;
            }
            if (!bankServiceType.empty()) bizData["bank_service_type"] = bankServiceType;
        }

        auto result = executeRequest(p, method, bizData);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        std::string orderId = result.data.get("order_id", "").asString();
        r.channelOrderNo = orderId;

        if (isQuickpay) {
            // Build cashier URL
            std::string cashierUrl = "https://h5.gomepay.com/cashier-h5/index.html#/pages/paymentB/cashRegister?";
            cashierUrl += "merchant_number=" + merchantNo + "&user_id=" + bizData["user_id"].asString() + "&order_number=" + req.orderId + "&type=wbsh";
            r.payUrl = cashierUrl;
        } else if (apptype == 2 || apptype == 3) {
            // JSAPI returns trans_data
            r.payUrl = result.data.get("trans_data", "").asString();
        } else {
            // Scan returns order_id, redirect to cashier
            std::string cashierUrl = "https://h5.gomepay.com/cashier-h5/index.html#/pages/preOrder/orderPay?orderId=" + orderId + "&showPayButton=0";
            if (payType == "02" && bankServiceType == "22") {
                // WeChat H5 scheme
                cashierUrl = "weixin://dl/business/?appid=wx135edf7e3c7a1e7d&path=pages/wechat/preOrder/orderpay&query=orderId=" + orderId + "&showPayButton=0&env_version=release";
            }
            r.payUrl = cashierUrl;
        }
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "01";
        auto dstbdataIt = params.find("dstbdata");
        auto dstbdatasignIt = params.find("dstbdatasign");
        if (dstbdataIt == params.end() || dstbdatasignIt == params.end()) return r;

        std::string productKey = channelParams.get("productkey", "").asString();
        std::string dstbdata = dstbdataIt->second;

        // DES-ECB decrypt: skip 107 chars header, skip 4 chars length, then cipher
        if (dstbdata.length() < 111) { r.verified = false; return r; }
        std::string afterHeader = dstbdata.substr(107);
        std::string cipher = afterHeader.substr(4);
        std::string decrypted = desEcbDecrypt(cipher, productKey);
        if (decrypted.empty()) { r.verified = false; return r; }

        // Split by  (4 separators)
        auto sepPos = decrypted.find("");
        if (sepPos == std::string::npos) { r.verified = false; return r; }
        std::string jsonStr = decrypted.substr(sepPos + 4);

        Json::Value data;
        if (!Json::Reader().parse(jsonStr, data)) { r.verified = false; return r; }

        r.verified = true;
        std::string orderstatus = data.get("orderstatus", "").asString();
        r.paid = (orderstatus == "00");
        r.orderId = data.get("dsorderid", "").asString();
        r.channelOrderNo = data.get("orderid", "").asString();
        try { r.paidAmount = std::stod(data.get("amount", "0").asString()); } catch (...) {}
        r.responseText = r.paid ? "00" : "01";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string merchantNo = p.get("appmchid", "").asString();
        std::string transcode = p.get("transcode", "").asString(); // From order ext

        std::string scene = (transcode == "T61") ? "0615" : "0606";

        Json::Value bizData;
        bizData["scene"] = scene;
        bizData["merchant_number"] = merchantNo;
        bizData["order_number"] = req.refundNo;
        bizData["old_order_number"] = req.orderId;
        bizData["old_order_id"] = req.channelOrderNo;
        bizData["amount"] = fmtAmount(req.refundAmount);
        bizData["currency"] = "CNY";
        bizData["async_notification_addr"] = req.notifyUrl;
        bizData["memo"] = "订单退款";

        auto result = executeRequest(p, "gepos.refund", bizData);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = result.data.get("order_id", "").asString();
        return r;
    }

private:
    static constexpr const char* GATEWAY_URL = "https://gc-gw.gomepay.com/gpayCashApi";

    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        Json::Value data;
    };

    static ApiResponse executeRequest(const Json::Value &p, const std::string &method, const Json::Value &bizData) {
        ApiResponse ar;
        std::string appId = p.get("appid", "").asString();
        std::string appSecret = p.get("appkey", "").asString();

        // Build params
        Json::Value params;
        params["app_id"] = appId;
        params["method"] = method;
        params["format"] = "JSON";
        params["charset"] = "UTF-8";
        params["sign_type"] = "MD5";
        params["timestamp"] = getTimestamp();
        params["version"] = "1.0";
        params["client_ip"] = "127.0.0.1";
        Json::FastWriter fw;
        params["data"] = fw.write(bizData);
        params["req_no"] = generateReqNo();
        params["terminal_type"] = "3"; // PC
        params["browser_brand"] = "99"; // Default

        params["sign"] = generateSign(params, appSecret);

        std::string jsonBody = fw.write(params);

        std::map<std::string, std::string> headers;
        headers["method"] = "cash-api@" + method;
        headers["Content-Type"] = "application/json; charset=utf-8";

        auto resp = SyncHttp::postJson(GATEWAY_URL, jsonBody, headers);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { ar.errMsg = "响应解析失败"; return ar; }

        std::string code = result.get("code", "").asString();
        if (code == "000000" || code == "900888" || code == "900889" || code == "900001") {
            ar.success = true;
            std::string dataStr = result.get("data", "").asString();
            Json::Reader().parse(dataStr, ar.data);
        } else if (result.isMember("sub_msg")) {
            ar.errMsg = "[" + result.get("sub_code", "").asString() + "]" + result.get("sub_msg", "").asString();
        } else if (result.isMember("data")) {
            // Try parsing data for op_ret_code
            std::string dataStr = result.get("data", "").asString();
            if (dataStr.find("op_ret_code") != std::string::npos) {
                Json::Value dataJson;
                if (Json::Reader().parse(dataStr, dataJson)) {
                    std::string opRetCode = dataJson.get("op_ret_code", "").asString();
                    if (opRetCode == "000" || opRetCode == "701") {
                        ar.success = true;
                        ar.data = dataJson;
                    } else {
                        ar.errMsg = "[" + dataJson.get("op_ret_subcode", "").asString() + "]" + dataJson.get("op_err_submsg", dataJson.get("op_ret_msg", "")).asString();
                    }
                }
            }
        } else {
            ar.errMsg = result.get("msg", "返回数据解析失败").asString();
        }
        return ar;
    }

    // Sign only specific keys: req_no, app_id, sign_type, charset, format, version, data, timestamp, method
    static std::string generateSign(const Json::Value &params, const std::string &appSecret) {
        std::vector<std::string> signKeys = {"req_no", "app_id", "sign_type", "charset", "format", "version", "data", "timestamp", "method"};

        std::string s;
        for (auto &k : signKeys) {
            if (!params.isMember(k)) continue;
            const Json::Value &v = params[k];
            if (v.isNull()) continue;
            if (v.isString() && v.asString().empty()) continue;
            std::string val;
            if (v.isString()) val = v.asString();
            else if (v.isInt()) val = std::to_string(v.asInt());
            else if (v.isInt64()) val = std::to_string(v.asInt64());
            else if (v.isDouble()) val = fmtAmount(v.asDouble());
            else if (v.isBool()) val = v.asBool() ? "1" : "0";
            else continue;
            s += k + "=" + val + "&";
        }
        s += "key=" + appSecret;
        std::string md5 = Md5Utils::md5(s);
        // Uppercase
        std::transform(md5.begin(), md5.end(), md5.begin(), ::toupper);
        return md5;
    }

    // Triple DES-ECB decrypt (DES-ECB-3)
    static std::string desEcbDecrypt(const std::string &hexCipher, const std::string &key) {
        // Key to 8 bytes (Triple DES uses 24 bytes but we pad/truncate to 8 for single key)
        unsigned char desKey[24] = {0};
        size_t keyLen = key.size() < 24 ? key.size() : 24;
        memcpy(desKey, key.data(), keyLen);
        // Triple DES needs 24 bytes, duplicate 8-byte key 3 times if shorter
        if (keyLen < 24) {
            for (size_t i = 8; i < 24; i++) {
                desKey[i] = desKey[i % 8];
            }
        }

        // Hex to binary
        std::vector<unsigned char> cipher;
        for (size_t i = 0; i + 1 < hexCipher.length(); i += 2) {
            unsigned char byte = std::stoul(hexCipher.substr(i, 2), nullptr, 16);
            cipher.push_back(byte);
        }

        // Pad to multiple of 8
        while (cipher.size() % 8 != 0) cipher.push_back(0);

        DES_key_schedule ks1, ks2, ks3;
        DES_set_key_unchecked((DES_cblock*)(desKey + 0), &ks1);
        DES_set_key_unchecked((DES_cblock*)(desKey + 8), &ks2);
        DES_set_key_unchecked((DES_cblock*)(desKey + 16), &ks3);

        std::vector<unsigned char> plain(cipher.size());
        for (size_t i = 0; i + 7 < cipher.size(); i += 8) {
            // Triple DES ECB decrypt
            DES_ecb3_encrypt((DES_cblock*)(cipher.data() + i), (DES_cblock*)(plain.data() + i), &ks1, &ks2, &ks3, DES_DECRYPT);
        }

        // Remove null padding
        size_t len = 0;
        for (; len < plain.size() && plain[len] != 0; len++);

        return std::string(plain.begin(), plain.begin() + len);
    }

    static std::string getTimestamp() {
        auto now = std::time(nullptr);
        std::tm *local = std::localtime(&now);
        std::ostringstream oss;
        oss << std::put_time(local, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    static std::string generateReqNo() {
        unsigned char buf[16];
        RAND_bytes(buf, 16);
        std::ostringstream oss;
        for (int i = 0; i < 16; i++) oss << std::hex << std::setfill('0') << std::setw(2) << (int)buf[i];
        return oss.str();
    }

    static std::string generateUserId() {
        unsigned char buf[8];
        RAND_bytes(buf, 8);
        std::ostringstream oss;
        for (int i = 0; i < 8; i++) oss << std::hex << std::setfill('0') << std::setw(2) << (int)buf[i];
        return oss.str();
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(YinyingtongPlugin);
