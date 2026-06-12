#include "AlertNotification.h"
#include <drogon/drogon.h>
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <thread>

namespace wepay {
namespace v3 {

// ── 前向声明的工具函数 ───────────────────────────────────────
static std::string urlEncode(const std::string& s);
static std::string buildStringToSign(const std::string& method,
                                    const std::map<std::string, std::string>& params);
static std::string buildPostBody(const std::map<std::string, std::string>& params);
static std::string formatIso8601DateTime(long long ms);
static std::string base64HmacSha256(const std::string& key, const std::string& data);
static std::string sha256Hex(const std::string& data);
static std::string hmacSha256(const std::string& key, const std::string& data);
static std::string hexEncode(const std::string& bin);

// ── CURL 回调 ───────────────────────────────────────────────
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ── Base64 ───────────────────────────────────────────────────
static std::string base64Encode(const unsigned char* data, size_t len) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    BIO_write(bio, data, (int)len);
    BIO_flush(bio);
    BUF_MEM* bufferPtr;
    BIO_get_mem_ptr(bio, &bufferPtr);
    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);
    return result;
}

// ── DingTalkNotifier ──────────────────────────────────────────
DingTalkNotifier::DingTalkNotifier(const std::string& webhook, const std::string& secret)
    : webhook_(webhook), secret_(secret) {
    LOG_INFO << "DingTalkNotifier initialized";
}

std::string DingTalkNotifier::generateSign(int64_t timestamp) const {
    if (secret_.empty()) return "";
    std::string stringToSign = std::to_string(timestamp) + "\n" + secret_;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    HMAC(EVP_sha256(), secret_.c_str(), (int)secret_.length(),
         (unsigned char*)stringToSign.c_str(), stringToSign.length(),
         hash, nullptr);
    return base64Encode(hash, SHA256_DIGEST_LENGTH);
}

bool DingTalkNotifier::sendText(const std::string& content,
                                const std::vector<std::string>& atMobiles) {
    nlohmann::json payload;
    payload["msgtype"] = "text";
    payload["text"]["content"] = content;
    if (!atMobiles.empty()) payload["at"]["atMobiles"] = atMobiles;
    return sendRequest(payload);
}

bool DingTalkNotifier::sendMarkdown(const std::string& title,
                                    const std::string& text,
                                    const std::vector<std::string>& atMobiles) {
    nlohmann::json payload;
    payload["msgtype"] = "markdown";
    payload["markdown"]["title"] = title;
    payload["markdown"]["text"] = text;
    if (!atMobiles.empty()) payload["at"]["atMobiles"] = atMobiles;
    return sendRequest(payload);
}

bool DingTalkNotifier::sendAlert(const AlertMessage& alert) {
    std::ostringstream markdown;
    markdown << "## " << alert.title << "\n\n";
    switch (alert.level) {
        case AlertLevel::INFO:     markdown << "ℹ️ 信息\n\n"; break;
        case AlertLevel::WARNING:  markdown << "⚠️ 警告\n\n"; break;
        case AlertLevel::ALERT_ERROR: markdown << "❌ 错误\n\n"; break;
        case AlertLevel::CRITICAL: markdown << "🔥 严重\n\n"; break;
    }
    markdown << "**内容**: " << alert.content << "\n\n";
    markdown << "**时间**: " << alert.timestamp << "\n\n";
    if (!alert.data.empty()) {
        markdown << "**详情**:\n```json\n" << alert.data.dump(2) << "\n```\n";
    }
    return sendMarkdown(alert.title, markdown.str());
}

bool DingTalkNotifier::sendRequest(const nlohmann::json& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = webhook_;
    if (!secret_.empty()) {
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        url += "&timestamp=" + std::to_string(ts);
        url += "&sign=" + generateSign(ts);
    }

    std::string postData = payload.dump();
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR << "DingTalk notification failed: " << curl_easy_strerror(res);
        return false;
    }
    LOG_INFO << "DingTalk notification sent: " << response;
    return true;
}

