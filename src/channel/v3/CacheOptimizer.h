#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <sw/redis++/redis++.h>
#include <nlohmann/json.hpp>

namespace wepay {
namespace v3 {

/**
 * 布隆过滤器
 * 用于快速判断设备ID/订单ID是否可能存在，防止缓存穿透
 */
class BloomFilter {
public:
    explicit BloomFilter(std::shared_ptr<sw::redis::Redis> redis,
                        const std::string& name,
                        int64_t expectedElements = 1000000,
                        double falsePositiveRate = 0.01);
    ~BloomFilter() = default;

    // 添加元素
    bool add(const std::string& element);

    // 检查元素是否可能存在
    bool mightContain(const std::string& element);

    // 批量添加
    void addBatch(const std::vector<std::string>& elements);

    // 清空过滤器
    void clear();

    // 获取统计信息
    struct Stats {
        int64_t expectedElements;
        int64_t numHashFunctions;
        int64_t numBits;
        double falsePositiveRate;
    };
    Stats getStats() const;

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    std::string name_;
    int64_t expectedElements_;
    double falsePositiveRate_;
    int64_t numBits_;
    int64_t numHashFunctions_;

    std::string redisKey_;

    // 计算最优位数组大小
    static int64_t calculateOptimalNumBits(int64_t n, double p);

    // 计算最优哈希函数数量
    static int64_t calculateOptimalNumHashFunctions(int64_t m, int64_t n);

    // 生成多个哈希值
    std::vector<int64_t> getHashValues(const std::string& element) const;

    // MurmurHash3
    static uint64_t murmurHash3(const std::string& key, uint32_t seed);
};

/**
 * 缓存预热管理器
 * 系统启动时预加载热点数据到Redis
 */
class CacheWarmer {
public:
    explicit CacheWarmer(std::shared_ptr<sw::redis::Redis> redis);
    ~CacheWarmer() = default;

    // 预热所有缓存
    bool warmupAll();

    // 预热IP白名单
    bool warmupIpWhitelist();

    // 预热商户配置
    bool warmupMerchantConfig();

    // 预热在线设备列表
    bool warmupOnlineDevices();

    // 预热系统配置
    bool warmupSystemConfig();

    // 预热布隆过滤器（设备ID）
    bool warmupDeviceBloomFilter(std::shared_ptr<BloomFilter> bloomFilter);

    // 预热布隆过滤器（订单ID）
    bool warmupOrderBloomFilter(std::shared_ptr<BloomFilter> bloomFilter);

    // 获取预热进度
    struct WarmupProgress {
        int totalTasks;
        int completedTasks;
        int failedTasks;
        std::vector<std::string> errors;
    };
    WarmupProgress getProgress() const;

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    WarmupProgress progress_;

    void recordSuccess();
    void recordFailure(const std::string& error);
};

} // namespace v3
} // namespace wepay
