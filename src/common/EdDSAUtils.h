// WePay-Cpp — EdDSA (Ed25519) 签名验证工具
// 用于设备密钥登录：客户端用私钥签名挑战值，服务端用公钥验证
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <stdexcept> // 异常库
#include <sodium.h> // libsodium 库
#include <trantor/utils/Logger.h> // 日志库

// EdDSA (Ed25519) 签名验证工具类，用于设备密钥登录认证
class EdDSAUtils {
public:
    // 初始化 libsodium 库（应用启动时调用一次）
    // 注意：必须在使用任何 libsodium 函数之前调用
    static void init() {
        // 初始化 libsodium 库
        if (sodium_init() < 0) {
            // 如果初始化失败，抛出异常
            throw std::runtime_error("libsodium initialization failed");
        }
    }

    // 生成 Ed25519 密钥对（客户端使用）
    // 返回：pair<公钥十六进制字符串, 私钥十六进制字符串>
    static std::pair<std::string, std::string> generateKeyPair() {
        // 公钥缓冲区（32 字节）
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        // 私钥缓冲区（64 字节）
        unsigned char sk[crypto_sign_SECRETKEYBYTES];

        // 使用 libsodium 生成 Ed25519 密钥对
        crypto_sign_keypair(pk, sk);

        // 返回十六进制编码的公钥和私钥
        return {
            // 将公钥转换为十六进制字符串
            bytesToHex(pk, crypto_sign_PUBLICKEYBYTES),
            // 将私钥转换为十六进制字符串
            bytesToHex(sk, crypto_sign_SECRETKEYBYTES)
        };
    }

    // Ed25519 签名（客户端使用）
    // 参数 message：待签名的消息（通常是 challenge + timestamp）
    // 参数 privateKeyHex：私钥的十六进制字符串
    // 返回：签名的十六进制字符串
    static std::string sign(const std::string &message, const std::string &privateKeyHex) {
        // 检查私钥长度是否正确（64 字节 = 128 个十六进制字符）
        if (privateKeyHex.size() != crypto_sign_SECRETKEYBYTES * 2) {
            // 如果长度不正确，抛出异常
            throw std::runtime_error("invalid private key length");
        }

        // 将十六进制私钥转换为字节数组
        auto sk = hexToBytes(privateKeyHex);
        // 签名缓冲区（64 字节）
        unsigned char sig[crypto_sign_BYTES];

        // 使用 libsodium 生成分离的签名（不包含消息）
        crypto_sign_detached(
            // 输出：签名
            sig,
            // 输出：签名长度（可选，设为 nullptr）
            nullptr,
            // 输入：消息数据
            reinterpret_cast<const unsigned char*>(message.data()),
            // 输入：消息长度
            message.size(),
            // 输入：私钥
            sk.data()
        );

        // 将签名转换为十六进制字符串并返回
        return bytesToHex(sig, crypto_sign_BYTES);
    }

    // Ed25519 验证签名（服务端使用）
    // 参数 message：原始消息
    // 参数 signatureHex：签名的十六进制字符串
    // 参数 publicKeyHex：公钥的十六进制字符串
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verify(const std::string &message,
                       const std::string &signatureHex,
                       const std::string &publicKeyHex) {
        // 使用 try-catch 捕获异常
        try {
            // 检查公钥长度是否正确（32 字节 = 64 个十六进制字符）
            if (publicKeyHex.size() != crypto_sign_PUBLICKEYBYTES * 2) {
                // 记录警告日志
                LOG_WARN << "invalid public key length: " << publicKeyHex.size();
                // 返回 false 表示验证失败
                return false;
            }
            // 检查签名长度是否正确（64 字节 = 128 个十六进制字符）
            if (signatureHex.size() != crypto_sign_BYTES * 2) {
                // 记录警告日志
                LOG_WARN << "invalid signature length: " << signatureHex.size();
                // 返回 false 表示验证失败
                return false;
            }

            // 将十六进制公钥转换为字节数组
            auto pk = hexToBytes(publicKeyHex);
            // 将十六进制签名转换为字节数组
            auto sig = hexToBytes(signatureHex);

            // 使用 libsodium 验证分离的签名
            int result = crypto_sign_verify_detached(
                // 输入：签名
                sig.data(),
                // 输入：消息数据
                reinterpret_cast<const unsigned char*>(message.data()),
                // 输入：消息长度
                message.size(),
                // 输入：公钥
                pk.data()
            );

            // 返回验证结果（0 表示有效，其他值表示无效）
            return result == 0;
        } catch (const std::exception &e) {
            // 捕获异常并记录错误日志
            LOG_ERROR << "EdDSA verify error: " << e.what();
            // 返回 false 表示验证失败
            return false;
        }
    }