// ── WeComNotifier ───────────────────────────────────────────
WeComNotifier::WeComNotifier(const std::string& webhook) : webhook_(webhook) {
    LOG_INFO << "WeComNotifier initialized";
}

bool WeComNotifier::sendText(const std::string& content,
                             const std::vector<std::string>& mentionedList) {
    nlohmann::json payload;
    payload["msgtype"] = "text";
    payload["text"]["content"] = content;
    if (!mentionedList.empty()) payload["text"]["mentioned_list"] = mentionedList;
    return sendRequest(payload);
}

bool WeComNotifier::sendMarkdown(const std::string& content) {
    nlohmann::json payload;
    payload["msgtype"] = "markdown";
    payload["markdown"]["content"] = content;
    return sendRequest(payload);
}

bool WeComNotifier::sendAlert(const AlertMessage& alert) {
    std::ostringstream markdown;
    markdown << "## " << alert.title << "\n";
    switch (alert.level) {
        case AlertLevel::INFO:     markdown << "> <font color=\"info\">ℹ️ 信息</font>\n"; break;
        case AlertLevel::WARNING:  markdown << "> <font color=\"warning\">⚠️ 警告</font>\n"; break;
        case AlertLevel::ALERT_ERROR: markdown << "> <font color=\"warning\">❌ 错误</font>\n"; break;
        case AlertLevel::CRITICAL: markdown << "> <font color=\"warning\">🔥 严重</font>\n"; break;
    }
    markdown << "\n**内容**: " << alert.content << "\n";
    markdown << "**时间**: " << alert.timestamp << "\n";
    return sendMarkdown(markdown.str());
}

bool WeComNotifier::sendRequest(const nlohmann::json& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string postData = payload.dump();
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, webhook_.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR << "WeCom notification failed: " << curl_easy_strerror(res);
        return false;
    }
    LOG_INFO << "WeCom notification sent: " << response;
    return true;
}

// ── SmsNotifier ─────────────────────────────────────────────
SmsNotifier::SmsNotifier(const SmsConfig& config) : config_(config) {
    LOG_INFO << "SmsNotifier initialized: provider=" << config_.provider;
}

bool SmsNotifier::sendSms(const std::string& phoneNumber,
                          const nlohmann::json& templateParams) {
    if (config_.provider == "aliyun") return sendAliyunSms(phoneNumber, templateParams);
    if (config_.provider == "tencent") return sendTencentSms(phoneNumber, templateParams);
    LOG_ERROR << "Unknown SMS provider: " << config_.provider;
    return false;
}

bool SmsNotifier::sendAlert(const AlertMessage& alert,
                            const std::vector<std::string>& phoneNumbers) {
    nlohmann::json params;
    params["title"] = alert.title;
    params["content"] = alert.content;
    bool allSuccess = true;
    for (const auto& phone : phoneNumbers) {
        if (!sendSms(phone, params)) allSuccess = false;
    }
    return allSuccess;
}

// ── 阿里云 SMS ───────────────────────────────────────────────
bool SmsNotifier::sendAliyunSms(const std::string& phoneNumber,
                                const nlohmann::json& params) {
    std::string endpoint = "https://dysmsapi.aliyuncs.com";
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::map<std::string, std::string> reqParams;
    reqParams["PhoneNumbers"] = phoneNumber;
    reqParams["SignName"]    = config_.signName;
    reqParams["TemplateCode"] = config_.templateCode;
    reqParams["TemplateParam"] = params.dump();
    reqParams["Format"]      = "JSON";
    reqParams["Version"]     = "2017-05-25";
    reqParams["AccessKeyId"] = config_.accessKeyId;
    reqParams["SignatureMethod"] = "HMAC-SHA256";
    reqParams["Timestamp"]   = formatIso8601DateTime(ms);
    reqParams["SignatureVersion"] = "1.0";
    reqParams["SignatureNonce"] = std::to_string(ms + (rand() % 10000));
    reqParams["Action"]     = "SendSms";
    reqParams["RegionId"]   = "cn-hangzhou";

    std::string signedParams = buildStringToSign("POST", reqParams);
    reqParams["Signature"] = base64HmacSha256(config_.accessKeySecret, signedParams);

    std::string postData = buildPostBody(reqParams);
    std::string response;

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR << "Aliyun SMS failed: " << curl_easy_strerror(res);
        return false;
    }
    try {
        auto j = nlohmann::json::parse(response);
        std::string code = j.value("Code", "");
        if (code == "OK") {
            LOG_INFO << "Aliyun SMS sent to " << phoneNumber
                     << " BizId=" << j.value("BizId", "");
            return true;
        }
        LOG_ERROR << "Aliyun SMS error: " << code << " " << j.value("Message", "");
    } catch (...) {
        LOG_ERROR << "Aliyun SMS parse error: " << response;
    }
    return false;
}

