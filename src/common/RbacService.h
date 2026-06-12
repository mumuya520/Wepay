// WePay-Cpp — RBAC 权限服务
// 提供用户权限查询、角色管理、权限验证等功能
// 超级管理员(is_super=1)自动拥有所有权限
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <set> // 集合容器
#include <mutex> // 互斥锁
#include <chrono> // 时间库
#include <unordered_map> // 哈希表
#include "PayDb.h" // 数据库操作

// RBAC（基于角色的访问控制）权限服务类
// 提供用户权限查询、角色管理、权限验证等功能
// 超级管理员(is_super=1)自动拥有所有权限
class RbacService {
public:
    // 用户信息结构体
    struct UserInfo {
        // 用户 ID
        int id = 0;
        // 用户名
        std::string username;
        // 真实姓名
        std::string realName;
        // 是否为超级管理员
        bool isSuper = false;
        // 账户状态（1=启用, 0=禁用）
        int state = 1;
    };

    // 根据用户名加载用户信息
    // 参数 username：用户名
    // 返回：用户信息结构体（如果用户不存在，返回默认值）
    static UserInfo loadUser(const std::string &username) {
        // 创建用户信息结构体
        UserInfo u;
        // 从数据库查询用户信息
        auto row = PayDb::instance().queryOne(
            // SQL 语句：查询用户的基本信息
            "SELECT id,username,real_name,is_super,state FROM sys_user WHERE username=?",
            // 参数：用户名
            {username});
        // 检查查询结果是否为空
        if (row.empty())
            // 如果用户不存在，返回默认的用户信息
            return u;
        // 尝试将用户 ID 字符串转换为整数
        try {
            u.id = std::stoi(row["id"]);
        } catch (...) {
            // 如果转换失败，id 保持为 0
        }
        // 设置用户名
        u.username = row["username"];
        // 设置真实姓名
        u.realName = row["real_name"];
        // 设置是否为超管（"1" 表示是超管）
        u.isSuper  = (row["is_super"] == "1");
        // 尝试将账户状态字符串转换为整数
        try {
            u.state = std::stoi(row["state"]);
        } catch (...) {
            // 如果转换失败，state 保持为 1（启用）
        }
        // 返回用户信息
        return u;
    }

    // 加载用户的所有权限编码
    // 参数 userId：用户 ID
    // 返回：权限编码集合（如 "order:create"、"order:close" 等）
    static std::set<std::string> loadUserPermissions(int userId) {
        // 检查权限缓存
        auto it = checkCache(userId);
        // 如果缓存命中，直接返回缓存的权限
        if (it.has)
            return it.perms;

        // 权限集合
        std::set<std::string> perms;
        // 从数据库查询用户的所有权限
        auto rows = PayDb::instance().query(
            // SQL 语句：通过用户-角色-权限的关联表查询权限
            "SELECT DISTINCT p.perm_code FROM sys_permission p "
            "INNER JOIN sys_role_permission rp ON p.id=rp.perm_id "
            "INNER JOIN sys_user_role ur ON ur.role_id=rp.role_id "
            "WHERE ur.user_id=?",
            // 参数：用户 ID
            {std::to_string(userId)});
        // 将查询结果中的权限编码添加到集合
        for (auto &r : rows)
            perms.insert(r["perm_code"]);

        // 将权限缓存起来（5 分钟过期）
        putCache(userId, perms);
        // 返回权限集合
        return perms;
    }

    // 加载用户的角色列表
    // 参数 userId：用户 ID
    // 返回：角色编码列表（如 "admin"、"operator" 等）
    static std::vector<std::string> loadUserRoles(int userId) {
        // 角色列表
        std::vector<std::string> roles;
        // 从数据库查询用户的所有角色
        auto rows = PayDb::instance().query(
            // SQL 语句：通过用户-角色关联表查询角色
            "SELECT r.role_code FROM sys_role r "
            "INNER JOIN sys_user_role ur ON ur.role_id=r.id "
            "WHERE ur.user_id=?",
            // 参数：用户 ID
            {std::to_string(userId)});
        // 将查询结果中的角色编码添加到列表
        for (auto &r : rows)
            roles.push_back(r["role_code"]);
        // 返回角色列表
        return roles;
    }

