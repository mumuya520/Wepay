// WePay-Cpp — 缓存抽象层
// 默认使用内存 LRU；若编译时定义 WEPAY_HAS_REDIS 且配置 cache.type="redis"，则走 Redis
// 使用方式:
//   CacheService::instance().set("key", "value", 300);  // 300秒 TTL
//   auto v = CacheService::instance().get("key");
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <mutex> // 互斥锁
#include <unordered_map> // 哈希表
#include <chrono> // 时间库
#include <memory> // 智能指针
#include <json/json.h> // JSON 库

// Redis 支持可选（通过 hiredis）
#ifdef WEPAY_HAS_REDIS
#  include <hiredis/hiredis.h>
#endif

// 缓存服务类
// 提供统一的缓存抽象层，支持内存 LRU 和 Redis 两种后端
// 默认使用内存 LRU；若编译时定义 WEPAY_HAS_REDIS 且配置 cache.type="redis"，则走 Redis
class CacheService {
public:
    // 缓存条目结构体
    struct Entry {
        // 缓存值
        std::string value;
        // 过期时间
        std::chrono::steady_clock::time_point expires;
    };

    // 获取缓存服务单例
    // 返回：全局唯一的 CacheService 实例
    static CacheService &instance() {
        // 使用静态变量实现单例模式（线程安全）
        static CacheService cs;
        // 返回实例引用
        return cs;
    }

    // 从 config.json 加载缓存配置
    // 配置格式：
    // {
    //   "cache": {
    //     "enabled": true,
    //     "type": "memory" 或 "redis",
    //     "redis_host": "127.0.0.1",
    //     "redis_port": 6379,
    //     "redis_password": ""
    //   }
    // }
    // 参数 cfg：JSON 配置对象
    void configure(const Json::Value &cfg) {
        // 读取是否启用缓存（默认启用）
        enabled_ = cfg.get("enabled", true).asBool();
        // 读取缓存类型（默认内存模式）
        type_    = cfg.get("type", "memory").asString();
// 如果编译了 Redis 支持
#ifdef WEPAY_HAS_REDIS
        // 如果配置为 Redis 模式
        if (type_ == "redis") {
            // 读取 Redis 主机地址
            std::string host = cfg.get("redis_host", "127.0.0.1").asString();
            // 读取 Redis 端口
            int port         = cfg.get("redis_port", 6379).asInt();
            // 读取 Redis 密码
            std::string pwd  = cfg.get("redis_password", "").asString();
            // 设置连接超时为 2 秒
            struct timeval tv = {2, 0};
            // 连接 Redis
            redis_ = redisConnectWithTimeout(host.c_str(), port, tv);
            // 检查连接是否成功
            if (!redis_ || redis_->err) {
                // 连接失败，记录错误并回退到内存模式
                std::cerr << "[CacheService] Redis 连接失败，回退内存模式" << std::endl;
                // 释放 Redis 连接
                if (redis_) {
                    redisFree(redis_);
                    redis_ = nullptr;
                }
                // 切换到内存模式
                type_ = "memory";
            } else if (!pwd.empty()) {
                // 如果设置了密码，执行 AUTH 命令
                auto *r = (redisReply*)redisCommand(redis_, "AUTH %s", pwd.c_str());
                // 释放 Redis 响应
                if (r)
                    freeReplyObject(r);
            }
        }
// 如果未编译 Redis 支持
#else
        // 如果配置为 Redis 模式但未编译支持
        if (type_ == "redis") {
            // 记录警告信息
            std::cerr << "[CacheService] 未编译 WEPAY_HAS_REDIS，Redis 配置被忽略，使用内存模式" << std::endl;
            // 强制使用内存模式
            type_ = "memory";
        }
#endif
        // 记录缓存服务配置信息
        std::cout << "[CacheService] 模式=" << type_ << " enabled=" << enabled_ << std::endl;
    }

