// WePay-Cpp — 消息通知服务 ( MsgNotice.php)
// 支持场景: 订单通知、余额变动、结算通知、异常告警
// 支持渠道: Email、WebHook(钉钉/飞书/企微机器人)、短信(通过 SmsService)
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include <json/json.h> // JSON 库
#include "PayDb.h" // 数据库操作
#include "HttpCaller.h" // HTTP 调用工具

// 消息通知服务
// 支持多种通知场景和渠道
// 场景：订单通知、余额变动、结算通知、异常告警
// 渠道：Email、WebHook（钉钉/飞书/企微机器人）、短信（通过 SmsService）
class MsgNoticeService {
public:
    // 通知场景枚举
    enum Scene {
        // 订单支付成功
        ORDER_PAID    = 1,
        // 订单退款
        ORDER_REFUND  = 2,
        // 结算完成
        SETTLE_OK     = 3,
        // 余额不足
        BALANCE_LOW   = 4,
        // 投诉/异常
        COMPLAINT     = 5,
        // 系统告警
        SYSTEM_ALERT  = 9
    };

    // 发送通知
    // 根据配置自动选择渠道（Email、WebHook、短信）
    // 参数 scene：通知场景
    // 参数 mchId：商户 ID（0 表示系统级通知）
    // 参数 title：通知标题
    // 参数 content：通知内容
    // 参数 extra：额外信息（预留）
    static void send(Scene scene, int mchId, const std::string &title,
                     const std::string &content, const std::string &extra = "") {
        // 获取数据库实例
        auto &db = PayDb::instance();

        // 检查该场景是否启用
        std::string sceneKey = "notify_scene_" + std::to_string((int)scene);
        // 如果场景被禁用，直接返回
        if (db.getSetting(sceneKey, "1") == "0")
            return;

        // ── Email 通知 ──────────────────────────────
        // 检查 Email 通知是否启用
        std::string emailEnabled = db.getSetting("notify_email_enabled", "0");
        if (emailEnabled == "1") {
            // 初始化收件人
            std::string to;
            // 如果指定了商户 ID，优先使用商户邮箱
            if (mchId > 0) {
                // 查询商户邮箱
                auto mch = db.queryOne("SELECT email FROM merchant WHERE id=?",
                                       {std::to_string(mchId)});
                // 如果查询成功，获取邮箱
                if (!mch.empty())
                    to = mch["email"];
            }
            // 如果商户邮箱为空，使用管理员邮箱
            if (to.empty())
                to = db.getSetting("notify_email_admin", "");
            // 如果邮箱不为空，发送邮件
            if (!to.empty())
                sendEmail(to, title, content);
        }

        // ── WebHook 通知（钉钉/飞书/企微机器人）──────────────────────────────
        // 获取 WebHook URL
        std::string webhookUrl = db.getSetting("notify_webhook_url", "");
        // 如果 WebHook URL 不为空，发送 WebHook
        if (!webhookUrl.empty()) {
            sendWebhook(webhookUrl, title, content);
        }

        // ── 短信通知（仅特定场景）──────────────────────────────
        // 仅在订单支付和系统告警场景发送短信
        if (scene == ORDER_PAID || scene == SYSTEM_ALERT) {
            // 检查短信通知是否启用
            std::string smsEnabled = db.getSetting("notify_sms_enabled", "0");
            if (smsEnabled == "1") {
                // 初始化收件人电话
                std::string phone;
                // 如果指定了商户 ID，优先使用商户电话
                if (mchId > 0) {
                    // 查询商户电话
                    auto mch = db.queryOne("SELECT phone FROM merchant WHERE id=?",
                                           {std::to_string(mchId)});
                    // 如果查询成功，获取电话
                    if (!mch.empty())
                        phone = mch["phone"];
                }
                // 如果商户电话为空，使用管理员电话
                if (phone.empty())
                    phone = db.getSetting("notify_sms_admin", "");
                // 如果电话不为空，发送短信
                if (!phone.empty())
                    sendSms(phone, content);
            }
        }

        // ── 记录通知日志 ──────────────────────────────
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 插入通知日志到数据库
        db.exec("INSERT INTO notify_log(scene,mch_id,title,content,created_at) VALUES(?,?,?,?,?)",
                {std::to_string((int)scene), std::to_string(mchId), title, content,
                 std::to_string(now)});
    }

    // 快捷方法：订单支付通知
    // 参数 mchId：商户 ID
    // 参数 orderId：订单 ID
    // 参数 amount：支付金额
    // 参数 payType：支付类型
    static void orderPaid(int mchId, const std::string &orderId, const std::string &amount,
                          const std::string &payType) {
        // 构建通知标题
        std::string title = "订单支付通知";
        // 构建通知内容
        std::string content = "订单 " + orderId + " 已支付 ¥" + amount + " (" + payType + ")";
        // 发送通知
        send(ORDER_PAID, mchId, title, content);
    }

