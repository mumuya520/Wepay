// WePay-Cpp — V3 邮件通知服务
// WePay V3 邮件通知 + 手动回调令牌服务
// 复用 EmailService (NotifyChannels.h) 的 libcurl SMTP，不引入额外依赖
// 令牌格式: base64( orderId|mchId|expireTs ) + "." + hmac_sha256 hex
// 所有邮件均用内嵌 HTML 模板，无须外部文件
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式化库
#include <thread>
#include <ctime>
#include <cstring>
#include <json/json.h>
#include "PayDb.h"
#include "Md5Utils.h"
#include "NotifyChannels.h"   // EmailService::instance()

// ───────────────────────────────────────────────────────────────────
//  令牌工具 (HMAC-SHA256, 无过期 = 24h)
// ───────────────────────────────────────────────────────────────────
class V3CallbackToken {
public:
    // 生成令牌: base64url(orderId|mchId|expireTs).hmac
    static std::string generate(const std::string &orderId, const std::string &mchId,
                                long long expireSec = 86400) {
        long long exp = std::time(nullptr) + expireSec;
        std::string plain = orderId + "|" + mchId + "|" + std::to_string(exp);
        std::string sig   = Md5Utils::hmacSha256(secret(), plain);
        return b64url(plain) + "." + sig;
    }

    // 验证令牌, 返回 {ok, orderId, mchId}
    struct Result { bool ok; std::string orderId; std::string mchId; };
    static Result verify(const std::string &token) {
        auto dot = token.rfind('.');
        if (dot == std::string::npos) return {false, {}, {}};
        std::string plain = unb64url(token.substr(0, dot));
        std::string sig   = token.substr(dot + 1);

        // 分割 plain
        auto p1 = plain.find('|');
        if (p1 == std::string::npos) return {false, {}, {}};
        auto p2 = plain.find('|', p1 + 1);
        if (p2 == std::string::npos) return {false, {}, {}};

        std::string orderId = plain.substr(0, p1);
        std::string mchId   = plain.substr(p1 + 1, p2 - p1 - 1);
        long long   exp     = 0;
        try { exp = std::stoll(plain.substr(p2 + 1)); } catch (...) { return {false, {}, {}}; }

        // 时间校验
        if (std::time(nullptr) > exp) return {false, {}, {}};

        // 签名校验
        std::string expected = Md5Utils::hmacSha256(secret(), plain);
        if (sig != expected) return {false, {}, {}};
        return {true, orderId, mchId};
    }

private:
    static std::string secret() {
        std::string k = PayDb::instance().getSetting("key");
        return k.empty() ? "wepay_v3_callback_secret" : k;
    }

    // URL-safe base64 (no padding)
    static std::string b64url(const std::string &s) {
        static const char *tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int i = 0; unsigned char b3[3]; unsigned char b4[4];
        for (char c : s) {
            b3[i++] = c;
            if (i == 3) {
                b4[0]=(b3[0]&0xfc)>>2; b4[1]=((b3[0]&0x03)<<4)|((b3[1]&0xf0)>>4);
                b4[2]=((b3[1]&0x0f)<<2)|((b3[2]&0xc0)>>6); b4[3]=b3[2]&0x3f;
                for (int j=0;j<4;j++) { char ch=tab[b4[j]]; if(ch=='+')ch='-'; if(ch=='/')ch='_'; out+=ch; }
                i=0;
            }
        }
        if (i) {
            for (int j=i;j<3;j++) b3[j]=0;
            b4[0]=(b3[0]&0xfc)>>2; b4[1]=((b3[0]&0x03)<<4)|((b3[1]&0xf0)>>4);
            b4[2]=((b3[1]&0x0f)<<2)|((b3[2]&0xc0)>>6);
            for(int j=0;j<i+1;j++) { char ch=tab[b4[j]]; if(ch=='+')ch='-'; if(ch=='/')ch='_'; out+=ch; }
        }
        return out;
    }

    static std::string unb64url(const std::string &s) {
        static const char *tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out; int bits=0,val=0;
        for (char raw : s) {
            char c = raw;
            if (c=='-') c='+'; if (c=='_') c='/';
            const char *p = strchr(tab, c);
            if (!p) continue;
            val = (val<<6)|(int)(p-tab); bits+=6;
            if (bits>=8) { bits-=8; out+=(char)((val>>bits)&0xff); }
        }
        return out;
    }
};

// ───────────────────────────────────────────────────────────────────
//  V3 邮件通知服务
// ───────────────────────────────────────────────────────────────────
class WepayV3EmailService {
public:
    static WepayV3EmailService &instance() {
        static WepayV3EmailService ins;
        return ins;
    }

