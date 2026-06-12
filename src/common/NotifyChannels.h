// WePay-Cpp — 通知通道服务 (SMS / Email / OSS)
// 提供统一接口，默认 stub 实现（仅打印日志），生产环境可对接真实服务。
//
// 配置示例 (config.json):
//   "sms": {
//     "enabled": false,
//     "provider": "aliyun",
//     "access_key_id": "xxx", "access_key_secret": "yyy",
//     "sign_name": "WePay", "template_codes": {"login_alert": "SMS_xxx"}
//   },
//   "email": {
//     "enabled": false,
//     "smtp_host": "smtp.qq.com", "smtp_port": 465,
//     "username": "noreply@example.com", "password": "xxx",
//     "from_name": "WePay 通知"
//   },
//   "oss": {
//     "enabled": false,
//     "provider": "aliyun" | "minio" | "local",
//     "endpoint": "...", "access_key": "...", "secret_key": "...",
//     "bucket": "wepay"
//   }
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <map> // 映射容器
#include <iostream> // 输入输出库
#include <fstream> // 文件流库
#include <filesystem> // 文件系统库
#include <random> // 随机数库
#include <openssl/evp.h> // OpenSSL EVP 库
#include <openssl/hmac.h> // OpenSSL HMAC 库
#include <openssl/sha.h> // OpenSSL SHA 库
#include <curl/curl.h> // libcurl 库
#include <chrono> // 时间库
#include <ctime> // C 时间库
#include <algorithm> // 算法库
#include <iomanip> // 输入输出格式化库
#include <sstream> // 字符串流库
#include <json/json.h> // JSON 库
#include "SyncHttp.h" // 同步 HTTP 库

// ═══════════════════════════════════════════════════════════════
// 短信服务
// ═══════════════════════════════════════════════════════════════
// 短信服务类
class SmsService {
public:
    // 获取单例实例
    static SmsService &instance() { static SmsService s; return s; } // 返回单例

    // 配置短信服务
    void configure(const Json::Value &cfg) {
        enabled_      = cfg.get("enabled", false).asBool(); // 读取启用标志
        provider_     = cfg.get("provider", "stub").asString(); // 读取提供商
        accessKeyId_  = cfg.get("access_key_id", "").asString(); // 读取访问密钥 ID
        accessKeySec_ = cfg.get("access_key_secret", "").asString(); // 读取访问密钥密文
        signName_     = cfg.get("sign_name", "WePay").asString(); // 读取签名名称
        if (cfg.isMember("template_codes")) { // 如果有模板代码
            for (auto &k : cfg["template_codes"].getMemberNames()) { // 遍历每个模板
                tplCodes_[k] = cfg["template_codes"][k].asString(); // 存储模板代码
            }
        }
        std::cout << "[SmsService] provider=" << provider_ << " enabled=" << enabled_ << std::endl; // 输出配置信息
    }

    // 发送短信方法
    // tplKey 见 config.template_codes 里的键
    bool send(const std::string &phone, const std::string &tplKey, // 电话、模板键
              const std::map<std::string, std::string> &params) { // 参数映射
        if (!enabled_) { // 如果未启用
            std::cout << "[SmsService] (disabled) " << phone << " tpl=" << tplKey; // 输出禁用日志
            for (auto &[k, v] : params) std::cout << " " << k << "=" << v; // 输出参数
            std::cout << std::endl; // 换行
            return true; // 返回成功
        }
        if (provider_ == "aliyun") return sendAliyun(phone, tplKey, params); // 如果是阿里云则调用阿里云方法
        // 其他 provider(腾讯云/华为云)预留
        std::cout << "[SmsService] unsupported provider: " << provider_ << std::endl; // 输出不支持的提供商
        return false; // 返回失败
    }

private:
    bool sendAliyun(const std::string &phone, const std::string &tplKey,
                    const std::map<std::string, std::string> &params) {
        auto it = tplCodes_.find(tplKey);
        if (it == tplCodes_.end()) {
            std::cerr << "[SmsService] 未配置模板: " << tplKey << std::endl;
            return false;
        }
        // 阿里云SMS POP API V3: dysmsapi.aliyuncs.com SendSms
        // 此处简化：仅 POST 到 RPC 端点(完整签名实现略,生产请补全 ACS3-HMAC-SHA256 签名)
        Json::Value body;
        body["PhoneNumbers"] = phone;
        body["SignName"]     = signName_;
        body["TemplateCode"] = it->second;
        Json::Value tplParam;
        for (auto &[k, v] : params) tplParam[k] = v;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        body["TemplateParam"] = Json::writeString(wb, tplParam);

        auto resp = SyncHttp::postJson("https://dysmsapi.aliyuncs.com/", Json::writeString(wb, body));
        std::cout << "[SmsService][aliyun] " << phone << " resp=" << resp.body << std::endl;
        return resp.success && resp.status < 400;
    }

