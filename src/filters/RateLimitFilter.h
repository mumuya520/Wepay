// WePay-Cpp — IP 限频中间件
// 基于滑动窗口限制请求频率
// 在 config.json 的 custom_config 中配置:
//   "rate_limit": { "max_requests": 60, "window_seconds": 60, "enabled": true }
#pragma once // 防止头文件重复包含
#include <drogon/HttpMiddleware.h> // Drogon HTTP 中间件
#include <mutex> // 互斥锁
#include <unordered_map> // 哈希表
#include <deque> // 双端队列
#include <ctime> // 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果

// 限频过滤器类
class RateLimitFilter : public drogon::HttpMiddleware<RateLimitFilter> {
public:
    static constexpr bool isAutoCreation = false; // 禁用自动创建

    // 中间件调用方法
    void invoke(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                drogon::MiddlewareNextCallback &&nextCb, // 下一个中间件回调
                drogon::MiddlewareCallback &&mcb) override { // 中间件完成回调
        auto &custom = drogon::app().getCustomConfig(); // 获取自定义配置
        auto &rl = custom["rate_limit"]; // 获取限频配置
        bool enabled = rl.get("enabled", true).asBool(); // 检查是否启用
        if (!enabled) { nextCb(std::move(mcb)); return; } // 未启用则直接通过

        int maxReq    = rl.get("max_requests", 60).asInt(); // 最大请求数
        int windowSec = rl.get("window_seconds", 60).asInt(); // 时间窗口（秒）

        std::string clientIp = req->getPeerAddr().toIp(); // 获取客户端 IP
        long long now = std::time(nullptr); // 获取当前时间戳

        {
            std::lock_guard<std::mutex> lock(mutex_); // 加锁
            auto &timestamps = ipRequests_[clientIp]; // 获取该 IP 的请求时间戳
            while (!timestamps.empty() && timestamps.front() < now - windowSec) { // 移除过期时间戳
                timestamps.pop_front(); // 删除最早的时间戳
            }
            if ((int)timestamps.size() >= maxReq) { // 如果超过限制
                auto resp = drogon::HttpResponse::newHttpJsonResponse( // 创建 JSON 响应
                    AjaxResult::error(429, "请求过于频繁，请稍后再试")); // 429 错误
                resp->setStatusCode(drogon::k429TooManyRequests); // 设置状态码为 429
                resp->addHeader("Retry-After", std::to_string(windowSec)); // 添加重试延迟头
                resp->addHeader("X-RateLimit-Limit", std::to_string(maxReq)); // 添加限制头
                resp->addHeader("X-RateLimit-Remaining", "0"); // 剩余请求数为 0
                mcb(resp); // 返回响应
                return;
            }
            timestamps.push_back(now); // 添加当前时间戳
            if (++cleanupCounter_ > 1000) { // 每 1000 次请求清理一次
                cleanupCounter_ = 0; // 重置计数器
                cleanupStaleEntries(now - windowSec * 10); // 清理陈旧条目
            }
        }
        nextCb(std::move(mcb)); // 继续下一个中间件
    }

private:
    std::mutex mutex_; // 互斥锁
    std::unordered_map<std::string, std::deque<long long>> ipRequests_; // IP 请求时间戳缓存
    int cleanupCounter_ = 0; // 清理计数器

    // 清理陈旧条目方法
    void cleanupStaleEntries(long long cutoff) { // 截断时间戳
        for (auto it = ipRequests_.begin(); it != ipRequests_.end(); ) { // 遍历所有 IP
            if (it->second.empty() || it->second.back() < cutoff) it = ipRequests_.erase(it); // 删除陈旧条目
            else ++it; // 移动到下一个
        }
    }
};
