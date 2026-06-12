// MD5 和 HMAC-SHA256 签名工具 — 兼容 V免签和 WePay 原生协议
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <sstream> // 字符串流
#include <iomanip> // 输入输出格式化
#include <openssl/evp.h> // OpenSSL EVP 库
#include <openssl/hmac.h> // OpenSSL HMAC 库

// MD5 和 HMAC-SHA256 工具类，支持 V免签和 WePay 原生协议签名
class Md5Utils {
public:
    // MD5 哈希方法
    // 参数 input：输入数据
    // 返回：MD5 哈希值的十六进制字符串
    static std::string md5(const std::string &input) {
        // 摘要缓冲区（最大摘要大小）
        unsigned char digest[EVP_MAX_MD_SIZE];
        // 摘要长度
        unsigned int  digestLen = 0;

        // 创建 EVP 消息摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 初始化 MD5 算法
        EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
        // 更新摘要数据
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

    // V免签订单签名方法
    // 签名格式：md5("payId=X&param=X&type=X&price=X&key=X")
    // 参数 payId：支付 ID
    // 参数 param：参数
    // 参数 type：支付类型
    // 参数 price：金额
    // 参数 key：密钥
    // 返回：签名字符串
    static std::string orderSign(const std::string &payId, const std::string &param,
                                  const std::string &type, const std::string &price,
                                  const std::string &key) {
        // 拼接签名数据：payId=X&param=X&type=X&price=X&key=X
        return md5("payId=" + payId + "&param=" + param +
                   "&type=" + type + "&price=" + price + "&key=" + key);
    }

    // V免签通知回调签名方法
    // 签名格式：md5(payId + param + type + price + reallyPrice + key)
    // 参数 payId：支付 ID
    // 参数 param：参数
    // 参数 type：支付类型
    // 参数 price：订单金额
    // 参数 reallyPrice：实际支付金额
    // 参数 key：密钥
    // 返回：签名字符串
    static std::string notifySign(const std::string &payId, const std::string &param,
                                   const std::string &type, const std::string &price,
                                   const std::string &reallyPrice, const std::string &key) {
        // 拼接签名数据：payId + param + type + price + reallyPrice + key
        return md5(payId + param + type + price + reallyPrice + key);
    }

    // V免签监控端心跳签名方法
    // 签名格式：md5(t + key)
    // 参数 t：时间戳
    // 参数 key：密钥
    // 返回：签名字符串
    static std::string heartSign(const std::string &t, const std::string &key) {
        // 拼接签名数据：t + key
        return md5(t + key);
    }

    // V免签监控端推送签名方法
    // 签名格式：md5(type + price + t + key)
    // 参数 type：支付类型
    // 参数 price：金额
    // 参数 t：时间戳
    // 参数 key：密钥
    // 返回：签名字符串
    static std::string pushSign(const std::string &type, const std::string &price,
                                 const std::string &t, const std::string &key) {
        // 拼接签名数据：type + price + t + key
        return md5(type + price + t + key);
    }

    // ── WePay 原生协议: HMAC-SHA256 ─────────────────────────────
    // HMAC-SHA256 哈希方法
    // 参数 key：密钥
    // 参数 data：数据
    // 返回：HMAC-SHA256 哈希值的十六进制字符串
    static std::string hmacSha256(const std::string &key, const std::string &data) {
        // 摘要缓冲区（最大摘要大小）
        unsigned char digest[EVP_MAX_MD_SIZE];
        // 摘要长度
        unsigned int  digestLen = 0;
        // 使用 OpenSSL 计算 HMAC-SHA256
        HMAC(EVP_sha256(), key.c_str(), (int)key.size(),
             (const unsigned char *)data.c_str(), data.size(),
             digest, &digestLen);
        // 创建字符串流用于十六进制转换
        std::ostringstream oss;
        // 遍历摘要中的每个字节
        for (unsigned int i = 0; i < digestLen; ++i)
            // 将字节转换为两位十六进制字符（小写）
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
        // 返回十六进制字符串
        return oss.str();
    }

    // WePay 心跳签名方法
    // 签名格式：hmac_sha256(key, "heart\nt\ndevice_id\nnonce")
    // 参数 t：时间戳
    // 参数 deviceId：设备 ID
    // 参数 nonce：随机数
    // 参数 key：密钥
    // 返回：签名字符串
    static std::string wepayHeartSign(const std::string &t, const std::string &deviceId,
                                       const std::string &nonce, const std::string &key) {
        // 拼接签名数据：heart\nt\ndevice_id\nnonce
        return hmacSha256(key, "heart\n" + t + "\n" + deviceId + "\n" + nonce);
    }

    // WePay 推送签名方法
    // 签名格式：hmac_sha256(key, "push\ntype\nprice\nt\norder_id\nnonce")
    // 参数 type：支付类型
    // 参数 price：金额
    // 参数 t：时间戳
    // 参数 orderId：订单 ID
    // 参数 nonce：随机数
    // 参数 key：密钥
    // 返回：签名字符串
    static std::string wepayPushSign(const std::string &type, const std::string &price,
                                      const std::string &t, const std::string &orderId,
                                      const std::string &nonce, const std::string &key) {
        // 拼接签名数据：push\ntype\nprice\nt\norder_id\nnonce
        return hmacSha256(key, "push\n" + type + "\n" + price + "\n" + t +
                          "\n" + orderId + "\n" + nonce);
    }

    // WePay 拉单签名方法
    // 签名格式：hmac_sha256(key, "pending\nt\ndevice_id\nnonce")
    // 参数 t：时间戳
    // 参数 deviceId：设备 ID
    // 参数 nonce：随机数
    // 参数 key：密钥
    // 返回：签名字符串
    static std::string wepayPendingSign(const std::string &t, const std::string &deviceId,
                                         const std::string &nonce, const std::string &key) {
        // 拼接签名数据：pending\nt\ndevice_id\nnonce
        return hmacSha256(key, "pending\n" + t + "\n" + deviceId + "\n" + nonce);
    }

    // URL 编码方法（RFC 3986 标准）
    // 参数 s：要编码的字符串
    // 返回：URL 编码后的字符串
    static std::string urlEncode(const std::string &s) {
        // 创建字符串流用于构建编码结果
        std::ostringstream oss;
        // 遍历字符串中的每个字符
        for (unsigned char c : s) {
            // 检查字符是否为不需要编码的字符（字母、数字、-._~）
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                // 直接输出字符
                oss << c;
            } else {
                // 将字符转换为 %XX 格式（XX 为十六进制）
                oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
            }
        }
        // 返回编码后的字符串
        return oss.str();
    }
// 类定义结束
};
