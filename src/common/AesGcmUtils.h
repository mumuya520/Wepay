// WePay-Cpp — AES-256-GCM 解密工具
// 微信支付 V3 回调/查询返回的 resource 密文使用 AES-256-GCM 算法:
//   - key: APIv3 密钥(32字节字符串)
//   - iv:  nonce (通常 12 字节)
//   - aad: associated_data
//   - ciphertext: Base64(密文 || 16字节tag)
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <openssl/evp.h> // OpenSSL EVP 库
#include <openssl/rand.h> // OpenSSL 随机数库
#include <openssl/sha.h> // OpenSSL SHA 库
#include "RsaUtils.h" // RSA 工具

// AES-256-GCM 加解密工具类
class AesGcmUtils {
public:
    // 微信支付 V3 风格解密方法
    // 密文格式：Base64(密文 || 16字节tag)
    // 参数 ciphertextB64：Base64 编码的密文（包含 16 字节的 GCM tag）
    // 参数 nonce：初始化向量（通常 12 字节）
    // 参数 aad：附加认证数据（可选）
    // 参数 apiV3Key：APIv3 密钥（32 字节）
    // 返回：解密后的明文字符串，失败返回空字符串
    static std::string decryptWxV3(const std::string &ciphertextB64,
                                    const std::string &nonce,
                                    const std::string &aad,
                                    const std::string &apiV3Key) {
        // 检查 APIv3 密钥长度是否为 32 字节
        if (apiV3Key.size() != 32)
            // 如果长度不对，返回空字符串表示失败
            return "";
        // 将 Base64 密文解码为原始字节
        auto raw = RsaUtils::base64Decode(ciphertextB64);
        // 检查解码后的数据长度是否至少为 16 字节（GCM tag）
        if (raw.size() < 16)
            // 如果长度不足，返回空字符串表示失败
            return "";

        // GCM tag 长度（16 字节）
        size_t tagLen = 16;
        // 密文长度（总长度 - tag 长度）
        size_t ctLen = raw.size() - tagLen;
        // 密文指针（指向原始数据的开始）
        const unsigned char *ct = raw.data();
        // GCM tag 指针（指向原始数据的末尾 16 字节）
        const unsigned char *tag = raw.data() + ctLen;

        // 创建 EVP 加密上下文
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        // 检查上下文是否创建成功
        if (!ctx)
            // 如果创建失败，返回空字符串
            return "";

        // 明文结果
        std::string plain;
        // 使用 do-while(false) 实现错误处理
        do {
            // 初始化 AES-256-GCM 解密上下文
            if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
                // 如果初始化失败，跳出循环
                break;
            // 设置 IV 长度
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)nonce.size(), nullptr) != 1)
                // 如果设置失败，跳出循环
                break;
            // 设置密钥和 IV
            if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                reinterpret_cast<const unsigned char*>(apiV3Key.data()),
                reinterpret_cast<const unsigned char*>(nonce.data())) != 1)
                // 如果设置失败，跳出循环
                break;

            // 输出长度
            int outl = 0;
            // 处理附加认证数据（AAD）
            if (!aad.empty()) {
                // 更新 AAD（不产生输出）
                if (EVP_DecryptUpdate(ctx, nullptr, &outl,
                    reinterpret_cast<const unsigned char*>(aad.data()),
                    (int)aad.size()) != 1)
                    // 如果处理失败，跳出循环
                    break;
            }

            // 创建输出缓冲区
            std::vector<unsigned char> out(ctLen + 16);
            // 解密密文
            if (EVP_DecryptUpdate(ctx, out.data(), &outl, ct, (int)ctLen) != 1)
                // 如果解密失败，跳出循环
                break;
            // 记录已输出的字节数
            int total = outl;

            // 设置 GCM tag 用于验证
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)tagLen,
                const_cast<unsigned char*>(tag)) != 1)
                // 如果设置失败，跳出循环
                break;

            // 完成解密并验证 tag
            if (EVP_DecryptFinal_ex(ctx, out.data() + total, &outl) != 1)
                // 如果验证失败或解密失败，跳出循环
                break;
            // 更新总输出长度
            total += outl;

            // 将解密结果转换为字符串
            plain.assign(reinterpret_cast<char*>(out.data()), total);
        } while (false);

        // 释放 EVP 上下文
        EVP_CIPHER_CTX_free(ctx);
        // 返回明文（失败时为空字符串）
        return plain;
    }

    // MPayQR/WePay App 二维码加密方法
    // 加密结果格式：MPAYQR1:<base64url(IV||CT||TAG)>
    // 参数 payload：要加密的数据
    // 参数 passphrase：与 APP 端共享的密码短语
    // 返回：加密后的 MPayQR 格式字符串
    static std::string encryptMpayQr(const std::string &payload,
                                     const std::string &passphrase) {
        // 使用 SHA256 对密码短语进行哈希得到密钥
        unsigned char keyBin[32];
        // 计算 SHA256 哈希
        SHA256(reinterpret_cast<const unsigned char*>(passphrase.data()),
               passphrase.size(), keyBin);
        // 初始化向量（12 字节）
        unsigned char iv[12];
        // 生成随机 IV
        if (RAND_bytes(iv, 12) != 1)
            // 如果生成随机数失败，返回空字符串
            return "";

        // 创建 EVP 加密上下文
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        // 检查上下文是否创建成功
        if (!ctx)
            // 如果创建失败，返回空字符串
            return "";
        // 输出缓冲区（预分配足够的空间）
        std::vector<unsigned char> out(payload.size() + 32);
        // GCM tag 缓冲区（16 字节）
        unsigned char tag[16];
        // 结果字符串
        std::string result;
        // 使用 do-while(false) 实现错误处理
        do {
            // 初始化 AES-256-GCM 加密上下文
            if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
                // 如果初始化失败，跳出循环
                break;
            // 设置 IV 长度为 12 字节
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
                // 如果设置失败，跳出循环
                break;
            // 设置密钥和 IV
            if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, keyBin, iv) != 1)
                // 如果设置失败，跳出循环
                break;
            // 输出长度
            int outl = 0;
            // 加密数据
            if (EVP_EncryptUpdate(ctx, out.data(), &outl,
                reinterpret_cast<const unsigned char*>(payload.data()),
                (int)payload.size()) != 1)
                // 如果加密失败，跳出循环
                break;
            // 记录已输出的字节数
            int total = outl;
            // 完成加密
            if (EVP_EncryptFinal_ex(ctx, out.data() + total, &outl) != 1)
                // 如果完成失败，跳出循环
                break;
            // 更新总输出长度
            total += outl;
            // 获取 GCM tag
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1)
                // 如果获取失败，跳出循环
                break;

            // 构建原始数据：IV || CT || TAG
            std::vector<unsigned char> raw;
            // 预分配空间（IV 12 字节 + 密文 + TAG 16 字节）
            raw.reserve(12 + total + 16);
            // 添加 IV
            raw.insert(raw.end(), iv, iv + 12);
            // 添加密文
            raw.insert(raw.end(), out.data(), out.data() + total);
            // 添加 GCM tag
            raw.insert(raw.end(), tag, tag + 16);

            // 构建最终结果：MPAYQR1:<base64url编码>
            result = "MPAYQR1:" + base64UrlEncode(raw);
        } while (false);
        // 释放 EVP 上下文
        EVP_CIPHER_CTX_free(ctx);
        // 返回结果（失败时为空字符串）
        return result;
    }

