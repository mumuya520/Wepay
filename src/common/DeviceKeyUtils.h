// WePay-Cpp — 设备密钥签名验证工具（支持 EdDSA 和 RSA）
// 用于设备密钥登录：客户端用私钥签名挑战值，服务端用公钥验证
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <stdexcept> // 异常库
#include <sodium.h> // libsodium 库
#include <openssl/rsa.h> // OpenSSL RSA 库
#include <openssl/pem.h> // OpenSSL PEM 库
#include <openssl/sha.h> // OpenSSL SHA 库
#include <openssl/evp.h> // OpenSSL EVP 库
#include <openssl/bio.h> // OpenSSL BIO 库
#include <openssl/buffer.h> // OpenSSL 缓冲区库
#include <trantor/utils/Logger.h> // 日志库

// 设备密钥工具类，支持 EdDSA 和 RSA 签名验证
class DeviceKeyUtils {
public:
    // 密钥类型枚举，定义支持的密钥算法
    enum class KeyType {
        // EdDSA (Ed25519) 算法
        ED25519,
        // RSA 2048-bit 算法
        RSA_2048,
        // RSA 4096-bit 算法
        RSA_4096
    };

    // 初始化 libsodium 库（应用启动时调用一次）
    // 注意：必须在使用任何 libsodium 函数之前调用
    static void init() {
        // 初始化 libsodium 库
        if (sodium_init() < 0) {
            // 如果初始化失败，抛出异常
            throw std::runtime_error("libsodium initialization failed");
        }
    }

    // ══════════════════════════════════════════════════════════════
    // EdDSA (Ed25519) 实现
    // ══════════════════════════════════════════════════════════════

