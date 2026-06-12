#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <curl/curl.h>
#include "nlohmann/json.hpp"
#include <drogon/HttpController.h>

namespace wepay {
namespace v3 {

// 邮件服务
class EmailService {
public:
    struct EmailConfig {
        std::string smtpHost;
        int smtpPort;
        std::string username;
        std::string password;
        std::string fromEmail;
        std::string fromName;
        bool useSsl;
        std::string templatePath; // HTML 模板目录（可选）
    };

    struct EmailData {
        std::string orderId;
        std::string merchantOrderId;
        std::string merchantId;
        std::string toEmail;
        std::string money;
        std::string payType;
        std::string payTime;
        std::string deviceId;
        std::string failReason;
        std::string createTime;
        std::string callbackUrl;       // 手动触发回调
        std::string resendEmailUrl;    // 重发通知邮件
        std::string orderViewUrl;      // 查看订单详情
        std::string closeOrderUrl;     // 关闭订单
        std::string editOrderUrl;      // 编辑订单
        std::string deleteOrderUrl;    // 删除订单
        std::string toggleChannelUrl;  // 开关支付通道
        std::string statisticUrl;      // 订单统计
    };

    EmailService(const EmailConfig& config);
    ~EmailService();

    // 从数据库加载全部启用账号（支持多账号轮询）
    void loadAccountsFromDb();

    // 运行时替换全部账号列表
    void reloadAccounts(const std::vector<EmailConfig>& accounts);

    // 运行时热更新单账号（向后兼容）
    void reloadConfig(const EmailConfig& cfg);

    // 获取当前首个发件账号的邮箱地址（用作开发者默认收件地址）
    std::string getFromEmail() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(accountsMu_));
        return accounts_.empty() ? "" : accounts_[0].fromEmail;
    }

    // 全局单例访问（供 AdminV3Ctrl 调用）
    static std::shared_ptr<EmailService> globalInstance();
    static void setGlobalInstance(std::shared_ptr<EmailService> svc);

    // 发送支付成功邮件
    bool sendPaySuccessEmail(const EmailData& data);

    // 发送支付失败邮件（带手动回调按钮）
    bool sendPayFailEmail(const EmailData& data);

    // 发送每日汇总邮件
    bool sendDailySummaryEmail(const std::string& merchantId,
                               const std::string& toEmail,
                               const nlohmann::json& summaryData);

    // 记录邮件发送日志
    void logEmailSend(const std::string& orderId,
                     const std::string& merchantId,
                     const std::string& toEmail,
                     const std::string& emailType,
                     const std::string& subject,
                     bool success,
                     const std::string& failReason = "");

private:
    std::vector<EmailConfig>   accounts_;   // 轮询账号池
    std::atomic<size_t>        rrIdx_{0};   // round-robin 计数器
    std::mutex                 accountsMu_;

    // 用指定账号发送（内部使用）
    bool sendEmailWith(const EmailConfig& cfg,
                       const std::string& to,
                       const std::string& subject,
                       const std::string& htmlBody);

    static std::shared_ptr<EmailService> globalInstance_;
    static std::mutex                    globalMu_;

    // 渲染HTML模板
    std::string renderTemplate(const std::string& templateName,
                              const nlohmann::json& data);
    // 渲染内置模板（templatePath 为空时使用）
    std::string renderBuiltinTemplate(const std::string& templateName,
                                      const nlohmann::json& data);

    // 发送邮件（底层实现）
    bool sendEmail(const std::string& to,
                  const std::string& subject,
                  const std::string& htmlBody);

    // CURL回调函数
    struct UploadStatus {
        const char* data;
        size_t bytesRead;
    };

    static size_t payloadSource(void* ptr, size_t size, size_t nmemb, void* userp);
};

// 手动回调令牌生成器
class CallbackTokenGenerator {
public:
    struct TokenData {
        std::string orderId;
        std::string merchantId;
        int64_t expireTime;
    };

    // 生成回调令牌
    static std::string generateToken(const TokenData& data, const std::string& secret);

    // 验证回调令牌
    static bool verifyToken(const std::string& token,
                           const std::string& orderId,
                           const std::string& merchantId,
                           const std::string& secret);

    // 生成完整的回调URL
    static std::string generateCallbackUrl(const std::string& baseUrl,
                                          const TokenData& data,
                                          const std::string& secret);

    // 校验是否适合作为邮件中的可点击公网 URL
    static bool isSafePublicUrl(const std::string& url);

private:
    static std::string hmacSha256(const std::string& data, const std::string& key);
    static std::string base64Encode(const unsigned char* data, size_t len);
    static std::string base64Decode(const std::string& encoded);
};

// 手动回调控制器
class ManualCallbackController : public drogon::HttpController<ManualCallbackController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ManualCallbackController::handleManualCallback,
                  "/api/wepay/v3/callback/manual", drogon::Get);
    METHOD_LIST_END

    void handleManualCallback(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    bool verifyCallbackToken(const std::string& token,
                            const std::string& orderId,
                            const std::string& merchantId);

    bool triggerMerchantCallback(const std::string& orderId,
                                const std::string& merchantId,
                                const std::string& clientIp,
                                const std::string& userAgent);

    void logManualCallback(const std::string& orderId,
                          const std::string& merchantId,
                          const std::string& token,
                          bool success,
                          const std::string& response,
                          const std::string& clientIp,
                          const std::string& userAgent);
};

} // namespace v3
} // namespace wepay
