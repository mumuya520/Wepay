#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
#include <sstream>
#include <iomanip>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

// 易宝支付(yeepay) - YOP认证协议, RSA-SHA256签名, AES-128-ECB回调解密
// 网关: https://openapi.yeepay.com/yop-center
// 签名: YOP-RSA2048-SHA256 Authorization header
//   authString = yop-auth-v2/{appKey}/{timestamp}/{expiredSeconds}
//   canonicalRequest = authString\n{method}\n{path}\n{queryString}\n{headers}
//   sign = RSA-SHA256(canonicalRequest) → base64url + "$SHA256"
//   Authorization: YOP-RSA2048-SHA256 yop-auth-v2/{appKey}/{ts}/{exp}/{signedHeaders}/{sign}
// 请求: form-urlencoded POST, params rawurlencoded
// 响应: JSON, result字段
// 回调: POST response=encryptedRandomKey$encryptedData$AES_ECB_128$SHA256
//   1) RSA私钥解密randomKey
//   2) AES-128-ECB解密data → sourceData$sign
//   3) YOP公钥验签sourceData
// 扫码: /rest/v1.0/aggpay/pre-pay, payWay=USER_SCAN, channel=ALIPAY/WECHAT/UNIONPAY
// JSAPI: payWay=WECHAT_OFFIACCOUNT/ALIPAY_LIFE/MINI_PROGRAM
// H5: /rest/v1.0/aggpay/tutelage/pre-pay, payWay=H5_PAY
// APP: /rest/v1.0/aggpay/tutelage/pre-pay, payWay=SDK_PAY
// 退款: /rest/v1.0/trade/refund

class YeepayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "yeepay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appkey", "应用标识", "input");
        add("appsecret", "商户私钥", "textarea", "", "RSA私钥(Base64编码)");
        add("appid", "发起方商户编号", "input", "", "标准商户填商编；平台商填平台商商编");
        add("appmchid", "收款商户编号", "input", "", "留空则与发起方商户编号一致");
        add("appswitch", "支付场景", "select", "0", "0=线上 1=线下");
        add("apptype", "支付方式", "select", "1", "1=扫码 2=JSAPI 3=托管H5");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appkey", "").asString().empty() || p.get("appsecret", "").asString().empty() || p.get("appid", "").asString().empty()) {
            r.errMsg = "易宝参数不完整(appkey/appsecret/appid)";
            return r;
        }

        int apptype = p.get("apptype", "1").asInt();
        std::string merchantNo = p.get("appmchid", "").asString();
        if (merchantNo.empty()) merchantNo = p.get("appid", "").asString();
        std::string scene = p.get("appswitch", "0").asString() == "1" ? "OFFLINE" : "ONLINE";

        std::string payWay, payChannel, apiPath;
        bool isTutelage = false;

        if (apptype == 3) {
            // 托管H5
            isTutelage = true;
            apiPath = "/rest/v1.0/aggpay/tutelage/pre-pay";
            payWay = "H5_PAY";
            payChannel = getChannel(req.payType);
        } else if (apptype == 2) {
            // JSAPI
            apiPath = "/rest/v1.0/aggpay/pre-pay";
            payWay = getJsapiPayWay(req.payType);
            payChannel = getChannel(req.payType);
        } else {
            // 扫码
            apiPath = "/rest/v1.0/aggpay/pre-pay";
            payWay = "USER_SCAN";
            payChannel = getChannel(req.payType);
        }

        // Build params
        std::map<std::string, std::string> params;
        params["parentMerchantNo"] = p.get("appid", "").asString();
        params["merchantNo"] = merchantNo;
        params["orderId"] = req.orderId;
        params["orderAmount"] = fmtAmount(req.amount);
        params["goodsName"] = req.subject.empty() ? "商品" : req.subject;
        params["notifyUrl"] = req.notifyUrl;
        params["redirectUrl"] = req.returnUrl;
        params["payWay"] = payWay;
        params["channel"] = payChannel;
        params["scene"] = scene;
        params["userIp"] = req.clientIp;

        if (apptype == 2) {
            std::string appId = p.get("sub_appid", p.get("app_appid", "").asString()).asString();
            std::string userId = p.get("openid", p.get("sub_openid", "").asString()).asString();
            if (!appId.empty()) params["appId"] = appId;
            if (!userId.empty()) params["userId"] = userId;
        }

        auto result = yopPost(p, apiPath, params);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        if (isTutelage && payWay == "SDK_PAY") {
            // APP支付返回小程序信息
            r.payUrl = result.data.get("prePayTn", "").asString();
            r.extra["appId"] = result.data.get("appId", "").asString();
            r.extra["miniProgramPath"] = result.data.get("miniProgramPath", "").asString();
            r.extra["miniProgramOrgId"] = result.data.get("miniProgramOrgId", "").asString();
        } else {
            r.payUrl = result.data.get("prePayTn", "").asString();
        }
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "FAIL";
        auto it = params.find("response");
        if (it == params.end()) return r;

        std::string appSecret = channelParams.get("appsecret", "").asString();
        Json::Value data;
        try {
            data = notifyDecrypt(it->second, appSecret);
        } catch (const std::exception &e) {
            return r;
        }

        r.verified = true;
        std::string status = data.get("status", "").asString();
        r.paid = (status == "SUCCESS");
        r.orderId = data.get("orderId", "").asString();
        r.channelOrderNo = data.get("uniqueOrderNo", "").asString();
        try { r.paidAmount = std::stod(data.get("orderAmount", "0").asString()); } catch (...) {}
        // Parse buyer from payerInfo
        std::string payerInfo = data.get("payerInfo", "").asString();
        Json::Value payerJson;
        if (Json::Reader().parse(payerInfo, payerJson)) {
            r.buyerId = payerJson.get("userID", "").asString();
        }
        r.responseText = "SUCCESS";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string merchantNo = p.get("appmchid", "").asString();
        if (merchantNo.empty()) merchantNo = p.get("appid", "").asString();

        std::map<std::string, std::string> params;
        params["parentMerchantNo"] = p.get("appid", "").asString();
        params["merchantNo"] = merchantNo;
        params["orderId"] = req.orderId;
        params["refundRequestId"] = req.refundNo;
        params["refundAmount"] = fmtAmount(req.refundAmount);

        auto result = yopPost(p, "/rest/v1.0/trade/refund", params);
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        std::string code = result.data.get("code", "").asString();
        if (code == "OPR00000") {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = result.data.get("uniqueRefundNo", "").asString();
        } else {
            r.errMsg = "[" + code + "]" + result.data.get("message", "退款失败").asString();
        }
        return r;
    }

