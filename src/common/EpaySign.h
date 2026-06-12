// WePay-Cpp — 易支付签名工具
#pragma once // 防止头文件重复包含
#include <drogon/HttpRequest.h> // Drogon HTTP 请求
#include <string> // 字符串库
#include <map> // 映射容器
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式化库
#include <cctype> // 字符类型库
#include <vector> // 向量容器
#include "Md5Utils.h"

#ifdef WEPAY_HAS_OPENSSL
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#endif

// 易支付兼容签名工具
// 支持 MD5 和 RSA-SHA256 两种签名方式
// MD5 签名：sign = MD5( ksort(params, exclude sign/sign_type/empty) joined as "k=v&k=v" + key )
// RSA 签名：sign = RSA_SHA256_Base64( ksort(params) joined as "k=v&k=v" )
class EpaySign {
public:
    // 构建待签名字符串
    // 按 key 字母顺序排序，排除 sign、sign_type 和空值
    // 参数 params：参数映射
    // 返回：待签名字符串（格式：k1=v1&k2=v2&...）
    static std::string buildSignStr(const std::map<std::string, std::string> &params) {
        // 收集非空的参数（排除 sign 和 sign_type）
        std::vector<std::pair<std::string, std::string>> items;
        // 遍历所有参数
        for (const auto &[k, v] : params) {
            // 排除 sign、sign_type 和空值
            if (k != "sign" && k != "sign_type" && !v.empty()) {
                // 添加到待处理列表
                items.push_back({k, v});
            }
        }
        
        // 按 key 字母顺序排序
        std::sort(items.begin(), items.end(), 
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        
        // 构建签名字符串
        std::string str;
        // 遍历排序后的参数
        for (const auto &[k, v] : items) {
            // 添加 & 分隔符（除了第一个参数）
            if (!str.empty())
                str += "&";
            // 拼接 k=v
            str += k + "=" + v;
        }
        // 返回签名字符串
        return str;
    }

    // MD5 签名
    // 参数 params：参数映射
    // 参数 key：签名密钥
    // 返回：MD5 签名值（32 位十六进制字符串）
    static std::string sign(std::map<std::string, std::string> params,
                             const std::string &key) {
        // 构建待签名字符串
        std::string str = buildSignStr(params);
        // 添加密钥到末尾
        str += key;
        // 计算 MD5 哈希
        return Md5Utils::md5(str);
    }

    // RSA-SHA256 签名
    // 参数 params：参数映射
    // 参数 privateKeyPem：PEM 格式的私钥
    // 返回：Base64 编码的签名值
    static std::string signRsa(const std::map<std::string, std::string> &params,
                                const std::string &privateKeyPem) {
// 如果编译了 OpenSSL 支持
#ifdef WEPAY_HAS_OPENSSL
        // 构建待签名字符串
        std::string str = buildSignStr(params);
        // 创建内存 BIO 用于读取私钥
        BIO *bio = BIO_new_mem_buf(privateKeyPem.data(), (int)privateKeyPem.size());
        // 检查 BIO 创建是否成功
        if (!bio)
            return "";
        // 从 BIO 中读取私钥
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        // 释放 BIO
        BIO_free(bio);
        // 检查私钥读取是否成功
        if (!pkey)
            return "";

        // 创建摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 签名结果
        std::string result;
        // 初始化签名操作（使用 SHA256）
        if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1) {
            // 更新待签名数据
            EVP_DigestSignUpdate(ctx, str.data(), str.size());
            // 获取签名长度
            size_t sigLen = 0;
            EVP_DigestSignFinal(ctx, nullptr, &sigLen);
            // 创建签名缓冲区
            std::vector<unsigned char> sig(sigLen);
            // 生成签名
            EVP_DigestSignFinal(ctx, sig.data(), &sigLen);
            // 对签名进行 Base64 编码
            result = base64Encode(sig.data(), sigLen);
        }
        // 释放摘要上下文
        EVP_MD_CTX_free(ctx);
        // 释放私钥
        EVP_PKEY_free(pkey);
        // 返回签名结果
        return result;
// 如果未编译 OpenSSL 支持
#else
        // 返回空字符串
        return "";
#endif
    }

