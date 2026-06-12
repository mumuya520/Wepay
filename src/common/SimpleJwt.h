// 轻量 JWT 工具 —— 专用于 wepay-cpp 管理后台认证
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <chrono> // 时间库
#include <stdexcept> // 异常库
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/defaults.h> // JWT 库
#include <drogon/drogon.h> // Drogon 框架

// JWT 特性类型别名（使用 jsoncpp 作为 JSON 解析器）
using jwt_t = jwt::traits::open_source_parsers_jsoncpp;

// 轻量 JWT 工具类，用于 WePay-Cpp 管理后台认证
class SimpleJwt {
public:
    // JWT 配置结构体
    struct Config {
        // JWT 签名密钥（默认值）
        std::string secret   = "wepay-cpp-secret";
        // Token 过期时间（小时，默认 24 小时）
        int expireHours      = 24;
    };

    // 获取配置单例
    // 返回：配置对象的引用
    static Config &cfg() {
        // 创建静态配置对象（单例模式）
        static Config c;
        // 返回配置对象引用
        return c;
    }

    // 从 config.json 加载 JWT 配置
    static void load() {
        // 从 Drogon 应用配置中获取 JWT 配置节点
        auto &jc  = drogon::app().getCustomConfig()["jwt"];
        // 加载 JWT 密钥（从配置文件读取，默认值为 "wepay-cpp-secret"）
        cfg().secret      = jc.get("secret",       "wepay-cpp-secret").asString();
        // 加载 Token 过期时间（从配置文件读取，默认值为 24 小时）
        cfg().expireHours = jc.get("expire_hours",  24).asInt();
    }

    // 签发 JWT Token（payload 包含用户名）
    // 参数 username：用户名
    // 返回：签名后的 JWT Token 字符串
    static std::string sign(const std::string &username) {
        // 获取当前系统时间
        auto now = std::chrono::system_clock::now();
        // 计算 Token 过期时间（当前时间 + 配置的过期小时数）
        auto exp = now + std::chrono::hours(cfg().expireHours);
        // 创建 JWT Token 构建器
        return jwt::create<jwt_t>()
            // 设置 Token 签发时间（iat claim）
            .set_issued_at(now)
            // 设置 Token 过期时间（exp claim）
            .set_expires_at(exp)
            // 设置 payload 中的 sub claim（主体，通常为用户名）
            .set_payload_claim("sub", jwt::basic_claim<jwt_t>(username))
            // 使用 HS256 算法和密钥签名 Token
            .sign(jwt::algorithm::hs256{cfg().secret});
    }

    // 验证 JWT Token 并返回用户名
    // 参数 token：JWT Token 字符串
    // 返回：Token 中的 sub claim（用户名）
    // 异常：如果 Token 无效或验证失败，抛出异常
    static std::string verify(const std::string &token) {
        // 创建 JWT 验证器
        auto verifier = jwt::verify<jwt_t>()
            // 允许使用 HS256 算法进行验证
            .allow_algorithm(jwt::algorithm::hs256{cfg().secret});
        // 解码 Token（不验证签名）
        auto decoded = jwt::decode<jwt_t>(token);
        // 验证 Token 的签名
        verifier.verify(decoded);
        // 检查 Token 中是否存在 sub claim
        if (!decoded.has_payload_claim("sub")) {
            // 如果 sub claim 不存在，抛出异常
            throw std::runtime_error("token missing sub");
        }
        // 返回 sub claim 的值（用户名）
        return decoded.get_payload_claim("sub").as_string();
    }

    // 从 HTTP Authorization 头中解析 JWT Token
    // 支持格式：Authorization: Bearer <token>
    // 参数 authHeader：HTTP Authorization 头的值
    // 返回：提取的 JWT Token 字符串
    // 异常：如果 Token 格式无效，抛出异常
    static std::string fromHeader(const std::string &authHeader) {
        // 用于存储提取的 Token
        std::string token;
        // 检查是否以 "Bearer " 前缀开头
        if (authHeader.size() > 7 && authHeader.substr(0, 7) == "Bearer ") {
            // 提取 "Bearer " 后面的 Token 部分
            token = authHeader.substr(7);
        } else {
            // 如果没有 "Bearer " 前缀，直接使用整个头值
            token = authHeader;
        }
        // JWT 必须是三个 base64url 段用点号分隔
        // 检查 Token 是否为空
        if (token.empty())
            // 如果为空，抛出异常
            throw std::runtime_error("empty token");
        // 点号计数器（JWT 应该有 2 个点号）
        int dots = 0;
        // 遍历 Token 中的每个字符
        for (char c : token) {
            // 如果是点号，计数并继续
            if (c == '.') {
                ++dots;
                continue;
            }
            // base64url 字母表：A-Z a-z 0-9 + / = - _
            // 检查字符是否为有效的 base64url 字符
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/' ||
                  c == '=' || c == '-' || c == '_')) {
                // 如果包含无效字符，抛出异常
                throw std::runtime_error("token contains invalid characters");
            }
        }
        // 检查点号数量是否为 2（JWT 格式：header.payload.signature）
        if (dots != 2) {
            // 如果点号数量不对，抛出异常
            throw std::runtime_error("malformed token (expected header.payload.signature)");
        }
        // 返回提取的 Token
        return token;
    }
// 类定义结束
};
