#pragma once
#include <string>
#include <ctime>
#include <map>
#include <vector>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>

namespace wepay {
namespace v3 {

// V3安全校验器
class SecurityValidator {
public:
    struct ValidationResult {
        bool success = false;
        std::string errorMsg;
        std::string deviceId;
        std::string timestamp;
        std::string nonce;
    };

    SecurityValidator(const std::string& hmacSecret, const std::string& rsaPublicKey = "");

    // 验证请求签名
    ValidationResult validateRequest(
        const std::string& deviceId,
        const std::string& timestamp,
        const std::string& nonce,
        const std::string& sign,
        const std::string& clientIp,
        const std::map<std::string, std::string>& extraParams = {}
    );

    // 验证时间戳（防重放）
    bool validateTimestamp(const std::string& timestamp, int windowSeconds = 300);

    // 验证Nonce（防重放）
    bool validateNonce(const std::string& nonce, const std::string& deviceId);

    // 验证IP白名单
    bool validateIpWhitelist(const std::string& clientIp, const std::vector<std::string>& whitelist);

    // 验证设备ID
    bool validateDeviceId(const std::string& deviceId);

    // HMAC-SHA256签名
    std::string generateHmacSign(const std::map<std::string, std::string>& params);

    // RSA签名验证
    bool verifyRsaSign(const std::string& data, const std::string& sign);

    // 从 DB 动态解析设备对应的 hmac_secret
    // 优先: monitor_device→v3_merchant_config, 次: plugin_config, 末: WepayV3Config
    static std::string resolveHmacSecret(const std::string& deviceId);

private:
    std::string hmacSecret_;
    std::string rsaPublicKey_;
    RSA* rsaKey_ = nullptr;

    // 构建待签名字符串
    std::string buildSignString(const std::map<std::string, std::string>& params);

    // 加载RSA公钥
    bool loadRsaPublicKey(const std::string& publicKey);
};

} // namespace v3
} // namespace wepay