    // 设置缓存值
    // 参数 key：缓存键
    // 参数 val：缓存值
    // 参数 ttlSec：生存时间（秒，默认 300 秒）
    void set(const std::string &key, const std::string &val, int ttlSec = 300) {
        // 如果缓存未启用，直接返回
        if (!enabled_)
            return;
// 如果编译了 Redis 支持
#ifdef WEPAY_HAS_REDIS
        // 如果使用 Redis 后端且连接成功
        if (type_ == "redis" && redis_) {
            // 执行 SETEX 命令（设置值并指定过期时间）
            auto *r = (redisReply*)redisCommand(redis_, "SETEX %s %d %s",
                key.c_str(), ttlSec, val.c_str());
            // 释放 Redis 响应
            if (r)
                freeReplyObject(r);
            // 返回
            return;
        }
#endif
        // 使用内存模式
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 创建缓存条目
        Entry e;
        // 设置缓存值
        e.value = val;
        // 计算过期时间（当前时间 + TTL）
        e.expires = std::chrono::steady_clock::now() + std::chrono::seconds(ttlSec);
        // 存储到内存缓存
        memStore_[key] = std::move(e);
    }

    // 获取缓存值
    // 参数 key：缓存键
    // 返回：缓存值（未命中或已过期返回空字符串）
    std::string get(const std::string &key) {
        // 如果缓存未启用，返回空字符串
        if (!enabled_)
            return "";
// 如果编译了 Redis 支持
#ifdef WEPAY_HAS_REDIS
        // 如果使用 Redis 后端且连接成功
        if (type_ == "redis" && redis_) {
            // 执行 GET 命令
            auto *r = (redisReply*)redisCommand(redis_, "GET %s", key.c_str());
            // 缓存值
            std::string val;
            // 如果获取到响应
            if (r) {
                // 检查响应类型是否为字符串
                if (r->type == REDIS_REPLY_STRING)
                    // 复制响应值
                    val.assign(r->str, r->len);
                // 释放 Redis 响应
                freeReplyObject(r);
            }
            // 返回缓存值
            return val;
        }
#endif
        // 使用内存模式
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 在内存缓存中查找键
        auto it = memStore_.find(key);
        // 如果键不存在，返回空字符串
        if (it == memStore_.end())
            return "";
        // 检查缓存是否已过期
        if (std::chrono::steady_clock::now() > it->second.expires) {
            // 如果已过期，删除该条目
            memStore_.erase(it);
            // 返回空字符串
            return "";
        }
        // 返回缓存值
        return it->second.value;
    }

    // 删除缓存值
    // 参数 key：缓存键
    void del(const std::string &key) {
        // 如果缓存未启用，直接返回
        if (!enabled_)
            return;
// 如果编译了 Redis 支持
#ifdef WEPAY_HAS_REDIS
        // 如果使用 Redis 后端且连接成功
        if (type_ == "redis" && redis_) {
            // 执行 DEL 命令
            auto *r = (redisReply*)redisCommand(redis_, "DEL %s", key.c_str());
            // 释放 Redis 响应
            if (r)
                freeReplyObject(r);
            // 返回
            return;
        }
#endif
        // 使用内存模式
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 从内存缓存中删除键
        memStore_.erase(key);
    }

    // 清理过期的缓存项
    // 注意：仅在内存模式下有效，Redis 自动处理过期
    void cleanup() {
        // 如果不是内存模式，直接返回
        if (type_ != "memory")
            return;
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 获取当前时间
        auto now = std::chrono::steady_clock::now();
        // 遍历内存缓存
        for (auto it = memStore_.begin(); it != memStore_.end();) {
            // 检查条目是否已过期
            if (now > it->second.expires)
                // 如果已过期，删除该条目
                it = memStore_.erase(it);
            else
                // 否则移动到下一条目
                ++it;
        }
    }

    // 获取缓存类型
    // 返回：缓存类型字符串（"memory" 或 "redis"）
    const std::string &type() const {
        return type_;
    }

    // 检查缓存是否启用
    // 返回：true 表示缓存已启用，false 表示缓存已禁用
    bool enabled() const {
        return enabled_;
    }

// 私有区域
private:
    // 构造函数（私有，禁止直接创建实例）
    CacheService() = default;

    // 析构函数
    ~CacheService() {
// 如果编译了 Redis 支持
#ifdef WEPAY_HAS_REDIS
        // 如果 Redis 连接存在，释放连接
        if (redis_)
            redisFree(redis_);
#endif
    }

    // 缓存是否启用
    bool enabled_ = true;
    // 缓存类型（"memory" 或 "redis"）
    std::string type_ = "memory";
    // 内存缓存的互斥锁
    std::mutex mutex_;
    // 内存缓存存储（键-值对）
    std::unordered_map<std::string, Entry> memStore_;
// 如果编译了 Redis 支持
#ifdef WEPAY_HAS_REDIS
    // Redis 连接上下文
    redisContext *redis_ = nullptr;
#endif
// 类定义结束
};
