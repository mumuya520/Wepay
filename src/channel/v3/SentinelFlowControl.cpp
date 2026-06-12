#include "SentinelFlowControl.h"
#include <drogon/drogon.h>
#include <algorithm>

namespace wepay {
namespace v3 {

// CircuitBreaker实现
CircuitBreaker::CircuitBreaker(const CircuitBreakerRule& rule,
                               std::shared_ptr<sw::redis::Redis> redis)
    : rule_(rule), redis_(redis), state_(CircuitBreakerState::CLOSED), lastOpenTime_(0) {
    LOG_INFO << "CircuitBreaker created: resource=" << rule_.resourceName
             << " threshold=" << rule_.threshold
             << " timeWindow=" << rule_.timeWindow;
}

bool CircuitBreaker::tryAcquire() {
    auto currentState = state_.load();

    // 如果熔断器打开，检查是否可以进入半开状态
    if (currentState == CircuitBreakerState::OPEN) {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (now - lastOpenTime_ >= rule_.retryTimeoutMs) {
            state_ = CircuitBreakerState::HALF_OPEN;
            LOG_INFO << "CircuitBreaker entering HALF_OPEN state: " << rule_.resourceName;
            return true;
        }
        return false;
    }

    // 半开状态只允许部分请求通过
    if (currentState == CircuitBreakerState::HALF_OPEN) {
        std::lock_guard<std::mutex> lock(windowMutex_);
        // 半开状态下只允许1个请求通过进行测试
        if (requestWindow_.size() > 0) {
            return false;
        }
    }

    return true;
}

void CircuitBreaker::recordSuccess(int64_t rt) {
    std::lock_guard<std::mutex> lock(windowMutex_);

    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    requestWindow_.push_back({now, true, rt});
    cleanExpiredRecords();

    // 如果在半开状态，成功后转为关闭状态
    if (state_ == CircuitBreakerState::HALF_OPEN) {
        state_ = CircuitBreakerState::CLOSED;
        LOG_INFO << "CircuitBreaker recovered to CLOSED state: " << rule_.resourceName;
    }
}

void CircuitBreaker::recordFailure(int64_t rt) {
    std::lock_guard<std::mutex> lock(windowMutex_);

    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    requestWindow_.push_back({now, false, rt});
    cleanExpiredRecords();

    // 检查是否应该打开熔断器
    if (shouldOpen()) {
        state_ = CircuitBreakerState::OPEN;
        lastOpenTime_ = now;
        LOG_WARN << "CircuitBreaker opened: " << rule_.resourceName;
    }
}

bool CircuitBreaker::shouldOpen() {
    if (requestWindow_.size() < static_cast<size_t>(rule_.minRequestAmount)) {
        return false;
    }

    int64_t totalRequests = requestWindow_.size();
    int64_t failureRequests = 0;
    int64_t slowRequests = 0;

    for (const auto& record : requestWindow_) {
        if (!record.success) {
            failureRequests++;
        }
        if (record.rt > rule_.maxSlowRequestRt) {
            slowRequests++;
        }
    }

    double errorRatio = static_cast<double>(failureRequests) / totalRequests;
    double slowRatio = static_cast<double>(slowRequests) / totalRequests;

    // 检查异常比例
    if (errorRatio >= rule_.errorRatioThreshold) {
        LOG_WARN << "Error ratio exceeded: " << errorRatio << " >= " << rule_.errorRatioThreshold;
        return true;
    }

    // 检查慢调用比例
    if (slowRatio >= rule_.slowRatioThreshold) {
        LOG_WARN << "Slow ratio exceeded: " << slowRatio << " >= " << rule_.slowRatioThreshold;
        return true;
    }

    return false;
}

void CircuitBreaker::cleanExpiredRecords() {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t windowMs = rule_.timeWindow * 1000;

    while (!requestWindow_.empty() &&
           now - requestWindow_.front().timestamp > windowMs) {
        requestWindow_.pop_front();
    }
}

CircuitBreaker::Stats CircuitBreaker::getStats() const {
    std::lock_guard<std::mutex> lock(windowMutex_);

    Stats stats;
    stats.state = state_;
    stats.totalRequests = requestWindow_.size();
    stats.successRequests = 0;
    stats.failureRequests = 0;
    stats.slowRequests = 0;

    for (const auto& record : requestWindow_) {
        if (record.success) {
            stats.successRequests++;
        } else {
            stats.failureRequests++;
        }
        if (record.rt > rule_.maxSlowRequestRt) {
            stats.slowRequests++;
        }
    }

    if (stats.totalRequests > 0) {
        stats.errorRatio = static_cast<double>(stats.failureRequests) / stats.totalRequests;
        stats.slowRatio = static_cast<double>(stats.slowRequests) / stats.totalRequests;
    } else {
        stats.errorRatio = 0.0;
        stats.slowRatio = 0.0;
    }

    return stats;
}

// RateLimiter实现
RateLimiter::RateLimiter(const RateLimitRule& rule,
                         std::shared_ptr<sw::redis::Redis> redis)
    : rule_(rule), redis_(redis), tokens_(rule.threshold), lastRefillTime_(0) {

    if (rule_.warmUp) {
        warmUpStartTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    } else {
        warmUpStartTime_ = 0;
    }

    lastRefillTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    LOG_INFO << "RateLimiter created: resource=" << rule_.resourceName
             << " threshold=" << rule_.threshold
             << " warmUp=" << rule_.warmUp;
}

bool RateLimiter::tryAcquire(int permits) {
    std::lock_guard<std::mutex> lock(tokenMutex_);

    refillTokens();

    double currentThreshold = rule_.threshold;
    if (rule_.warmUp) {
        currentThreshold = getWarmUpThreshold();
    }

    bool passed = (tokens_ >= permits);
    if (passed) {
        tokens_ -= permits;
    }

    // 写入 Redis 统计（不阻塞主流程，失败仅记录日志）
    if (redis_) {
        try {
            std::string key = "sentinel:ratelimit:" + rule_.resourceName + ":stats";
            redis_->hincrby(key, "total", 1);
            if (passed) redis_->hincrby(key, "passed", 1);
            else        redis_->hincrby(key, "blocked", 1);
            redis_->expire(key, 3600);  // 统计 key 有效期 1 小时
        } catch (const std::exception& e) {
            LOG_WARN << "[Sentinel] Redis stats write failed: " << e.what();
        }
    }

    return passed;
}

void RateLimiter::refillTokens() {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t timePassed = now - lastRefillTime_;
    if (timePassed <= 0) {
        return;
    }

    // 计算应该添加的令牌数
    double tokensToAdd = (static_cast<double>(rule_.threshold) / 1000.0) * timePassed;

    tokens_ = std::min(tokens_.load() + tokensToAdd, static_cast<double>(rule_.threshold));
    lastRefillTime_ = now;
}

double RateLimiter::getWarmUpThreshold() const {
    if (!rule_.warmUp || warmUpStartTime_ == 0) {
        return rule_.threshold;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t warmUpElapsed = now - warmUpStartTime_;
    int64_t warmUpPeriodMs = rule_.warmUpPeriodSec * 1000;

    if (warmUpElapsed >= warmUpPeriodMs) {
        return rule_.threshold;
    }

    // 线性预热：从threshold/3逐渐增加到threshold
    double minThreshold = rule_.threshold / 3.0;
    double progress = static_cast<double>(warmUpElapsed) / warmUpPeriodMs;
    return minThreshold + (rule_.threshold - minThreshold) * progress;
}

RateLimiter::Stats RateLimiter::getStats() const {
    Stats stats;
    stats.totalRequests = 0;
    stats.passedRequests = 0;
    stats.blockedRequests = 0;
    stats.passRate = 0.0;

    if (!redis_) return stats;

    try {
        std::string key = "sentinel:ratelimit:" + rule_.resourceName + ":stats";
        std::unordered_map<std::string, std::string> hash;
        redis_->hgetall(key, std::inserter(hash, hash.begin()));
        if (!hash.empty()) {
            if (hash.count("total"))  stats.totalRequests  = std::stoll(hash.at("total"));
            if (hash.count("passed")) stats.passedRequests = std::stoll(hash.at("passed"));
            if (hash.count("blocked")) stats.blockedRequests = std::stoll(hash.at("blocked"));
            long long total = stats.totalRequests;
            if (total > 0) {
                stats.passRate = static_cast<double>(stats.passedRequests) / total;
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN << "[Sentinel] Redis stats read failed: " << e.what();
    }
    return stats;
}

// SentinelFlowManager实现
void SentinelFlowManager::init(std::shared_ptr<sw::redis::Redis> redis) {
    redis_ = redis;
    LOG_INFO << "SentinelFlowManager initialized";
}

void SentinelFlowManager::registerCircuitBreakerRule(const CircuitBreakerRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto breaker = std::make_shared<CircuitBreaker>(rule, redis_);
    circuitBreakers_[rule.resourceName] = breaker;

    LOG_INFO << "CircuitBreaker rule registered: " << rule.resourceName;
}

void SentinelFlowManager::registerRateLimitRule(const RateLimitRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto limiter = std::make_shared<RateLimiter>(rule, redis_);
    rateLimiters_[rule.resourceName] = limiter;

    LOG_INFO << "RateLimit rule registered: " << rule.resourceName;
}

std::unique_ptr<SentinelFlowManager::Entry>
SentinelFlowManager::entry(const std::string& resourceName) {
    return std::make_unique<Entry>(resourceName, this);
}

std::shared_ptr<CircuitBreaker>
SentinelFlowManager::getCircuitBreaker(const std::string& resourceName) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = circuitBreakers_.find(resourceName);
    if (it != circuitBreakers_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<RateLimiter>
SentinelFlowManager::getRateLimiter(const std::string& resourceName) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rateLimiters_.find(resourceName);
    if (it != rateLimiters_.end()) {
        return it->second;
    }
    return nullptr;
}

void SentinelFlowManager::recordEntry(const std::string& resourceName,
                                      bool success, int64_t rt) {
    auto breaker = getCircuitBreaker(resourceName);
    if (breaker) {
        if (success) {
            breaker->recordSuccess(rt);
        } else {
            breaker->recordFailure(rt);
        }
    }
}

// Entry实现
SentinelFlowManager::Entry::Entry(const std::string& resourceName,
                                  SentinelFlowManager* manager)
    : resourceName_(resourceName), manager_(manager), blocked_(false) {

    startTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 检查限流
    auto limiter = manager_->getRateLimiter(resourceName_);
    if (limiter && !limiter->tryAcquire()) {
        blocked_ = true;
        LOG_WARN << "Request blocked by rate limiter: " << resourceName_;
        return;
    }

    // 检查熔断
    auto breaker = manager_->getCircuitBreaker(resourceName_);
    if (breaker && !breaker->tryAcquire()) {
        blocked_ = true;
        LOG_WARN << "Request blocked by circuit breaker: " << resourceName_;
        return;
    }
}

SentinelFlowManager::Entry::~Entry() {
    if (!blocked_) {
        int64_t endTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t rt = endTime - startTime_;

        // 默认记录为成功（如果需要记录失败，需要显式调用recordFailure）
        manager_->recordEntry(resourceName_, true, rt);
    }
}

void SentinelFlowManager::Entry::recordSuccess() {
    if (!blocked_) {
        int64_t endTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t rt = endTime - startTime_;
        manager_->recordEntry(resourceName_, true, rt);
    }
}

void SentinelFlowManager::Entry::recordFailure() {
    if (!blocked_) {
        int64_t endTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t rt = endTime - startTime_;
        manager_->recordEntry(resourceName_, false, rt);
    }
}

} // namespace v3
} // namespace wepay
