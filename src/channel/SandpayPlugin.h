#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/aes.h>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 杉德支付 - RSA-SHA256签名 + AES-128-ECB加密 + RSA公钥加密AES密钥
// 网关: https://openapi.sandpay.com.cn (生产) / https://openapi-uat01.sand.com.cn (测试)
// 证书: sand.cer(杉德公钥) + client.pfx(商户私钥)

class SandpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "sandpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户编号", "input");
        add("appkey", "私钥证书密码", "input");
        add("appswitch", "环境选择", "select", "0", "0=生产环境 1=测试环境");
        add("product", "市场产品", "select", "QZF", "QZF=标准线上收款 CSDB=企业杉德宝");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=JSAPI/公众号 3=快捷支付");
        add("merchant_pem", "商户RSA私钥(PEM)", "textarea", "", "从PFX导出的PEM格式私钥");
        add("sand_pubkey", "杉德公钥(PEM)", "textarea", "", "从sand.cer导出的PEM格式公钥");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("merchant_pem", "").asString().empty()) {
            r.errMsg = "杉德支付参数不完整(appid/merchant_pem)";
            return r;
        }
        std::string payType = getPayType(req.payType);
        std::string payMode = getPayMode(req.payType, p.get("apptype", "1").asInt());

        Json::Value params;
        params["marketProduct"] = p.get("product", "QZF").asString();
        params["outReqTime"] = now();
        params["mid"] = p.get("appid", "").asString();
        params["outOrderNo"] = req.orderId;
        params["description"] = req.subject.empty() ? "商品" : req.subject;
        params["goodsClass"] = "01";
        params["amount"] = fmtAmount(req.amount);
        params["payType"] = payType;
        params["payMode"] = payMode;
        Json::Value payerInfo;
        payerInfo["payAccLimit"] = "";
        params["payerInfo"] = payerInfo;
        params["notifyUrl"] = req.notifyUrl;
        Json::Value riskInfo;
        riskInfo["sourceIp"] = req.clientIp;
        params["riskmgtInfo"] = riskInfo;

        // JSAPI extend
        if (payMode == "JSAPI" || payMode == "MINI") {
            std::string subAppid = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
            Json::Value pi;
            if (!subAppid.empty()) {
                pi["subAppId"] = subAppid;
                pi["subUserId"] = openid;
            } else {
                pi["userId"] = openid;
            }
            pi["frontUrl"] = req.returnUrl;
            params["payerInfo"] = pi;
        } else if (payMode == "SANDH5") {
            std::string userId = p.get("openid", "").asString();
            if (userId.empty()) userId = randomStr(10);
            Json::Value pi;
            pi["userId"] = userId;
            pi["frontUrl"] = req.returnUrl;
            params["payerInfo"] = pi;
        }

        auto resp = executeApi(p, "/v4/sd-receipts/api/trans/trans.order.create", params);
        r.rawResponse = resp.rawResponse;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        r.success = true;
        r.channelOrderNo = resp.data.get("sandSerialNo", "").asString();
        Json::Value credential = resp.data["credential"];
        if (credential.isMember("qrCode")) {
            r.payUrl = credential["qrCode"].asString();
        } else if (credential.isMember("cashierUrl")) {
            r.payUrl = credential["cashierUrl"].asString();
        } else if (credential.isMember("tradeNo")) {
            r.payUrl = credential["tradeNo"].asString(); // JSAPI tradeNo
        }
        r.extra["credential"] = credential;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "respCode=020002";
        auto signIt = params.find("sign");
        auto dataIt = params.find("bizData");
        if (signIt == params.end() || dataIt == params.end()) { r.verified = false; return r; }
        std::string pubKey = channelParams.get("sand_pubkey", "").asString();
        if (pubKey.empty()) { r.verified = false; return r; }
        // Verify sign on bizData
        r.verified = RsaUtils::verifySha256(dataIt->second, signIt->second, normalizePublicKey(pubKey));
        if (!r.verified) return r;
        // Parse bizData JSON
        Json::Value data;
        if (!Json::Reader().parse(dataIt->second, data)) return r;
        std::string orderStatus = data.get("orderStatus", "").asString();
        r.paid = (orderStatus == "success");
        r.orderId = data.get("outOrderNo", "").asString();
        r.channelOrderNo = data.get("sandSerialNo", "").asString();
        try { r.paidAmount = std::stod(data.get("amount", "0").asString()); } catch (...) {}
        if (data.isMember("payer")) {
            r.buyerId = data["payer"].get("payerAccNo", "").asString();
        }
        r.responseText = r.paid ? "respCode=000000" : "respCode=020002";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        Json::Value params;
        params["marketProduct"] = p.get("product", "QZF").asString();
        params["outReqTime"] = now();
        params["mid"] = p.get("appid", "").asString();
        params["outOrderNo"] = req.refundNo;
        params["oriOutOrderNo"] = req.orderId;
        params["amount"] = fmtAmount(req.refundAmount);
        params["notifyUrl"] = req.notifyUrl;

        auto resp = executeApi(p, "/v4/sd-receipts/api/trans/trans.order.refund", params);
        r.rawResponse = resp.rawResponse;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = resp.data.get("sandSerialNo", "").asString();
        return r;
    }