    // 生成 Ed25519 密钥对（客户端使用）
    // 返回：pair<公钥十六进制字符串, 私钥十六进制字符串>
    static std::pair<std::string, std::string> generateEd25519KeyPair() {
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
    // 参数 message：要签名的消息
    // 参数 privateKeyHex：私钥的十六进制字符串
    // 返回：签名的十六进制字符串
    static std::string signEd25519(const std::string &message, const std::string &privateKeyHex) {
        // 检查私钥长度是否正确（64 字节 = 128 个十六进制字符）
        if (privateKeyHex.size() != crypto_sign_SECRETKEYBYTES * 2) {
            // 如果长度不正确，抛出异常
            throw std::runtime_error("invalid Ed25519 private key length");
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
    static bool verifyEd25519(const std::string &message,
                              const std::string &signatureHex,
                              const std::string &publicKeyHex) {
        // 使用 try-catch 捕获异常
        try {
            // 检查公钥长度是否正确（32 字节 = 64 个十六进制字符）
            if (publicKeyHex.size() != crypto_sign_PUBLICKEYBYTES * 2) {
                // 记录警告日志
                LOG_WARN << "invalid Ed25519 public key length: " << publicKeyHex.size();
                // 返回 false 表示验证失败
                return false;
            }
            // 检查签名长度是否正确（64 字节 = 128 个十六进制字符）
            if (signatureHex.size() != crypto_sign_BYTES * 2) {
                // 记录警告日志
                LOG_WARN << "invalid Ed25519 signature length: " << signatureHex.size();
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
            LOG_ERROR << "Ed25519 verify error: " << e.what();
            // 返回 false 表示验证失败
            return false;
        }
    }

    // ══════════════════════════════════════════════════════════════
    // RSA 实现
    // ══════════════════════════════════════════════════════════════

    // 生成 RSA 密钥对（客户端使用）
    // 参数 bits：密钥位数，支持 2048 或 4096
    // 返回：pair<公钥PEM格式字符串, 私钥PEM格式字符串>
    static std::pair<std::string, std::string> generateRSAKeyPair(int bits = 2048) {
        // 检查密钥位数是否有效
        if (bits != 2048 && bits != 4096) {
            // 如果位数不支持，抛出异常
            throw std::runtime_error("RSA key size must be 2048 or 4096");
        }

        // 创建 EVP_PKEY_CTX 上下文用于 RSA 密钥生成
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        // 检查上下文是否创建成功
        if (!ctx)
            // 如果创建失败，抛出异常
            throw std::runtime_error("EVP_PKEY_CTX_new_id failed");

        // 初始化密钥生成操作
        if (EVP_PKEY_keygen_init(ctx) <= 0) {
            // 释放上下文
            EVP_PKEY_CTX_free(ctx);
            // 抛出异常
            throw std::runtime_error("EVP_PKEY_keygen_init failed");
        }

        // 设置 RSA 密钥位数
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0) {
            // 释放上下文
            EVP_PKEY_CTX_free(ctx);
            // 抛出异常
            throw std::runtime_error("EVP_PKEY_CTX_set_rsa_keygen_bits failed");
        }

        // 生成密钥对
        EVP_PKEY *pkey = nullptr;
        // 执行密钥生成
        if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
            // 释放上下文
            EVP_PKEY_CTX_free(ctx);
            // 抛出异常
            throw std::runtime_error("EVP_PKEY_keygen failed");
        }
        // 释放上下文
        EVP_PKEY_CTX_free(ctx);

        // 导出公钥为 PEM 格式
        // 创建内存 BIO 用于存储公钥
        BIO *pubBio = BIO_new(BIO_s_mem());
        // 将公钥写入 BIO
        PEM_write_bio_PUBKEY(pubBio, pkey);
        // 获取 BIO 中的内存指针
        BUF_MEM *pubMem;
        // 获取内存缓冲区
        BIO_get_mem_ptr(pubBio, &pubMem);
        // 将公钥数据转换为字符串
        std::string publicKeyPem(pubMem->data, pubMem->length);
        // 释放 BIO
        BIO_free(pubBio);

        // 导出私钥为 PEM 格式
        // 创建内存 BIO 用于存储私钥
        BIO *privBio = BIO_new(BIO_s_mem());
        // 将私钥写入 BIO（不使用密码保护）
        PEM_write_bio_PrivateKey(privBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        // 获取 BIO 中的内存指针
        BUF_MEM *privMem;
        // 获取内存缓冲区
        BIO_get_mem_ptr(privBio, &privMem);
        // 将私钥数据转换为字符串
        std::string privateKeyPem(privMem->data, privMem->length);
        // 释放 BIO
        BIO_free(privBio);

        // 释放 EVP_PKEY 对象
        EVP_PKEY_free(pkey);

        // 返回公钥和私钥
        return {publicKeyPem, privateKeyPem};
    }

    // RSA 签名（客户端使用，使用 SHA256 哈希算法）
    // 参数 message：要签名的消息
    // 参数 privateKeyPem：私钥的 PEM 格式字符串
    // 返回：签名的十六进制字符串
    static std::string signRSA(const std::string &message, const std::string &privateKeyPem) {
        // 创建内存 BIO 用于读取私钥
        BIO *bio = BIO_new_mem_buf(privateKeyPem.data(), privateKeyPem.size());
        // 从 PEM 格式读取私钥
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        // 释放 BIO
        BIO_free(bio);

        // 检查私钥是否读取成功
        if (!pkey) {
            // 如果读取失败，抛出异常
            throw std::runtime_error("invalid RSA private key");
        }

        // 创建消息摘要上下文
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        // 初始化签名操作（使用 SHA256 算法）
        if (EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
            // 释放上下文
            EVP_MD_CTX_free(mdctx);
            // 释放密钥
            EVP_PKEY_free(pkey);
            // 抛出异常
            throw std::runtime_error("EVP_DigestSignInit failed");
        }

        // 更新签名数据（处理消息）
        if (EVP_DigestSignUpdate(mdctx, message.data(), message.size()) <= 0) {
            // 释放上下文
            EVP_MD_CTX_free(mdctx);
            // 释放密钥
            EVP_PKEY_free(pkey);
            // 抛出异常
            throw std::runtime_error("EVP_DigestSignUpdate failed");
        }

        // 获取签名长度
        size_t sigLen;
        // 调用 EVP_DigestSignFinal 获取签名长度
        if (EVP_DigestSignFinal(mdctx, nullptr, &sigLen) <= 0) {
            // 释放上下文
            EVP_MD_CTX_free(mdctx);
            // 释放密钥
            EVP_PKEY_free(pkey);
            // 抛出异常
            throw std::runtime_error("EVP_DigestSignFinal (get length) failed");
        }

        // 创建签名缓冲区
        std::vector<unsigned char> sig(sigLen);
        // 生成签名
        if (EVP_DigestSignFinal(mdctx, sig.data(), &sigLen) <= 0) {
            // 释放上下文
            EVP_MD_CTX_free(mdctx);
            // 释放密钥
            EVP_PKEY_free(pkey);
            // 抛出异常
            throw std::runtime_error("EVP_DigestSignFinal failed");
        }

        // 释放上下文
        EVP_MD_CTX_free(mdctx);
        // 释放密钥
        EVP_PKEY_free(pkey);

        // 将签名转换为十六进制字符串并返回
        return bytesToHex(sig.data(), sigLen);
    }

    // RSA 验证签名（服务端使用，使用 SHA256 哈希算法）
    // 参数 message：原始消息
    // 参数 signatureHex：签名的十六进制字符串
    // 参数 publicKeyPem：公钥的 PEM 格式字符串
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verifyRSA(const std::string &message,
                          const std::string &signatureHex,
                          const std::string &publicKeyPem) {
        // 使用 try-catch 捕获异常
        try {
            // 创建内存 BIO 用于读取公钥
            BIO *bio = BIO_new_mem_buf(publicKeyPem.data(), publicKeyPem.size());
            // 从 PEM 格式读取公钥
            EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
            // 释放 BIO
            BIO_free(bio);

            // 检查公钥是否读取成功
            if (!pkey) {
                // 记录警告日志
                LOG_WARN << "invalid RSA public key";
                // 返回 false 表示验证失败
                return false;
            }

            // 将十六进制签名转换为字节数组
            auto sig = hexToBytes(signatureHex);

            // 创建消息摘要上下文
            EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
            // 初始化验证操作（使用 SHA256 算法）
            if (EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
                // 释放上下文
                EVP_MD_CTX_free(mdctx);
                // 释放密钥
                EVP_PKEY_free(pkey);
                // 返回 false 表示验证失败
                return false;
            }

            // 更新验证数据（处理消息）
            if (EVP_DigestVerifyUpdate(mdctx, message.data(), message.size()) <= 0) {
                // 释放上下文
                EVP_MD_CTX_free(mdctx);
                // 释放密钥
                EVP_PKEY_free(pkey);
                // 返回 false 表示验证失败
                return false;
            }

            // 执行最终验证
            int result = EVP_DigestVerifyFinal(mdctx, sig.data(), sig.size());

            // 释放上下文
            EVP_MD_CTX_free(mdctx);
            // 释放密钥
            EVP_PKEY_free(pkey);

            // 返回验证结果（1 表示有效，0 表示无效）
            return result == 1;
        } catch (const std::exception &e) {
            // 捕获异常并记录错误日志
            LOG_ERROR << "RSA verify error: " << e.what();
            // 返回 false 表示验证失败
            return false;
        }
    }

    // ══════════════════════════════════════════════════════════════
    // 通用接口
    // ══════════════════════════════════════════════════════════════

    // 根据密钥类型验证签名（通用接口）
    // 参数 message：原始消息
    // 参数 signature：签名（十六进制字符串或 PEM 格式）
    // 参数 publicKey：公钥（十六进制字符串或 PEM 格式）
    // 参数 keyType：密钥类型（ED25519、RSA_2048 或 RSA_4096）
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verifySignature(const std::string &message,
                                const std::string &signature,
                                const std::string &publicKey,
                                KeyType keyType) {
        // 根据密钥类型调用相应的验证函数
        switch (keyType) {
            // 如果是 Ed25519 密钥
            case KeyType::ED25519:
                // 调用 Ed25519 验证函数
                return verifyEd25519(message, signature, publicKey);
            // 如果是 RSA 2048 或 4096 密钥
            case KeyType::RSA_2048:
            case KeyType::RSA_4096:
                // 调用 RSA 验证函数
                return verifyRSA(message, signature, publicKey);
            // 如果是未知的密钥类型
            default:
                // 记录错误日志
                LOG_ERROR << "unsupported key type";
                // 返回 false 表示验证失败
                return false;
        }
    }

    // 生成随机挑战值（用于设备登录）
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

    // 检测并返回公钥的类型
    // 参数 publicKey：公钥（十六进制字符串或 PEM 格式）
    // 返回：检测到的密钥类型
    static KeyType detectKeyType(const std::string &publicKey) {
        // Ed25519 公钥是 64 个十六进制字符（32 字节）
        if (publicKey.size() == 64 && isHexString(publicKey)) {
            // 返回 Ed25519 密钥类型
            return KeyType::ED25519;
        }
        // RSA 公钥是 PEM 格式，以 "-----BEGIN PUBLIC KEY-----" 开头
        if (publicKey.find("-----BEGIN PUBLIC KEY-----") != std::string::npos) {
            // 根据 PEM 长度估算密钥位数
            // PEM 长度小于 500 字节通常是 2048 位
            if (publicKey.size() < 500)
                // 返回 RSA 2048 密钥类型
                return KeyType::RSA_2048;
            // 否则返回 RSA 4096 密钥类型
            else
                return KeyType::RSA_4096;
        }
        throw std::runtime_error("unknown key type");
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

    // 检查字符串是否为有效的十六进制字符串
    // 参数 str：要检查的字符串
    // 返回：true 表示是有效的十六进制字符串，false 表示不是
    static bool isHexString(const std::string &str) {
        // 遍历字符串中的每个字符
        for (char c : str) {
            // 检查字符是否为有效的十六进制字符
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                // 如果找到无效字符，返回 false
                return false;
            }
        }
        // 如果所有字符都有效，返回 true
        return true;
    }
// 类定义结束
};
