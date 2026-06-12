// WePay-Cpp — Ed25519 签名服务
#pragma once // 防止头文件重复包含

#include <string> // 字符串库
#include <memory> // 智能指针
#include <openssl/evp.h> // OpenSSL EVP 库
#include <openssl/rand.h> // OpenSSL 随机数库
#include <openssl/bio.h> // OpenSSL BIO 库
#include <openssl/buffer.h> // OpenSSL 缓冲区库
#include <openssl/err.h>
#include <drogon/utils/Utilities.h>

// Ed25519 密钥管理和签名验证服务（使用 OpenSSL）
class Ed25519Service {
public:
    // 密钥对结构
    struct KeyPair {
        std::string publicKey;   // Base64 编码的公钥
        std::string privateKey;  // Base64 编码的私钥
    };

    // 生成 Ed25519 密钥对
    static KeyPair generateKeyPair() {
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        
        if (!ctx) {
            LOG_ERROR << "Failed to create EVP_PKEY_CTX for Ed25519";
            return {"", ""};
        }

        if (EVP_PKEY_keygen_init(ctx) <= 0) {
            LOG_ERROR << "Failed to initialize keygen: " << ERR_error_string(ERR_get_error(), nullptr);
            EVP_PKEY_CTX_free(ctx);
            return {"", ""};
        }

        if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
            LOG_ERROR << "Failed to generate Ed25519 key: " << ERR_error_string(ERR_get_error(), nullptr);
            EVP_PKEY_CTX_free(ctx);
            return {"", ""};
        }

        EVP_PKEY_CTX_free(ctx);

        // 提取公钥
        size_t publen = 0;
        if (EVP_PKEY_get_raw_public_key(pkey, nullptr, &publen) <= 0) {
            LOG_ERROR << "Failed to get public key length";
            EVP_PKEY_free(pkey);
            return {"", ""};
        }

        std::string pubKeyRaw(publen, '\0');
        if (EVP_PKEY_get_raw_public_key(pkey, (unsigned char*)pubKeyRaw.data(), &publen) <= 0) {
            LOG_ERROR << "Failed to extract public key";
            EVP_PKEY_free(pkey);
            return {"", ""};
        }

        // 提取私钥
        size_t privlen = 0;
        if (EVP_PKEY_get_raw_private_key(pkey, nullptr, &privlen) <= 0) {
            LOG_ERROR << "Failed to get private key length";
            EVP_PKEY_free(pkey);
            return {"", ""};
        }

        std::string privKeyRaw(privlen, '\0');
        if (EVP_PKEY_get_raw_private_key(pkey, (unsigned char*)privKeyRaw.data(), &privlen) <= 0) {
            LOG_ERROR << "Failed to extract private key";
            EVP_PKEY_free(pkey);
            return {"", ""};
        }

        EVP_PKEY_free(pkey);

        KeyPair result;
        result.publicKey = base64Encode(pubKeyRaw);
        result.privateKey = base64Encode(privKeyRaw);

        LOG_INFO << "Ed25519 key pair generated successfully";
        return result;
    }

    // 签名验证
    static bool verifySignature(const std::string& message, 
                               const std::string& signatureB64,
                               const std::string& publicKeyB64) {
        try {
            // Base64 解码
            std::string signature = base64Decode(signatureB64);
            std::string publicKeyRaw = base64Decode(publicKeyB64);

            if (signature.empty() || publicKeyRaw.empty()) {
                LOG_ERROR << "Failed to decode signature or public key";
                return false;
            }

            // 创建 EVP_PKEY 从原始公钥
            EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(
                EVP_PKEY_ED25519, 
                nullptr,
                (const unsigned char*)publicKeyRaw.data(),
                publicKeyRaw.size()
            );

            if (!pkey) {
                LOG_ERROR << "Failed to create EVP_PKEY from public key: " << ERR_error_string(ERR_get_error(), nullptr);
                return false;
            }

            // 创建验证上下文
            EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
            if (!mdctx) {
                EVP_PKEY_free(pkey);
                return false;
            }

            // 验证签名
            int result = EVP_DigestVerifyInit(mdctx, nullptr, nullptr, nullptr, pkey);
            if (result <= 0) {
                LOG_ERROR << "EVP_DigestVerifyInit failed: " << ERR_error_string(ERR_get_error(), nullptr);
                EVP_MD_CTX_free(mdctx);
                EVP_PKEY_free(pkey);
                return false;
            }

            result = EVP_DigestVerifyUpdate(mdctx, message.data(), message.size());
            if (result <= 0) {
                LOG_ERROR << "EVP_DigestVerifyUpdate failed: " << ERR_error_string(ERR_get_error(), nullptr);
                EVP_MD_CTX_free(mdctx);
                EVP_PKEY_free(pkey);
                return false;
            }

            result = EVP_DigestVerifyFinal(
                mdctx,
                (const unsigned char*)signature.data(),
                signature.size()
            );

            EVP_MD_CTX_free(mdctx);
            EVP_PKEY_free(pkey);

            if (result == 1) {
                LOG_INFO << "Signature verification successful";
                return true;
            } else if (result == 0) {
                LOG_WARN << "Signature verification failed: signature does not match";
                return false;
            } else {
                LOG_ERROR << "Signature verification error: " << ERR_error_string(ERR_get_error(), nullptr);
                return false;
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "Signature verification exception: " << e.what();
            return false;
        }
    }

private:
    // Base64 编码
    static std::string base64Encode(const std::string& data) {
        BIO *bio, *b64;
        BUF_MEM *buffer_ptr;

        b64 = BIO_new(BIO_f_base64());
        bio = BIO_new(BIO_s_mem());
        bio = BIO_push(b64, bio);

        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bio, data.data(), data.size());
        BIO_flush(bio);
        BIO_get_mem_ptr(bio, &buffer_ptr);

        std::string result(buffer_ptr->data, buffer_ptr->length);
        BIO_free_all(bio);

        return result;
    }

    // Base64 解码
    static std::string base64Decode(const std::string& data) {
        BIO *bio, *b64;
        std::string result(data.size(), '\0');

        bio = BIO_new_mem_buf(data.data(), data.size());
        b64 = BIO_new(BIO_f_base64());
        bio = BIO_push(b64, bio);

        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        int decoded_size = BIO_read(bio, (void*)result.data(), data.size());
        BIO_free_all(bio);

        if (decoded_size > 0) {
            result.resize(decoded_size);
        } else {
            result.clear();
        }

        return result;
    }
};
