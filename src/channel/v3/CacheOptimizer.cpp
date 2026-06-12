#include "CacheOptimizer.h"
#include <drogon/drogon.h>
#include <cmath>
// 注：原始实现依赖 libpqxx 直连 PostgreSQL 预热缓存，
// 主工程使用 PayDb (SQLite/libpq)，为避免重复引入 libpqxx，
// 这里的 warmup* 均改为 stub；运行时缓存/布隆过滤器会随实际请求逐步填充。

namespace wepay {
namespace v3 {

// BloomFilter实现
BloomFilter::BloomFilter(std::shared_ptr<sw::redis::Redis> redis,
                         const std::string& name,
                         int64_t expectedElements,
                         double falsePositiveRate)
    : redis_(redis),
      name_(name),
      expectedElements_(expectedElements),
      falsePositiveRate_(falsePositiveRate) {

    numBits_ = calculateOptimalNumBits(expectedElements_, falsePositiveRate_);
    numHashFunctions_ = calculateOptimalNumHashFunctions(numBits_, expectedElements_);

    redisKey_ = "bloom:" + name_;

    LOG_INFO << "BloomFilter initialized: name=" << name_
             << " expectedElements=" << expectedElements_
             << " numBits=" << numBits_
             << " numHashFunctions=" << numHashFunctions_
             << " falsePositiveRate=" << falsePositiveRate_;
}

int64_t BloomFilter::calculateOptimalNumBits(int64_t n, double p) {
    // m = -n * ln(p) / (ln(2)^2)
    return static_cast<int64_t>(std::ceil(-n * std::log(p) / (std::log(2) * std::log(2))));
}

int64_t BloomFilter::calculateOptimalNumHashFunctions(int64_t m, int64_t n) {
    // k = (m/n) * ln(2)
    return static_cast<int64_t>(std::ceil((static_cast<double>(m) / n) * std::log(2)));
}

uint64_t BloomFilter::murmurHash3(const std::string& key, uint32_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;

    uint64_t h = seed ^ (key.length() * m);

    const uint64_t* data = reinterpret_cast<const uint64_t*>(key.data());
    const uint64_t* end = data + (key.length() / 8);

    while (data != end) {
        uint64_t k = *data++;
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
    }

    const unsigned char* data2 = reinterpret_cast<const unsigned char*>(data);

    switch (key.length() & 7) {
        case 7: h ^= uint64_t(data2[6]) << 48;
        case 6: h ^= uint64_t(data2[5]) << 40;
        case 5: h ^= uint64_t(data2[4]) << 32;
        case 4: h ^= uint64_t(data2[3]) << 24;
        case 3: h ^= uint64_t(data2[2]) << 16;
        case 2: h ^= uint64_t(data2[1]) << 8;
        case 1: h ^= uint64_t(data2[0]);
                h *= m;
    }

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

std::vector<int64_t> BloomFilter::getHashValues(const std::string& element) const {
    std::vector<int64_t> hashes;
    hashes.reserve(numHashFunctions_);

    uint64_t hash1 = murmurHash3(element, 0);
    uint64_t hash2 = murmurHash3(element, hash1);

    for (int64_t i = 0; i < numHashFunctions_; i++) {
        uint64_t combinedHash = hash1 + i * hash2;
        hashes.push_back(combinedHash % numBits_);
    }

    return hashes;
}

bool BloomFilter::add(const std::string& element) {
    try {
        auto hashes = getHashValues(element);

        for (int64_t bitIndex : hashes) {
            // MSYS2 redis++ 1.3.15 Windows 包无 setbit()，改用通用 command 接口
            redis_->command<long long>("SETBIT", redisKey_, bitIndex, 1LL);
        }

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to add element to bloom filter: " << e.what();
        return false;
    }
}

bool BloomFilter::mightContain(const std::string& element) {
    try {
        auto hashes = getHashValues(element);

        for (int64_t bitIndex : hashes) {
            if (redis_->getbit(redisKey_, bitIndex) == 0) {
                return false;
            }
        }

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to check bloom filter: " << e.what();
        return false;
    }
}

void BloomFilter::addBatch(const std::vector<std::string>& elements) {
    for (const auto& element : elements) {
        add(element);
    }
}

void BloomFilter::clear() {
    try {
        redis_->del(redisKey_);
        LOG_INFO << "BloomFilter cleared: " << name_;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to clear bloom filter: " << e.what();
    }
}

BloomFilter::Stats BloomFilter::getStats() const {
    return Stats{
        expectedElements_,
        numHashFunctions_,
        numBits_,
        falsePositiveRate_
    };
}

// CacheWarmer实现
CacheWarmer::CacheWarmer(std::shared_ptr<sw::redis::Redis> redis)
    : redis_(redis) {
    progress_.totalTasks = 0;
    progress_.completedTasks = 0;
    progress_.failedTasks = 0;
}

bool CacheWarmer::warmupAll() {
    LOG_INFO << "Starting cache warmup...";

    progress_.totalTasks = 4;
    progress_.completedTasks = 0;
    progress_.failedTasks = 0;
    progress_.errors.clear();

    bool success = true;

    if (!warmupIpWhitelist()) success = false;
    if (!warmupMerchantConfig()) success = false;
    if (!warmupOnlineDevices()) success = false;
    if (!warmupSystemConfig()) success = false;

    LOG_INFO << "Cache warmup completed: success=" << success
             << " completed=" << progress_.completedTasks
             << " failed=" << progress_.failedTasks;

    return success;
}

bool CacheWarmer::warmupIpWhitelist() {
    LOG_INFO << "[V3] IP whitelist warmup skipped (no pqxx, will populate at runtime)";
    recordSuccess();
    return true;
}

bool CacheWarmer::warmupMerchantConfig() {
    LOG_INFO << "[V3] Merchant config warmup skipped (no pqxx)";
    recordSuccess();
    return true;
}

bool CacheWarmer::warmupOnlineDevices() {
    LOG_INFO << "[V3] Online devices warmup skipped (no pqxx)";
    recordSuccess();
    return true;
}

bool CacheWarmer::warmupSystemConfig() {
    LOG_INFO << "[V3] System config warmup skipped (no pqxx)";
    recordSuccess();
    return true;
}

bool CacheWarmer::warmupDeviceBloomFilter(std::shared_ptr<BloomFilter> /*bloomFilter*/) {
    LOG_INFO << "[V3] Device bloom filter warmup skipped (no pqxx)";
    return true;
}

bool CacheWarmer::warmupOrderBloomFilter(std::shared_ptr<BloomFilter> /*bloomFilter*/) {
    LOG_INFO << "[V3] Order bloom filter warmup skipped (no pqxx)";
    return true;
}

void CacheWarmer::recordSuccess() {
    progress_.completedTasks++;
}

void CacheWarmer::recordFailure(const std::string& error) {
    progress_.failedTasks++;
    progress_.errors.push_back(error);
    LOG_ERROR << error;
}

CacheWarmer::WarmupProgress CacheWarmer::getProgress() const {
    return progress_;
}

} // namespace v3
} // namespace wepay