// ── 腾讯云 SMS ───────────────────────────────────────────────
bool SmsNotifier::sendTencentSms(const std::string& phoneNumber,
                                 const nlohmann::json& params) {
    std::string url = "https://sms.tencentcloudapi.com";
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string timestamp = std::to_string(sec);

    nlohmann::json bodyJson;
    bodyJson["SmsSdkAppId"] = config_.signName;
    bodyJson["Sign"] = config_.templateCode;
    bodyJson["TemplateId"] = config_.templateCode;
    bodyJson["PhoneNumberSet"] = {"+86" + phoneNumber};

    std::vector<std::string> tplParams;
    for (auto& [k, v] : params.items()) {
        tplParams.push_back(v.is_string() ? v.get<std::string>() : v.dump());
    }
    if (!tplParams.empty()) bodyJson["TemplateParamSet"] = tplParams;

    std::string bodyStr = bodyJson.dump();
    std::string hashedPayload = sha256Hex(bodyStr);
    std::string hashedCanonicalReq = sha256Hex(
        "POST\n/sms/api/v3/send-sms\n\n"
        "content-type:application/json\nhost:sms.tencentcloudapi.com\n\n"
        "content-type:application/json\nhost:sms.tencentcloudapi.com\n"
        + hashedPayload);

    std::string stringToSign = "TC3-HMAC-SHA256\n" + timestamp
        + "\n2021-03-01/sms/tc3_request\n" + hashedCanonicalReq;

    char dateStr[11];
    std::strftime(dateStr, sizeof(dateStr), "%Y-%m-%d",
                  std::localtime((const time_t*)&sec));

    std::string sigKey = hmacSha256("TC3" + config_.accessKeySecret, dateStr);
    sigKey = hmacSha256(sigKey, "2021-03-01");
    sigKey = hmacSha256(sigKey, "sms");
    sigKey = hmacSha256(sigKey, "tc3_request");
    std::string signature = hexEncode(hmacSha256(sigKey, stringToSign));

    std::string auth = "TC3-HMAC-SHA256 Credential=" + config_.accessKeyId
        + "/" + std::string(dateStr) + "/2021-03-01/sms/tc3_request,"
        " SignedHeaders=content-type;host, Signature=" + signature;

    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string tsHeader = "X-TC-Timestamp: " + timestamp;
    std::string authHeader = "Authorization: " + auth;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Host: sms.tencentcloudapi.com");
    headers = curl_slist_append(headers, "X-TC-Action: SendSms");
    headers = curl_slist_append(headers, "X-TC-Version: 2021-03-01");
    headers = curl_slist_append(headers, tsHeader.c_str());
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR << "Tencent SMS failed: " << curl_easy_strerror(res);
        return false;
    }
    try {
        auto j = nlohmann::json::parse(response);
        if (j.contains("Response") && j["Response"].contains("SendStatusSet")) {
            auto& status = j["Response"]["SendStatusSet"];
            if (!status.empty() && status[0].value("Code", "") == "Ok") {
                LOG_INFO << "Tencent SMS sent to " << phoneNumber;
                return true;
            }
            LOG_ERROR << "Tencent SMS error: "
                      << (status.empty() ? "" : status[0].value("Message", ""));
        }
    } catch (...) {
        LOG_ERROR << "Tencent SMS parse error: " << response;
    }
    return false;
}

