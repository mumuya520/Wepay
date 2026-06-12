#include "EmailService.h"
#include "WepayV3Config.h"
#include <drogon/drogon.h>
#include <fstream>
#include <sstream>
#include <map>
#include <iomanip>
#include <regex>
#include <cctype>
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include "common/PayDb.h"
#include "common/SmtpUtils.h"

namespace wepay {
namespace v3 {

// 全局单例静态成员定义
std::shared_ptr<EmailService> EmailService::globalInstance_;
std::mutex                    EmailService::globalMu_;

std::shared_ptr<EmailService> EmailService::globalInstance() {
    std::lock_guard<std::mutex> lk(globalMu_);
    return globalInstance_;
}

void EmailService::setGlobalInstance(std::shared_ptr<EmailService> svc) {
    std::lock_guard<std::mutex> lk(globalMu_);
    globalInstance_ = svc;
}

// 从数据库加载全部启用账号
void EmailService::loadAccountsFromDb() {
    try {
        auto& db = PayDb::instance();
        auto rows = db.query(
            "SELECT smtp_host,smtp_port,username,password,from_email,from_name,use_ssl "
            "FROM v3_email_account WHERE status=1 ORDER BY id ASC", {});
        std::vector<EmailConfig> list;
        for (auto& r : rows) {
            EmailConfig c;
            c.smtpHost    = r["smtp_host"];
            c.smtpPort    = r["smtp_port"].empty() ? 465 : std::stoi(r["smtp_port"]);
            c.username    = r["username"];
            c.password    = r["password"];
            c.fromEmail   = r["from_email"];
            c.fromName    = r["from_name"];
            c.useSsl      = r["use_ssl"] != "0";
            c.templatePath = "./templates";
            list.push_back(std::move(c));
        }
        {
            std::lock_guard<std::mutex> lk(accountsMu_);
            accounts_ = std::move(list);
            rrIdx_.store(0);
        }
        LOG_INFO << "[EmailService] loaded " << accounts_.size() << " email account(s) from DB";
    } catch (const std::exception& e) {
        LOG_WARN << "[EmailService] loadAccountsFromDb failed: " << e.what();
    }
}

// 运行时替换账号列表
void EmailService::reloadAccounts(const std::vector<EmailConfig>& accounts) {
    std::lock_guard<std::mutex> lk(accountsMu_);
    accounts_ = accounts;
    rrIdx_.store(0);
    LOG_INFO << "[EmailService] reloaded " << accounts_.size() << " email account(s)";
}

// 向后兼容：单账号热更新
void EmailService::reloadConfig(const EmailConfig& cfg) {
    std::lock_guard<std::mutex> lk(accountsMu_);
    accounts_ = {cfg};
    rrIdx_.store(0);
    LOG_INFO << "[EmailService] single account reloaded: host=" << cfg.smtpHost
             << " port=" << cfg.smtpPort << " from=" << cfg.fromEmail;
}

// EmailService实现
EmailService::EmailService(const EmailConfig& config) {
    accounts_ = {config};
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

EmailService::~EmailService() {
    curl_global_cleanup();
}

bool EmailService::sendPaySuccessEmail(const EmailData& data) {
    try {
        nlohmann::json templateData;
        templateData["orderId"] = data.orderId;
        templateData["merchantOrderId"] = data.merchantOrderId;
        templateData["merchantId"] = data.merchantId;
        templateData["money"] = data.money;
        templateData["payType"] = data.payType;
        templateData["payTime"] = data.payTime;
        templateData["deviceId"] = data.deviceId;

        std::string htmlBody = renderTemplate("pay_success", templateData);
        std::string subject = "【支付成功】订单 " + data.orderId + " 已完成支付";

        bool success = sendEmail(data.toEmail, subject, htmlBody);

        logEmailSend(data.orderId, data.merchantId, data.toEmail,
                    "PAY_SUCCESS", subject, success);

        return success;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send pay success email: " << e.what();
        return false;
    }
}

bool EmailService::sendPayFailEmail(const EmailData& data) {
    try {
        // 生成手动回调令牌
        CallbackTokenGenerator::TokenData tokenData;
        tokenData.orderId = data.orderId;
        tokenData.merchantId = data.merchantId;
        tokenData.expireTime = std::time(nullptr) + 300; // 5分钟

        auto& config = WepayV3Config::getInstance();
        std::string token = CallbackTokenGenerator::generateToken(
            tokenData, config.security.hmacSecret
        );

        std::string callbackUrl = CallbackTokenGenerator::generateCallbackUrl(
            config.baseUrl, tokenData, config.security.hmacSecret
        );
        bool hasSafeCallbackUrl = CallbackTokenGenerator::isSafePublicUrl(callbackUrl);
        if (!hasSafeCallbackUrl) {
            LOG_WARN << "[EmailService] skip unsafe manual callback URL for email, baseUrl="
                     << config.baseUrl << " orderId=" << data.orderId;
        }

        nlohmann::json templateData;
        templateData["orderId"]          = data.orderId;
        templateData["merchantOrderId"]  = data.merchantOrderId;
        templateData["merchantId"]       = data.merchantId;
        templateData["toEmail"]          = data.toEmail;
        templateData["money"]            = data.money;
        templateData["payType"]          = data.payType;
        templateData["failReason"]       = data.failReason;
        templateData["createTime"]       = data.createTime;
        templateData["deviceId"]         = data.deviceId;
        // URL 优先用调用方传入的，否则用 token 生成的
        std::string emailCallbackUrl = data.callbackUrl.empty() ? callbackUrl : data.callbackUrl;
        bool callbackUrlVisible = CallbackTokenGenerator::isSafePublicUrl(emailCallbackUrl);
        if (!callbackUrlVisible) {
            emailCallbackUrl = "";
        }
        templateData["callbackUrl"]      = emailCallbackUrl;
        templateData["callbackUrlVisible"] = callbackUrlVisible ? "block" : "none";
        templateData["callbackUrlHidden"]  = callbackUrlVisible ? "none" : "block";
        templateData["resendEmailUrl"]   = data.resendEmailUrl;
        templateData["orderViewUrl"]     = data.orderViewUrl;
        templateData["closeOrderUrl"]    = data.closeOrderUrl;
        templateData["editOrderUrl"]     = data.editOrderUrl;
        templateData["deleteOrderUrl"]   = data.deleteOrderUrl;
        templateData["toggleChannelUrl"] = data.toggleChannelUrl;
        templateData["statisticUrl"]     = data.statisticUrl;

        std::string htmlBody = renderTemplate("pay_fail", templateData);
        std::string subject = "【新订单】" + data.orderId + " 等待处理";

        bool success = sendEmail(data.toEmail, subject, htmlBody);

        logEmailSend(data.orderId, data.merchantId, data.toEmail,
                    "PAY_FAIL", subject, success);

        return success;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send pay fail email: " << e.what();
        return false;
    }
}

bool EmailService::sendDailySummaryEmail(const std::string& merchantId,
                                        const std::string& toEmail,
                                        const nlohmann::json& summaryData) {
    try {
        std::string htmlBody = renderTemplate("daily_summary", summaryData);
        std::string subject = "【昨日收款汇总】" + summaryData.value("date", "") + " 收款统计";

        bool success = sendEmail(toEmail, subject, htmlBody);

        logEmailSend("", merchantId,
                    toEmail, "DAILY_SUMMARY", subject, success,
                    success ? "" : "SMTP发送失败");

        return success;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to send daily summary email: " << e.what();
        return false;
    }
}

void EmailService::logEmailSend(const std::string& orderId,
                                const std::string& merchantId,
                                const std::string& toEmail,
                                const std::string& emailType,
                                const std::string& subject,
                                bool success,
                                const std::string& failReason) {
    try {
        auto& db = PayDb::instance();
        long now = std::time(nullptr);
        db.exec(
            "INSERT INTO v3_email_log"
            "(order_id,merchant_id,email_to,email_type,subject,send_status,send_time,fail_reason,created_at,updated_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?)",
            {orderId, merchantId, toEmail, emailType, subject,
             success ? "1" : "2",
             std::to_string(now),
             failReason,
             std::to_string(now),
             std::to_string(now)});
    } catch (const std::exception& e) {
        LOG_WARN << "[V3-Email] logEmailSend DB error: " << e.what();
    }
}

std::string EmailService::renderTemplate(const std::string& templateName,
                                         const nlohmann::json& data) {
    // 读取模板文件
    std::string tplDir;
    {
        std::lock_guard<std::mutex> lk(accountsMu_);
        tplDir = accounts_.empty() ? "./templates" : accounts_[0].templatePath;
    }
    std::string templatePath = tplDir + "/" + templateName + ".html";
    std::ifstream file(templatePath);

    if (!file.is_open()) {
        // 如果模板文件不存在，使用内置模板
        return renderBuiltinTemplate(templateName, data);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // 简单的模板变量替换
    for (auto& [key, value] : data.items()) {
        std::string placeholder = "{{" + key + "}}";
        std::string valueStr = value.is_string() ? value.get<std::string>() : value.dump();

        size_t pos = 0;
        while ((pos = content.find(placeholder, pos)) != std::string::npos) {
            content.replace(pos, placeholder.length(), valueStr);
            pos += valueStr.length();
        }
    }

    return content;
}

// 内置模板 HTML（与 build/templates/*.html 内容相同，供文件读取失败时使用）
static const char* kTplPaySuccess = R"HTML(
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml"><head>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
<title>WePay V3 支付系统</title>
<style>.textbutton a{font-family:'open sans',arial,sans-serif!important;color:#fff!important;}
body{margin:0;padding:0;}table{border-spacing:0;border-collapse:collapse;table-layout:fixed;margin:0 auto;}
table table table{table-layout:auto;}table td{border-collapse:collapse;}
a{color:#ff646a;text-decoration:none;}</style></head>
<body><table width="100%" border="0" align="center" cellpadding="0" cellspacing="0" bgcolor="#ffffff">
<tr><td align="center"><table bgcolor="#f8f8f8" width="100%" border="0" align="center" cellpadding="0" cellspacing="0">
<tr align="center" valign="top"><td><table width="600" border="0" align="center" cellpadding="0" cellspacing="0"><tr>
<td width="208" align="center" valign="top" bgcolor="#4a7c59"><table width="158" border="0" align="center" cellpadding="0" cellspacing="0">
<tr><td height="50"></td></tr>
<tr><td align="center"><span style="font-family:'Open Sans',Arial,sans-serif;font-size:22px;color:#fff;font-weight:bold;letter-spacing:2px;">W PAY.</span></td></tr>
<tr><td height="40"></td></tr>
<tr><td style="font-family:'Open Sans',Arial,sans-serif;font-size:16px;color:#fff;line-height:26px;font-weight:bold;">V3 支付系统</td></tr>
<tr><td height="5"></td></tr>
<tr><td style="font-family:'Open Sans',Arial,sans-serif;font-size:13px;color:#fff;line-height:26px;">专业可靠<br/>自动到账通知</td></tr>
<tr><td height="25"></td></tr></table></td>
<td width="392" align="center" valign="top"><table width="342" border="0" align="center" cellpadding="0" cellspacing="0">
<tr><td height="50"></td></tr>
<tr><td align="right" style="font-family:'Open Sans',Arial,sans-serif;font-size:38px;color:#3b3b3b;line-height:26px;">支付成功通知</td></tr>
<tr><td height="25"></td></tr>
<tr><td align="right"><table align="right" width="50" border="0" cellpadding="0" cellspacing="0"><tr><td bgcolor="#4a7c59" height="3" style="line-height:0;font-size:0;">&nbsp;</td></tr></table></td></tr>
<tr><td height="15"></td></tr>
<tr><td align="right" style="font-family:'Open Sans',Arial,sans-serif;font-size:16px;color:#3b3b3b;line-height:26px;font-weight:bold;">WePay V3 收款系统</td></tr>
<tr><td height="5"></td></tr>
<tr><td align="right" style="font-family:'Open Sans',Arial,sans-serif;font-size:13px;color:#7f8c8d;line-height:26px;">商户 {{merchantId}}</td></tr>
<tr><td height="25"></td></tr></table></td>
</tr></table></td></tr></table></td></tr>
<tr><td align="center"><table align="center" width="600" border="0" cellspacing="0" cellpadding="0"><tr>
<td align="center" style="border-bottom:3px solid #bcbcbc;"><table align="center" width="550" border="0" cellspacing="0" cellpadding="0">
<tr><td height="20"></td></tr>
<tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr>
<td align="left" style="font-size:13px;color:#3b3b3b;line-height:26px;">您好，您收到了一笔 <strong>{{payType}}</strong> 支付成功的订单通知，商户回调已自动触发，请知悉：</td>
</tr></table></td></tr><tr><td height="10"></td></tr>
</table></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">平台订单号</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{orderId}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">商户订单号</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{merchantOrderId}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">支付金额</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:18px;color:#4a7c59;line-height:26px;font-weight:bold;">&#165; {{money}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">支付方式</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{payType}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">支付时间</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{payTime}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">收款设备</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#7f8c8d;line-height:26px;font-weight:bold;">{{deviceId}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table align="center" width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" height="0" style="border-bottom:3px solid #3b3b3b;"></td></tr></table></td></tr>
<tr><td align="center"><table width="100%" border="0" align="center" cellpadding="0" cellspacing="0"><tr><td height="15" align="center" valign="top" style="border-bottom:10px solid #ecf0f1;"><table width="600" border="0" align="center" cellpadding="0" cellspacing="0"><tr><td height="25"></td></tr><tr><td align="center" style="font-family:'Open Sans',Arial,sans-serif;font-size:13px;color:#7f8c8d;line-height:26px;">此邮件由 WePay V3 支付系统自动发送，请勿回复</td></tr><tr><td height="25"></td></tr></table></td></tr></table></td></tr>
</table></body></html>
)HTML";

static const char* kTplPayFail = R"HTML(
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml"><head>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
<title>WePay V3 支付系统</title>
<style>.textbutton a{font-family:'open sans',arial,sans-serif!important;color:#fff!important;}
body{margin:0;padding:0;}table{border-spacing:0;border-collapse:collapse;table-layout:fixed;margin:0 auto;}
table table table{table-layout:auto;}table td{border-collapse:collapse;}
a{color:#ff646a;text-decoration:none;}</style></head>
<body><table width="100%" border="0" align="center" cellpadding="0" cellspacing="0" bgcolor="#ffffff">
<tr><td align="center"><table bgcolor="#f8f8f8" width="100%" border="0" align="center" cellpadding="0" cellspacing="0">
<tr align="center" valign="top"><td><table width="600" border="0" align="center" cellpadding="0" cellspacing="0"><tr>
<td width="208" align="center" valign="top" bgcolor="#c0392b"><table width="158" border="0" align="center" cellpadding="0" cellspacing="0">
<tr><td height="50"></td></tr>
<tr><td align="center"><span style="font-family:'Open Sans',Arial,sans-serif;font-size:22px;color:#fff;font-weight:bold;letter-spacing:2px;">W PAY.</span></td></tr>
<tr><td height="40"></td></tr>
<tr><td style="font-family:'Open Sans',Arial,sans-serif;font-size:16px;color:#fff;line-height:26px;font-weight:bold;">V3 支付系统</td></tr>
<tr><td height="5"></td></tr>
<tr><td style="font-family:'Open Sans',Arial,sans-serif;font-size:13px;color:#fff;line-height:26px;">专业可靠<br/>需要您手动处理</td></tr>
<tr><td height="25"></td></tr></table></td>
<td width="392" align="center" valign="top"><table width="342" border="0" align="center" cellpadding="0" cellspacing="0">
<tr><td height="50"></td></tr>
<tr><td align="right" style="font-family:'Open Sans',Arial,sans-serif;font-size:38px;color:#3b3b3b;line-height:26px;">支付异常通知</td></tr>
<tr><td height="25"></td></tr>
<tr><td align="right"><table align="right" width="50" border="0" cellpadding="0" cellspacing="0"><tr><td bgcolor="#ff646a" height="3" style="line-height:0;font-size:0;">&nbsp;</td></tr></table></td></tr>
<tr><td height="15"></td></tr>
<tr><td align="right" style="font-family:'Open Sans',Arial,sans-serif;font-size:16px;color:#3b3b3b;line-height:26px;font-weight:bold;">WePay V3 收款系统</td></tr>
<tr><td height="5"></td></tr>
<tr><td align="right" style="font-family:'Open Sans',Arial,sans-serif;font-size:13px;color:#7f8c8d;line-height:26px;">商户 {{merchantId}}</td></tr>
<tr><td height="25"></td></tr></table></td>
</tr></table></td></tr></table></td></tr>
<tr><td align="center"><table align="center" width="600" border="0" cellspacing="0" cellpadding="0"><tr>
<td align="center" style="border-bottom:3px solid #bcbcbc;"><table align="center" width="550" border="0" cellspacing="0" cellpadding="0">
<tr><td height="20"></td></tr>
<tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr>
<td align="left" style="font-size:13px;color:#3b3b3b;line-height:26px;">您好，以下订单出现异常：<strong style="color:#c0392b;">{{failReason}}</strong>。若订单实际已支付，请点击下方按钮手动触发回调：</td>
</tr></table></td></tr><tr><td height="10"></td></tr>
</table></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">平台订单号</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{orderId}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">商户订单号</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{merchantOrderId}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">订单金额</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:18px;color:#c0392b;line-height:26px;font-weight:bold;">&#165; {{money}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">支付方式</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{payType}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" style="border-bottom:1px solid #ecf0f1;"><table width="550" border="0" cellspacing="0" cellpadding="0"><tr><td height="15"></td></tr><tr><td><table width="100%" border="0" cellspacing="0" cellpadding="0"><tr><td width="220" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">创建时间</td><td width="330" align="left" style="font-family:'Open Sans',Arial,sans-serif;font-size:14px;color:#3b3b3b;line-height:26px;font-weight:bold;">{{createTime}}</td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr><tr><td height="5"></td></tr></table></td></tr>
<tr><td align="center"><table align="center" width="600" border="0" cellspacing="0" cellpadding="0"><tr><td align="center" height="0" style="border-bottom:3px solid #3b3b3b;"></td></tr></table></td></tr>
<tr><td align="center"><table align="center" width="600" border="0" cellspacing="0" cellpadding="0">
<tr><td height="20"></td></tr>
<tr><td style="font-size:13px;color:#7f8c8d;line-height:26px;display:{{callbackUrlVisible}};">若该订单实际已成功支付，可点击下方按钮补发一次商户回调。按钮仅展示公网可访问的安全地址，链接有效期 24 小时：</td></tr>
<tr><td style="font-size:13px;color:#c0392b;line-height:26px;display:{{callbackUrlHidden}};">当前系统未生成可供邮件访问的公网安全链接，因此已隐藏“手动触发商户回调”按钮。请先在系统配置中将 <strong>站点根地址</strong> 设置为公网可访问的正式域名（例如 <strong>https://pay.example.com</strong>，末尾不要加斜杠），保存后再重发邮件。</td></tr>
<tr><td height="12"></td></tr>
<tr><td style="font-size:12px;color:#98a2b3;line-height:22px;display:{{callbackUrlHidden}};">开发环境、内网地址、localhost、127.0.0.1 或缺少协议头的地址都不会在邮件中展示该按钮，以避免商户点开无效链接。</td></tr>
<tr><td height="20"></td></tr>
<tr style="display:{{callbackUrlVisible}};"><td align="center"><table width="80%" border="0" align="center" cellpadding="0" cellspacing="0" bgcolor="#27ae60" class="textbutton" style="border-radius:5px;border-bottom:3px solid #e6e6e6"><tr><td height="45" align="center" style="font-size:16px;color:#fff;line-height:28px;padding-left:15px;padding-right:15px;"><a href="{{callbackUrl}}" style="display:block;color:#fff;text-decoration:none;">手动触发商户回调</a></td></tr></table></td></tr>
<tr style="display:{{callbackUrlVisible}};"><td height="12"></td></tr>
<tr style="display:{{callbackUrlVisible}};"><td align="center" style="font-size:12px;color:#98a2b3;line-height:22px;word-break:break-all;">如按钮无法点击，可复制以下链接在浏览器打开：<br/>{{callbackUrl}}</td></tr>
<tr style="display:{{callbackUrlVisible}};"><td height="30"></td></tr>
<tr><td height="15" style="border-bottom:3px solid #bcbcbc;"></td></tr>
</table></td></tr>
<tr><td align="center"><table width="100%" border="0" align="center" cellpadding="0" cellspacing="0"><tr><td height="15" align="center" valign="top" style="border-bottom:10px solid #ecf0f1;"><table width="600" border="0" align="center" cellpadding="0" cellspacing="0"><tr><td height="25"></td></tr><tr><td align="center" style="font-family:'Open Sans',Arial,sans-serif;font-size:13px;color:#7f8c8d;line-height:26px;">此邮件由 WePay V3 支付系统自动发送，请勿回复</td></tr><tr><td height="25"></td></tr></table></td></tr></table></td></tr>
</table></body></html>
)HTML";

static const char* kTplDailySummary = R"HTML(
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml"><head>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>WePay V3 每日汇总</title>
<style>
body{margin:0;padding:0;background:#f4f7fb;-webkit-text-size-adjust:none;-ms-text-size-adjust:none;}
table{border-spacing:0;border-collapse:collapse;table-layout:fixed;margin:0 auto;}table table table{table-layout:auto;}table td{border-collapse:collapse;}
a{color:#1677ff;text-decoration:none;}
</style></head>
<body><table width="100%" border="0" align="center" cellpadding="0" cellspacing="0" bgcolor="#f4f7fb">
<tr><td align="center">
<table width="100%" border="0" align="center" cellpadding="0" cellspacing="0">
<tr><td height="28"></td></tr>
<tr><td align="center">
<table width="640" border="0" align="center" cellpadding="0" cellspacing="0" style="width:640px;max-width:640px;background:#ffffff;border-radius:24px;overflow:hidden;box-shadow:0 12px 40px rgba(15,23,42,0.10);">
<tr><td align="center" style="background:linear-gradient(135deg,#1677ff 0%,#36cfc9 100%);">
<table width="560" border="0" align="center" cellpadding="0" cellspacing="0" style="width:560px;max-width:560px;">
<tr><td height="34"></td></tr>
<tr><td align="left"><span style="display:inline-block;padding:8px 18px;border-radius:999px;background:rgba(255,255,255,0.18);border:1px solid rgba(255,255,255,0.24);font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:18px;color:#ffffff;letter-spacing:0.5px;">WEPAY V3 · 每日经营简报</span></td></tr>
<tr><td height="24"></td></tr>
<tr><td align="left" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:34px;line-height:42px;color:#ffffff;font-weight:bold;">昨日订单汇总已生成</td></tr>
<tr><td height="12"></td></tr>
<tr><td align="left" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:15px;line-height:26px;color:rgba(255,255,255,0.88);">您好，以下为 <strong>{{date}}</strong> 的订单经营数据快照，方便您快速完成晨间对账、异常复核与收款趋势判断。</td></tr>
<tr><td height="24"></td></tr>
<tr><td align="center">
<table width="100%" border="0" align="center" cellpadding="0" cellspacing="0" style="background:rgba(255,255,255,0.95);border-radius:18px;border:1px solid rgba(255,255,255,0.38);">
<tr><td height="24"></td></tr>
<tr><td align="center" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:22px;color:#6b7280;">统计日期</td></tr>
<tr><td height="8"></td></tr>
<tr><td align="center" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:28px;line-height:34px;color:#111827;font-weight:bold;">{{date}}</td></tr>
<tr><td height="6"></td></tr>
<tr><td align="center" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:22px;color:#6b7280;">统计口径：按昨日自然日订单创建时间汇总</td></tr>
<tr><td height="24"></td></tr>
</table>
</td></tr>
<tr><td height="34"></td></tr>
</table>
</td></tr>
<tr><td align="center">
<table width="560" border="0" align="center" cellpadding="0" cellspacing="0" style="width:560px;max-width:560px;">
<tr><td height="34"></td></tr>
<tr><td align="left" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:20px;line-height:28px;color:#111827;font-weight:bold;">核心指标概览</td></tr>
<tr><td height="8"></td></tr>
<tr><td align="left" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:14px;line-height:24px;color:#6b7280;">建议优先关注交易总量、成功转化和失败订单数量，快速判断昨日收款是否稳定。</td></tr>
<tr><td height="20"></td></tr>
</table>
</td></tr>
<tr><td align="center">
<table width="560" border="0" align="center" cellpadding="0" cellspacing="0" style="width:560px;max-width:560px;">
<tr>
<td width="272" valign="top">
<table width="272" border="0" align="left" cellpadding="0" cellspacing="0" style="width:272px;background:#f8fbff;border:1px solid #e6f4ff;border-radius:18px;">
<tr><td height="22"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:22px;color:#6b7280;">总订单数</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:32px;line-height:38px;color:#1677ff;font-weight:bold;">{{totalOrders}}</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:12px;line-height:22px;color:#8c8c8c;">昨日进入系统统计范围的全部订单笔数</td></tr>
<tr><td height="22"></td></tr>
</table>
</td>
<td width="16"></td>
<td width="272" valign="top">
<table width="272" border="0" align="right" cellpadding="0" cellspacing="0" style="width:272px;background:#f6ffed;border:1px solid #d9f7be;border-radius:18px;">
<tr><td height="22"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:22px;color:#6b7280;">成功订单</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:32px;line-height:38px;color:#16a34a;font-weight:bold;">{{successOrders}}</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:12px;line-height:22px;color:#8c8c8c;">已完成支付并计入成功收款的订单笔数</td></tr>
<tr><td height="22"></td></tr>
</table>
</td>
</tr>
<tr><td height="16"></td></tr>
<tr>
<td width="272" valign="top">
<table width="272" border="0" align="left" cellpadding="0" cellspacing="0" style="width:272px;background:#fff2f0;border:1px solid #ffccc7;border-radius:18px;">
<tr><td height="22"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:22px;color:#6b7280;">失败 / 超时订单</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:32px;line-height:38px;color:#cf1322;font-weight:bold;">{{failedOrders}}</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:12px;line-height:22px;color:#8c8c8c;">建议结合通道波动、设备离线和网络状态复核</td></tr>
<tr><td height="22"></td></tr>
</table>
</td>
<td width="16"></td>
<td width="272" valign="top">
<table width="272" border="0" align="right" cellpadding="0" cellspacing="0" style="width:272px;background:#f6ffed;border:1px solid #d9f7be;border-radius:18px;">
<tr><td height="22"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:22px;color:#6b7280;">总交易金额</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:32px;line-height:38px;color:#16a34a;font-weight:bold;">&#165; {{totalAmount}}</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:24px;padding-right:24px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:12px;line-height:22px;color:#8c8c8c;">按成功支付订单汇总的到账金额</td></tr>
<tr><td height="22"></td></tr>
</table>
</td>
</tr>
</table>
</td></tr>
<tr><td align="center">
<table width="560" border="0" align="center" cellpadding="0" cellspacing="0" style="width:560px;max-width:560px;">
<tr><td height="28"></td></tr>
<tr><td align="left" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:18px;line-height:26px;color:#111827;font-weight:bold;">阅读建议</td></tr>
<tr><td height="12"></td></tr>
</table>
</td></tr>
<tr><td align="center">
<table width="560" border="0" align="center" cellpadding="0" cellspacing="0" style="width:560px;max-width:560px;background:#f9fafb;border:1px dashed #d1d5db;border-radius:16px;">
<tr><td height="18"></td></tr>
<tr><td style="padding-left:22px;padding-right:22px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:24px;color:#6b7280;">1. 若失败/超时订单明显偏高，建议优先检查通道状态、设备在线情况及异步通知链路。</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:22px;padding-right:22px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:24px;color:#6b7280;">2. 总交易金额按成功订单汇总，适合用于晨间快速对账；如需逐单核对，请结合后台订单明细进一步确认。</td></tr>
<tr><td height="8"></td></tr>
<tr><td style="padding-left:22px;padding-right:22px;font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:13px;line-height:24px;color:#6b7280;">3. 本邮件仅作经营概览提醒，不替代商户财务最终结算报表。</td></tr>
<tr><td height="18"></td></tr>
</table>
</td></tr>
<tr><td align="center">
<table width="100%" border="0" align="center" cellpadding="0" cellspacing="0">
<tr><td height="30"></td></tr>
<tr><td align="center" style="font-family:'Microsoft YaHei','Open Sans',Arial,sans-serif;font-size:12px;line-height:22px;color:#9ca3af;padding-left:20px;padding-right:20px;">此邮件由 WePay V3 支付系统每日 10 点自动发送，请勿直接回复。</td></tr>
<tr><td height="26"></td></tr>
</table>
</td></tr>
</table>
</td></tr>
<tr><td height="28"></td></tr>
</table>
</td></tr>
</table></body></html>
)HTML";

std::string EmailService::renderBuiltinTemplate(const std::string& templateName,
                                               const nlohmann::json& data) {
    // 选择内置模板字符串
    std::string content;
    if (templateName == "pay_success")    content = kTplPaySuccess;
    else if (templateName == "pay_fail")  content = kTplPayFail;
    else if (templateName == "daily_summary") content = kTplDailySummary;
    else return "";

    // 复用与 renderTemplate 相同的 {{key}} 替换逻辑
    for (auto& [key, value] : data.items()) {
        std::string placeholder = "{{" + key + "}}";
        std::string val = value.is_string() ? value.get<std::string>() : value.dump();
        size_t pos = 0;
        while ((pos = content.find(placeholder, pos)) != std::string::npos) {
            content.replace(pos, placeholder.length(), val);
            pos += val.length();
        }
    }
    return content;
}

// 内部：用指定账号发送
bool EmailService::sendEmailWith(const EmailConfig& cfg,
                                  const std::string& to,
                                  const std::string& subject,
                                  const std::string& htmlBody) {
    if (cfg.smtpHost.empty()) {
        LOG_WARN << "[EmailService] SMTP host empty, skip send to " << to;
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* recipients = nullptr;
    CURLcode res = CURLE_OK;
    try {
        std::string smtpUrl = (cfg.useSsl ? "smtps://" : "smtp://")
                              + cfg.smtpHost + ":" + std::to_string(cfg.smtpPort);
        curl_easy_setopt(curl, CURLOPT_URL, smtpUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, ("<" + cfg.fromEmail + ">").c_str());
        recipients = curl_slist_append(recipients, ("<" + to + ">").c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        std::ostringstream payload;
        payload << "From: " << cfg.fromName << " <" << cfg.fromEmail << ">\r\n"
                << "To: <" << to << ">\r\n"
                << "Subject: " << subject << "\r\n"
                << "MIME-Version: 1.0\r\n"
                << "Content-Type: text/html; charset=UTF-8\r\n"
                << "\r\n" << htmlBody;

        std::string payloadStr = payload.str();
        UploadStatus upload_ctx{payloadStr.c_str(), 0};
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, payloadSource);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        if (cfg.useSsl) curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            LOG_ERROR << "[EmailService] SMTP send failed: " << curl_easy_strerror(res)
                      << " (code=" << res << ")"
                      << " host=" << cfg.smtpHost << ":" << cfg.smtpPort
                      << " ssl=" << cfg.useSsl
                      << " user=" << cfg.username;
        }
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        return res == CURLE_OK;
    } catch (const std::exception& e) {
        LOG_ERROR << "[EmailService] sendEmailWith failed: " << e.what();
        if (recipients) curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        return false;
    }
}

// 公开发送：转用 SmtpUtils（已验证可用）发 HTML 邮件
bool EmailService::sendEmail(const std::string& to,
                             const std::string& subject,
                             const std::string& htmlBody) {
    // 确保 SmtpUtils 已加载 v3_email_account 配置
    if (!SmtpUtils::instance().isConfigured()) {
        try {
            auto& db = PayDb::instance();
            auto rows = db.query(
                "SELECT smtp_host,smtp_port,username,password,from_name "
                "FROM v3_email_account WHERE status=1 ORDER BY id ASC LIMIT 5", {});
            if (rows.empty()) {
                LOG_WARN << "[EmailService] no email accounts configured";
                return false;
            }
            std::vector<SmtpUtils::Sender> senders;
            for (auto& r : rows) senders.push_back({r["username"], r["password"]});
            int port = rows[0]["smtp_port"].empty() ? 465 : std::stoi(rows[0]["smtp_port"]);
            SmtpUtils::instance().loadConfig(
                rows[0]["smtp_host"], port,
                rows[0]["from_name"].empty() ? "WePay" : rows[0]["from_name"],
                senders);
        } catch (const std::exception& e) {
            LOG_WARN << "[EmailService] loadConfig failed: " << e.what();
            return false;
        }
    }
    LOG_INFO << "[EmailService] sending HTML via SmtpUtils -> " << to;
    bool ok = SmtpUtils::instance().sendHtml(to, subject, htmlBody);
    if (ok) {
        try {
            PayDb::instance().exec(
                "UPDATE v3_email_account SET send_count=send_count+1,updated_at=? WHERE status=1",
                {std::to_string(std::time(nullptr))});
        } catch (...) {}
    }
    return ok;
}

size_t EmailService::payloadSource(void* ptr, size_t size, size_t nmemb, void* userp) {
    UploadStatus* upload_ctx = static_cast<UploadStatus*>(userp);
    const char* data = upload_ctx->data;

    if ((size == 0) || (nmemb == 0) || ((size * nmemb) < 1)) {
        return 0;
    }

    size_t len = strlen(data + upload_ctx->bytesRead);
    size_t to_copy = (len < (size * nmemb)) ? len : (size * nmemb);

    if (to_copy > 0) {
        memcpy(ptr, data + upload_ctx->bytesRead, to_copy);
        upload_ctx->bytesRead += to_copy;
    }

    return to_copy;
}

// CallbackTokenGenerator实现
std::string CallbackTokenGenerator::generateToken(const TokenData& data,
                                                 const std::string& secret) {
    // 构建待签名字符串
    std::ostringstream oss;
    oss << data.orderId << "|"
        << data.merchantId << "|"
        << data.expireTime;

    std::string signData = oss.str();
    std::string signature = hmacSha256(signData, secret);

    // 构建令牌：base64(orderId|merchantId|expireTime|signature)
    std::string token = signData + "|" + signature;
    return base64Encode((unsigned char*)token.c_str(), token.length());
}

bool CallbackTokenGenerator::verifyToken(const std::string& token,
                                        const std::string& orderId,
                                        const std::string& merchantId,
                                        const std::string& secret) {
    try {
        // Base64解码
        std::string decoded = base64Decode(token);

        // 分割字段
        std::vector<std::string> parts;
        std::stringstream ss(decoded);
        std::string part;
        while (std::getline(ss, part, '|')) {
            parts.push_back(part);
        }

        if (parts.size() != 4) {
            return false;
        }

        // 验证订单号和商户ID
        if (parts[0] != orderId || parts[1] != merchantId) {
            return false;
        }

        // 验证过期时间
        int64_t expireTime = std::stoll(parts[2]);
        if (std::time(nullptr) > expireTime) {
            return false; // 令牌已过期
        }

        // 验证签名
        std::string signData = parts[0] + "|" + parts[1] + "|" + parts[2];
        std::string expectedSignature = hmacSha256(signData, secret);

        return parts[3] == expectedSignature;

    } catch (...) {
        return false;
    }
}

std::string CallbackTokenGenerator::generateCallbackUrl(const std::string& baseUrl,
                                                       const TokenData& data,
                                                       const std::string& secret) {
    std::string token = generateToken(data, secret);

    std::ostringstream url;
    url << baseUrl << "/api/wepay/v3/callback/manual"
        << "?orderId=" << data.orderId
        << "&merchantId=" << data.merchantId
        << "&token=" << token;

    return url.str();
}

bool CallbackTokenGenerator::isSafePublicUrl(const std::string& url) {
    static const std::regex urlPattern(R"(^https?://([^/:?#]+)(?::\d+)?(?:[/?#].*)?$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, urlPattern)) {
        return false;
    }

    std::string host = match[1].str();
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (host == "localhost" || host == "127.0.0.1" || host == "0.0.0.0" || host == "::1") {
        return false;
    }

    static const std::regex ipv4Pattern(R"(^(
        (25[0-5]|2[0-4]\d|1?\d?\d)\.
        (25[0-5]|2[0-4]\d|1?\d?\d)\.
        (25[0-5]|2[0-4]\d|1?\d?\d)\.
        (25[0-5]|2[0-4]\d|1?\d?\d)
    )$)", std::regex::extended | std::regex::icase);
    std::smatch ipMatch;
    if (std::regex_match(host, ipMatch, ipv4Pattern)) {
        auto octet = [](const std::ssub_match& v) { return std::stoi(v.str()); };
        int first = octet(ipMatch[2]);
        int second = octet(ipMatch[3]);
        int third = octet(ipMatch[4]);
        int fourth = octet(ipMatch[5]);
        (void)third;
        (void)fourth;

        if (first == 10 || first == 127) return false;
        if (first == 172 && second >= 16 && second <= 31) return false;
        if (first == 192 && second == 168) return false;
        if (first == 169 && second == 254) return false;
        if (first == 0) return false;
    }

    return true;
}

std::string CallbackTokenGenerator::hmacSha256(const std::string& data,
                                              const std::string& key) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    HMAC(EVP_sha256(),
         key.c_str(), key.length(),
         (unsigned char*)data.c_str(), data.length(),
         hash, nullptr);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    return oss.str();
}

std::string CallbackTokenGenerator::base64Encode(const unsigned char* data, size_t len) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    BIO_write(bio, data, len);
    BIO_flush(bio);

    BUF_MEM* bufferPtr;
    BIO_get_mem_ptr(bio, &bufferPtr);

    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);

    return result;
}

std::string CallbackTokenGenerator::base64Decode(const std::string& encoded) {
    BIO* bio = BIO_new_mem_buf(encoded.c_str(), encoded.length());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    std::vector<unsigned char> buffer(encoded.length());
    int decodedLength = BIO_read(bio, buffer.data(), encoded.length());
    BIO_free_all(bio);

    return std::string((char*)buffer.data(), decodedLength);
}

// ManualCallbackController实现
void ManualCallbackController::handleManualCallback(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        std::string orderId = req->getParameter("orderId");
        std::string merchantId = req->getParameter("merchantId");
        std::string token = req->getParameter("token");

        if (orderId.empty() || merchantId.empty() || token.empty()) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setBody("Missing parameters");
            callback(resp);
            return;
        }

        // 验证令牌
        if (!verifyCallbackToken(token, orderId, merchantId)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k403Forbidden);
            resp->setBody("Invalid or expired token");
            callback(resp);
            return;
        }