private:
    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        Json::Value data;
    };

    static ApiResponse executeApi(const Json::Value &p, const std::string &path, const Json::Value &params) {
        ApiResponse ar;
        bool isTest = p.get("appswitch", "0").asInt() == 1;
        std::string baseUrl = isTest ? "https://openapi-uat01.sand.com.cn" : "https://openapi.sandpay.com.cn";
        std::string privateKey = p.get("merchant_pem", "").asString();
        std::string pubKey = p.get("sand_pubkey", "").asString();
        std::string mid = p.get("appid", "").asString();

        if (privateKey.empty() || pubKey.empty()) {
            ar.errMsg = "杉德证书未配置(merchant_pem/sand_pubkey)";
            return ar;
        }

        // ksort params and JSON encode
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string data = Json::writeString(wb, params);

        // Generate random AES key (16 bytes)
        std::string aesKey = randomStr(16);
        // AES-128-ECB encrypt bizData
        std::string bizData = aesEncrypt(data, aesKey);
        // RSA public encrypt AES key
        std::string encryptKey = rsaPublicEncrypt(aesKey, normalizePublicKey(pubKey));

        // Build public params
        Json::Value publicParams;
        publicParams["accessMid"] = mid;
        publicParams["timestamp"] = nowDatetime();
        publicParams["version"] = "4.0.0";
        publicParams["signType"] = "RSA";
        publicParams["encryptType"] = "AES";
        publicParams["encryptKey"] = encryptKey;
        publicParams["bizData"] = bizData;
        // Sign bizData with merchant private key
        publicParams["sign"] = RsaUtils::signSha256(bizData, normalizePrivateKey(privateKey));

        std::string body = Json::writeString(wb, publicParams);
        ar.rawResponse = body;

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";

        auto resp = SyncHttp::postJson(baseUrl + path, body, headers);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { ar.errMsg = "响应解析失败"; return ar; }
        std::string respCode = result.get("respCode", "").asString();
        if (respCode == "fail") {
            ar.errMsg = result.get("respDesc", "请求失败").asString();
            return ar;
        }
        if (respCode != "success") { ar.errMsg = "响应码异常: " + respCode; return ar; }

        // Verify response sign
        std::string respBizData = result.get("bizData", "").asString();
        std::string respSign = result.get("sign", "").asString();
        if (!RsaUtils::verifySha256(respBizData, respSign, normalizePublicKey(pubKey))) {
            ar.errMsg = "返回数据验签失败";
            return ar;
        }

        // Decrypt AES key with merchant private key
        std::string respEncryptKey = result.get("encryptKey", "").asString();
        std::string decryptAesKey = rsaPrivateDecrypt(respEncryptKey, normalizePrivateKey(privateKey));
        if (decryptAesKey.empty()) { ar.errMsg = "AES密钥解密失败"; return ar; }

        // AES decrypt bizData
        std::string plainText = aesDecrypt(respBizData, decryptAesKey);
        if (plainText.empty()) { ar.errMsg = "AES解密失败"; return ar; }

        Json::Value bizResult;
        if (!Json::Reader().parse(plainText, bizResult)) { ar.errMsg = "bizData解析失败"; return ar; }
        if (bizResult.get("resultStatus", "").asString() == "fail") {
            ar.errMsg = "[" + bizResult.get("errorCode", "").asString() + "]" + bizResult.get("errorDesc", "").asString();
            return ar;
        }
        ar.success = true;
        ar.data = bizResult;
        return ar;
    }

    // ── AES-128-ECB ──
    static std::string aesEncrypt(const std::string &plain, const std::string &key) {
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, (const unsigned char*)key.c_str(), nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 1);
        int outlen = 0, tmplen = 0;
        std::vector<unsigned char> out(plain.size() + 16);
        EVP_EncryptUpdate(ctx, out.data(), &outlen, (const unsigned char*)plain.c_str(), (int)plain.size());
        EVP_EncryptFinal_ex(ctx, out.data() + outlen, &tmplen);
        outlen += tmplen;
        EVP_CIPHER_CTX_free(ctx);
        return base64Encode(std::string((char*)out.data(), outlen));
    }

    static std::string aesDecrypt(const std::string &cipher, const std::string &key) {
        std::string raw = base64Decode(cipher);
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, (const unsigned char*)key.c_str(), nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 1);
        int outlen = 0, tmplen = 0;
        std::vector<unsigned char> out(raw.size() + 16);
        EVP_DecryptUpdate(ctx, out.data(), &outlen, (const unsigned char*)raw.c_str(), (int)raw.size());
        EVP_DecryptFinal_ex(ctx, out.data() + outlen, &tmplen);
        outlen += tmplen;
        EVP_CIPHER_CTX_free(ctx);
        return std::string((char*)out.data(), outlen);
    }

    // ── RSA public encrypt (PKCS1) ──
    static std::string rsaPublicEncrypt(const std::string &data, const std::string &pubKeyPem) {
        BIO *bio = BIO_new_mem_buf(pubKeyPem.c_str(), (int)pubKeyPem.size());
        EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return "";
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
        EVP_PKEY_encrypt_init(ctx);
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
        size_t outlen = 0;
        EVP_PKEY_encrypt(ctx, nullptr, &outlen, (const unsigned char*)data.c_str(), data.size());
        std::vector<unsigned char> out(outlen);
        EVP_PKEY_encrypt(ctx, out.data(), &outlen, (const unsigned char*)data.c_str(), data.size());
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return base64Encode(std::string((char*)out.data(), outlen));
    }

    // ── RSA private decrypt (PKCS1) ──
    static std::string rsaPrivateDecrypt(const std::string &cipher, const std::string &privKeyPem) {
        std::string raw = base64Decode(cipher);
        BIO *bio = BIO_new_mem_buf(privKeyPem.c_str(), (int)privKeyPem.size());
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return "";
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
        EVP_PKEY_decrypt_init(ctx);
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
        size_t outlen = 0;
        EVP_PKEY_decrypt(ctx, nullptr, &outlen, (const unsigned char*)raw.c_str(), raw.size());
        std::vector<unsigned char> out(outlen);
        EVP_PKEY_decrypt(ctx, out.data(), &outlen, (const unsigned char*)raw.c_str(), raw.size());
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::string((char*)out.data(), outlen);
    }

    // ── Helpers ──
    static std::string normalizePrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN PRIVATE KEY-----\n" + b + "-----END PRIVATE KEY-----\n";
    }
    static std::string normalizePublicKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN PUBLIC KEY-----\n" + b + "-----END PUBLIC KEY-----\n";
    }

    static std::string getPayType(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "WXPAY";
        if (payType.find("ali") != std::string::npos) return "ALIPAY";
        return "CUPPAY"; // 银联/云闪付
    }
    static std::string getPayMode(const std::string &payType, int apptype) {
        if (apptype == 2) {
            if (payType.find("wx") != std::string::npos) return "JSAPI";
            if (payType.find("ali") != std::string::npos) return "JSAPI";
        }
        if (apptype == 3) return "SANDH5"; // 快捷支付
        return "QR"; // 扫码
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string now() {
        auto t = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[16]; std::strftime(b, sizeof(b), "%Y%m%d%H%M%S", &tmv); return b;
    }

    static std::string nowDatetime() {
        auto t = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[24]; std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv); return b;
    }

    static std::string randomStr(int len) {
        static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string s;
        unsigned char buf[32];
        RAND_bytes(buf, len < 32 ? len : 32);
        for (int i = 0; i < len; ++i) s += chars[buf[i % 32] % (sizeof(chars) - 1)];
        return s;
    }

    static std::string base64Encode(const std::string &input) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : input) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) { out.push_back(table[(val >> valb) & 0x3F]); valb -= 6; }
        }
        if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    }

    static std::string base64Decode(const std::string &input) {
        static int d[256]; static bool init = false;
        if (!init) { memset(d, -1, sizeof(d)); for (int i = 0; i < 64; i++) d[(unsigned char)("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i])] = i; init = true; }
        std::string out;
        int val = 0, valb = -8;
        for (unsigned char c : input) {
            if (d[c] == -1) break;
            val = (val << 6) + d[c];
            valb += 6;
            if (valb >= 0) { out.push_back(char((val >> valb) & 0xFF)); valb -= 8; }
        }
        return out;
    }
};

REGISTER_CHANNEL_PLUGIN(SandpayPlugin);