// ── 工具函数实现 ─────────────────────────────────────────────
static std::string urlEncode(const std::string& s) {
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

static std::string buildStringToSign(const std::string& method,
                                    const std::map<std::string, std::string>& params) {
    std::string sorted;
    std::vector<std::pair<std::string, std::string>> v(params.begin(), params.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& p : v) {
        if (!sorted.empty()) sorted += "&";
        sorted += urlEncode(p.first) + "=" + urlEncode(p.second);
    }
    return method + "&" + urlEncode("/") + "&" + urlEncode(sorted);
}

static std::string buildPostBody(const std::map<std::string, std::string>& params) {
    std::string body;
    std::vector<std::pair<std::string, std::string>> v(params.begin(), params.end());
    for (auto& p : v) {
        if (!body.empty()) body += "&";
        body += urlEncode(p.first) + "=" + urlEncode(p.second);
    }
    return body;
}

static std::string formatIso8601DateTime(long long ms) {
    std::time_t t = ms / 1000;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

static std::string base64HmacSha256(const std::string& key, const std::string& data) {
    unsigned char hash[32];
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         (unsigned char*)data.data(), data.size(), hash, nullptr);
    return base64Encode(hash, 32);
}

static std::string sha256Hex(const std::string& data) {
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)data.data(), data.size(), h);
    std::string bin((char*)h, SHA256_DIGEST_LENGTH);
    return hexEncode(bin);
}

static std::string hmacSha256(const std::string& key, const std::string& data) {
    unsigned char h[32];
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         (unsigned char*)data.data(), data.size(), h, nullptr);
    return std::string((char*)h, 32);
}

static std::string hexEncode(const std::string& bin) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bin.size() * 2);
    for (size_t i = 0; i < bin.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(bin[i]);
        out += hex[c >> 4];
        out += hex[c & 0xf];
    }
    return out;
}

// ── EmailNotifier ────────────────────────────────────────────
EmailNotifier::EmailNotifier(const EmailConfig& cfg) : cfg_(cfg) {}

bool EmailNotifier::sendAlert(const AlertMessage& alert,
                              const std::vector<std::string>& recipients) {
    if (recipients.empty()) return false;
    std::ostringstream body;
    body << "告警级别: ";
    switch (alert.level) {
        case AlertLevel::INFO:     body << "信息"; break;
        case AlertLevel::WARNING:  body << "警告"; break;
        case AlertLevel::ALERT_ERROR: body << "错误"; break;
        case AlertLevel::CRITICAL: body << "严重"; break;
    }
    body << "\n标题: " << alert.title;
    body << "\n内容: " << alert.content;
    body << "\n时间: " << alert.timestamp;
    if (!alert.data.empty()) body << "\n详情: " << alert.data.dump(2);

    bool ok = false;
    for (const auto& to : recipients)
        ok |= send(to, alert.title, body.str());
    return ok;
}

bool EmailNotifier::send(const std::string& to, const std::string& subject,
                         const std::string& htmlBody) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string from = cfg_.from_name + " <" + cfg_.username + ">";
    std::string url = (cfg_.use_ssl ? "smtps://" : "smtp://")
                    + cfg_.smtp_host + ":" + std::to_string(cfg_.smtp_port);

    std::ostringstream msg;
    msg << "To: " << to << "\r\n";
    msg << "From: " << from << "\r\n";
    msg << "Subject: =?UTF-8?B?" << base64EncodeStr(subject) << "?=\r\n";
    msg << "Content-Type: text/plain; charset=UTF-8\r\n\r\n";
    msg << htmlBody;
    std::string bodyStr = msg.str();

    struct UploadCtx { const char* data; size_t pos, len; };
    UploadCtx ctx{ bodyStr.c_str(), 0, bodyStr.size() };

    struct curl_slist* recipients = nullptr;
    recipients = curl_slist_append(recipients, ("<" + to + ">").c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL,
                      (long)(cfg_.use_ssl ? CURLUSESSL_ALL : CURLUSESSL_NONE));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, ("<" + cfg_.username + ">").c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,
        +[](char* buf, size_t sz, size_t nmemb, void* userp) -> size_t {
            auto* c = (UploadCtx*)userp;
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
        LOG_ERROR << "Email send failed: " << curl_easy_strerror(res);
        return false;
    }
    LOG_INFO << "Email sent: to=" << to << " subject=" << subject;
    return true;
}