    // 校验用户是否拥有指定权限（超管自动放行）
    // 参数 userId：用户 ID
    // 参数 permCode：权限编码（如 "order:create"）
    // 返回：true 表示拥有权限，false 表示没有权限
    static bool hasPermission(int userId, const std::string &permCode) {
        // 加载用户信息
        auto user = loadUserById(userId);
        // 如果是超管，直接返回 true（超管拥有所有权限）
        if (user.isSuper)
            return true;
        // 加载用户的权限集合
        auto perms = loadUserPermissions(userId);
        // 检查权限集合中是否包含指定的权限编码
        return perms.count(permCode) > 0;
    }

    // 清除单个用户的权限缓存（用户角色变更时调用）
    // 参数 userId：用户 ID
    static void invalidate(int userId) {
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 从缓存中删除该用户的权限
        cache_.erase(userId);
    }

    // 清除所有用户的权限缓存
    static void invalidateAll() {
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 清空整个缓存
        cache_.clear();
    }

    // 根据用户 ID 加载用户信息
    // 参数 userId：用户 ID
    // 返回：用户信息结构体（如果用户不存在，返回默认值）
    static UserInfo loadUserById(int userId) {
        // 创建用户信息结构体
        UserInfo u;
        // 从数据库查询用户信息
        auto row = PayDb::instance().queryOne(
            // SQL 语句：根据 ID 查询用户的基本信息
            "SELECT id,username,real_name,is_super,state FROM sys_user WHERE id=?",
            // 参数：用户 ID
            {std::to_string(userId)});
        // 检查查询结果是否为空
        if (row.empty())
            // 如果用户不存在，返回默认的用户信息
            return u;
        // 尝试将用户 ID 字符串转换为整数
        try {
            u.id = std::stoi(row["id"]);
        } catch (...) {
            // 如果转换失败，id 保持为 0
        }
        // 设置用户名
        u.username = row["username"];
        // 设置真实姓名
        u.realName = row["real_name"];
        // 设置是否为超管（"1" 表示是超管）
        u.isSuper  = (row["is_super"] == "1");
        // 尝试将账户状态字符串转换为整数
        try {
            u.state = std::stoi(row["state"]);
        } catch (...) {
            // 如果转换失败，state 保持为 1（启用）
        }
        // 返回用户信息
        return u;
    }

// 私有辅助函数和数据成员区域
private:
    // 缓存条目结构体
    struct CacheEntry {
        // 权限集合
        std::set<std::string> perms;
        // 缓存过期时间
        std::chrono::steady_clock::time_point expires;
    };

    // 缓存查询结果结构体
    struct CacheHit {
        // 是否命中缓存
        bool has = false;
        // 缓存的权限集合
        std::set<std::string> perms;
    };

    // 检查权限缓存
    // 参数 userId：用户 ID
    // 返回：缓存查询结果（包含是否命中和权限集合）
    static CacheHit checkCache(int userId) {
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 在缓存中查找用户的权限
        auto it = cache_.find(userId);
        // 如果缓存中不存在该用户的权限
        if (it == cache_.end())
            // 返回缓存未命中的结果
            return {};
        // 检查缓存是否已过期
        if (std::chrono::steady_clock::now() > it->second.expires) {
            // 如果缓存已过期，从缓存中删除
            cache_.erase(it);
            // 返回缓存未命中的结果
            return {};
        }
        // 返回缓存命中的结果，包含权限集合
        return { true, it->second.perms };
    }

    // 将权限缓存起来
    // 参数 userId：用户 ID
    // 参数 perms：权限集合
    static void putCache(int userId, const std::set<std::string> &perms) {
        // 获取互斥锁以保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 创建缓存条目
        CacheEntry e;
        // 设置权限集合
        e.perms = perms;
        // 设置缓存过期时间（5 分钟后过期）
        e.expires = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        // 将缓存条目存储到缓存中
        cache_[userId] = std::move(e);
    }

    // 互斥锁（用于保护缓存的线程安全）
    static inline std::mutex mutex_;
    // 权限缓存（键为用户 ID，值为缓存条目）
    static inline std::unordered_map<int, CacheEntry> cache_;
// 类定义结束
};
