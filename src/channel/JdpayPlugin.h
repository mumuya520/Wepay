#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/des.h>
#include <openssl/err.h>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class JdpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "jdpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户号", "input");
        add("appkey", "商户DES密钥", "input", "", "Base64编码的3DES密钥(24字节)");
        add("appsecret", "商户RSA私钥", "textarea", "", "PEM格式，用于签名和加密");
        add("appmchid_pubkey", "京东公钥", "textarea", "", "PEM格式，用于验签和解密");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() || p.get("appsecret", "").asString().empty()) {
            r.errMsg = "京东支付参数不完整(appid/appkey/appsecret)";
            return r;
        }
        std::map<std::string, std::string> m;
        m["version"] = "V2.0";
        m["merchant"] = p.get("appid", "").asString();
        m["tradeNum"] = req.orderId;
        m["tradeName"] = req.subject.empty() ? "商品" : req.subject;
        m["tradeTime"] = now();
        m["amount"] = std::to_string((long long)std::llround(req.amount * 100.0));
        m["currency"] = "CNY";
        m["callbackUrl"] = req.returnUrl;
        m["notifyUrl"] = req.notifyUrl;
        m["ip"] = req.clientIp;
        m["userId"] = "";
        m["orderType"] = "1";
        // sign: sort params, concat k=v&, remove empty & "sign", SHA-256, RSA private encrypt
        std::string signStr = signString(m, {"sign"});
        std::string sha256hex = sha256Hex(signStr);
        m["sign"] = rsaPrivateEncryptBase64(sha256hex, normalizePrivateKey(p.get("appsecret", "").asString()));
        // 3DES encrypt fields
        std::string desKey = base64Decode(p.get("appkey", "").asString());
        m["tradeNum"] = tdesEncrypt2Hex(desKey, m["tradeNum"]);
        m["tradeName"] = tdesEncrypt2Hex(desKey, m["tradeName"]);
        m["tradeTime"] = tdesEncrypt2Hex(desKey, m["tradeTime"]);
        m["amount"] = tdesEncrypt2Hex(desKey, m["amount"]);
        m["currency"] = tdesEncrypt2Hex(desKey, m["currency"]);
        m["callbackUrl"] = tdesEncrypt2Hex(desKey, m["callbackUrl"]);
        m["notifyUrl"] = tdesEncrypt2Hex(desKey, m["notifyUrl"]);
        m["ip"] = tdesEncrypt2Hex(desKey, m["ip"]);
        // Build form HTML for redirect
        std::string url = "https://wepay.jd.com/jdpay/saveOrder";
        std::ostringstream html;
        html << "<form action=\"" << url << "\" method=\"post\" id=\"dopay\">";
        for (auto &kv : m) html << "<input type=\"hidden\" name=\"" << kv.first << "\" value=\"" << kv.second << "\" />";
        html << "<input type=\"submit\" value=\"正在跳转\"></form><script>document.getElementById(\"dopay\").submit();</script>";
        r.success = true;
        r.payUrl = url;
        r.rawResponse = html.str();
        r.extra["html"] = html.str();
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "error";
        // JD pay notify: XML with <encrypt> field, decrypt with 3DES, verify sign with JD public key
        std::string desKey = base64Decode(channelParams.get("appkey", "").asString());
        std::string jdPubKey = normalizePublicKey(channelParams.get("appmchid_pubkey", "").asString());
        // Parse XML body to get version, merchant, encrypt, result
        std::string encryptStr = xmlGetValue(body, "encrypt");
        if (encryptStr.empty()) return r;
        std::string encryptBin = base64Decode(encryptStr);
        std::string decryptedXml = tdesDecrypt4Hex(desKey, encryptBin);
        // Remove <sign>...</sign> from decrypted XML for SHA-256
        std::string xmlNoSign = removeXmlTag(decryptedXml, "sign");
        std::string localDigest = sha256Hex(xmlNoSign);
        std::string inputSign = xmlGetValue(decryptedXml, "sign");
        std::string decryptedSign = rsaPublicDecryptBase64(inputSign, jdPubKey);
        r.verified = (decryptedSign == localDigest);
        if (!r.verified) return r;
        // Parse decrypted XML fields
        std::string tradeNum = xmlGetValue(decryptedXml, "tradeNum");
        std::string amount = xmlGetValue(decryptedXml, "amount");
        std::string status = xmlGetValue(decryptedXml, "status");
        r.orderId = tradeNum;
        r.paid = (status == "2");
        try { r.paidAmount = std::stod(amount) / 100.0; } catch (...) {}
        r.responseText = r.paid ? "success" : "error";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string desKey = base64Decode(p.get("appkey", "").asString());
        // Build inner XML
        std::map<std::string, std::string> inner;
        inner["version"] = "V2.0";
        inner["merchant"] = p.get("appid", "").asString();
        inner["tradeNum"] = req.refundNo;
        inner["oTradeNum"] = req.channelOrderNo.empty() ? req.orderId : req.channelOrderNo;
        inner["amount"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        inner["currency"] = "CNY";
        std::string innerXml = mapToXml(inner, "jdpay");
        // Sign: SHA-256 of inner XML, RSA private encrypt
        std::string sign = rsaPrivateEncryptBase64(sha256Hex(innerXml), normalizePrivateKey(p.get("appsecret", "").asString()));
        // Insert sign into XML
        std::string signedXml = insertXmlBeforeEnd(innerXml, "sign", sign);
        // 3DES encrypt, base64
        std::string encrypted = base64Encode(tdesEncrypt2HexRaw(desKey, signedXml));
        // Build outer XML
        std::map<std::string, std::string> outer;
        outer["version"] = "V2.0";
        outer["merchant"] = p.get("appid", "").asString();
        outer["encrypt"] = encrypted;
        std::string reqXml = mapToXml(outer, "jdpay");
        auto resp = SyncHttp::postJson("https://paygate.jd.com/service/refund", reqXml, {{"Content-Type", "application/xml; charset=utf-8"}});
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        // Decrypt response
        std::string respEncrypt = xmlGetValue(resp.body, "encrypt");
        if (respEncrypt.empty()) { r.errMsg = "京东退款响应解析失败"; return r; }
        std::string respDecrypted = tdesDecrypt4Hex(desKey, base64Decode(respEncrypt));
        std::string respNoSign = removeXmlTag(respDecrypted, "sign");
        std::string respSign = xmlGetValue(respDecrypted, "sign");
        std::string respLocalDigest = sha256Hex(respNoSign);
        std::string respDecSign = rsaPublicDecryptBase64(respSign, normalizePublicKey(p.get("appmchid_pubkey", "").asString()));
        if (respLocalDigest != respDecSign) { r.errMsg = "京东退款响应验签失败"; return r; }
        std::string status = xmlGetValue(respDecrypted, "status");
        if (status == "1") {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = xmlGetValue(respDecrypted, "oTradeNum");
        } else {
            r.errMsg = "[" + xmlGetValue(respDecrypted, "code") + "]" + xmlGetValue(respDecrypted, "desc");
        }
        return r;
    }

