#pragma once

#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * WePay C++ SDK
 * 支持 MD5 和 RSA 签名的支付网关 SDK
 */
class WePay {
private:
    std::string mchId;
    std::string mchKey;
    std::string baseUrl;
    std::string signType;
    std::string privateKeyPem;

    // 辅助函数
    static std::string md5(const std::string& str) {
        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5_CTX ctx;
        MD5_Init(&ctx);
        MD5_Update(&ctx, str.c_str(), str.length());
        MD5_Final(digest, &ctx);

        std::stringstream ss;
        for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
            char buf[3];
            sprintf(buf, "%02x", digest[i]);
            ss << buf;
        }
        return ss.str();
    }

    static std::string base64Encode(const unsigned char* buffer, size_t length) {
        static const char* base64_chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        std::string ret;
        int i = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];

        while (length--) {
            char_array_3[i++] = *(buffer++);
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                for (i = 0; i < 4; i++)
                    ret += base64_chars[char_array_4[i]];
                i = 0;
            }
        }

        if (i) {
            for (int j = i; j < 3; j++)
                char_array_3[j] = '\0';

            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

            for (int j = 0; j <= i; j++)
                ret += base64_chars[char_array_4[j]];

            while (i++ < 3)
                ret += '=';
        }

        return ret;
    }

    static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

public:
    WePay(const std::string& mchId, const std::string& mchKey, 
          const std::string& baseUrl = "http://127.0.0.1:8088",
          const std::string& signType = "MD5", 
          const std::string& privateKeyPem = "")
        : mchId(mchId), mchKey(mchKey), baseUrl(baseUrl), 
          signType(signType), privateKeyPem(privateKeyPem) {
        // 移除末尾的 /
        if (!this->baseUrl.empty() && this->baseUrl.back() == '/') {
            this->baseUrl.pop_back();
        }
    }

    /**
     * 生成签名
     */
    std::string sign(std::map<std::string, std::string>& params) {
        if (signType == "RSA") {
            return signRsa(params);
        } else {
            return signMd5(params);
        }
    }

    /**
     * MD5 签名
     */
    std::string signMd5(std::map<std::string, std::string>& params) {
        std::vector<std::pair<std::string, std::string>> sortedParams;
        for (auto& p : params) {
            if (p.first != "sign" && p.first != "sign_type" && !p.second.empty()) {
                sortedParams.push_back(p);
            }
        }
        std::sort(sortedParams.begin(), sortedParams.end());

        std::stringstream ss;
        for (size_t i = 0; i < sortedParams.size(); i++) {
            if (i > 0) ss << "&";
            ss << sortedParams[i].first << "=" << sortedParams[i].second;
        }
        ss << mchKey;

        return md5(ss.str());
    }

    /**
     * RSA-SHA256 签名
     */
    std::string signRsa(std::map<std::string, std::string>& params) {
        if (privateKeyPem.empty()) {
            throw std::runtime_error("RSA 签名需要提供私钥");
        }

        std::vector<std::pair<std::string, std::string>> sortedParams;
        for (auto& p : params) {
            if (p.first != "sign" && p.first != "sign_type" && !p.second.empty()) {
                sortedParams.push_back(p);
            }
        }
        std::sort(sortedParams.begin(), sortedParams.end());

        std::stringstream ss;
        for (size_t i = 0; i < sortedParams.size(); i++) {
            if (i > 0) ss << "&";
            ss << sortedParams[i].first << "=" << sortedParams[i].second;
        }

        std::string signStr = ss.str();

        // 加载私钥
        BIO* bio = BIO_new_mem_buf(privateKeyPem.c_str(), privateKeyPem.length());
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        if (!pkey) {
            throw std::runtime_error("私钥格式错误");
        }

        // 签名
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        unsigned char signature[256];
        unsigned int sigLen = 0;

        EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
        EVP_DigestSignUpdate(ctx, (unsigned char*)signStr.c_str(), signStr.length());
        EVP_DigestSignFinal(ctx, signature, &sigLen);

        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);

        // Base64 编码
        return base64Encode(signature, sigLen);
    }

    /**
     * 统一下单
     */
    json createOrder(std::map<std::string, std::string> params) {
        params["mch_id"] = mchId;
        params["sign_type"] = signType;
        params["sign"] = sign(params);

        return post("/gateway/create", params);
    }

    /**
     * 查询订单
     */
    json queryOrder(const std::string& outTradeNo) {
        std::map<std::string, std::string> params;
        params["mch_id"] = mchId;
        params["out_trade_no"] = outTradeNo;
        params["sign_type"] = signType;
        params["sign"] = sign(params);

        return post("/gateway/query", params);
    }

    /**
     * 申请退款
     */
    json refund(const std::string& tradeNo, const std::string& refundAmount) {
        std::map<std::string, std::string> params;
        params["mch_id"] = mchId;
        params["trade_no"] = tradeNo;
        params["refund_amount"] = refundAmount;
        params["sign_type"] = signType;
        params["sign"] = sign(params);

        return post("/gateway/refund", params);
    }

    /**
     * 验证异步通知签名
     */
    bool verifyNotify(std::map<std::string, std::string>& params, const std::string& sign) {
        std::string calcSign = this->sign(params);
        return calcSign == sign;
    }

private:
    /**
     * POST 请求
     */
    json post(const std::string& path, const std::map<std::string, std::string>& params) {
        std::string url = baseUrl + path;
        json jsonData = json::object();
        for (const auto& p : params) {
            jsonData[p.first] = p.second;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("CURL 初始化失败");
        }

        std::string readBuffer;
        std::string jsonStr = jsonData.dump();

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            throw std::runtime_error(std::string("请求失败: ") + curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return json::parse(readBuffer);
    }
};
