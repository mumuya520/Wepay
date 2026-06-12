#include "SecurityValidator.h"
#include "WepayV3Config.h"
#include "../../common/PayDb.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <redis++/redis++.h>

namespace wepay {
namespace v3 {

SecurityValidator::SecurityValidator(const std::string& hmacSecret, const std::string& rsaPublicKey)
    : hmacSecret_(hmacSecret), rsaPublicKey_(rsaPublicKey) {
    if (!rsaPublicKey_.empty()) {
        loadRsaPublicKey(rsaPublicKey_);
    }
}

SecurityValidator::ValidationResult SecurityValidator::validateRequest(
    const std::string& deviceId,
    const std::string& timestamp,
    const std::string& nonce,
    const std::string& sign,
    const std::string& clientIp,
    const std::map<std::string, std::string>& extraParams) {

    ValidationResult result;
    result.deviceId = deviceId;
    result.timestamp = timestamp;
    result.nonce = nonce;

    // 1. 验证必填参数
    if (deviceId.empty() || timestamp.empty() || nonce.empty() || sign.empty()) {
        result.errorMsg = "Missing required parameters";
        return result;
    }

    // 2. 验证时间戳（防重放）
    if (!validateTimestamp(timestamp)) {
        result.errorMsg = "Timestamp expired or invalid";
        return result;
    }

    // 3. 验证Nonce（防重复）
    if (!validateNonce(nonce, deviceId)) {
        result.errorMsg = "Nonce already used";
        return result;
    }

    // 4. 验证设备ID
    if (!validateDeviceId(deviceId)) {
        result.errorMsg = "Invalid device ID";
        return result;
    }

    // 5. 构建签名参数
    std::map<std::string, std::string> signParams;
    signParams["deviceId"] = deviceId;
    signParams["timestamp"] = timestamp;
    signParams["nonce"] = nonce;

    // 添加额外参数
    for (const auto& [key, value] : extraParams) {
        signParams[key] = value;
    }

    // 6. 验证HMAC签名
    std::string expectedSign = generateHmacSign(signParams);
    if (sign != expectedSign) {
        std::string dbgStr = buildSignString(signParams);
        LOG_WARN << "[validateRequest] sign mismatch deviceId=" << deviceId
                 << " signStr=" << dbgStr.substr(0, 120)
                 << " expected=" << expectedSign.substr(0, 8)
                 << " got=" << sign.substr(0, 8);
        result.errorMsg = "Invalid signature";
        return result;
    }

    result.success = true;
    return result;
}

bool SecurityValidator::validateTimestamp(const std::string& timestamp, int windowSeconds) {
    try {
        int64_t ts = std::stoll(timestamp);
        int64_t now = std::time(nullptr);

        // 检查时间戳是否在有效窗口内
        if (std::abs(now - ts) > windowSeconds) {
            return false;
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool SecurityValidator::validateNonce(const std::string& nonce, const std::string& deviceId) {
    try {
        // 使用Redis SET结构存储nonce，TTL为时间戳窗口时长
        auto redis = sw::redis::Redis("tcp://127.0.0.1:6379");

        std::string key = "wepay:v3:nonce:" + deviceId + ":" + nonce;

        // SETNX：如果key不存在则设置，返回true；存在则返回false
        bool isNew = redis.setnx(key, "1");

        if (isNew) {
            // 设置过期时间（300秒）
            redis.expire(key, 300);
            return true;
        }

        return false; // nonce已被使用
    } catch (...) {
        // Redis 不可用时放行（降级：跳过防重放，但不拒绝合法请求）
        LOG_WARN << "[SecurityValidator] Redis unavailable, skipping nonce check for device=" << deviceId;
        return true;
    }
}

bool SecurityValidator::validateIpWhitelist(const std::string& clientIp,
                                           const std::vector<std::string>& whitelist) {
    if (whitelist.empty()) {
        return true; // 白名单为空，不限制
    }

    return std::find(whitelist.begin(), whitelist.end(), clientIp) != whitelist.end();
}

bool SecurityValidator::validateDeviceId(const std::string& deviceId) {
    // 验证设备ID格式（字母数字下划线，长度8-64）
    if (deviceId.length() < 8 || deviceId.length() > 64) {
        return false;
    }

    for (char c : deviceId) {
        if (!std::isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }

    return true;
}

std::string SecurityValidator::generateHmacSign(const std::map<std::string, std::string>& params) {
    // 构建待签名字符串
    std::string signString = buildSignString(params);

    // HMAC-SHA256签名
    unsigned char hash[SHA256_DIGEST_LENGTH];
    HMAC(EVP_sha256(),
         hmacSecret_.c_str(), hmacSecret_.length(),
         (unsigned char*)signString.c_str(), signString.length(),
         hash, nullptr);

    // 转换为十六进制字符串
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    return oss.str();
}

std::string SecurityValidator::buildSignString(const std::map<std::string, std::string>& params) {
    // 按key排序
    std::vector<std::pair<std::string, std::string>> sortedParams(params.begin(), params.end());
    std::sort(sortedParams.begin(), sortedParams.end());

    // 拼接：key1=value1&key2=value2&key=secret
    // amount/price 等金额字段强制两位小数，避免 0.04 → 0.040000000000000001
    std::ostringstream oss;
    for (size_t i = 0; i < sortedParams.size(); i++) {
        if (i > 0) oss << "&";
        const std::string& k = sortedParams[i].first;
        std::string v = sortedParams[i].second;
        // 金额类字段格式化
        if ((k == "amount" || k == "price") && !v.empty()) {
            try {
                double d = std::stod(v);
                std::ostringstream fmtd;
                fmtd << std::fixed << std::setprecision(2) << d;
                v = fmtd.str();
            } catch (...) {}
        }
        oss << k << "=" << v;
    }
    oss << "&key=" << hmacSecret_;

    return oss.str();
}

bool SecurityValidator::loadRsaPublicKey(const std::string& publicKey) {
    BIO* bio = BIO_new_mem_buf(publicKey.c_str(), -1);
    if (!bio) return false;

    rsaKey_ = PEM_read_bio_RSA_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    return rsaKey_ != nullptr;
}

bool SecurityValidator::verifyRsaSign(const std::string& data, const std::string& sign) {
    if (!rsaKey_) return false;

    // RSA签名验证实现
    // ...
    return true;
}

std::string SecurityValidator::resolveHmacSecret(const std::string& deviceId) {
    std::string secret;
    try {
        auto& db = PayDb::instance();
        auto devRow = db.queryOne(
            "SELECT mch_id FROM monitor_device_merchant WHERE device_id=? LIMIT 1", {deviceId});
        if (!devRow.empty()) {
            auto cfgRow = db.queryOne(
                "SELECT hmac_secret FROM v3_merchant_config WHERE merchant_id=? LIMIT 1",
                {devRow["mch_id"]});
            if (!cfgRow.empty()) secret = cfgRow["hmac_secret"];
        }
        if (secret.empty()) {
            auto row = db.queryOne(
                "SELECT default_params FROM plugin_store WHERE plugin_code='wepay_v3' LIMIT 1", {});
            if (!row.empty() && row.count("default_params")) {
                try {
                    auto j = nlohmann::json::parse(row.at("default_params"));
                    if (j.contains("key") && j["key"].is_string())
                        secret = j["key"].get<std::string>();
                } catch (...) {}
            }
        }
    } catch (...) {}
    if (secret.empty()) secret = WepayV3Config::getInstance().security.hmacSecret;
    return secret;
}

} // namespace v3
} // namespace wepay
