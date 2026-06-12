// WePay-Cpp — 令牌服务: 签发 access + refresh token, 黑名单登出
// 基于 SimpleJwt(HS256) + SQLite 存储
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <random> // 随机数库
#include <sstream> // 字符串流库
#include <ctime> // C 时间库
#include <openssl/sha.h> // OpenSSL SHA 库
#include <iomanip> // 输入输出格式化库
#include "SimpleJwt.h" // JWT 工具
#include "PayDb.h" // 数据库操作

// Token 服务类，管理 JWT Token 的签发、刷新和撤销
class TokenService {
public:
    // Token 对结构体（access token + refresh token）
    struct Pair {
        // Access Token（短期，用于 API 认证）
        std::string accessToken;
        // Refresh Token（长期，用于续签 access token）
        std::string refreshToken;
        // Access Token 过期时间（秒级 Unix 时间戳）
        long long accessExpires = 0;
        // Refresh Token 过期时间（秒级 Unix 时间戳）
        long long refreshExpires = 0;
    };

    // 签发 access token + refresh token
    // 参数 userType：用户类型（1=系统管理员, 2=商户, 3=代理商）
    // 参数 userId：用户 ID
    // 参数 subject：JWT subject claim（格式自定，如 "sys:1"、"mch:5"、"agent:2"）
    // 返回：包含两个 token 的 Pair 结构体
    static Pair issue(int userType, int userId, const std::string &subject) {
        // 创建返回结构体
        Pair p;
        // 获取当前时间戳（秒级）
        long long now = std::time(nullptr);
        // 计算 access token 过期时间（当前时间 + 配置的过期小时数）
        p.accessExpires  = now + SimpleJwt::cfg().expireHours * 3600LL;
        // 计算 refresh token 过期时间（当前时间 + 7 天）
        p.refreshExpires = now + 7 * 24 * 3600LL;
        // 使用 JWT 签发 access token
        p.accessToken    = SimpleJwt::sign(subject);
        // 生成 refresh token
        p.refreshToken   = genRefreshToken();

        // 将 refresh token 存储到数据库
        PayDb::instance().exec(
            // SQL 语句：插入 refresh token 记录
            "INSERT INTO jwt_refresh_token(refresh_token,user_type,user_id,expires_at,"
            "revoked,created_at) VALUES(?,?,?,?,0,?)",
            // 参数：refresh token、用户类型、用户 ID、过期时间、创建时间
            {p.refreshToken,
             std::to_string(userType),
             std::to_string(userId),
             std::to_string(p.refreshExpires),
             std::to_string(now)});
        // 返回 token 对
        return p;
    }

    // 刷新 Token 结果结构体
    struct RefreshResult {
        // 是否成功
        bool success = false;
        // 错误信息
        std::string errMsg;
        // 用户类型
        int userType = 0;
        // 用户 ID
        int userId = 0;
        // 新的 Token 对
        Pair pair;
    };

    // 通过 refresh token 续签 access token
    // 参数 refreshToken：refresh token 字符串
    // 参数 subject：新的 JWT subject claim
    // 返回：RefreshResult 结构体（包含成功标志、错误信息和新的 token 对）
    static RefreshResult refresh(const std::string &refreshToken,
                                  const std::string &subject) {
        // 创建返回结构体
        RefreshResult r;
        // 从数据库查询 refresh token 记录
        auto row = PayDb::instance().queryOne(
            // SQL 语句：查询 refresh token 的详细信息
            "SELECT id,user_type,user_id,expires_at,revoked FROM jwt_refresh_token "
            "WHERE refresh_token=?", {refreshToken});
        // 检查查询结果是否为空
        if (row.empty()) {
            // 如果 refresh token 不存在，设置错误信息
            r.errMsg = "refresh token 无效";
            // 返回失败结果
            return r;
        }
        // 检查 refresh token 是否已被撤销
        if (row["revoked"] == "1") {
            // 如果已撤销，设置错误信息
            r.errMsg = "refresh token 已撤销";
            // 返回失败结果
            return r;
        }

        // 获取 refresh token 的过期时间
        long long exp = 0;
        // 尝试将过期时间字符串转换为 long long
        try {
            exp = std::stoll(row["expires_at"]);
        } catch(...) {
            // 如果转换失败，exp 保持为 0
        }
        // 检查 refresh token 是否已过期
        if (exp < std::time(nullptr)) {
            // 如果已过期，设置错误信息
            r.errMsg = "refresh token 已过期";
            // 返回失败结果
            return r;
        }

        // 获取用户类型
        try {
            r.userType = std::stoi(row["user_type"]);
        } catch(...) {
            // 如果转换失败，userType 保持为 0
        }
        // 获取用户 ID
        try {
            r.userId   = std::stoi(row["user_id"]);
        } catch(...) {
            // 如果转换失败，userId 保持为 0
        }

        // 撤销旧的 refresh token（一次性使用）
        PayDb::instance().exec(
            // SQL 语句：更新 refresh token 的撤销状态
            "UPDATE jwt_refresh_token SET revoked=1 WHERE id=?",
            // 参数：refresh token 的数据库 ID
            {row["id"]});

        // 签发新的 token 对
        r.pair = issue(r.userType, r.userId, subject);
        // 标记刷新成功
        r.success = true;
        // 返回成功结果
        return r;
    }