    // RSA-SHA256 验签
    // 参数 params：参数映射
    // 参数 publicKeyPem：PEM 格式的公钥
    // 参数 signBase64：Base64 编码的签名值
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verifyRsa(const std::map<std::string, std::string> &params,
                           const std::string &publicKeyPem,
                           const std::string &signBase64) {
        // 构建待签名字符串
        std::string str = buildSignStr(params);
        // 对签名进行 Base64 解码
        auto sigBytes = base64Decode(signBase64);
        // 检查解码是否成功
        if (sigBytes.empty())
            return false;

        // 创建内存 BIO 用于读取公钥
        BIO *bio = BIO_new_mem_buf(publicKeyPem.data(), (int)publicKeyPem.size());
        // 检查 BIO 创建是否成功
        if (!bio)
            return false;
        // 从 BIO 中读取公钥
        EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        // 释放 BIO
        BIO_free(bio);
        // 检查公钥读取是否成功
        if (!pkey)
            return false;

        // 创建摘要上下文
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        // 验签结果
        bool ok = false;
        // 初始化验签操作（使用 SHA256）
        if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1) {
            // 更新待验签数据
            EVP_DigestVerifyUpdate(ctx, str.data(), str.size());
            // 执行验签，返回 1 表示签名有效
            ok = (EVP_DigestVerifyFinal(ctx, sigBytes.data(), sigBytes.size()) == 1);
        }
        // 释放摘要上下文
        EVP_MD_CTX_free(ctx);
        // 释放公钥
        EVP_PKEY_free(pkey);
        // 返回验签结果
        return ok;
    }

    // 从 Drogon HTTP 请求中提取全部参数
    // 包括 query string、form 和 JSON body 中的参数
    // 参数 req：HTTP 请求对象
    // 返回：参数映射
    static std::map<std::string, std::string>
    paramsFromRequest(const drogon::HttpRequestPtr &req) {
        // 创建参数映射
        std::map<std::string, std::string> m;
        // 获取 query string 参数
        auto &qmap = req->getParameters();
        // 添加 query 参数到映射
        for (auto &[k, v] : qmap)
            m[k] = v;
        // 尝试获取 JSON body
        auto body = req->getJsonObject();
        // 如果 JSON body 存在
        if (body) {
            // 遍历 JSON 对象的所有成员
            for (auto &k : body->getMemberNames()) {
                // 如果是字符串类型
                if ((*body)[k].isString())
                    m[k] = (*body)[k].asString();
                // 如果是数值类型
                else if ((*body)[k].isNumeric())
                    m[k] = (*body)[k].asString();
                // 其他类型转换为字符串
                else
                    m[k] = (*body)[k].toStyledString();
            }
        }
        // 返回参数映射
        return m;
    }

    // 验证 HTTP 请求签名
    // 自动检测 sign_type，选择 MD5 或 RSA 验签
    // 参数 req：HTTP 请求对象
    // 参数 key：MD5 签名密钥
    // 参数 publicKeyPem：RSA 公钥（可选）
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verify(const drogon::HttpRequestPtr &req, const std::string &key,
                       const std::string &publicKeyPem = "") {
        // 从请求中提取参数
        auto params = paramsFromRequest(req);
        // 查找 sign 参数
        auto it = params.find("sign");
        // 如果 sign 参数不存在，返回 false
        if (it == params.end())
            return false;
        // 获取签名值
        std::string reqSign = it->second;
        // 调用自动验签方法
        return verifyAuto(params, key, publicKeyPem, reqSign);
    }

    // 验证参数签名（重载版本）
    // 参数 params：参数映射
    // 参数 key：MD5 签名密钥
    // 参数 reqSign：请求中的签名值
    // 参数 publicKeyPem：RSA 公钥（可选）
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verify(const std::map<std::string, std::string> &params,
                       const std::string &key,
                       const std::string &reqSign,
                       const std::string &publicKeyPem = "") {
        // 调用自动验签方法
        return verifyAuto(params, key, publicKeyPem, reqSign);
    }

    // 自动选择验签方式
    // 如果 sign_type=RSA，则使用 RSA 验签；否则使用 MD5 验签
    // 参数 params：参数映射
    // 参数 key：MD5 签名密钥
    // 参数 publicKeyPem：RSA 公钥
    // 参数 reqSign：请求中的签名值
    // 返回：true 表示签名有效，false 表示签名无效
    static bool verifyAuto(const std::map<std::string, std::string> &params,
                            const std::string &key,
                            const std::string &publicKeyPem,
                            const std::string &reqSign) {
        // 查找 sign_type 参数
        auto st = params.find("sign_type");
        // 如果 sign_type 为 RSA（不区分大小写）
        if (st != params.end() && (st->second == "RSA" || st->second == "rsa")) {
            // 检查公钥是否提供
            if (publicKeyPem.empty())
                return false;
            // 使用 RSA 验签
            return verifyRsa(params, publicKeyPem, reqSign);
        }
        // 使用 MD5 验签
        return sign(params, key) == reqSign;
    }

    // 构建易支付通知 URL
    // 按易支付通知格式构建 URL，包括签名
    // 参数 baseUrl：基础 URL
    // 参数 pid：商户 ID
    // 参数 tradeNo：系统交易号
    // 参数 outTradeNo：商户订单号
    // 参数 type：支付类型
    // 参数 name：商品名称
    // 参数 money：支付金额
    // 参数 param：自定义参数
    // 参数 key：签名密钥
    // 返回：完整的通知 URL
    static std::string buildNotifyUrl(const std::string &baseUrl,
                                       const std::string &pid,
                                       const std::string &tradeNo,
                                       const std::string &outTradeNo,
                                       const std::string &type,
                                       const std::string &name,
                                       const std::string &money,
                                       const std::string &param,
                                       const std::string &key) {
        // 创建参数映射
        std::map<std::string, std::string> m;
        // 设置支付金额
        m["money"]         = money;
        // 设置商品名称
        m["name"]          = name;
        // 设置商户订单号
        m["out_trade_no"]  = outTradeNo;
        // 设置自定义参数
        m["param"]         = param;
        // 设置商户 ID
        m["pid"]           = pid;
        // 设置系统交易号
        m["trade_no"]      = tradeNo;
        // 设置交易状态为成功
        m["trade_status"]  = "TRADE_SUCCESS";
        // 设置支付类型
        m["type"]          = type;

        // 对参数进行 MD5 签名
        std::string s = sign(m, key);
        // 添加签名到参数
        m["sign"]      = s;
        // 设置签名类型为 MD5
        m["sign_type"] = "MD5";

        // 构建查询字符串
        std::string query;
        // 遍历参数
        for (auto &[k, v] : m) {
            // 添加 & 分隔符（除了第一个参数）
            if (!query.empty())
                query += "&";
            // 拼接 k=v（值进行 URL 编码）
            query += k + "=" + urlEncode(v);
        }

        // 判断 URL 中是否已有查询参数，选择 ? 或 & 作为分隔符
        std::string sep = (baseUrl.find('?') == std::string::npos) ? "?" : "&";
        // 返回完整的通知 URL
        return baseUrl + sep + query;
    }

    // 支付类型字符串与整数互转
    // wxpay/wechat/weixin/1 → 1
    // alipay/zfb/2 → 2
    // 参数 t：支付类型字符串
    // 返回：支付类型整数（1=微信，2=支付宝，0=未知）
    static int typeToInt(const std::string &t) {
        // 检查微信支付
        if (t == "1" || t == "wxpay" || t == "wechat" || t == "weixin")
            return 1;
        // 检查支付宝支付
        if (t == "2" || t == "alipay" || t == "zfb")
            return 2;
        // 未知类型
        return 0;
    }

    // 从字符串获取支付类型整数
    // 参数 t：支付类型字符串
    // 返回：支付类型整数
    static int typeFromStr(const std::string &t) {
        return typeToInt(t);
    }

    // 支付类型整数转字符串
    // 1 → "wxpay"
    // 2 → "alipay"
    // 参数 t：支付类型整数
    // 返回：支付类型字符串
    static std::string typeToStr(int t) {
        // 微信支付
        if (t == 1)
            return "wxpay";
        // 支付宝支付
        if (t == 2)
            return "alipay";
        // 未知类型
        return "unknown";
    }

