// WePay-Cpp — 设备密钥登录挑战值缓存服务
// 内存缓存 + 自动过期 + 一次性使用保护
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <unordered_map> // 哈希表
#include <mutex> // 互斥锁
#include <chrono> // 时间库

class DeviceChallengeCache {
public:
    struct Challenge {
        std::string value;
        long long timestamp;
        bool used;  // 一次性使用标记
    };

    static DeviceChallengeCache& instance() {
        static DeviceChallengeCache inst;
        return inst;
    }

    // 存储挑战值
    void set(const std::string &publicKey, const std::string &challenge, long long timestamp) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[publicKey] = {challenge, timestamp, false};
    }

    // 获取并标记为已使用（一次性）
    Challenge getAndMarkUsed(const std::string &publicKey) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(publicKey);
        if (it == cache_.end()) {
            return {"", 0, true};  // 不存在
        }

        Challenge ch = it->second;

        // 检查是否已使用
        if (ch.used) {
            return {"", 0, true};  // 已使用
        }

        // 标记为已使用
        it->second.used = true;
        return ch;
    }

    // 删除挑战值
    void remove(const std::string &publicKey) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(publicKey);
    }

    // 清理过期挑战值（超过 5 分钟）
    void cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);
        long long now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        auto it = cache_.begin();
        while (it != cache_.end()) {
            if (now - it->second.timestamp > 300) {  // 5 分钟
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 获取缓存大小（用于监控）
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

private:
    DeviceChallengeCache() = default;
    std::unordered_map<std::string, Challenge> cache_;
    std::mutex mutex_;
};