std::string EmailNotifier::base64EncodeStr(const std::string& in) const {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_write(b64, in.data(), (int)in.size());
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    return out;
}

// ── AlertNotificationManager ─────────────────────────────────
void AlertNotificationManager::init(const nlohmann::json& config) {
    LOG_INFO << "AlertNotificationManager initializing...";

    if (config.contains("dingtalk")) {
        for (const auto& item : config["dingtalk"]) {
            std::string name = item["name"].get<std::string>();
            std::string webhook = item["webhook"].get<std::string>();
            std::string secret = item.value("secret", "");
            auto notifier = std::make_shared<DingTalkNotifier>(webhook, secret);
            registerDingTalkNotifier(name, notifier);
        }
    }

    if (config.contains("wecom")) {
        for (const auto& item : config["wecom"]) {
            std::string name = item["name"].get<std::string>();
            std::string webhook = item["webhook"].get<std::string>();
            auto notifier = std::make_shared<WeComNotifier>(webhook);
            registerWeComNotifier(name, notifier);
        }
    }

    if (config.contains("sms")) {
        SmsNotifier::SmsConfig smsConfig;
        smsConfig.provider = config["sms"]["provider"].get<std::string>();
        smsConfig.accessKeyId = config["sms"]["accessKeyId"].get<std::string>();
        smsConfig.accessKeySecret = config["sms"]["accessKeySecret"].get<std::string>();
        smsConfig.signName = config["sms"]["signName"].get<std::string>();
        smsConfig.templateCode = config["sms"]["templateCode"].get<std::string>();
        auto notifier = std::make_shared<SmsNotifier>(smsConfig);
        registerSmsNotifier(notifier);
    }

    if (config.contains("email")) {
        EmailNotifier::EmailConfig emailConfig;
        emailConfig.smtp_host = config["email"].value("smtp_host", "");
        emailConfig.smtp_port = config["email"].value("smtp_port", 465);
        emailConfig.username = config["email"].value("username", "");
        emailConfig.password = config["email"].value("password", "");
        emailConfig.from_name = config["email"].value("from_name", "WePay");
        emailConfig.use_ssl = config["email"].value("use_ssl", true);
        if (!emailConfig.smtp_host.empty() && !emailConfig.username.empty()) {
            auto notifier = std::make_shared<EmailNotifier>(emailConfig);
            registerEmailNotifier(notifier);
        }
    }

    if (config.contains("alert_phones")) {
        std::vector<std::string> phones;
        for (auto& p : config["alert_phones"])
            phones.push_back(p.get<std::string>());
        setAlertPhones(phones);
    }

    LOG_INFO << "AlertNotificationManager initialized";
}

void AlertNotificationManager::registerDingTalkNotifier(
    const std::string& name, std::shared_ptr<DingTalkNotifier> notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    dingTalkNotifiers_[name] = notifier;
    LOG_INFO << "DingTalk notifier registered: " << name;
}

void AlertNotificationManager::registerWeComNotifier(
    const std::string& name, std::shared_ptr<WeComNotifier> notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    weComNotifiers_[name] = notifier;
    LOG_INFO << "WeCom notifier registered: " << name;
}

void AlertNotificationManager::registerSmsNotifier(
    std::shared_ptr<SmsNotifier> notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    smsNotifier_ = notifier;
    LOG_INFO << "SMS notifier registered";
}

void AlertNotificationManager::registerEmailNotifier(
    std::shared_ptr<EmailNotifier> notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    emailNotifier_ = notifier;
    LOG_INFO << "Email notifier registered";
}