        // 获取客户端信息
        std::string clientIp = req->getPeerAddr().toIp();
        std::string userAgent = req->getHeader("User-Agent");

        // 触发商户回调
        bool success = triggerMerchantCallback(orderId, merchantId, clientIp, userAgent);

        // 记录手动回调日志
        logManualCallback(orderId, merchantId, token, success,
                         success ? "Success" : "Failed", clientIp, userAgent);

        // 返回结果页面
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_HTML);

        if (success) {
            resp->setBody(R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>回调成功</title>
    <style>
        body { font-family: Arial; text-align: center; padding: 50px; }
        .success { color: #4CAF50; font-size: 24px; }
    </style>
</head>
<body>
    <h1 class="success">✅ 回调触发成功</h1>
    <p>订单号：)" + orderId + R"(</p>
    <p>商户回调已成功触发，请检查订单状态</p>
</body>
</html>
)");
        } else {
            resp->setBody(R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>回调失败</title>
    <style>
        body { font-family: Arial; text-align: center; padding: 50px; }
        .error { color: #f44336; font-size: 24px; }
    </style>
</head>
<body>
    <h1 class="error">❌ 回调触发失败</h1>
    <p>订单号：)" + orderId + R"(</p>
    <p>请联系技术支持处理</p>
</body>
</html>
)");
        }

        callback(resp);

    } catch (const std::exception& e) {
        LOG_ERROR << "Manual callback error: " << e.what();
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k500InternalServerError);
        resp->setBody("Internal server error");
        callback(resp);
    }
}