    // 快捷方法：退款通知
    // 参数 mchId：商户 ID
    // 参数 orderId：订单 ID
    // 参数 amount：退款金额
    static void orderRefunded(int mchId, const std::string &orderId, const std::string &amount) {
        // 构建通知标题
        std::string title = "退款通知";
        // 构建通知内容
        std::string content = "订单 " + orderId + " 已退款 ¥" + amount;
        // 发送通知
        send(ORDER_REFUND, mchId, title, content);
    }

    // 快捷方法：结算通知
    // 参数 mchId：商户 ID
    // 参数 settleNo：结算单号
    // 参数 amount：结算金额
    static void settleCompleted(int mchId, const std::string &settleNo, const std::string &amount) {
        // 构建通知标题
        std::string title = "结算通知";
        // 构建通知内容
        std::string content = "结算单 " + settleNo + " 已完成，金额 ¥" + amount;
        // 发送通知
        send(SETTLE_OK, mchId, title, content);
    }

    // 快捷方法：系统告警
    // 参数 msg：告警消息
    static void systemAlert(const std::string &msg) {
        // 发送系统级告警通知（mchId=0）
        send(SYSTEM_ALERT, 0, "系统告警", msg);
    }

    // 初始化通知日志表
    // 创建 notify_log 表（如果不存在）
    static void initTables() {
        // 执行 CREATE TABLE IF NOT EXISTS 语句
        PayDb::instance().exec(
            "CREATE TABLE IF NOT EXISTS notify_log ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  scene INTEGER DEFAULT 0,"
            "  mch_id INTEGER DEFAULT 0,"
            "  title TEXT,"
            "  content TEXT,"
            "  created_at INTEGER"
            ")", {});
    }

// 私有区域
private:
    // 发送邮件通知
    // 委托给 EmailService（已在 NotifyChannels.h 中定义）
    // 参数 to：收件人邮箱
    // 参数 subject：邮件主题
    // 参数 body：邮件内容
    static void sendEmail(const std::string &to, const std::string &subject,
                          const std::string &body) {
        // 这里只做接口桥接，实际发送由 EmailService 处理
        try {
            // 声明外部函数 emailSendAsync
            extern void emailSendAsync(const std::string &, const std::string &, const std::string &);
            // 异步发送邮件
            emailSendAsync(to, subject, body);
        } catch (...) {
            // 忽略异常
        }
    }

    // 发送 WebHook 通知
    // 自动检测 WebHook 类型（钉钉/飞书/企微），并使用相应的消息格式
    // 参数 url：WebHook URL
    // 参数 title：通知标题
    // 参数 content：通知内容
    static void sendWebhook(const std::string &url, const std::string &title,
                             const std::string &content) {
        // 创建 JSON 负载
        Json::Value payload;
        // 自动检测 WebHook 类型
        if (url.find("dingtalk") != std::string::npos ||
            url.find("oapi.dingtalk") != std::string::npos) {
            // ── 钉钉机器人 ──────────────────────────────
            // 设置消息类型为 markdown
            payload["msgtype"] = "markdown";
            // 设置标题
            payload["markdown"]["title"] = title;
            // 设置内容（markdown 格式）
            payload["markdown"]["text"] = "## " + title + "\n" + content;
        } else if (url.find("feishu") != std::string::npos ||
                   url.find("open.feishu") != std::string::npos) {
            // ── 飞书机器人 ──────────────────────────────
            // 设置消息类型为交互式卡片
            payload["msg_type"] = "interactive";
            // 创建卡片对象
            Json::Value card;
            // 设置卡片标题
            card["header"]["title"]["content"] = title;
            card["header"]["title"]["tag"] = "plain_text";
            // 创建卡片元素
            Json::Value elem;
            // 设置元素类型为 div
            elem["tag"] = "div";
            // 设置元素内容
            elem["text"]["content"] = content;
            elem["text"]["tag"] = "plain_text";
            // 添加元素到卡片
            card["elements"].append(elem);
            // 设置卡片到负载
            payload["card"] = card;
        } else {
            // ── 企业微信机器人 / 通用 WebHook ──────────────────────────────
            // 设置消息类型为 markdown
            payload["msgtype"] = "markdown";
            // 设置内容（markdown 格式）
            payload["markdown"]["content"] = "## " + title + "\n" + content;
        }

        // 创建 JSON 写入器
        Json::StreamWriterBuilder wb;
        // 将 JSON 对象序列化为字符串
        std::string body = Json::writeString(wb, payload);
        // 异步发送 POST 请求到 WebHook URL
        HttpCaller::asyncPost(url, body, "application/json",
            // 回调函数（忽略响应）
            [](bool, int, const std::string &) {});
    }

    // 发送短信通知
    // 委托给 SmsService（已在 NotifyChannels.h 中定义）
    // 参数 phone：收件人电话
    // 参数 content：短信内容
    static void sendSms(const std::string &phone, const std::string &content) {
        // 这里只做接口桥接，实际发送由 SmsService 处理
        try {
            // 声明外部函数 smsSendAsync
            extern void smsSendAsync(const std::string &, const std::string &);
            // 异步发送短信
            smsSendAsync(phone, content);
        } catch (...) {
            // 忽略异常
        }
    }
// 类定义结束
};