void AlertNotificationManager::setAlertPhones(const std::vector<std::string>& phones) {
    std::lock_guard<std::mutex> lock(mutex_);
    alertPhones_ = phones;
}

std::string AlertNotificationManager::formatAlertMessage(const AlertMessage& alert) const {
    std::ostringstream oss;
    oss << "[" << (int)alert.level << "] " << alert.title << ": " << alert.content;
    return oss.str();
}

bool AlertNotificationManager::sendAlert(const AlertMessage& alert) {
    bool success = true;
    for (AlertChannel channel : alert.channels) {
        if (!sendToChannel(alert, channel)) success = false;
    }
    return success;
}

bool AlertNotificationManager::sendToChannel(const AlertMessage& alert,
                                            AlertChannel channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool success = false;

    switch (channel) {
        case AlertChannel::DINGTALK:
            for (const auto& [name, notifier] : dingTalkNotifiers_)
                if (notifier->sendAlert(alert)) success = true;
            break;
        case AlertChannel::WECOM:
            for (const auto& [name, notifier] : weComNotifiers_)
                if (notifier->sendAlert(alert)) success = true;
            break;
        case AlertChannel::SMS:
            if (smsNotifier_) success = smsNotifier_->sendAlert(alert, alertPhones_);
            break;
        case AlertChannel::EMAIL:
            if (emailNotifier_) success = emailNotifier_->sendAlert(alert, alertPhones_);
            break;
    }

    if (success) recordSuccess(channel);
    else recordFailure(channel);
    return success;
}

void AlertNotificationManager::recordSuccess(AlertChannel channel) {
    stats_.totalAlerts++;
    stats_.successAlerts++;
    std::string ch;
    switch (channel) {
        case AlertChannel::DINGTALK: ch = "dingtalk"; break;
        case AlertChannel::WECOM:    ch = "wecom";    break;
        case AlertChannel::SMS:      ch = "sms";      break;
        case AlertChannel::EMAIL:   ch = "email";    break;
    }
    stats_.channelStats[ch]++;
}

void AlertNotificationManager::recordFailure(AlertChannel channel) {
    stats_.totalAlerts++;
    stats_.failedAlerts++;
}

AlertNotificationManager::Stats AlertNotificationManager::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ── AlertManagementController ─────────────────────────────────
void AlertManagementController::sendTestAlert(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    try {
        AlertMessage alert;
        alert.type = AlertType::SYSTEM_ERROR;
        alert.level = AlertLevel::INFO;
        alert.title = "测试告警";
        alert.content = "这是一条测试告警消息";
        alert.timestamp = std::time(nullptr);
        alert.channels = {AlertChannel::DINGTALK, AlertChannel::WECOM};
        bool ok = AlertNotificationManager::getInstance().sendAlert(alert);
        callback(buildResponse(ok ? 0 : -1,
            ok ? "Test alert sent" : "Failed to send test alert"));
    } catch (const std::exception& e) {
        LOG_ERROR << "Send test alert error: " << e.what();
        callback(buildResponse(-1, "Internal error"));
    }
}

void AlertManagementController::getAlertStats(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    try {
        auto stats = AlertNotificationManager::getInstance().getStats();
        nlohmann::json data;
        data["totalAlerts"]  = stats.totalAlerts;
        data["successAlerts"] = stats.successAlerts;
        data["failedAlerts"]  = stats.failedAlerts;
        data["channelStats"] = stats.channelStats;
        callback(buildResponse(0, "Success", data));
    } catch (const std::exception& e) {
        LOG_ERROR << "Get alert stats error: " << e.what();
        callback(buildResponse(-1, "Internal error"));
    }
}

drogon::HttpResponsePtr AlertManagementController::buildResponse(
    int code, const std::string& message, const nlohmann::json& data) {
    nlohmann::json response;
    response["code"] = code;
    response["message"] = message;
    response["data"] = data;
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("application/json");
    resp->setBody(response.dump());
    return resp;
}

} // namespace v3
} // namespace wepay