    // 支付成功通知（异步发送，不阻塞业务线程）
    void sendPaySuccess(const std::string &orderId, const std::string &mchId,
                        const std::string &toEmail, const std::string &amount,
                        const std::string &payType, const std::string &deviceId) {
        if (toEmail.empty()) return;
        logEmail(orderId, mchId, toEmail, "PAY_SUCCESS");
        std::thread([=]() {
            std::string body = tplSuccess(orderId, mchId, amount, payType, deviceId);
            bool ok = EmailService::instance().send(
                toEmail, "【WePay V3】支付成功通知 - 订单" + orderId, body);
            updateEmailLog(orderId, "PAY_SUCCESS", ok, ok ? "" : "SMTP发送失败");
        }).detach();
    }

    // 支付失败/回调失败通知（含手动回调链接）
    void sendPayFail(const std::string &orderId, const std::string &mchId,
                     const std::string &toEmail, const std::string &amount,
                     const std::string &failReason, const std::string &baseUrl) {
        if (toEmail.empty()) return;
        std::string token      = V3CallbackToken::generate(orderId, mchId);
        std::string callbackUrl = baseUrl + "/api/wepay/v3/callback/manual"
                                  "?order_id=" + orderId
                                  + "&mch_id=" + mchId
                                  + "&token=" + token;
        saveCallbackToken(orderId, mchId, token);
        logEmail(orderId, mchId, toEmail, "PAY_FAIL");
        std::thread([=]() {
            std::string body = tplFail(orderId, mchId, amount, failReason, callbackUrl);
            bool ok = EmailService::instance().send(
                toEmail, "【WePay V3】支付失败通知 - 订单" + orderId, body);
            updateEmailLog(orderId, "PAY_FAIL", ok, ok ? "" : "SMTP发送失败");
        }).detach();
    }

    // 每日汇总邮件（由 CronService 每天 10:00 调用）
    void sendDailySummary(const std::string &mchId, const std::string &toEmail,
                          const std::string &baseUrl) {
        if (toEmail.empty()) return;

        auto &db   = PayDb::instance();
        long long dayStart = (std::time(nullptr) / 86400 - 1) * 86400;  // 昨日 00:00
        long long dayEnd   = dayStart + 86400;

        auto stats = db.queryOne(
            "SELECT COUNT(*) AS total,"
            " SUM(CASE WHEN state=1 THEN 1 ELSE 0 END) AS success,"
            " SUM(CASE WHEN state<>1 THEN 1 ELSE 0 END) AS fail,"
            " COALESCE(SUM(CASE WHEN state=1 THEN CAST(real_amount AS REAL) ELSE 0 END),0) AS amount "
            "FROM pay_order WHERE mch_id=? AND created_at>=? AND created_at<?",
            {mchId, std::to_string(dayStart), std::to_string(dayEnd)});

        auto failOrders = db.query(
            "SELECT order_id,real_amount,created_at FROM pay_order "
            "WHERE mch_id=? AND state<>1 AND created_at>=? AND created_at<? LIMIT 30",
            {mchId, std::to_string(dayStart), std::to_string(dayEnd)});

        // 为每条失败订单生成手动回调链接
        Json::Value failList(Json::arrayValue);
        for (auto &o : failOrders) {
            std::string token = V3CallbackToken::generate(o["order_id"], mchId);
            saveCallbackToken(o["order_id"], mchId, token);
            Json::Value item;
            item["order_id"] = o["order_id"];
            item["amount"]   = o["real_amount"];
            item["time"]     = o["created_at"];
            item["callback_url"] = baseUrl + "/api/wepay/v3/callback/manual"
                                   "?order_id=" + o["order_id"]
                                   + "&mch_id=" + mchId
                                   + "&token=" + token;
            failList.append(item);
        }

        // 格式化日期
        char dateBuf[16]; time_t ds = (time_t)dayStart;
        struct tm *tm = localtime(&ds);
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", tm);

        logEmail("", mchId, toEmail, "DAILY_SUMMARY");
        std::thread([=]() mutable {
            std::string body = tplDailySummary(
                dateBuf,
                stats.count("total")  ? stats.at("total")   : "0",
                stats.count("success")? stats.at("success") : "0",
                stats.count("fail")   ? stats.at("fail")    : "0",
                stats.count("amount") ? stats.at("amount")  : "0.00",
                failList);
            bool ok = EmailService::instance().send(
                toEmail, std::string("【WePay V3】") + dateBuf + " 每日订单汇总", body);
            updateEmailLog("", "DAILY_SUMMARY", ok, ok ? "" : "SMTP发送失败");
        }).detach();
    }

private:
    // ── 数据库操作 ──────────────────────────────────────────────────
    static void logEmail(const std::string &orderId, const std::string &mchId,
                         const std::string &toEmail, const std::string &type) {
        long long now = std::time(nullptr);
        PayDb::instance().exec(
            "INSERT OR IGNORE INTO v3_email_log(order_id,mch_id,email_to,email_type,send_status,created_at)"
            " VALUES(?,?,?,?,0,?)",
            {orderId, mchId, toEmail, type, std::to_string(now)});
    }

