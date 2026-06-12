#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <json/json.h>

namespace wepay {
namespace v3 {

// V3配置类
class WepayV3Config {
public:
    struct SecurityConfig {
        bool enableHmac = true;              // 启用HMAC签名
        bool enableRsa = false;              // 启用RSA签名
        std::string hmacSecret;              // HMAC密钥
        std::string rsaPublicKey;            // RSA公钥
        int timestampWindow = 300;           // 时间戳窗口（秒）
        int maxDevices = 100;                // 最大设备数
        std::vector<std::string> ipWhitelist; // IP白名单
    };

    struct RocketMQConfig {
        std::string namesrvAddr;
        std::string orderPushTopic = "wepay_order_push";
        std::string ocrTaskTopic = "wepay_ocr_task";
        std::string emailNotifyTopic = "wepay_email_notify";
        std::string alertNotifyTopic = "wepay_alert_notify";
        std::string orderTimeoutTopic = "wepay_order_timeout";
        std::string deviceStatusTopic = "wepay_device_status";
    };

    struct EmailConfig {
        std::string smtpHost;
        int smtpPort = 587;
        std::string username;
        std::string password;
        std::string fromEmail;
        std::string fromName;
        bool useSsl = true;
        std::string templatePath = "./templates";
    };

    struct RedisConfig {
        std::string host = "127.0.0.1";
        int port = 6379;
        std::string password;
        int database = 0;
        int poolSize = 10;
    };

    struct MinIOConfig {
        std::string endpoint;
        std::string accessKey;
        std::string secretKey;
        std::string bucket = "wepay";
        bool useSSL = true;
    };

    struct AlertConfig {
        bool enabled = false;
        std::string dingtalkWebhook;
        std::string wecomWebhook;
    };

    static WepayV3Config& getInstance() {
        static WepayV3Config instance;
        return instance;
    }

    // 从主工程 config.json 的 wepay_v3 节加载配置
    bool loadFromFile(const std::string& configFile);

    SecurityConfig security;
    RocketMQConfig rocketmq;
    EmailConfig email;
    RedisConfig redis;
    MinIOConfig minio;
    AlertConfig alert;

    std::string baseUrl;
    int heartbeatInterval = 10;      // 心跳间隔（秒）
    int heartbeatTimeout = 180;      // 心跳超时（秒）
    int orderTimeout = 300;          // 订单超时（秒）

private:
    WepayV3Config() = default;
};

} // namespace v3
} // namespace wepay
