#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <deque>
#include <sw/redis++/redis++.h>

namespace wepay {
namespace v3 {

/**
 * 熔断器状态
 */
enum class CircuitBreakerState {
    CLOSED,      // 关闭状态，正常处理请求
    OPEN,        // 打开状态，拒绝所有请求
    HALF_OPEN    // 半开状态，允许部分请求通过以测试服务是否恢复
};

/**
 * 熔断规则
 */
struct CircuitBreakerRule {
    std::string resourceName;           // 资源名称
    int threshold;                      // 阈值
    int timeWindow;                     // 时间窗口（秒）
    int minRequestAmount;               // 最小请求数
    double slowRatioThreshold;          // 慢调用比例阈值
    int maxSlowRequestRt;               // 最大慢调用响应时间（毫秒）
    double errorRatioThreshold;         // 异常比例阈值
    int retryTimeoutMs;                 // 熔断后重试超时（毫秒）
};

/**
 * 限流规则
 */
struct RateLimitRule {
    std::string resourceName;           // 资源名称
    int threshold;                      // QPS阈值
    int timeWindow;                     // 时间窗口（秒）
    bool warmUp;                        // 是否启用预热
    int warmUpPeriodSec;                // 预热时长（秒）
};

/**
 * 熔断器实现
 */
class CircuitBreaker {
public:
    explicit CircuitBreaker(const CircuitBreakerRule& rule,
                           std::shared_ptr<sw::redis::Redis> redis);
    ~CircuitBreaker() = default;

    // 尝试获取许可
    bool tryAcquire();

    // 记录成功
    void recordSuccess(int64_t rt);

    // 记录失败
    void recordFailure(int64_t rt);

    // 获取当前状态
    CircuitBreakerState getState() const { return state_; }

    // 获取统计信息
    struct Stats {
        CircuitBreakerState state;
        int64_t totalRequests;
        int64_t successRequests;
        int64_t failureRequests;
        int64_t slowRequests;
        double errorRatio;
        double slowRatio;
    };
    Stats getStats() const;

private:
    CircuitBreakerRule rule_;
    std::shared_ptr<sw::redis::Redis> redis_;
    std::atomic<CircuitBreakerState> state_;
    std::atomic<int64_t> lastOpenTime_;

    // 滑动窗口统计
    struct RequestRecord {
        int64_t timestamp;
        bool success;
        int64_t rt;
    };
    std::deque<RequestRecord> requestWindow_;
    mutable std::mutex windowMutex_;

    // 检查是否应该打开熔断器
    bool shouldOpen();

    // 尝试从半开状态恢复
    void tryRecover();

    // 清理过期记录
    void cleanExpiredRecords();
};

/**
 * 限流器实现（令牌桶算法）
 */
class RateLimiter {
public:
    explicit RateLimiter(const RateLimitRule& rule,
                        std::shared_ptr<sw::redis::Redis> redis);
    ~RateLimiter() = default;

    // 尝试获取许可
    bool tryAcquire(int permits = 1);

    // 获取统计信息
    struct Stats {
        int64_t totalRequests;
        int64_t passedRequests;
        int64_t blockedRequests;
        double passRate;
    };
    Stats getStats() const;

private:
    RateLimitRule rule_;
    std::shared_ptr<sw::redis::Redis> redis_;

    // 令牌桶参数
    std::atomic<double> tokens_;
    std::atomic<int64_t> lastRefillTime_;
    mutable std::mutex tokenMutex_;

    // 预热相关
    int64_t warmUpStartTime_;
    double getWarmUpThreshold() const;

    // 填充令牌
    void refillTokens();
};

/**
 * Sentinel流量控制管理器
 */
class SentinelFlowManager {
public:
    static SentinelFlowManager& getInstance() {
        static SentinelFlowManager instance;
        return instance;
    }

    // 初始化
    void init(std::shared_ptr<sw::redis::Redis> redis);

    // 注册熔断规则
    void registerCircuitBreakerRule(const CircuitBreakerRule& rule);

    // 注册限流规则
    void registerRateLimitRule(const RateLimitRule& rule);

    // 进入资源
    class Entry {
    public:
        Entry(const std::string& resourceName, SentinelFlowManager* manager);
        ~Entry();

        bool isBlocked() const { return blocked_; }
        void recordSuccess();
        void recordFailure();

    private:
        std::string resourceName_;
        SentinelFlowManager* manager_;
        bool blocked_;
        int64_t startTime_;
    };

    std::unique_ptr<Entry> entry(const std::string& resourceName);

    // 获取熔断器
    std::shared_ptr<CircuitBreaker> getCircuitBreaker(const std::string& resourceName);

    // 获取限流器
    std::shared_ptr<RateLimiter> getRateLimiter(const std::string& resourceName);

private:
    SentinelFlowManager() = default;
    ~SentinelFlowManager() = default;

    std::shared_ptr<sw::redis::Redis> redis_;
    std::unordered_map<std::string, std::shared_ptr<CircuitBreaker>> circuitBreakers_;
    std::unordered_map<std::string, std::shared_ptr<RateLimiter>> rateLimiters_;
    mutable std::mutex mutex_;

    void recordEntry(const std::string& resourceName, bool success, int64_t rt);
};

// 便捷宏定义
#define SENTINEL_ENTRY(resource) \
    auto __sentinel_entry = wepay::v3::SentinelFlowManager::getInstance().entry(resource); \
    if (__sentinel_entry->isBlocked()) { \
        LOG_WARN << "Request blocked by Sentinel: " << resource; \
        return; \
    }

#define SENTINEL_ENTRY_WITH_RETURN(resource, returnValue) \
    auto __sentinel_entry = wepay::v3::SentinelFlowManager::getInstance().entry(resource); \
    if (__sentinel_entry->isBlocked()) { \
        LOG_WARN << "Request blocked by Sentinel: " << resource; \
        return returnValue; \
    }

} // namespace v3
} // namespace wepay