    // 撤销 Token（登出）
    // 将 access token 加入黑名单，并撤销 refresh token
    // 参数 accessToken：要撤销的 access token
    // 参数 refreshToken：要撤销的 refresh token（可选）
    static void revoke(const std::string &accessToken,
                       const std::string &refreshToken = "") {
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 计算 access token 的过期时间
        long long exp = now + SimpleJwt::cfg().expireHours * 3600LL;
        // 计算 access token 的 SHA256 哈希值
        std::string th = sha256Hex(accessToken);
        // 将 access token 哈希值加入黑名单
        PayDb::instance().exec(
            // SQL 语句：插入或替换黑名单记录
            "INSERT OR REPLACE INTO jwt_blacklist(token_hash,expires_at,revoked_at) "
            "VALUES(?,?,?)",
            // 参数：token 哈希值、过期时间、撤销时间
            {th, std::to_string(exp), std::to_string(now)});

        // 检查是否提供了 refresh token
        if (!refreshToken.empty()) {
            // 撤销对应的 refresh token
            PayDb::instance().exec(
                // SQL 语句：更新 refresh token 的撤销状态
                "UPDATE jwt_refresh_token SET revoked=1 WHERE refresh_token=?",
                // 参数：refresh token
                {refreshToken});
        }
    }

    // 定期清理过期的黑名单和 refresh token 记录
    static void cleanup() {
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 删除过期的黑名单记录
        db.exec(
            // SQL 语句：删除过期时间小于当前时间的黑名单记录
            "DELETE FROM jwt_blacklist WHERE expires_at < ?",
            // 参数：当前时间戳
            {std::to_string(now)});
        // 删除过期或已撤销的 refresh token 记录
        db.exec(
            // SQL 语句：删除过期或已撤销的 refresh token 记录
            "DELETE FROM jwt_refresh_token WHERE expires_at < ? OR revoked=1",
            // 参数：当前时间戳
            {std::to_string(now)});
    }

// 私有辅助函数区域
private:
    // 生成 refresh token
    // 返回：48 个随机字符 + 时间戳组成的字符串
    static std::string genRefreshToken() {
        // 字符集：小写字母、大写字母、数字
        static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        // 创建随机数生成器
        std::mt19937 rng((unsigned)std::random_device{}());
        // 结果字符串
        std::string s;
        // 生成 48 个随机字符
        for (int i = 0; i < 48; ++i)
            // 从字符集中随机选择一个字符
            s += cs[rng() % (sizeof(cs) - 1)];
        // 添加时间戳后缀以保证唯一性
        s += std::to_string(std::time(nullptr));
        // 返回生成的 refresh token
        return s;
    }

    // 计算字符串的 SHA256 哈希值（十六进制格式）
    // 参数 data：输入字符串
    // 返回：SHA256 哈希值的十六进制字符串
    static std::string sha256Hex(const std::string &data) {
        // SHA256 哈希值缓冲区（32 字节）
        unsigned char hash[SHA256_DIGEST_LENGTH];
        // 计算 SHA256 哈希
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
        // 创建字符串流用于十六进制转换
        std::ostringstream oss;
        // 遍历哈希值的每个字节
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            // 将字节转换为两位十六进制字符
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        // 返回十六进制字符串
        return oss.str();
    }
// 类定义结束
};