    bool enabled_ = false;
    std::string provider_ = "stub";
    std::string accessKeyId_, accessKeySec_, signName_;
    std::map<std::string, std::string> tplCodes_;
};

// ═══════════════════════════════════════════════════════════════
// 邮件服务（基于 libcurl SMTP）
// ═══════════════════════════════════════════════════════════════
// 邮件服务类
class EmailService {
public:
    // 获取单例实例
    static EmailService &instance() { static EmailService e; return e; } // 返回单例

    // 配置邮件服务
    void configure(const Json::Value &cfg) {
        enabled_  = cfg.get("enabled", false).asBool(); // 读取启用标志
        host_     = cfg.get("smtp_host", "").asString(); // 读取 SMTP 主机
        port_     = cfg.get("smtp_port", 465).asInt(); // 读取 SMTP 端口
        username_ = cfg.get("username", "").asString(); // 读取用户名
        password_ = cfg.get("password", "").asString(); // 读取密码
        fromName_ = cfg.get("from_name", "WePay").asString(); // 读取发件人名称
        std::cout << "[EmailService] host=" << host_ << ":" << port_ << " enabled=" << enabled_ << std::endl; // 输出配置信息
    }

    // 发送邮件方法
    bool send(const std::string &to, const std::string &subject, const std::string &htmlBody) { // 收件人、主题、HTML 内容
        if (!enabled_) { // 如果未启用
            std::cout << "[EmailService] (disabled) to=" << to << " subj=" << subject << std::endl; // 输出禁用日志
            return true; // 返回成功
        }
        if (host_.empty() || username_.empty()) { // 如果配置不完整
            std::cerr << "[EmailService] 配置不完整" << std::endl; // 输出错误信息
            return false; // 返回失败
        }

        // 通过 libcurl SMTP
        CURL *curl = curl_easy_init();
        if (!curl) return false;

        std::string url = (port_ == 465 ? "smtps://" : "smtp://") + host_ + ":" + std::to_string(port_);
        std::string from = fromName_ + " <" + username_ + ">";

        // 构造 RFC 5322 邮件
        std::ostringstream msg;
        msg << "To: " << to << "\r\n";
        msg << "From: " << from << "\r\n";
        msg << "Subject: =?UTF-8?B?" << base64(subject) << "?=\r\n";
        msg << "Content-Type: text/html; charset=UTF-8\r\n\r\n";
        msg << htmlBody;
        std::string body = msg.str();

        struct UploadCtx { const char *data; size_t pos, len; };
        UploadCtx ctx{ body.c_str(), 0, body.size() };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, username_.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password_.c_str());
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, ("<" + username_ + ">").c_str());
        struct curl_slist *recipients = nullptr;
        recipients = curl_slist_append(recipients, ("<" + to + ">").c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION,
            +[](char *buf, size_t sz, size_t nmemb, void *userp) -> size_t {
                auto *c = (UploadCtx*)userp;
                size_t avail = c->len - c->pos;
                size_t want = sz * nmemb;
                size_t copy = std::min(avail, want);
                if (copy > 0) { memcpy(buf, c->data + c->pos, copy); c->pos += copy; }
                return copy;
            });
        curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "[EmailService] 发送失败: " << curl_easy_strerror(res) << std::endl;
            return false;
        }
        std::cout << "[EmailService] 发送成功 to=" << to << std::endl;
        return true;
    }

