// WePay-Cpp — 登录尝试日志 + 防爆破锁定
//
// 配置 (config.json):
//   "security": {
//     "login_attempt": {
//       "enabled": true,
//       "max_fails": 5,        // 失败次数阈值
//       "window_minutes": 15,  // 时间窗
//       "lock_minutes": 30     // 锁定时长
//     }
//   }
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include <drogon/HttpRequest.h> // Drogon HTTP 请求
#include <drogon/HttpAppFramework.h> // Drogon 应用框架
#include "PayDb.h" // 数据库操作

// 登录尝试服务类
// 记录登录尝试日志，实现防爆破锁定机制
class LoginAttemptService {
public:
    // 用户类型枚举
    enum UserType {
        // 管理员
        ADMIN = 1,
        // 商户
        MERCHANT = 2,
        // 代理商
        AGENT = 3
    };

    // 登录防爆破配置结构体
    struct Config {
        // 是否启用登录防爆破
        bool enabled = true;
        // 失败次数阈值（超过此次数则锁定）
        int  maxFails = 5;
        // 时间窗（分钟，在此窗口内统计失败次数）
        int  windowMinutes = 15;
        // 锁定时长（分钟，锁定后多久自动解锁）
        int  lockMinutes = 30;
    };

    // 获取登录防爆破配置
    // 从 config.json 中读取配置，使用单例模式缓存
    // 返回：全局唯一的配置对象
    static const Config &cfg() {
        // 使用静态变量和 lambda 实现配置的单例加载
        static Config c = []() {
            // 创建默认配置
            Config x;
            // 尝试从 Drogon 应用配置中读取自定义配置
            try {
                // 获取 Drogon 应用的自定义配置
                auto &custom = drogon::app().getCustomConfig();
                // 检查是否存在 security.login_attempt 配置
                if (custom.isMember("security") &&
                    custom["security"].isMember("login_attempt")) {
                    // 获取登录防爆破配置
                    auto &la = custom["security"]["login_attempt"];
                    // 读取是否启用（默认 true）
                    x.enabled = la.get("enabled", true).asBool();
                    // 读取失败次数阈值（默认 5）
                    x.maxFails = la.get("max_fails", 5).asInt();
                    // 读取时间窗（默认 15 分钟）
                    x.windowMinutes = la.get("window_minutes", 15).asInt();
                    // 读取锁定时长（默认 30 分钟）
                    x.lockMinutes = la.get("lock_minutes", 30).asInt();
                }
            } catch (...) {
                // 配置读取失败，使用默认值
            }
            // 返回配置对象
            return x;
        }();
        // 返回缓存的配置
        return c;
    }

    // 检查用户是否处于锁定状态
    // 基于时间窗内的失败登录次数判断
    // 参数 username：用户名
    // 参数 ip：IP 地址（当前未使用，预留用于后续扩展）
    // 参数 userType：用户类型（默认管理员）
    // 返回：剩余锁定秒数（>0 表示被锁定，0 表示未锁定）
    static int isLocked(const std::string &username, const std::string &ip,
                        UserType userType = ADMIN) {
        // 如果防爆破功能未启用，直接返回 0（未锁定）
        if (!cfg().enabled)
            return 0;
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 计算时间窗的起始时间（当前时间 - 时间窗）
        long long windowStart = now - cfg().windowMinutes * 60LL;

        // 获取数据库实例
        auto &db = PayDb::instance();
        // 查询时间窗内该用户的失败登录次数和最后失败时间
        auto row = db.queryOne(
            // SQL 语句：统计失败次数和最后失败时间
            "SELECT COUNT(*) AS c, MAX(created_at) AS last "
            "FROM login_attempt WHERE username=? AND user_type=? AND success=0 "
            "AND created_at>=?",
            // 参数：用户名、用户类型、时间窗起始时间
            {username, std::to_string((int)userType), std::to_string(windowStart)});
        // 如果查询结果为空，表示没有失败记录，返回 0（未锁定）
        if (row.empty())
            return 0;
        // 初始化失败次数和最后失败时间
        int fails = 0;
        long long last = 0;
        // 尝试解析失败次数
        try {
            fails = std::stoi(row["c"]);
        } catch (...) {
        }
        // 尝试解析最后失败时间
        try {
            last = std::stoll(row["last"]);
        } catch (...) {
        }

        // 检查失败次数是否超过阈值
        if (fails >= cfg().maxFails) {
            // 计算解锁时间（最后失败时间 + 锁定时长）
            long long unlockAt = last + cfg().lockMinutes * 60LL;
            // 如果解锁时间还未到达，返回剩余锁定秒数
            if (unlockAt > now)
                return (int)(unlockAt - now);
        }
        // 未被锁定，返回 0
        return 0;
    }

    // 记录一次登录尝试
    // 参数 username：用户名
    // 参数 ip：登录者 IP 地址
    // 参数 userAgent：用户代理字符串（浏览器信息）
    // 参数 success：登录是否成功
    // 参数 failReason：失败原因（成功时为空）
    // 参数 userType：用户类型（默认管理员）
    static void record(const std::string &username, const std::string &ip,
                       const std::string &userAgent, bool success,
                       const std::string &failReason = "",
                       UserType userType = ADMIN) {
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 插入登录尝试记录到数据库
        PayDb::instance().exec(
            // SQL 语句：插入登录尝试记录
            "INSERT INTO login_attempt(username,ip,user_agent,success,fail_reason,"
            "user_type,created_at) VALUES(?,?,?,?,?,?,?)",
            // 参数：用户名、IP、用户代理（限制长度 200）、成功标志、失败原因、用户类型、创建时间
            {username, ip, userAgent.substr(0, 200),
             success ? "1" : "0", failReason,
             std::to_string((int)userType), std::to_string(now)});
    }

    // 清理超过 7 天的旧记录
    // 可由定时任务调用，定期清理过期的登录尝试记录
    static void cleanup() {
        // 计算 7 天前的时间戳（86400 秒 = 1 天）
        long long cutoff = std::time(nullptr) - 7 * 86400LL;
        // 删除 7 天前的所有登录尝试记录
        PayDb::instance().exec(
            // SQL 语句：删除过期记录
            "DELETE FROM login_attempt WHERE created_at < ?",
            // 参数：截断时间戳
            {std::to_string(cutoff)});
    }
// 类定义结束
};