    // 生成随机挑战值（服务端使用）
    // 参数 length：挑战值长度（字节数），默认 32 字节
    // 返回：随机挑战值的十六进制字符串
    static std::string generateChallenge(size_t length = 32) {
        // 创建缓冲区用于存储随机数据
        std::vector<unsigned char> buf(length);
        // 使用 libsodium 生成随机数据
        randombytes_buf(buf.data(), length);
        // 将随机数据转换为十六进制字符串并返回
        return bytesToHex(buf.data(), length);
    }

// 私有辅助函数区域
private:
    // 将字节数组转换为十六进制字符串
    // 参数 data：字节数组指针
    // 参数 len：字节数组长度
    // 返回：十六进制字符串（小写）
    static std::string bytesToHex(const unsigned char *data, size_t len) {
        // 十六进制字符表
        static const char hex[] = "0123456789abcdef";
        // 结果字符串
        std::string result;
        // 预分配空间（每个字节转换为 2 个十六进制字符）
        result.reserve(len * 2);
        // 遍历每个字节
        for (size_t i = 0; i < len; ++i) {
            // 将字节的高 4 位转换为十六进制字符
            result.push_back(hex[(data[i] >> 4) & 0xF]);
            // 将字节的低 4 位转换为十六进制字符
            result.push_back(hex[data[i] & 0xF]);
        }
        // 返回十六进制字符串
        return result;
    }

    // 将十六进制字符串转换为字节数组
    // 参数 hex：十六进制字符串
    // 返回：字节数组
    static std::vector<unsigned char> hexToBytes(const std::string &hex) {
        // 检查十六进制字符串长度是否为偶数
        if (hex.size() % 2 != 0) {
            // 如果长度为奇数，抛出异常
            throw std::runtime_error("invalid hex string length");
        }
        // 字节数组
        std::vector<unsigned char> bytes;
        // 预分配空间（十六进制字符串长度的一半）
        bytes.reserve(hex.size() / 2);
        // 每次处理 2 个十六进制字符
        for (size_t i = 0; i < hex.size(); i += 2) {
            // 初始化字节值
            unsigned char byte = 0;
            // 处理两个十六进制字符
            for (int j = 0; j < 2; ++j) {
                // 获取当前字符
                char c = hex[i + j];
                // 左移 4 位为下一个字符腾出空间
                byte <<= 4;
                // 检查是否为数字字符（0-9）
                if (c >= '0' && c <= '9')
                    // 将数字字符转换为对应的值
                    byte |= (c - '0');
                // 检查是否为小写字母（a-f）
                else if (c >= 'a' && c <= 'f')
                    // 将小写字母转换为对应的值（10-15）
                    byte |= (c - 'a' + 10);
                // 检查是否为大写字母（A-F）
                else if (c >= 'A' && c <= 'F')
                    // 将大写字母转换为对应的值（10-15）
                    byte |= (c - 'A' + 10);
                // 如果不是有效的十六进制字符
                else
                    // 抛出异常
                    throw std::runtime_error("invalid hex character");
            }
            // 将转换后的字节添加到数组
            bytes.push_back(byte);
        }
        // 返回字节数组
        return bytes;
    }
// 类定义结束
};
