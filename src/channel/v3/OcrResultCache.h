#pragma once

#include <string>
#include <memory>
#include <optional>
#include <sw/redis++/redis++.h>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

namespace wepay {
namespace v3 {

/**
 * OCR识别结果缓存
 * 使用图片SHA256哈希作为键，避免重复识别相同截图
 */
class OcrResultCache {
public:
    struct OcrResult {
        std::string imageHash;
        std::string amount;
        std::string payType;
        std::string orderId;
        std::string transactionId;
        int64_t recognizeTime;
        double confidence;
        bool success;
        std::string errorMessage;
    };

    explicit OcrResultCache(std::shared_ptr<sw::redis::Redis> redis);
    ~OcrResultCache() = default;

    // 计算图片SHA256哈希
    static std::string calculateImageHash(const std::string& imagePath);
    static std::string calculateImageHashFromData(const unsigned char* data, size_t len);

    // 缓存OCR结果
    bool cacheResult(const std::string& imageHash, const OcrResult& result, int ttlSeconds = 86400);

    // 获取缓存的OCR结果
    std::optional<OcrResult> getCachedResult(const std::string& imageHash);

    // 检查是否已缓存
    bool isCached(const std::string& imageHash);

    // 删除缓存
    void invalidateCache(const std::string& imageHash);

    // 批量删除缓存（按前缀）
    void invalidateCacheByPrefix(const std::string& prefix);

    // 获取缓存统计
    struct CacheStats {
        int64_t totalHits;
        int64_t totalMisses;
        double hitRate;
    };
    CacheStats getStats();

private:
    std::shared_ptr<sw::redis::Redis> redis_;

    static constexpr const char* REDIS_KEY_PREFIX = "ocr:cache:";
    static constexpr const char* REDIS_STATS_HITS = "ocr:stats:hits";
    static constexpr const char* REDIS_STATS_MISSES = "ocr:stats:misses";

    std::string buildRedisKey(const std::string& imageHash) const;

    // 记录缓存命中/未命中
    void recordHit();
    void recordMiss();
};

} // namespace v3
} // namespace wepay
