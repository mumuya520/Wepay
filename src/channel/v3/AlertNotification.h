#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>

namespace wepay {
namespace v3 {

/**
 * 告警类型
 */
enum class AlertType {
    DEVICE_OFFLINE,     // 设备离线
    ORDER_TIMEOUT,      // 订单超时
    PAYMENT_FAILED,     // 支付失败
    SECURITY_EVENT,     // 安全事件
    SYSTEM_ERROR,       // 系统错误
    PERFORMANCE         // 性能告警
};

/**
 * 告警级别
 */
enum class AlertLevel {
    INFO,        // 信息
    WARNING,     // 警告
    ALERT_ERROR, // 错误（避免与 wingdi.h #define ERROR 0 冲突）
    CRITICAL     // 严重
};

/**
 * 告警通知渠道
 */
enum class AlertChannel {
    DINGTALK,   // 钉钉
    WECOM,      // 企业微信
    SMS,        // 短信
    EMAIL       // 邮件
};

/**
 * 告警消息
 */
struct AlertMessage {
    AlertType type;
    AlertLevel level;
    std::string title;
    std::string content;
    nlohmann::json data;
    int64_t timestamp;
    std::vector<AlertChannel> channels;
};

/**
 * 钉钉机器人通知器
 */
class DingTalkNotifier {
public:
    explicit DingTalkNotifier(const std::string& webhook, const std::string& secret = "");
    ~DingTalkNotifier() = default;

    // 发送文本消息
    bool sendText(const std::string& content, const std::vector<std::string>& atMobiles = {});

    // 发送Markdown消息
    bool sendMarkdown(const std::string& title, const std::string& text,
                     const std::vector<std::string>& atMobiles = {});

    // 发送告警消息
    bool sendAlert(const AlertMessage& alert);

private:
    std::string webhook_;
    std::string secret_;

    // 生成签名
    std::string generateSign(int64_t timestamp) const;

    // 发送HTTP请求
    bool sendRequest(const nlohmann::json& payload);
};

/**
 * 企业微信机器人通知器
 */
class WeComNotifier {
public:
    explicit WeComNotifier(const std::string& webhook);
    ~WeComNotifier() = default;

    // 发送文本消息
    bool sendText(const std::string& content, const std::vector<std::string>& mentionedList = {});

    // 发送Markdown消息
    bool sendMarkdown(const std::string& content);

    // 发送告警消息
    bool sendAlert(const AlertMessage& alert);

private:
    std::string webhook_;

    // 发送HTTP请求
    bool sendRequest(const nlohmann::json& payload);
};

/**
 * 短信通知器
 */
class SmsNotifier {
public:
    struct SmsConfig {
        std::string provider;       // 服务商（aliyun/tencent）
        std::string accessKeyId;
        std::string accessKeySecret;
        std::string signName;       // 签名
        std::string templateCode;   // 模板代码
    };

    explicit SmsNotifier(const SmsConfig& config);
    ~SmsNotifier() = default;

    // 发送短信
    bool sendSms(const std::string& phoneNumber, const nlohmann::json& templateParams);

    // 发送告警消息
    bool sendAlert(const AlertMessage& alert, const std::vector<std::string>& phoneNumbers);

private:
    SmsConfig config_;

    // 阿里云短信
    bool sendAliyunSms(const std::string& phoneNumber, const nlohmann::json& params);

    // 腾讯云短信
    bool sendTencentSms(const std::string& phoneNumber, const nlohmann::json& params);
};

/**
 * 邮件通知器
 */
class EmailNotifier {
public:
    struct EmailConfig {
        std::string smtp_host;
        int smtp_port = 465;
        std::string username;
        std::string password;
        std::string from_name;
        bool use_ssl = true;
    };

    explicit EmailNotifier(const EmailConfig& cfg);
    ~EmailNotifier() = default;

    bool sendAlert(const AlertMessage& alert, const std::vector<std::string>& recipients);

    bool send(const std::string& to, const std::string& subject, const std::string& body);

private:
    EmailConfig cfg_;
    std::string base64EncodeStr(const std::string& in) const;
};

/**
 * 告警通知管理器
 */
class AlertNotificationManager {
public:
    static AlertNotificationManager& getInstance() {
        static AlertNotificationManager instance;
        return instance;
    }

    void init(const nlohmann::json& config);

    void registerDingTalkNotifier(const std::string& name, std::shared_ptr<DingTalkNotifier> notifier);
    void registerWeComNotifier(const std::string& name, std::shared_ptr<WeComNotifier> notifier);
    void registerSmsNotifier(std::shared_ptr<SmsNotifier> notifier);
    void registerEmailNotifier(std::shared_ptr<EmailNotifier> notifier);
    void setAlertPhones(const std::vector<std::string>& phones);

    bool sendAlert(const AlertMessage& alert);
    bool sendToChannel(const AlertMessage& alert, AlertChannel channel);

    struct Stats {
        int64_t totalAlerts = 0;
        int64_t successAlerts = 0;
        int64_t failedAlerts = 0;
        std::unordered_map<std::string, int64_t> channelStats;
    };
    Stats getStats() const;

private:
    AlertNotificationManager() = default;
    ~AlertNotificationManager() = default;

    std::unordered_map<std::string, std::shared_ptr<DingTalkNotifier>> dingTalkNotifiers_;
    std::unordered_map<std::string, std::shared_ptr<WeComNotifier>> weComNotifiers_;
    std::shared_ptr<SmsNotifier> smsNotifier_;
    std::shared_ptr<EmailNotifier> emailNotifier_;
    std::vector<std::string> alertPhones_;

    mutable std::mutex mutex_;
    Stats stats_;

    std::string formatAlertMessage(const AlertMessage& alert) const;
    void recordSuccess(AlertChannel channel);
    void recordFailure(AlertChannel channel);
};

/**
 * 告警管理API控制器
 */
class AlertManagementController : public drogon::HttpController<AlertManagementController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AlertManagementController::sendTestAlert,
                  "/api/wepay/v3/alert/test", drogon::Post);
    ADD_METHOD_TO(AlertManagementController::getAlertStats,
                  "/api/wepay/v3/alert/stats", drogon::Get);
    METHOD_LIST_END

    // 发送测试告警
    void sendTestAlert(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // 获取告警统计
    void getAlertStats(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    drogon::HttpResponsePtr buildResponse(int code, const std::string& message,
                                         const nlohmann::json& data = {});
};

} // namespace v3
} // namespace wepay