private:
    // ── Sign string: sort map, concat k=v&, skip keys in unsign, skip empty ──
    static std::string signString(const std::map<std::string, std::string> &m, const std::vector<std::string> &skip) {
        std::string s;
        for (auto &kv : m) {
            if (kv.second.empty()) continue;
            if (std::find(skip.begin(), skip.end(), kv.first) != skip.end()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    // ── SHA-256 hex ──
    static std::string sha256Hex(const std::string &data) {
        unsigned char hash[32]; unsigned int len = 0;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, data.data(), data.size());
        EVP_DigestFinal_ex(ctx, hash, &len);
        EVP_MD_CTX_free(ctx);
        std::ostringstream oss;
        for (unsigned int i = 0; i < len; ++i) oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        return oss.str();
    }

    // ── RSA private encrypt (PKCS1) → Base64 ──
    static std::string rsaPrivateEncryptBase64(const std::string &data, const std::string &pem) {
        BIO *bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return "";
        RSA *rsa = EVP_PKEY_get1_RSA(pkey);
        EVP_PKEY_free(pkey);
        if (!rsa) return "";
        int size = RSA_size(rsa);
        std::vector<unsigned char> buf(size);
        int n = RSA_private_encrypt((int)data.size(), (const unsigned char *)data.data(), buf.data(), rsa, RSA_PKCS1_PADDING);
        RSA_free(rsa);
        if (n <= 0) return "";
        return base64Encode(std::string((char *)buf.data(), n));
    }

    // ── RSA public decrypt (PKCS1) → string ──
    static std::string rsaPublicDecryptBase64(const std::string &b64Sign, const std::string &pem) {
        BIO *bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
        EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return "";
        RSA *rsa = EVP_PKEY_get1_RSA(pkey);
        EVP_PKEY_free(pkey);
        if (!rsa) return "";
        std::string bin = base64Decode(b64Sign);
        int size = RSA_size(rsa);
        std::vector<unsigned char> buf(size);
        int n = RSA_public_decrypt((int)bin.size(), (const unsigned char *)bin.data(), buf.data(), rsa, RSA_PKCS1_PADDING);
        RSA_free(rsa);
        if (n <= 0) return "";
        return std::string((char *)buf.data(), n);
    }

    // ── 3DES-EDE3 encrypt with custom padding (4-byte length prefix + zero pad to 8) ──
    static std::string tdesEncrypt2Hex(const std::string &key, const std::string &src) {
        // Build padded data: 4-byte big-endian length + src + zero padding to 8-byte boundary
        size_t len = src.size();
        std::string padded;
        padded.resize(4);
        padded[0] = (char)((len >> 24) & 0xFF);
        padded[1] = (char)((len >> 16) & 0xFF);
        padded[2] = (char)((len >> 8) & 0xFF);
        padded[3] = (char)(len & 0xFF);
        padded += src;
        size_t totalLen = padded.size();
        size_t padLen = (8 - totalLen % 8) % 8;
        padded.append(padLen, '\0');
        // 3DES-ECB encrypt
        std::string enc = desEcbEncrypt(key, padded);
        return binToHex(enc);
    }

    static std::string tdesEncrypt2HexRaw(const std::string &key, const std::string &src) {
        return hexToBin(tdesEncrypt2Hex(key, src));
    }

    // ── 3DES-EDE3 decrypt with custom padding ──
    static std::string tdesDecrypt4Hex(const std::string &key, const std::string &hexData) {
        std::string bin = hexToBin(hexData);
        std::string dec = desEcbDecrypt(key, bin);
        if (dec.size() < 4) return "";
        size_t len = ((unsigned char)dec[0] << 24) | ((unsigned char)dec[1] << 16) | ((unsigned char)dec[2] << 8) | (unsigned char)dec[3];
        if (len > dec.size() - 4) return dec.substr(4);
        return dec.substr(4, len);
    }

    static std::string desEcbEncrypt(const std::string &key, const std::string &data) {
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        EVP_CIPHER_CTX_init(ctx);
        EVP_EncryptInit_ex(ctx, EVP_des_ede3(), nullptr, (const unsigned char *)key.data(), nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 0); // no padding, we handle it ourselves
        std::string out(data.size() + 8, '\0');
        int outLen = 0, tmpLen = 0;
        EVP_EncryptUpdate(ctx, (unsigned char *)out.data(), &outLen, (const unsigned char *)data.data(), (int)data.size());
        EVP_EncryptFinal_ex(ctx, (unsigned char *)out.data() + outLen, &tmpLen);
        EVP_CIPHER_CTX_free(ctx);
        out.resize(outLen + tmpLen);
        return out;
    }

    static std::string desEcbDecrypt(const std::string &key, const std::string &data) {
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        EVP_CIPHER_CTX_init(ctx);
        EVP_DecryptInit_ex(ctx, EVP_des_ede3(), nullptr, (const unsigned char *)key.data(), nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        std::string out(data.size(), '\0');
        int outLen = 0, tmpLen = 0;
        EVP_DecryptUpdate(ctx, (unsigned char *)out.data(), &outLen, (const unsigned char *)data.data(), (int)data.size());
        EVP_DecryptFinal_ex(ctx, (unsigned char *)out.data() + outLen, &tmpLen);
        EVP_CIPHER_CTX_free(ctx);
        out.resize(outLen + tmpLen);
        return out;
    }

    // ── XML helpers ──
    static std::string xmlGetValue(const std::string &xml, const std::string &tag) {
        std::string open = "<" + tag + ">";
        std::string close = "</" + tag + ">";
        auto s = xml.find(open);
        auto e = xml.find(close);
        if (s == std::string::npos || e == std::string::npos) return "";
        return xml.substr(s + open.size(), e - s - open.size());
    }

    static std::string removeXmlTag(const std::string &xml, const std::string &tag) {
        std::string open = "<" + tag + ">";
        std::string close = "</" + tag + ">";
        auto s = xml.find(open);
        auto e = xml.find(close);
        if (s == std::string::npos || e == std::string::npos) return xml;
        return xml.substr(0, s) + xml.substr(e + close.size());
    }

    static std::string insertXmlBeforeEnd(const std::string &xml, const std::string &tag, const std::string &value) {
        std::string close = "</jdpay>";
        auto pos = xml.find(close);
        if (pos == std::string::npos) return xml;
        return xml.substr(0, pos) + "<" + tag + ">" + value + "</" + tag + ">" + close;
    }

    static std::string mapToXml(const std::map<std::string, std::string> &m, const std::string &root) {
        std::ostringstream oss;
        oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><" << root << ">";
        for (auto &kv : m) oss << "<" << kv.first << ">" << kv.second << "</" << kv.first << ">";
        oss << "</" << root << ">";
        return oss.str();
    }

    // ── Encoding helpers ──
    static std::string base64Encode(const std::string &bin) { return RsaUtils::base64Encode(bin); }
    static std::string base64Decode(const std::string &b64) { auto v = RsaUtils::base64Decode(b64); return std::string((char*)v.data(), v.size()); }
    static std::string binToHex(const std::string &bin) {
        std::ostringstream oss;
        for (unsigned char c : bin) oss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        return oss.str();
    }
    static std::string hexToBin(const std::string &hex) {
        std::string bin;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            char h[3] = {hex[i], hex[i + 1], 0};
            bin.push_back((char)strtol(h, nullptr, 16));
        }
        return bin;
    }

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
};

REGISTER_CHANNEL_PLUGIN(JdpayPlugin);