private:
    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                oss << c;
            else
                oss << '%' << std::uppercase << std::hex
                    << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }

    // Base64 编码/解码 (RSA 签名用)
    static std::string base64Encode(const unsigned char *data, size_t len) {
        static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string r;
        r.reserve(((len + 2) / 3) * 4);
        for (size_t i = 0; i < len; i += 3) {
            unsigned b = (data[i] << 16) | (i + 1 < len ? data[i + 1] << 8 : 0) | (i + 2 < len ? data[i + 2] : 0);
            r += t[(b >> 18) & 0x3F];
            r += t[(b >> 12) & 0x3F];
            r += (i + 1 < len) ? t[(b >> 6) & 0x3F] : '=';
            r += (i + 2 < len) ? t[b & 0x3F] : '=';
        }
        return r;
    }

    static std::vector<unsigned char> base64Decode(const std::string &s) {
        static const int dt[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
        };
        std::vector<unsigned char> r;
        r.reserve(s.size() * 3 / 4);
        int val = 0, bits = -8;
        for (unsigned char c : s) {
            if (dt[c] == -1) break;
            val = (val << 6) + dt[c];
            bits += 6;
            if (bits >= 0) { r.push_back((val >> bits) & 0xFF); bits -= 8; }
        }
        return r;
    }
};