bool ManualCallbackController::verifyCallbackToken(const std::string& token,
                                                   const std::string& orderId,
                                                   const std::string& merchantId) {
    auto& config = WepayV3Config::getInstance();
    return CallbackTokenGenerator::verifyToken(token, orderId, merchantId,
                                              config.security.hmacSecret);
}

bool ManualCallbackController::triggerMerchantCallback(const std::string& orderId,
                                                       const std::string& merchantId,
                                                       const std::string& clientIp,
                                                       const std::string& userAgent) {
    try {
        auto& db = PayDb::instance();

        // 1. 查订单
        auto orders = db.query(
            "SELECT order_id,merchant_order_id,amount,pay_type,status,pay_time "
            "FROM v3_order WHERE order_id=? LIMIT 1", {orderId});
        if (orders.empty()) {
            LOG_WARN << "[V3-ManualCB] order not found: " << orderId;
            return false;
        }
        auto& ord = orders[0];

        // 2. 查商户回调地址和密钥
        auto cfgs = db.query(
            "SELECT callback_url,hmac_secret FROM v3_merchant_config "
            "WHERE merchant_id=? AND status=1 LIMIT 1", {merchantId});
        if (cfgs.empty() || cfgs[0]["callback_url"].empty()) {
            LOG_WARN << "[V3-ManualCB] no callback_url for merchant: " << merchantId;
            return false;
        }
        std::string callbackUrl = cfgs[0]["callback_url"];
        std::string secret      = cfgs[0]["hmac_secret"];

        // 3. 构建回调 payload（sorted-qs HMAC-SHA256 签名）
        long payTime = ord.count("pay_time") && !ord.at("pay_time").empty()
                       ? std::stol(ord.at("pay_time")) : 0;
        long ts = std::time(nullptr);

        nlohmann::json body;
        body["orderId"]         = orderId;
        body["merchantOrderId"] = ord.count("merchant_order_id") ? ord.at("merchant_order_id") : "";
        body["merchantId"]      = merchantId;
        body["amount"]          = ord.count("amount")   ? ord.at("amount")   : "0";
        body["payType"]         = ord.count("pay_type") ? ord.at("pay_type") : "";
        body["status"]          = ord.count("status")   ? ord.at("status")   : "PAID";
        body["payTime"]         = std::to_string(payTime);
        body["timestamp"]       = std::to_string(ts);
        body["source"]          = "manual_callback";
        body["clientIp"]        = clientIp;

        // sorted-qs 签名
        std::map<std::string,std::string> params;
        for (auto& [k,v] : body.items()) {
            if (v.is_string()) params[k] = v.get<std::string>();
            else               params[k] = v.dump();
        }
        std::ostringstream qs;
        for (auto& [k,v] : params) qs << k << "=" << v << "&";
        std::string toSign = qs.str();
        if (!toSign.empty()) toSign.pop_back();

        unsigned char hash[SHA256_DIGEST_LENGTH];
        HMAC(EVP_sha256(),
             secret.c_str(), secret.size(),
             (unsigned char*)toSign.c_str(), toSign.size(),
             hash, nullptr);
        std::ostringstream sigHex;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            sigHex << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        body["sign"] = sigHex.str();

        // 4. curl POST
        std::string payload = body.dump();
        bool success = false;

        CURL* curl = curl_easy_init();
        if (curl) {
            struct curl_slist* hdrs = nullptr;
            hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_URL, callbackUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                long httpCode = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                success = (httpCode >= 200 && httpCode < 300);
            } else {
                LOG_WARN << "[V3-ManualCB] curl error: " << curl_easy_strerror(res);
            }
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(curl);
        }

        LOG_INFO << "[V3-ManualCB] callback " << (success ? "OK" : "FAIL")
                 << " orderId=" << orderId << " url=" << callbackUrl;
        return success;

    } catch (const std::exception& e) {
        LOG_ERROR << "[V3-ManualCB] triggerMerchantCallback error: " << e.what();
        return false;
    }
}