private:
    static std::string base64(const std::string &in) {
        BIO *b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO *mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);
        BIO_write(b64, in.data(), (int)in.size());
        BIO_flush(b64);
        BUF_MEM *bptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string out(bptr->data, bptr->length);
        BIO_free_all(b64);
        return out;
    }

    bool enabled_ = false;
    std::string host_, username_, password_, fromName_;
    int port_ = 465;
};

// ═══════════════════════════════════════════════════════════════
// 对象存储服务 (本地文件 / 阿里云OSS / MinIO)
// ═══════════════════════════════════════════════════════════════
// 对象存储服务类
class OssService {
public:
    // 获取单例实例
    static OssService &instance() { static OssService o; return o; } // 返回单例

    // 配置对象存储服务
    void configure(const Json::Value &cfg) {
        enabled_  = cfg.get("enabled", true).asBool(); // 读取启用标志
        provider_ = cfg.get("provider", "local").asString(); // 读取提供商
        endpoint_ = cfg.get("endpoint", "").asString(); // 读取端点
        accessKey_= cfg.get("access_key", "").asString(); // 读取访问密钥
        secretKey_= cfg.get("secret_key", "").asString(); // 读取密钥
        bucket_   = cfg.get("bucket",   "wepay").asString(); // 读取桶名
        region_   = cfg.get("region",   "us-east-1").asString(); // 读取区域
        localDir_ = cfg.get("local_dir", "upload").asString(); // 读取本地目录
        baseUrl_  = cfg.get("base_url",  "/upload").asString(); // 读取基础 URL
        std::filesystem::create_directories(localDir_); // 创建本地目录
        if (provider_ != "local" && !endpoint_.empty()) { // 如果不是本地且有端点
            // 快速连通检测（2s 超时），不通则自动降级 local
            CURL *c = curl_easy_init(); // 初始化 curl
            if (c) { // 如果初始化成功
                curl_easy_setopt(c, CURLOPT_URL, endpoint_.c_str()); // 设置 URL
                curl_easy_setopt(c, CURLOPT_NOBODY, 1L); // 仅获取头部
                curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 2000L); // 设置超时 2 秒
                curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); // 禁用信号
                curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); // 不验证 SSL 证书
                CURLcode rc = curl_easy_perform(c); // 执行请求
                curl_easy_cleanup(c); // 清理 curl
                if (rc != CURLE_OK) { // 如果请求失败
                    std::cerr << "[OssService] " << provider_ << " endpoint 不可达 (" // 输出错误信息
                              << curl_easy_strerror(rc) << ")，已降级为 local 存储" << std::endl; // 降级提示
                    provider_ = "local"; // 降级到本地
                }
            }
        }
        std::cout << "[OssService] provider=" << provider_ << " bucket=" << bucket_ << std::endl; // 输出配置信息
    }

    // 上传文件，返回可访问的 URL 方法
    std::string upload(const std::string &keyName, // 文件键名
                       const std::string &localFilePath) { // 本地文件路径
        if (!enabled_) return ""; // 如果未启用则返回空
        if (provider_ == "local")  return uploadLocal(keyName, localFilePath); // 本地上传
        if (provider_ == "minio")  return uploadS3(keyName, localFilePath); // MinIO 上传
        if (provider_ == "aliyun") return uploadS3(keyName, localFilePath); // 阿里云上传
        std::cerr << "[OssService] 未知 provider: " << provider_ << std::endl; // 输出未知提供商错误
        return uploadLocal(keyName, localFilePath); // 回退到本地上传
    }

    // 删除文件方法
    bool remove(const std::string &keyName) { // 文件键名
        if (!enabled_) return false; // 如果未启用则返回失败
        if (provider_ == "local") { // 如果是本地
            try { std::filesystem::remove(localDir_ + "/" + keyName); return true; } // 删除本地文件
            catch (...) { return false; } // 异常返回失败
        }
        if (provider_ == "minio" || provider_ == "aliyun") { // 如果是 MinIO 或阿里云
            return s3Delete(keyName); // 调用 S3 删除方法
        }
        return false; // 返回失败
    }

