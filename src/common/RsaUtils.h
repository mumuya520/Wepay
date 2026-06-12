// WePay-Cpp — RSA 签名/验签/加解密工具
// 基于 OpenSSL EVP API，支持:
//   - RSA-SHA256 签名/验签 (微信V3、支付宝RSA2)
//   - Base64 编码/解码
//   - PEM 格式私钥/公钥加载
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <stdexcept> // 异常库
#include <openssl/evp.h> // OpenSSL EVP 库
#include <openssl/pem.h> // OpenSSL PEM 库
#include <openssl/rsa.h> // OpenSSL RSA 库
#include <openssl/bio.h> // OpenSSL BIO 库
#include <openssl/buffer.h> // OpenSSL 缓冲区库
#include <openssl/err.h> // OpenSSL 错误库

// RSA 签名验证工具类，支持微信支付 V3 和支付宝 RSA 签名
class RsaUtils {
public:
    // ── Base64 编码解码 ──────────────────────────────────────────────────
    // Base64 编码方法（字符串版本）
    // 参数 data：输入数据字符串
    // 返回：Base64 编码后的字符串
    static std::string base64Encode(const std::string &data) {
        // 调用字节数组版本的 base64Encode
        return base64Encode(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    }

    // Base64 编码方法（字节数组版本）
    // 参数 data：输入字节数组指针
    // 参数 len：输入数据长度
    // 返回：Base64 编码后的字符串
    static std::string base64Encode(const unsigned char *data, size_t len) {
        // 创建 Base64 过滤 BIO
        BIO *b64 = BIO_new(BIO_f_base64());
        // 设置 Base64 BIO 标志（不添加换行符）
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        // 创建内存 BIO
        BIO *mem = BIO_new(BIO_s_mem());
        // 将 Base64 BIO 和内存 BIO 连接
        b64 = BIO_push(b64, mem);
        // 写入数据到 BIO
        BIO_write(b64, data, (int)len);
        // 刷新 BIO
        BIO_flush(b64);
        // 获取内存 BIO 中的数据指针
        BUF_MEM *bptr;
        BIO_get_mem_ptr(b64, &bptr);
        // 将编码结果转换为字符串
        std::string result(bptr->data, bptr->length);
        // 释放所有 BIO
        BIO_free_all(b64);
        // 返回 Base64 编码后的字符串
        return result;
    }

    // Base64 解码方法
    // 参数 b64：Base64 编码的字符串
    // 返回：解码后的字节数组
    static std::vector<unsigned char> base64Decode(const std::string &b64) {
        // 创建内存 BIO 用于读取 Base64 数据
        BIO *bio = BIO_new_mem_buf(b64.data(), (int)b64.size());
        // 创建 Base64 过滤 BIO
        BIO *b64f = BIO_new(BIO_f_base64());
        // 设置 Base64 BIO 标志（不处理换行符）
        BIO_set_flags(b64f, BIO_FLAGS_BASE64_NO_NL);
        // 将 Base64 BIO 和内存 BIO 连接
        bio = BIO_push(b64f, bio);
        // 创建输出缓冲区（预分配足够的空间）
        std::vector<unsigned char> out(b64.size());
        // 从 BIO 读取解码数据
        int n = BIO_read(bio, out.data(), (int)out.size());
        // 释放所有 BIO
        BIO_free_all(bio);
        // 检查读取是否成功
        if (n <= 0)
            // 如果读取失败，返回空向量
            return {};
        // 调整输出向量大小为实际读取的字节数
        out.resize(n);
        // 返回解码后的字节数组
        return out;
    }

    // ── 加载 PEM 私钥(RSA) ──────────────────────────────────────
    // 加载 RSA 私钥（PEM 格式）
    // 参数 pemKey：PEM 格式的私钥字符串（可以是完整 PEM 或去掉头尾的 Base64）
    // 返回：EVP_PKEY 指针（需要调用 EVP_PKEY_free 释放）
    static EVP_PKEY* loadPrivateKey(const std::string &pemKey) {
        // 规范化 PEM 格式（添加头尾）
        std::string pem = normalizePem(pemKey, "PRIVATE KEY");
        // 创建内存 BIO 用于读取 PEM 数据
        BIO *bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
        // 检查 BIO 是否创建成功
        if (!bio)
            // 如果创建失败，返回 nullptr
            return nullptr;
        // 从 PEM 格式读取私钥
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        // 释放 BIO
        BIO_free(bio);
        // 返回 EVP_PKEY 指针
        return pkey;
    }

    // 加载 RSA 公钥（PEM 格式）
    // 参数 pemKey：PEM 格式的公钥字符串（可以是完整 PEM 或去掉头尾的 Base64）
    // 返回：EVP_PKEY 指针（需要调用 EVP_PKEY_free 释放）
    static EVP_PKEY* loadPublicKey(const std::string &pemKey) {
        // 规范化 PEM 格式（添加头尾）
        std::string pem = normalizePem(pemKey, "PUBLIC KEY");
        // 创建内存 BIO 用于读取 PEM 数据
        BIO *bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
        // 检查 BIO 是否创建成功
        if (!bio)
            // 如果创建失败，返回 nullptr
            return nullptr;
        // 从 PEM 格式读取公钥
        EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        // 释放 BIO
        BIO_free(bio);
        // 返回 EVP_PKEY 指针
        return pkey;
    }

    // ── RSA-SHA256 签名 → Base64 ────────────────────────────────
    // 用于微信支付 V3 (WECHATPAY2-SHA256-RSA2048) 和支付宝 RSA2
    // RSA-SHA256 签名方法
    // 参数 data：要签名的数据
    // 参数 privateKeyPem：RSA 私钥（PEM 格式）
    // 返回：Base64 编码的签名字符串，失败返回空字符串
    static std::string signSha256(const std::string &data, const std::string &privateKeyPem) {
        // 加载私钥
        EVP_PKEY *pkey = loadPrivateKey(privateKeyPem);
        // 检查私钥是否加载成功
        if (!pkey)
            // 如果加载失败，返回空字符串
            return "";
        // 创建消息摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 签名结果
        std::string result;
        // 检查上下文是否创建成功，并初始化签名操作
        if (ctx && EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
            // 更新签名数据
            EVP_DigestSignUpdate(ctx, data.data(), data.size()) == 1) {
            // 获取签名长度
            size_t siglen = 0;
            EVP_DigestSignFinal(ctx, nullptr, &siglen);
            // 创建签名缓冲区
            std::vector<unsigned char> sig(siglen);
            // 生成签名
            if (EVP_DigestSignFinal(ctx, sig.data(), &siglen) == 1) {
                // 将签名转换为 Base64 编码
                result = base64Encode(sig.data(), siglen);
            }
        }
        // 释放上下文
        if (ctx)
            EVP_MD_CTX_free(ctx);
        // 释放私钥
        EVP_PKEY_free(pkey);
        // 返回签名结果
        return result;
    }

    // ── RSA-SHA256 验签 ─────────────────────────────────────────
    // RSA-SHA256 验签方法
    // 参数 data：原始数据
    // 参数 signB64：Base64 编码的签名
    // 参数 publicKeyPem：RSA 公钥（PEM 格式）
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verifySha256(const std::string &data, const std::string &signB64,
                              const std::string &publicKeyPem) {
        // 加载公钥
        EVP_PKEY *pkey = loadPublicKey(publicKeyPem);
        // 检查公钥是否加载成功
        if (!pkey)
            // 如果加载失败，返回 false
            return false;
        // 解码 Base64 签名
        auto sig = base64Decode(signB64);
        // 检查解码是否成功
        if (sig.empty()) {
            // 释放公钥并返回 false
            EVP_PKEY_free(pkey);
            return false;
        }

        // 创建消息摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 验证结果
        bool ok = false;
        // 检查上下文是否创建成功，并初始化验证操作
        if (ctx && EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
            // 更新验证数据
            EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1) {
            // 执行最终验证
            ok = (EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1);
        }
        // 释放上下文
        if (ctx)
            EVP_MD_CTX_free(ctx);
        // 释放公钥
        EVP_PKEY_free(pkey);
        // 返回验证结果
        return ok;
    }

    // ── RSA-SHA1 签名/验签 (支付宝 RSA) ──────────────────────────
    // RSA-SHA1 签名方法（支付宝使用）
    // 参数 data：要签名的数据
    // 参数 privateKeyPem：RSA 私钥（PEM 格式）
    // 返回：Base64 编码的签名字符串，失败返回空字符串
    static std::string signSha1(const std::string &data, const std::string &privateKeyPem) {
        // 加载私钥
        EVP_PKEY *pkey = loadPrivateKey(privateKeyPem);
        // 检查私钥是否加载成功
        if (!pkey)
            // 如果加载失败，返回空字符串
            return "";
        // 创建消息摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 签名结果
        std::string result;
        // 检查上下文是否创建成功，并初始化签名操作（使用 SHA1）
        if (ctx && EVP_DigestSignInit(ctx, nullptr, EVP_sha1(), nullptr, pkey) == 1 &&
            // 更新签名数据
            EVP_DigestSignUpdate(ctx, data.data(), data.size()) == 1) {
            // 获取签名长度
            size_t siglen = 0;
            EVP_DigestSignFinal(ctx, nullptr, &siglen);
            // 创建签名缓冲区
            std::vector<unsigned char> sig(siglen);
            // 生成签名
            if (EVP_DigestSignFinal(ctx, sig.data(), &siglen) == 1) {
                // 将签名转换为 Base64 编码
                result = base64Encode(sig.data(), siglen);
            }
        }
        // 释放上下文
        if (ctx)
            EVP_MD_CTX_free(ctx);
        // 释放私钥
        EVP_PKEY_free(pkey);
        // 返回签名结果
        return result;
    }

    // RSA-SHA1 验签方法（支付宝使用）
    // 参数 data：原始数据
    // 参数 signB64：Base64 编码的签名
    // 参数 publicKeyPem：RSA 公钥（PEM 格式）
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verifySha1(const std::string &data, const std::string &signB64,
                            const std::string &publicKeyPem) {
        // 加载公钥
        EVP_PKEY *pkey = loadPublicKey(publicKeyPem);
        // 检查公钥是否加载成功
        if (!pkey)
            // 如果加载失败，返回 false
            return false;
        // 解码 Base64 签名
        auto sig = base64Decode(signB64);
        // 检查解码是否成功
        if (sig.empty()) {
            // 释放公钥并返回 false
            EVP_PKEY_free(pkey);
            return false;
        }
        // 创建消息摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 验证结果
        bool ok = false;
        // 检查上下文是否创建成功，并初始化验证操作（使用 SHA1）
        if (ctx && EVP_DigestVerifyInit(ctx, nullptr, EVP_sha1(), nullptr, pkey) == 1 &&
            // 更新验证数据
            EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1) {
            // 执行最终验证
            ok = (EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1);
        }
        // 释放上下文
        if (ctx)
            EVP_MD_CTX_free(ctx);
        // 释放公钥
        EVP_PKEY_free(pkey);
        // 返回验证结果
        return ok;
    }

// 私有辅助函数区域
private:
    // 规范化 PEM 格式
    // 若传入的不包含 -----BEGIN 头尾，则按 64 字符换行并补充头尾
    // 参数 key：PEM 密钥字符串（可以是完整 PEM 或去掉头尾的 Base64）
    // 参数 keyType：密钥类型（"PRIVATE KEY" 或 "PUBLIC KEY"）
    // 返回：规范化后的 PEM 格式字符串
    static std::string normalizePem(const std::string &key, const std::string &keyType) {
        // 检查是否已经是 PEM 格式（包含 -----BEGIN）
        if (key.find("-----BEGIN") != std::string::npos)
            // 如果已经是 PEM 格式，直接返回
            return key;
        // 构建 PEM 格式的 body（按 64 字符换行）
        std::string body;
        // 每次处理 64 个字符
        for (size_t i = 0; i < key.size(); i += 64) {
            // 添加 64 个字符的子字符串
            body += key.substr(i, 64);
            // 添加换行符
            body += "\n";
        }
        // 构建 PEM 头
        std::string head = "-----BEGIN " + keyType + "-----\n";
        // 构建 PEM 尾
        std::string tail = "-----END "   + keyType + "-----\n";
        // 返回完整的 PEM 格式字符串
        return head + body + tail;
    }
// 类定义结束
};
