#include "WepayV3Config.h"
#include <drogon/drogon.h>

namespace wepay {
namespace v3 {

// 从主工程 config.json 的 wepay_v3 节加载配置
// 若 configFile 内没有 wepay_v3 节，则保留全部默认值并返回 true
bool WepayV3Config::loadFromFile(const std::string& configFile) {
    try {
        std::ifstream ifs(configFile);
        if (!ifs.is_open()) return false;

        Json::Value root;
        Json::CharReaderBuilder rb;
        std::string errs;
        if (!Json::parseFromStream(rb, ifs, &root, &errs)) return false;

        // 若主配置里没有 wepay_v3 节，保持默认值，静默成功
        if (!root.isMember("wepay_v3")) return true;
        const Json::Value& v = root["wepay_v3"];

        // 基础
        if (v.isMember("base_url"))          baseUrl           = v["base_url"].asString();
        if (v.isMember("heartbeat_interval")) heartbeatInterval = v["heartbeat_interval"].asInt();
        if (v.isMember("heartbeat_timeout"))  heartbeatTimeout  = v["heartbeat_timeout"].asInt();
        if (v.isMember("order_timeout"))      orderTimeout      = v["order_timeout"].asInt();

        // Security
        if (v.isMember("security")) {
            const auto& s = v["security"];
            if (s.isMember("enable_hmac"))       security.enableHmac      = s["enable_hmac"].asBool();
            if (s.isMember("hmac_secret"))        security.hmacSecret      = s["hmac_secret"].asString();
            if (s.isMember("rsa_public_key"))     security.rsaPublicKey    = s["rsa_public_key"].asString();
            if (s.isMember("timestamp_window"))   security.timestampWindow = s["timestamp_window"].asInt();
            if (s.isMember("max_devices"))        security.maxDevices      = s["max_devices"].asInt();
            if (s.isMember("ip_whitelist"))
                for (const auto& ip : s["ip_whitelist"])
                    security.ipWhitelist.push_back(ip.asString());
        }

        // RocketMQ
        if (v.isMember("rocketmq")) {
            const auto& mq = v["rocketmq"];
            if (mq.isMember("namesrv_addr"))      rocketmq.namesrvAddr     = mq["namesrv_addr"].asString();
            if (mq.isMember("order_push_topic"))  rocketmq.orderPushTopic  = mq["order_push_topic"].asString();
            if (mq.isMember("ocr_task_topic"))    rocketmq.ocrTaskTopic    = mq["ocr_task_topic"].asString();
            if (mq.isMember("email_notify_topic"))rocketmq.emailNotifyTopic= mq["email_notify_topic"].asString();
        }

        // Email
        if (v.isMember("email")) {
            const auto& e = v["email"];
            if (e.isMember("smtp_host"))  email.smtpHost  = e["smtp_host"].asString();
            if (e.isMember("smtp_port"))  email.smtpPort  = e["smtp_port"].asInt();
            if (e.isMember("username"))   email.username  = e["username"].asString();
            if (e.isMember("password"))   email.password  = e["password"].asString();
            if (e.isMember("from_email")) email.fromEmail = e["from_email"].asString();
            if (e.isMember("from_name"))  email.fromName  = e["from_name"].asString();
            if (e.isMember("use_ssl"))    email.useSsl    = e["use_ssl"].asBool();
        }

        // Redis
        if (v.isMember("redis")) {
            const auto& r = v["redis"];
            if (r.isMember("host"))     redis.host     = r["host"].asString();
            if (r.isMember("port"))     redis.port     = r["port"].asInt();
            if (r.isMember("password")) redis.password = r["password"].asString();
            if (r.isMember("database")) redis.database = r["database"].asInt();
            if (r.isMember("pool_size"))redis.poolSize = r["pool_size"].asInt();
        }

        // MinIO
        if (v.isMember("minio")) {
            const auto& m = v["minio"];
            if (m.isMember("endpoint"))   minio.endpoint   = m["endpoint"].asString();
            if (m.isMember("access_key")) minio.accessKey  = m["access_key"].asString();
            if (m.isMember("secret_key")) minio.secretKey  = m["secret_key"].asString();
            if (m.isMember("bucket"))     minio.bucket     = m["bucket"].asString();
            if (m.isMember("use_ssl"))    minio.useSSL     = m["use_ssl"].asBool();
        }

        // Alert
        if (v.isMember("alert")) {
            const auto& a = v["alert"];
            if (a.isMember("enabled"))          alert.enabled         = a["enabled"].asBool();
            if (a.isMember("dingtalk_webhook")) alert.dingtalkWebhook = a["dingtalk_webhook"].asString();
            if (a.isMember("wecom_webhook"))    alert.wecomWebhook    = a["wecom_webhook"].asString();
        }

        return true;
    } catch (const std::exception& e) {
        LOG_WARN << "[V3Config] loadFromFile error: " << e.what();
        return false;
    }
}

} // namespace v3
} // namespace wepay