// 私有辅助函数区域
private:
    // Base64URL 编码方法
    // 参数 raw：原始字节数据
    // 返回：Base64URL 编码后的字符串
    static std::string base64UrlEncode(const std::vector<unsigned char> &raw) {
        // Base64URL 字母表（使用 - 和 _ 代替 + 和 /）
        static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        // 输出字符串
        std::string out;
        // 原始数据长度
        size_t n = raw.size();
        // 每次处理 3 个字节
        for (size_t i = 0; i < n; i += 3) {
            // 初始化 24 位值（第一个字节左移 16 位）
            unsigned int v = raw[i] << 16;
            // 计算剩余字节数（最多 3 个）
            int rem = (int)std::min<size_t>(3, n - i);
            // 如果有第二个字节，添加到值中
            if (rem > 1)
                v |= raw[i + 1] << 8;
            // 如果有第三个字节，添加到值中
            if (rem > 2)
                v |= raw[i + 2];
            // 输出第一个 Base64 字符（高 6 位）
            out.push_back(T[(v >> 18) & 63]);
            // 输出第二个 Base64 字符（次高 6 位）
            out.push_back(T[(v >> 12) & 63]);
            // 如果有第二个字节，输出第三个 Base64 字符
            if (rem > 1)
                out.push_back(T[(v >> 6) & 63]);
            // 如果有第三个字节，输出第四个 Base64 字符（低 6 位）
            if (rem > 2)
                out.push_back(T[v & 63]);
        }
        // 返回 Base64URL 编码后的字符串
        return out;
    }
// 类定义结束
};