private:
    // ── 本地文件存储 ──────────────────────────────────────────────
    std::string uploadLocal(const std::string &keyName, const std::string &src) {
        std::string safe = keyName;
        std::replace(safe.begin(), safe.end(), '\\', '/');
        if (safe.find("..") != std::string::npos) return "";
        std::string dest = localDir_ + "/" + safe;
        std::filesystem::create_directories(std::filesystem::path(dest).parent_path());
        try {
            std::filesystem::copy_file(src, dest,
                std::filesystem::copy_options::overwrite_existing);
        } catch (const std::exception &e) {
            std::cerr << "[OssService][local] 拷贝失败: " << e.what() << std::endl;
            return "";
        }
        return baseUrl_ + "/" + safe;
    }

    // ── MinIO / 阿里云OSS  S3 Signature V4 上传 ──────────────────
    std::string uploadS3(const std::string &keyName, const std::string &filePath) {
        // 读文件内容
        std::ifstream f(filePath, std::ios::binary);
        if (!f) {
            std::cerr << "[OssService][s3] 无法打开: " << filePath << std::endl;
            return "";
        }
        std::string body((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

        // 解析 endpoint  → host + scheme
        std::string ep = endpoint_;
        std::string scheme = "http";
        if (ep.substr(0, 8) == "https://") { scheme = "https"; ep = ep.substr(8); }
        else if (ep.substr(0, 7) == "http://") { ep = ep.substr(7); }
        // 去掉末尾 /
        while (!ep.empty() && ep.back() == '/') ep.pop_back();
        std::string host = ep;

        // 时间
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char dateBuf[9], dtBuf[17];
        std::strftime(dateBuf, sizeof(dateBuf), "%Y%m%d",    std::gmtime(&t));
        std::strftime(dtBuf,   sizeof(dtBuf),   "%Y%m%dT%H%M%SZ", std::gmtime(&t));
        std::string dateStr(dateBuf), dateTimeStr(dtBuf);

        // Content-Type
        std::string ct = guessMime(keyName);

        // SHA256(body)
        std::string bodyHash = sha256Hex(body);

        // CanonicalRequest
        std::string canonicalUri = "/" + bucket_ + "/" + keyName;
        std::string canonicalHeaders =
            "content-type:" + ct + "\n" +
            "host:" + host + "\n" +
            "x-amz-content-sha256:" + bodyHash + "\n" +
            "x-amz-date:" + dateTimeStr + "\n";
        std::string signedHeaders = "content-type;host;x-amz-content-sha256;x-amz-date";
        std::string canonicalReq =
            "PUT\n" + canonicalUri + "\n\n" +
            canonicalHeaders + "\n" + signedHeaders + "\n" + bodyHash;

        // StringToSign
        std::string region = region_.empty() ? "us-east-1" : region_;
        std::string credScope = dateStr + "/" + region + "/s3/aws4_request";
        std::string sts = "AWS4-HMAC-SHA256\n" + dateTimeStr + "\n" + credScope
                        + "\n" + sha256Hex(canonicalReq);

        // SigningKey
        std::string sigKey = hmacSha256(
            hmacSha256(hmacSha256(hmacSha256("AWS4" + secretKey_, dateStr), region), "s3"),
            "aws4_request");
        std::string signature = hexEncode(hmacSha256(sigKey, sts));

        std::string auth = "AWS4-HMAC-SHA256 Credential=" + accessKey_ + "/" + credScope
                         + ",SignedHeaders=" + signedHeaders
                         + ",Signature=" + signature;

        // PUT request
        std::string url = scheme + "://" + host + canonicalUri;
        CURL *c = curl_easy_init();
        if (!c) return "";

        struct curl_slist *hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, ("content-type: " + ct).c_str());
        hdrs = curl_slist_append(hdrs, ("x-amz-content-sha256: " + bodyHash).c_str());
        hdrs = curl_slist_append(hdrs, ("x-amz-date: " + dateTimeStr).c_str());
        hdrs = curl_slist_append(hdrs, ("Authorization: " + auth).c_str());

        long httpCode = 0;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        CURLcode rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);

        if (rc != CURLE_OK || httpCode / 100 != 2) {
            std::cerr << "[OssService][s3] 上传失败 rc=" << rc
                      << " http=" << httpCode << "，回退到本地存储" << std::endl;
            return uploadLocal(keyName, filePath);  // 自动回退本地
        }
        return baseUrl_ + "/" + keyName;
    }

    bool s3Delete(const std::string &keyName) {
        // 简化: 直接 DELETE，MinIO 默认允许无签名删除（按 bucket 策略）
        // 生产环境可按 uploadS3 同样方式补签名
        std::string ep = endpoint_;
        if (ep.substr(0, 8) == "https://") ep = ep.substr(8);
        else if (ep.substr(0, 7) == "http://") ep = ep.substr(7);
        while (!ep.empty() && ep.back() == '/') ep.pop_back();
        std::string url = "http://" + ep + "/" + bucket_ + "/" + keyName;
        CURL *c = curl_easy_init();
        if (!c) return false;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        CURLcode rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        return rc == CURLE_OK;
    }

    // ── 工具函数 ──────────────────────────────────────────────────
    static std::string guessMime(const std::string &name) {
        auto pos = name.rfind('.');
        if (pos == std::string::npos) return "application/octet-stream";
        std::string ext = name.substr(pos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "png")  return "image/png";
        if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
        if (ext == "gif")  return "image/gif";
        if (ext == "webp") return "image/webp";
        if (ext == "pdf")  return "application/pdf";
        return "application/octet-stream";
    }

    static std::string sha256Hex(const std::string &data) {
        unsigned char hash[32];
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
        return hexEncode(std::string(reinterpret_cast<char*>(hash), 32));
    }

    static std::string hmacSha256(const std::string &key, const std::string &msg) {
        unsigned char out[32]; unsigned int len = 32;
        HMAC(EVP_sha256(),
             key.data(), (int)key.size(),
             reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
             out, &len);
        return std::string(reinterpret_cast<char*>(out), len);
    }

    static std::string hexEncode(const std::string &bin) {
        static const char hex[] = "0123456789abcdef";
        std::string out; out.reserve(bin.size() * 2);
        for (unsigned char c : bin) {
            out += hex[c >> 4];
            out += hex[c & 0xf];
        }
        return out;
    }

    bool enabled_ = true;
    std::string provider_ = "local";
    std::string endpoint_, accessKey_, secretKey_, bucket_;
    std::string region_;
    std::string localDir_ = "upload";
    std::string baseUrl_ = "/upload";
};

// ═══════════════════════════════════════════════════════════════
// 异步发送 stub (供 MsgNoticeService 等 extern 引用)
// 委托给各 Service 单例，无配置时安全忽略
// ═══════════════════════════════════════════════════════════════
inline void emailSendAsync(const std::string &to,
                            const std::string &subject,
                            const std::string &body) {
    EmailService::instance().send(to, subject, body);
}

inline void smsSendAsync(const std::string &phone,
                          const std::string &content) {
    SmsService::instance().send(phone, "notify", {{"content", content}});
}
