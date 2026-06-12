// WePay-Cpp — 密码加盐哈希工具
// 使用 OpenSSL SHA256 + 随机盐
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <sstream> // 字符串流
#include <iomanip> // 输入输出格式化
#include <random> // 随机数库
#include <openssl/evp.h> // OpenSSL EVP 库

// 密码工具类，提供密码加盐哈希、验证和随机数生成功能
class PasswordUtils {
public:
    // 生成随机盐（用于密码哈希）
    // 参数 len：盐的长度，默认 16 字符
    // 返回：随机生成的盐字符串
    static std::string generateSalt(int len = 16) {
        // 盐字符集：小写字母、数字、大写字母
        static const char *cs = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        // 创建随机数生成器（使用系统随机设备作为种子）
        std::mt19937 rng((unsigned)std::random_device{}());
        // 盐字符串
        std::string s;
        // 预分配空间以提高性能
        s.reserve(len);
        // 循环生成指定长度的随机字符
        for (int i = 0; i < len; ++i)
            // 从字符集中随机选择一个字符（62 个字符）
            s += cs[rng() % 62];
        // 返回生成的盐
        return s;
    }

    // 使用 SHA256 算法对密码进行加盐哈希
    // 参数 password：用户输入的密码
    // 参数 salt：随机盐值
    // 返回：SHA256 哈希值的十六进制字符串
    static std::string hashPassword(const std::string &password, const std::string &salt) {
        // 拼接盐和密码（盐在前，密码在后）
        std::string input = salt + password;
        // 摘要缓冲区（最大摘要大小）
        unsigned char digest[EVP_MAX_MD_SIZE];
        // 摘要长度
        unsigned int digestLen = 0;

        // 创建 EVP 消息摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 初始化 SHA256 算法
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        // 更新摘要数据（处理盐+密码）
        EVP_DigestUpdate(ctx, input.c_str(), input.size());
        // 完成摘要计算，获取结果
        EVP_DigestFinal_ex(ctx, digest, &digestLen);
        // 释放 EVP 上下文
        EVP_MD_CTX_free(ctx);

        // 创建字符串流用于十六进制转换
        std::ostringstream oss;
        // 遍历摘要中的每个字节
        for (unsigned int i = 0; i < digestLen; ++i)
            // 将字节转换为两位十六进制字符（小写）
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
        // 返回十六进制字符串
        return oss.str();
    }

    // 验证密码是否正确
    // 参数 password：用户输入的密码
    // 参数 salt：存储的盐值
    // 参数 storedHash：存储的哈希值
    // 返回：true 表示密码正确，false 表示密码错误
    static bool verify(const std::string &password, const std::string &salt,
                       const std::string &storedHash) {
        // 使用相同的盐对输入密码进行哈希
        // 比较计算得到的哈希值与存储的哈希值
        return hashPassword(password, salt) == storedHash;
    }

    // 生成随机密钥（用于 API Key 或通讯密钥）
    // 参数 len：密钥长度，默认 32 字符
    // 返回：随机生成的密钥字符串
    static std::string generateKey(int len = 32) {
        // 密钥字符集：小写字母和数字
        static const char *cs = "abcdefghijklmnopqrstuvwxyz0123456789";
        // 创建随机数生成器（使用系统随机设备作为种子）
        std::mt19937 rng((unsigned)std::random_device{}());
        // 密钥字符串
        std::string s;
        // 预分配空间以提高性能
        s.reserve(len);
        // 循环生成指定长度的随机字符
        for (int i = 0; i < len; ++i)
            // 从字符集中随机选择一个字符（36 个字符）
            s += cs[rng() % 36];
        // 返回生成的密钥
        return s;
    }

    // 生成商户号（格式：M + 6位数字）
    // 返回：商户号字符串（例如：M123456）
    static std::string generateMchNo() {
        // 创建随机数生成器（使用系统随机设备作为种子）
        std::mt19937 rng((unsigned)std::random_device{}());
        // 创建均匀分布的随机数生成器（100000-999999）
        std::uniform_int_distribution<int> d(100000, 999999);
        // 返回 "M" + 6位随机数字
        return "M" + std::to_string(d(rng));
    }

    // 生成应用 ID（8位随机数字）
    // 返回：应用 ID 字符串（例如：12345678）
    static std::string generateAppId() {
        // 创建随机数生成器（使用系统随机设备作为种子）
        std::mt19937 rng((unsigned)std::random_device{}());
        // 创建均匀分布的随机数生成器（10000000-99999999）
        std::uniform_int_distribution<int> d(10000000, 99999999);
        // 返回 8位随机数字字符串
        return std::to_string(d(rng));
    }
// 类定义结束
};