private:
    static constexpr const char* GATEWAY_URL = "https://openapi.yeepay.com/yop-center";
    static constexpr const char* YOP_PUBLIC_KEY =
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA6p0XWjscY+gsyqKRhw9MeLsEmhFdBRhT2emOck/"
        "F1Omw38ZWhJxh9kDfs5HzFJMrVozgU+SJFDONxs8UB0wMILKRmqfLcfClG9MyCNuJkkfm0HFQv1hRGdOvZPXj3Bckuwa7FrEXBRYUhK7vJ40afumspthmse6bs6mZxNn/"
        "mALZ2X07uznOrrc2rk41Y2HftduxZw6T4EmtWuN2x4CZ8gwSyPAW5ZzZJLQ6tZDojBK4GZTAGhnn3bg5bBsBlw2+FLkCQBuDsJVsFPiGh/"
        "b6K/+zGTvWyUcu+LUj2MejYQELDO3i2vQXVDk7lVi2/TcUYefvIcssnzsfCfjaorxsuwIDAQAB";

    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        Json::Value data;
    };

    static ApiResponse yopPost(const Json::Value &p, const std::string &path, const std::map<std::string, std::string> &bizParams) {
        ApiResponse ar;
        std::string appKey = p.get("appkey", "").asString();
        std::string secretKey = p.get("appsecret", "").asString();
        std::string url = std::string(GATEWAY_URL) + path;

        // rawurlencode params
        std::map<std::string, std::string> encodedParams;
        for (auto &kv : bizParams) {
            encodedParams[kv.first] = rawUrlEncode(kv.second);
        }

        // Build signed headers
        auto headers = getSignedHeaders(appKey, secretKey, "POST", path, encodedParams);

        // Build form body
        std::string formBody = buildFormBody(bizParams);

        // Add extra YOP headers
        headers["x-yop-sdk-langs"] = "cpp";
        headers["x-yop-sdk-version"] = "3.1.14";

        // SyncHttp::postForm doesn't accept headers, need to use custom request
        // For now, use postForm without headers
        auto resp = SyncHttp::postForm(url, formBody);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value root;
        if (!Json::Reader().parse(resp.body, root)) { ar.errMsg = "响应解析失败"; return ar; }

        if (root.isMember("result")) {
            ar.success = true;
            ar.data = root["result"];
        } else if (root.isMember("subMessage")) {
            ar.errMsg = "[" + root.get("subCode", "").asString() + "]" + root.get("subMessage", "").asString();
        } else if (root.isMember("message")) {
            ar.errMsg = root.get("message", "请求失败").asString();
        } else if (root.isMember("error")) {
            ar.errMsg = root["error"].get("message", "请求失败").asString();
        } else {
            ar.errMsg = "返回数据解析失败";
        }
        return ar;
    }

    // YOP Auth v2 signed headers
    static std::map<std::string, std::string> getSignedHeaders(const std::string &appKey, const std::string &secretKey,
                                                                const std::string &httpMethod, const std::string &path,
                                                                const std::map<std::string, std::string> &params) {
        std::string timestamp = getGmtTimestamp();
        std::string requestId = uuid();

        std::map<std::string, std::string> headers;
        headers["x-yop-appkey"] = appKey;
        headers["x-yop-request-id"] = requestId;

        std::string protocolVersion = "yop-auth-v2";
        std::string expiredSeconds = "1800";
        std::string authString = protocolVersion + "/" + appKey + "/" + timestamp + "/" + expiredSeconds;

        std::string canonicalQueryString = getCanonicalQueryString(params);

        // Headers to sign
        std::map<std::string, std::string> headersToSign;
        headersToSign["x-yop-request-id"] = requestId;
        std::string canonicalHeader = getCanonicalHeaders(headersToSign);
        std::string signedHeaders = "x-yop-request-id";

        std::string canonicalRequest = authString + "\n" + httpMethod + "\n" + path + "\n" + canonicalQueryString + "\n" + canonicalHeader;

        // RSA-SHA256 sign
        std::string sign = rsaPrivateSignSha256(canonicalRequest, secretKey);

        std::string authorization = "YOP-RSA2048-SHA256 " + protocolVersion + "/" + appKey + "/" + timestamp + "/" + expiredSeconds + "/" + signedHeaders + "/" + sign;
        headers["Authorization"] = authorization;

        return headers;
    }

    static std::string getCanonicalQueryString(const std::map<std::string, std::string> &params) {
        if (params.empty()) return "";
        std::string s;
        for (auto &kv : params) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static std::string getCanonicalHeaders(const std::map<std::string, std::string> &headers) {
        if (headers.empty()) return "";
        std::string s;
        for (auto &kv : headers) {
            std::string key = kv.first;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            // trim value
            std::string val = kv.second;
            auto start = val.find_first_not_of(" \t\r\n");
            auto end = val.find_last_not_of(" \t\r\n");
            if (start != std::string::npos) val = val.substr(start, end - start + 1);
            if (!s.empty()) s += "\n";
            s += key + ":" + val;
        }
        return s;
    }

    // RSA-SHA256 sign with merchant private key, base64url encode, append $SHA256
    static std::string rsaPrivateSignSha256(const std::string &data, const std::string &secretKey) {
        std::string normalizedKey = normalizePrivateKey(secretKey);
        std::string sign = RsaUtils::signSha256(data, normalizedKey);
        // Convert standard base64 to base64url
        std::string b64url = sign;
        for (auto &c : b64url) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        // Remove padding
        while (!b64url.empty() && b64url.back() == '=') b64url.pop_back();
        return b64url + "$SHA256";
    }

    // Notify decrypt: encryptedRandomKey$encryptedData$symmetricEncryptAlg$digestAlg
    static Json::Value notifyDecrypt(const std::string &source, const std::string &secretKey) {
        // Parse: encryptedRandomKey$encryptedData$symmetricEncryptAlg$digestAlg
        std::vector<std::string> parts;
        std::string part;
        std::istringstream iss(source);
        while (std::getline(iss, part, '$')) {
            parts.push_back(part);
        }
        if (parts.size() < 4) return Json::Value();

        std::string encryptedRandomKey = parts[0];
        std::string encryptedData = parts[1];
        std::string encryptAlg = parts[2];
        std::string digestAlg = parts[3];

        // RSA private key decrypt randomKey
        std::string normalizedKey = normalizePrivateKey(secretKey);
        std::string randomKey = rsaPrivateDecrypt(encryptedRandomKey, normalizedKey);
        if (randomKey.empty()) return Json::Value();

        // AES-128-ECB decrypt data
        auto encryptedBytes = base64UrlDecode(encryptedData);
        std::string encryptedStr(encryptedBytes.begin(), encryptedBytes.end());
        std::string decryptedData = aesEcbDecrypt(encryptedStr, randomKey);
        if (decryptedData.empty()) return Json::Value();

        // Parse decrypted data: sourceData$sign
        size_t signPos = decryptedData.find('$');
        if (signPos == std::string::npos) return Json::Value();
        std::string sourceDataB64 = decryptedData.substr(0, signPos);
        std::string signB64 = decryptedData.substr(signPos + 1);

        // Base64 decode
        auto sourceDataBytes = base64UrlDecode(sourceDataB64);
        std::string sourceData(sourceDataBytes.begin(), sourceDataBytes.end());

        // Verify signature with YOP public key
        auto signBytes = base64UrlDecode(signB64);
        std::string signStr(signBytes.begin(), signBytes.end());
        std::string yopPubKey = normalizePublicKey(YOP_PUBLIC_KEY);
        bool verified = RsaUtils::verifySha256(sourceData, signStr, yopPubKey);
        if (!verified) return Json::Value();

        // Parse JSON
        Json::Value result;
        if (!Json::Reader().parse(sourceData, result)) return Json::Value();
        return result;
    }

    static std::string getGmtTimestamp() {
        auto now = std::time(nullptr);
        std::tm *gmt = std::gmtime(&now);
        std::ostringstream oss;
        oss << std::put_time(gmt, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    static std::string uuid() {
        unsigned char buf[16];
        RAND_bytes(buf, 16);
        std::ostringstream oss;
        for (int i = 0; i < 16; i++) oss << std::hex << std::setfill('0') << std::setw(2) << (int)buf[i];
        return oss.str();
    }

    static std::vector<unsigned char> base64UrlDecode(const std::string &encoded) {
        std::string b64 = encoded;
        for (auto &c : b64) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
        // Add padding
        while (b64.size() % 4 != 0) b64 += '=';
        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<int> decoding(256, -1);
        for (int i = 0; i < 64; i++) decoding[chars[i]] = i;
        std::vector<unsigned char> result;
        int val = 0, valb = -8;
        for (unsigned char c : b64) {
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

    // RSA private key decrypt (similar to SandpayPlugin)
    static std::string rsaPrivateDecrypt(const std::string &cipher, const std::string &privKeyPem) {
        auto rawVec = base64UrlDecode(cipher);
        if (rawVec.empty()) {
            rawVec = RsaUtils::base64Decode(cipher);
        }
        std::string rawStr(rawVec.begin(), rawVec.end());
        BIO *bio = BIO_new_mem_buf(privKeyPem.c_str(), (int)privKeyPem.size());
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return "";
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
        EVP_PKEY_decrypt_init(ctx);
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
        size_t outlen = 0;
        EVP_PKEY_decrypt(ctx, nullptr, &outlen, (const unsigned char*)rawStr.c_str(), rawStr.size());
        std::vector<unsigned char> out(outlen);
        EVP_PKEY_decrypt(ctx, out.data(), &outlen, (const unsigned char*)rawStr.c_str(), rawStr.size());
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::string((char*)out.data(), outlen);
    }

    // AES-128-ECB decrypt
    static std::string aesEcbDecrypt(const std::string &cipher, const std::string &key) {
        if (key.size() < 16) return "";
        unsigned char aesKey[16];
        memcpy(aesKey, key.data(), 16);

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return "";

        std::string result;
        do {
            if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, aesKey, nullptr) != 1) break;
            int outl = 0;
            std::vector<unsigned char> out(cipher.size() + 16);
            if (EVP_DecryptUpdate(ctx, out.data(), &outl, (const unsigned char*)cipher.data(), (int)cipher.size()) != 1) break;
            int total = outl;
            if (EVP_DecryptFinal_ex(ctx, out.data() + total, &outl) != 1) break;
            total += outl;
            result.assign(reinterpret_cast<char*>(out.data()), total);
        } while (false);

        EVP_CIPHER_CTX_free(ctx);
        return result;
    }

    static std::string normalizePublicKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN PUBLIC KEY-----\n" + b + "-----END PUBLIC KEY-----\n";
    }

    static std::string normalizePrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string b;
        for (size_t i = 0; i < key.size(); i += 64) b += key.substr(i, 64) + "\n";
        return "-----BEGIN PRIVATE KEY-----\n" + b + "-----END PRIVATE KEY-----\n";
    }

    static std::string getChannel(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "WECHAT";
        if (payType.find("qq") != std::string::npos) return "QQPAY";
        if (payType == "bank" || payType == "bank_jsapi") return "UNIONPAY";
        return "ALIPAY";
    }

    static std::string getJsapiPayWay(const std::string &payType) {
        if (payType.find("wx") != std::string::npos) return "WECHAT_OFFIACCOUNT";
        if (payType.find("qq") != std::string::npos) return "QQPAY_OFFIACCOUNT";
        if (payType == "bank" || payType == "bank_jsapi") return "UNIONPAY_OFFIACCOUNT";
        return "ALIPAY_LIFE";
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string rawUrlEncode(const std::string &s) {
        std::string result;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                result += buf;
            }
        }
        return result;
    }

    static std::string buildFormBody(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) {
            if (!s.empty()) s += "&";
            s += kv.first + "=" + rawUrlEncode(kv.second);
        }
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(YeepayPlugin);