    static void updateEmailLog(const std::string &orderId, const std::string &type,
                                bool ok, const std::string &reason) {
        long long now = std::time(nullptr);
        PayDb::instance().exec(
            "UPDATE v3_email_log SET send_status=?,fail_reason=?,send_time=?"
            " WHERE order_id=? AND email_type=? AND send_status=0",
            {ok ? "1" : "2", reason, std::to_string(now), orderId, type});
    }

    static void saveCallbackToken(const std::string &orderId, const std::string &mchId,
                                  const std::string &token) {
        long long now = std::time(nullptr);
        long long exp = now + 86400;
        PayDb::instance().exec(
            "INSERT OR REPLACE INTO v3_manual_callback_log"
            "(order_id,mch_id,callback_token,token_expire,callback_status,created_at)"
            " VALUES(?,?,?,?,0,?)",
            {orderId, mchId, token, std::to_string(exp), std::to_string(now)});
    }

    // ── HTML 模板（内嵌，无外部文件依赖）──────────────────────────
    static std::string tplSuccess(const std::string &orderId, const std::string &mchId,
                                  const std::string &amount, const std::string &payType,
                                  const std::string &deviceId) {
        char timeBuf[32]; time_t t = std::time(nullptr);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", localtime(&t));
        return R"(<!DOCTYPE html><html><head><meta charset="UTF-8">
<style>body{font-family:Arial,sans-serif;background:#f5f5f5;padding:20px}
.c{max-width:600px;margin:0 auto;background:#fff;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.1);overflow:hidden}
.hd{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;padding:30px;text-align:center}
.bd{padding:30px}.amt{font-size:36px;color:#28a745;font-weight:700;text-align:center;margin:20px 0}
.box{background:#f8f9fa;border-left:4px solid #28a745;padding:15px;margin:20px 0}
.row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #e9ecef}
.lbl{color:#6c757d;font-weight:500}.val{color:#212529;font-weight:600}
.ft{background:#f8f9fa;padding:20px;text-align:center;color:#6c757d;font-size:12px}</style>
</head><body><div class="c">
<div class="hd"><h1>&#x1F4B0; 支付成功通知</h1><p>您的订单已成功支付</p></div>
<div class="bd"><div style="font-size:48px;text-align:center">&#x2705;</div>
<div class="amt">&#xFFE5; )" + amount + R"(</div>
<div class="box">
<div class="row"><span class="lbl">订单号：</span><span class="val">)" + orderId + R"(</span></div>
<div class="row"><span class="lbl">商户ID：</span><span class="val">)" + mchId + R"(</span></div>
<div class="row"><span class="lbl">支付方式：</span><span class="val">)" + payType + R"(</span></div>
<div class="row"><span class="lbl">支付时间：</span><span class="val">)" + timeBuf + R"(</span></div>
<div class="row"><span class="lbl">设备ID：</span><span class="val">)" + deviceId + R"(</span></div>
</div></div>
<div class="ft"><p>WePay V3 个人收款支付系统</p><p>此邮件由系统自动发送，请勿回复</p></div>
</div></body></html>)";
    }

    static std::string tplFail(const std::string &orderId, const std::string &mchId,
                               const std::string &amount, const std::string &failReason,
                               const std::string &callbackUrl) {
        char timeBuf[32]; time_t t = std::time(nullptr);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", localtime(&t));
        return R"(<!DOCTYPE html><html><head><meta charset="UTF-8">
<style>body{font-family:Arial,sans-serif;background:#f5f5f5;padding:20px}
.c{max-width:600px;margin:0 auto;background:#fff;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.1);overflow:hidden}
.hd{background:linear-gradient(135deg,#f093fb,#f5576c);color:#fff;padding:30px;text-align:center}
.bd{padding:30px}.box{background:#fff3cd;border-left:4px solid #ffc107;padding:15px;margin:20px 0}
.row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #e9ecef}
.lbl{color:#6c757d;font-weight:500}.val{color:#212529;font-weight:600}
.btn{display:inline-block;background:#007bff;color:#fff;padding:12px 30px;text-decoration:none;border-radius:5px;font-weight:600}
.ft{background:#f8f9fa;padding:20px;text-align:center;color:#6c757d;font-size:12px}</style>
</head><body><div class="c">
<div class="hd"><h1>&#x26A0;&#xFE0F; 支付失败通知</h1><p>订单支付失败或超时</p></div>
<div class="bd"><div style="font-size:48px;text-align:center">&#x274C;</div>
<div class="box">
<div class="row"><span class="lbl">订单号：</span><span class="val">)" + orderId + R"(</span></div>
<div class="row"><span class="lbl">商户ID：</span><span class="val">)" + mchId + R"(</span></div>
<div class="row"><span class="lbl">订单金额：</span><span class="val">&#xFFE5; )" + amount + R"(</span></div>
<div class="row"><span class="lbl">失败原因：</span><span class="val">)" + failReason + R"(</span></div>
<div class="row"><span class="lbl">创建时间：</span><span class="val">)" + timeBuf + R"(</span></div>
</div>
<p style="color:#856404;background:#fff3cd;padding:15px;border-radius:5px">
<strong>提示：</strong>如果用户已实际支付但系统未识别，可点击下方按钮手动触发回调通知。</p>
<div style="text-align:center"><a href=")" + callbackUrl + R"(" class="btn">&#x1F504; 手动触发回调</a></div>
<p style="color:#6c757d;font-size:12px;margin-top:20px">
* 手动回调链接有效期为24小时，过期后将无法使用<br>
* 点击后系统将立即向您的服务器发送回调通知</p>
</div>
<div class="ft"><p>WePay V3 个人收款支付系统</p><p>此邮件由系统自动发送，请勿回复</p></div>
</div></body></html>)";
    }

    static std::string tplDailySummary(const std::string &date,
                                       const std::string &total, const std::string &success,
                                       const std::string &fail,  const std::string &amount,
                                       const Json::Value &failOrders) {
        std::ostringstream html;
        html << R"(<!DOCTYPE html><html><head><meta charset="UTF-8">
<style>body{font-family:Arial,sans-serif;background:#f5f5f5;padding:20px}
.c{max-width:800px;margin:0 auto;background:#fff;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.1);overflow:hidden}
.hd{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;padding:30px;text-align:center}
.stats{display:flex;justify-content:space-around;padding:30px;background:#f8f9fa}
.sb{text-align:center}.sn{font-size:36px;font-weight:700;color:#667eea}.sl{color:#6c757d;margin-top:10px}
.ol{padding:20px}.oi{border:1px solid #e9ecef;border-radius:5px;padding:15px;margin:10px 0}
.oh{display:flex;justify-content:space-between;margin-bottom:10px}
.oid{font-weight:600;color:#212529}.oamt{font-size:18px;color:#dc3545;font-weight:700}
.btn{display:inline-block;background:#007bff;color:#fff;padding:8px 20px;text-decoration:none;border-radius:4px;font-size:14px}
.ft{background:#f8f9fa;padding:20px;text-align:center;color:#6c757d;font-size:12px}</style>
</head><body><div class="c">
<div class="hd"><h1>&#x1F4CA; 每日订单汇总</h1><p>)" << date << R"( 订单统计报告</p></div>
<div class="stats">
<div class="sb"><div class="sn">)" << total << R"(</div><div class="sl">总订单数</div></div>
<div class="sb"><div class="sn">)" << success << R"(</div><div class="sl">成功订单</div></div>
<div class="sb"><div class="sn">)" << fail << R"(</div><div class="sl">失败订单</div></div>
<div class="sb"><div class="sn">&#xFFE5;)" << amount << R"(</div><div class="sl">成功金额</div></div>
</div>)";
        if (failOrders.size() > 0) {
            html << R"(<div class="ol"><h3 style="color:#dc3545;padding:0 5px">&#x26A0;&#xFE0F; 失败订单列表</h3>)";
            for (auto &o : failOrders) {
                html << R"(<div class="oi"><div class="oh">)"
                     << R"(<span class="oid">订单号: )" << o["order_id"].asString() << "</span>"
                     << R"(<span class="oamt">&#xFFE5;)" << o["amount"].asString() << "</span></div>"
                     << R"(<div style="color:#6c757d;font-size:14px;margin-bottom:10px">时间: )"
                     << o["time"].asString() << "</div>"
                     << R"(<a href=")" << o["callback_url"].asString() << R"(" class="btn">手动回调</a>)"
                     << "</div>";
            }
            html << "</div>";
        }
        html << R"(<div class="ft"><p>WePay V3 个人收款支付系统</p><p>此邮件由系统自动发送，请勿回复</p></div>
</div></body></html>)";
        return html.str();
    }
};