void ManualCallbackController::logManualCallback(const std::string& orderId,
                                                 const std::string& merchantId,
                                                 const std::string& token,
                                                 bool success,
                                                 const std::string& response,
                                                 const std::string& clientIp,
                                                 const std::string& userAgent) {
    try {
        auto& db = PayDb::instance();
        long now = std::time(nullptr);

        // 查商户回调URL（用于记录）
        std::string cbUrl;
        auto urlRows = db.query(
            "SELECT callback_url FROM v3_merchant_config WHERE merchant_id=? LIMIT 1",
            {merchantId});
        if (!urlRows.empty()) cbUrl = urlRows[0].count("callback_url") ? urlRows[0].at("callback_url") : "";

        // 查是否已存在（保留 created_at 和 token_expire）
        auto existing = db.query(
            "SELECT created_at,token_expire FROM v3_manual_callback_log WHERE order_id=? LIMIT 1",
            {orderId});

        if (existing.empty()) {
            // 首次插入
            db.exec(
                "INSERT INTO v3_manual_callback_log"
                "(order_id,merchant_id,callback_url,callback_token,token_expire,"
                " callback_status,callback_time,callback_response,client_ip,user_agent,"
                " created_at,updated_at)"
                " VALUES(?,?,?,?,0,?,?,?,?,?,?,?)",
                {orderId, merchantId, cbUrl, token,
                 success ? "1" : "2",
                 std::to_string(now),
                 response.substr(0, 500),
                 clientIp,
                 userAgent.substr(0, 500),
                 std::to_string(now),
                 std::to_string(now)});
        } else {
            // 更新已有记录
            db.exec(
                "UPDATE v3_manual_callback_log SET"
                " callback_status=?,callback_time=?,callback_response=?,"
                " client_ip=?,user_agent=?,updated_at=?"
                " WHERE order_id=?",
                {success ? "1" : "2",
                 std::to_string(now),
                 response.substr(0, 500),
                 clientIp,
                 userAgent.substr(0, 500),
                 std::to_string(now),
                 orderId});
        }
    } catch (const std::exception& e) {
        LOG_WARN << "[V3-ManualCB] logManualCallback DB error: " << e.what();
    }
}

} // namespace v3
} // namespace wepay
