#include "OcrResultCache.h"
#include <drogon/drogon.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <openssl/evp.h>

namespace wepay {
namespace v3 {

OcrResultCache::OcrResultCache(std::shared_ptr<sw::redis::Redis> redis)
    : redis_(redis) {
    LOG_INFO << "OcrResultCache initialized";
}

std::string OcrResultCache::calculateImageHash(const std::string& imagePath) {
    std::ifstream file(imagePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open image file: " + imagePath);
    }

    // 读取文件内容
    std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(file), {});
    file.close();

    return calculateImageHashFromData(buffer.data(), buffer.size());
}

std::string OcrResultCache::calculateImageHashFromData(const unsigned char* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, nullptr);
    EVP_MD_CTX_free(ctx);

    // 转换为十六进制字符串
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    return oss.str();
}

std::string OcrResultCache::buildRedisKey(const std::string& imageHash) const {
    return std::string(REDIS_KEY_PREFIX) + imageHash;
}

bool OcrResultCache::cacheResult(const std::string& imageHash,
                                 const OcrResult& result,
                                 int ttlSeconds) {
    try {
        nlohmann::json j;
        j["imageHash"] = result.imageHash;
        j["amount"] = result.amount;
        j["payType"] = result.payType;
        j["orderId"] = result.orderId;
        j["transactionId"] = result.transactionId;
        j["recognizeTime"] = result.recognizeTime;
        j["confidence"] = result.confidence;
        j["success"] = result.success;
        j["errorMessage"] = result.errorMessage;

        std::string key = buildRedisKey(imageHash);
        redis_->setex(key, ttlSeconds, j.dump());

        LOG_DEBUG << "OCR result cached: imageHash=" << imageHash
                  << " ttl=" << ttlSeconds << "s";

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to cache OCR result: " << e.what();
        return false;
    }
}

std::optional<OcrResultCache::OcrResult>
OcrResultCache::getCachedResult(const std::string& imageHash) {
    try {
        std::string key = buildRedisKey(imageHash);
        auto value = redis_->get(key);

        if (!value) {
            recordMiss();
            return std::nullopt;
        }

        recordHit();

        nlohmann::json j = nlohmann::json::parse(*value);

        OcrResult result;
        result.imageHash = j["imageHash"].get<std::string>();
        result.amount = j["amount"].get<std::string>();
        result.payType = j["payType"].get<std::string>();
        result.orderId = j.value("orderId", "");
        result.transactionId = j.value("transactionId", "");
        result.recognizeTime = j["recognizeTime"].get<int64_t>();
        result.confidence = j["confidence"].get<double>();
        result.success = j["success"].get<bool>();
        result.errorMessage = j.value("errorMessage", "");

        LOG_DEBUG << "OCR result cache hit: imageHash=" << imageHash;

        return result;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get cached OCR result: " << e.what();
        recordMiss();
        return std::nullopt;
    }
}

bool OcrResultCache::isCached(const std::string& imageHash) {
    try {
        std::string key = buildRedisKey(imageHash);
        return redis_->exists(key) > 0;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to check cache: " << e.what();
        return false;
    }
}

void OcrResultCache::invalidateCache(const std::string& imageHash) {
    try {
        std::string key = buildRedisKey(imageHash);
        redis_->del(key);

        LOG_DEBUG << "OCR cache invalidated: imageHash=" << imageHash;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to invalidate cache: " << e.what();
    }
}

void OcrResultCache::invalidateCacheByPrefix(const std::string& prefix) {
    try {
        std::string pattern = std::string(REDIS_KEY_PREFIX) + prefix + "*";

        auto cursor = 0LL;
        std::vector<std::string> keys;

        do {
            cursor = redis_->scan(cursor, pattern, 100, std::back_inserter(keys));
        } while (cursor != 0);

        if (!keys.empty()) {
            redis_->del(keys.begin(), keys.end());
            LOG_INFO << "Invalidated " << keys.size() << " OCR cache entries with prefix: " << prefix;
        }

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to invalidate cache by prefix: " << e.what();
    }
}

void OcrResultCache::recordHit() {
    try {
        redis_->incr(REDIS_STATS_HITS);
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to record cache hit: " << e.what();
    }
}

void OcrResultCache::recordMiss() {
    try {
        redis_->incr(REDIS_STATS_MISSES);
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to record cache miss: " << e.what();
    }
}

OcrResultCache::CacheStats OcrResultCache::getStats() {
    CacheStats stats{0, 0, 0.0};

    try {
        auto hitsVal = redis_->get(REDIS_STATS_HITS);
        auto missesVal = redis_->get(REDIS_STATS_MISSES);

        if (hitsVal) {
            stats.totalHits = std::stoll(*hitsVal);
        }

        if (missesVal) {
            stats.totalMisses = std::stoll(*missesVal);
        }

        int64_t total = stats.totalHits + stats.totalMisses;
        if (total > 0) {
            stats.hitRate = static_cast<double>(stats.totalHits) / total * 100.0;
        }

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get cache stats: " << e.what();
    }

    return stats;
}

} // namespace v3
} // namespace wepay
