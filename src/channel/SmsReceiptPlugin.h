// WePay-Cpp — 手动确认免签收款插件
// 用户扫码付款后, 在收银台提交支付宝/微信交易订单号完成确认
// 可选: 发邮件给管理员审核(带一键确认/拒绝链接)
//
// 流程:
//   1. 下单 → 分配唯一金额(+0~99分偏移) → 展示收款码 + 输入框
//   2. 用户扫码付指定金额
//   3. 用户在收银台输入支付宝/微信的交易订单号 → POST /gateway/confirm/{orderId}
//   4a. auto_confirm=true  → 立即确认支付
//   4b. auto_confirm=false → 发邮件给管理员, 管理员点链接确认/拒绝
//
// 不需要挂机/不需要 Python/不需要 API 资质
//
// 通道参数(params_json):
//   qrcode_url           : 收款码内容(必填)
//   qrcode_image         : 收款码图片URL(可选)
//   amount_offset_max    : 最大偏移分数(默认99)
//   receipt_valid_seconds : 订单有效期秒(默认300)
//   auto_confirm         : 自动确认(默认1; 0=需管理员邮件审核)
//   admin_email          : 管理员邮箱(auto_confirm=0 时必填)
#pragma once
#include "ChannelPlugin.h"
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../common/PaymentService.h"
#include "../common/Md5Utils.h"
#include "../common/MsgNoticeService.h"
#include "../common/NotifyChannels.h"
#include <mutex>
#include <cmath>
#include <sstream>
#include <random>

class SmsReceiptPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "sms_receipt"; }

    Json::Value paramSchema() const override {
        Json::Value s(Json::arrayValue);
        auto add = [&](const char* k, const char* l, const char* t,
                       const char* d, const char* h, bool req = false) {
            Json::Value f;
            f["key"]=k; f["label"]=l; f["type"]=t;
            f["default"]=d; f["help"]=h; f["required"]=req;
            s.append(f);
        };
        add("qrcode_url", "收款码内容", "input", "",
            "支付宝/微信个人收款码 URL", true);
        add("qrcode_image", "收款码图片", "image", "",
            "展示给用户的收款码图片地址", false);
        add("amount_offset_max", "最大偏移(分)", "number", "99",
            "金额偏移范围 0~N 分, 99=最多+0.99 元", false);
        add("receipt_valid_seconds", "有效期(秒)", "number", "300",
            "订单有效期", false);
        add("auto_confirm", "自动确认", "select", "1",
            "1=用户提交订单号后立即确认; 0=发邮件由管理员审核", false);
        add("admin_email", "管理员邮箱", "input", "",
            "审核模式下发送确认邮件的目标邮箱", false);
        return s;
    }

    // ── 下单: 分配唯一金额 ──────────────────────────────────

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        auto &db = PayDb::instance();

        std::string qrcodeUrl = p.get("qrcode_url", "").asString();
        if (qrcodeUrl.empty()) {
            result.errMsg = "未配置收款码";
            return result;
        }

        int maxOffset = p.get("amount_offset_max", 99).asInt();
        if (maxOffset <= 0) maxOffset = 99;
        int validSec = p.get("receipt_valid_seconds", 300).asInt();
        if (validSec <= 0) validSec = 300;
        bool autoConfirm = p.get("auto_confirm", "1").asString() != "0";

        // 分配唯一金额
        double realAmount = -1;
        {
            static std::mutex mtx;
            std::lock_guard<std::mutex> lock(mtx);

            long long cutoff = std::time(nullptr) - validSec;
            long long baseCent = (long long)std::round(req.amount * 100);

            for (int off = 0; off <= maxOffset; ++off) {
                double candidate = (baseCent + off) / 100.0;
                std::string cStr = ChannelService::fmtAmount(candidate);

                auto dup = db.queryOne(
                    "SELECT order_id FROM pay_order "
                    "WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
                    "AND ABS(CAST(real_amount AS REAL)-CAST(? AS REAL))<0.001 "
                    "AND created_at>?",
                    {req.payType, cStr, std::to_string(cutoff)});

                if (dup.empty()) {
                    realAmount = candidate;
                    break;
                }
            }
        }

        if (realAmount < 0) {
            result.errMsg = "同金额订单过多, 请稍后再试";
            return result;
        }

        // 生成确认令牌(管理员审核链接用)
        std::string token = genToken();

        // ext_json
        Json::Value ext;
        ext["plugin"] = "sms_receipt";
        ext["original_amount"] = ChannelService::fmtAmount(req.amount);
        ext["receipt_amount"] = ChannelService::fmtAmount(realAmount);
        ext["need_confirm"] = true;
        ext["auto_confirm"] = autoConfirm;
        ext["confirm_token"] = token;
        ext["confirmed"] = false;
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string extJson = Json::writeString(wb, ext);

        db.exec("UPDATE pay_order SET real_amount=?,ext_json=? WHERE order_id=?",
                {ChannelService::fmtAmount(realAmount), extJson, req.orderId});

        // 金额锁
        int typeInt = (req.payType == "alipay") ? 2 :
                      (req.payType == "qqpay") ? 3 : 1;
        db.exec("INSERT INTO tmp_price(oid,type,price) VALUES(?,?,?)",
                {req.orderId, std::to_string(typeInt),
                 ChannelService::fmtAmount(realAmount)});

        result.success = true;
        result.payUrl = qrcodeUrl;
        result.qrCode = qrcodeUrl;
        result.extra["real_amount"] = ChannelService::fmtAmount(realAmount);
        result.extra["need_confirm"] = true;
        result.extra["qrcode_image"] = p.get("qrcode_image", "").asString();
        return result;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &,
        const std::string &,
        const Json::Value &) override
    {
        ChannelNotifyResult r;
        r.verified = true;
        r.paid = true;
        r.responseText = "success";
        return r;
    }

    // ══════════════════════════════════════════════════════════
    //  静态方法: 用户提交确认 / 管理员审核
    // ══════════════════════════════════════════════════════════

    // 用户在收银台提交交易订单号
    // 返回: 0=成功, 1=需管理员审核(已发邮件), <0=失败
    static int userConfirm(const std::string &orderId,
                            const std::string &tradeNo,
                            std::string &outMsg)
    {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (row.empty()) { outMsg = "订单不存在"; return -1; }

        int state = 0;
        try { state = std::stoi(row["state"]); } catch (...) {}
        if (state != 0) { outMsg = "订单已处理"; return -2; }

        if (tradeNo.empty()) { outMsg = "请输入交易订单号"; return -3; }

        // 检查交易号是否已被使用
        auto used = db.queryOne(
            "SELECT order_id FROM pay_order WHERE channel_order_no=? AND state=1",
            {tradeNo});
        if (!used.empty()) { outMsg = "该交易订单号已被使用"; return -4; }

        // 写入交易号
        long long now = std::time(nullptr);
        db.exec("UPDATE pay_order SET channel_order_no=?,updated_at=? WHERE order_id=?",
                {tradeNo, std::to_string(now), orderId});

        // 解析 ext_json
        Json::Value ext;
        std::string extStr = row.count("ext_json") ? row["ext_json"] : "";
        if (!extStr.empty()) {
            Json::CharReaderBuilder rb;
            std::istringstream ss(extStr);
            std::string errs;
            Json::parseFromStream(rb, ss, &ext, &errs);
        }

        bool autoConfirm = ext.get("auto_confirm", true).asBool();

        if (autoConfirm) {
            // 直接确认
            return doConfirm(orderId, row, ext, outMsg);
        } else {
            // 标记待审核 + 发邮件
            ext["user_trade_no"] = tradeNo;
            ext["pending_review"] = true;
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            db.exec("UPDATE pay_order SET ext_json=? WHERE order_id=?",
                    {Json::writeString(wb, ext), orderId});

            sendReviewEmail(orderId, row, ext, tradeNo);
            outMsg = "已提交, 等待管理员审核确认";
            return 1;
        }
    }

    // 管理员通过令牌确认/拒绝
    // action: "confirm" 或 "reject"
    static int adminAction(const std::string &orderId,
                            const std::string &token,
                            const std::string &action,
                            std::string &outMsg)
    {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (row.empty()) { outMsg = "订单不存在"; return -1; }

        int state = 0;
        try { state = std::stoi(row["state"]); } catch (...) {}
        if (state != 0) { outMsg = "订单已处理"; return -2; }

        // 验证 token
        Json::Value ext;
        std::string extStr = row.count("ext_json") ? row["ext_json"] : "";
        if (!extStr.empty()) {
            Json::CharReaderBuilder rb;
            std::istringstream ss(extStr);
            std::string errs;
            Json::parseFromStream(rb, ss, &ext, &errs);
        }

        std::string cfgToken = ext.get("confirm_token", "").asString();
        if (cfgToken.empty() || cfgToken != token) {
            outMsg = "令牌无效或已过期";
            return -3;
        }

        if (action == "confirm") {
            return doConfirm(orderId, row, ext, outMsg);
        } else {
            // 拒绝: 关闭订单
            long long now = std::time(nullptr);
            db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                    {std::to_string(now), orderId});
            db.exec("DELETE FROM tmp_price WHERE oid=?", {orderId});
            outMsg = "订单已拒绝";
            return 0;
        }
    }

private:
    // 执行确认支付
    static int doConfirm(const std::string &orderId,
                          const std::unordered_map<std::string, std::string> &row,
                          Json::Value &ext,
                          std::string &outMsg)
    {
        auto &db = PayDb::instance();
        std::string payType = row.count("pay_type") ? row.at("pay_type") : "alipay";
        std::string realAmt = row.count("real_amount") ? row.at("real_amount") : "0";

        // 用 processPayment 走完整支付流程
        std::string matched = PaymentService::processPayment(payType, realAmt);

        if (matched.empty()) {
            outMsg = "匹配失败, 请联系管理员";
            return -5;
        }

        // 恢复原始金额
        std::string origAmt = ext.get("original_amount", "").asString();
        if (!origAmt.empty()) {
            db.exec("UPDATE pay_order SET real_amount=? WHERE order_id=?",
                    {origAmt, matched});
        }

        // 更新 ext_json
        ext["confirmed"] = true;
        ext["confirmed_at"] = (Json::Int64)std::time(nullptr);
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        db.exec("UPDATE pay_order SET ext_json=? WHERE order_id=?",
                {Json::writeString(wb, ext), matched});

        outMsg = "支付确认成功";
        return 0;
    }

    // 发送审核邮件
    static void sendReviewEmail(const std::string &orderId,
                                 const std::unordered_map<std::string, std::string> &row,
                                 const Json::Value &ext,
                                 const std::string &tradeNo)
    {
        auto &db = PayDb::instance();
        std::string adminEmail = "";

        // 从通道参数获取
        if (row.count("channel_id")) {
            auto ch = db.queryOne(
                "SELECT params_json FROM pay_channel WHERE id=?",
                {row.at("channel_id")});
            if (!ch.empty() && !ch["params_json"].empty()) {
                Json::Value cp;
                Json::CharReaderBuilder rb;
                std::istringstream ss(ch["params_json"]);
                std::string e;
                Json::parseFromStream(rb, ss, &cp, &e);
                adminEmail = cp.get("admin_email", "").asString();
            }
        }

        // 回退到系统配置
        if (adminEmail.empty())
            adminEmail = db.getSetting("notify_email_admin", "");
        if (adminEmail.empty()) return;

        std::string token = ext.get("confirm_token", "").asString();
        std::string siteUrl = db.getSetting("siteUrl", "http://localhost:9527");
        std::string amount = row.count("amount") ? row.at("amount") : "?";
        std::string realAmt = row.count("real_amount") ? row.at("real_amount") : "?";
        std::string subject = row.count("subject") ? row.at("subject") : "";

        std::string confirmUrl = siteUrl + "/gateway/confirm/" + orderId
            + "?token=" + token + "&action=confirm";
        std::string rejectUrl  = siteUrl + "/gateway/confirm/" + orderId
            + "?token=" + token + "&action=reject";

        std::string title = "收款待审核 - " + orderId;

        // 构造 HTML 邮件正文(含可点击的确认/拒绝按钮)
        std::string htmlBody =
            "<div style='font-family:sans-serif;max-width:600px;margin:0 auto'>"
            "<h2 style='color:#1a1a1a'>收款待确认</h2>"
            "<table style='border-collapse:collapse;margin:16px 0;width:100%'>"
            "<tr><td style='padding:8px 12px;border:1px solid #e5e5e5;font-weight:600;background:#fafafa;width:120px'>订单号</td>"
            "<td style='padding:8px 12px;border:1px solid #e5e5e5;font-family:monospace'>" + orderId + "</td></tr>"
            "<tr><td style='padding:8px 12px;border:1px solid #e5e5e5;font-weight:600;background:#fafafa'>原始金额</td>"
            "<td style='padding:8px 12px;border:1px solid #e5e5e5'>" + amount + " 元</td></tr>"
            "<tr><td style='padding:8px 12px;border:1px solid #e5e5e5;font-weight:600;background:#fafafa'>实付金额</td>"
            "<td style='padding:8px 12px;border:1px solid #e5e5e5;color:#f5222d;font-weight:700'>" + realAmt + " 元</td></tr>"
            "<tr><td style='padding:8px 12px;border:1px solid #e5e5e5;font-weight:600;background:#fafafa'>商品</td>"
            "<td style='padding:8px 12px;border:1px solid #e5e5e5'>" + subject + "</td></tr>"
            "<tr><td style='padding:8px 12px;border:1px solid #e5e5e5;font-weight:600;background:#fafafa'>用户交易号</td>"
            "<td style='padding:8px 12px;border:1px solid #e5e5e5;font-family:monospace'>" + tradeNo + "</td></tr>"
            "</table>"
            "<div style='margin:24px 0'>"
            "<a href='" + confirmUrl + "' style='display:inline-block;padding:12px 32px;"
            "background:#10b981;color:#fff;text-decoration:none;border-radius:8px;"
            "margin-right:16px;font-weight:700;font-size:16px'>✅ 确认收款</a>"
            "<a href='" + rejectUrl + "' style='display:inline-block;padding:12px 32px;"
            "background:#ef4444;color:#fff;text-decoration:none;border-radius:8px;"
            "font-weight:700;font-size:16px'>❌ 拒绝</a>"
            "</div>"
            "<p style='color:#999;font-size:12px;margin-top:20px'>"
            "请在支付宝/微信中核实该交易是否真实到账后再操作。<br>"
            "此链接仅供管理员使用，请勿转发。</p>"
            "</div>";

        // 1) 直接发 HTML 邮件给管理员 (不依赖 notify_email_enabled 开关)
        EmailService::instance().send(adminEmail, title, htmlBody);

        // 2) 同时走 MsgNoticeService 发 Webhook/钉钉/飞书等
        std::string plainContent =
            "订单: " + orderId + "\n"
            "金额: " + amount + " 元 (实付 " + realAmt + " 元)\n"
            "商品: " + subject + "\n"
            "用户交易号: " + tradeNo + "\n\n"
            "确认: " + confirmUrl + "\n"
            "拒绝: " + rejectUrl;
        MsgNoticeService::send(MsgNoticeService::ORDER_PAID, 0, title, plainContent);
    }

    static std::string genToken() {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::uniform_int_distribution<int> dist(0, sizeof(chars) - 2);
        std::string token;
        token.reserve(32);
        for (int i = 0; i < 32; ++i) token += chars[dist(rng)];
        return token;
    }
};

REGISTER_CHANNEL_PLUGIN(SmsReceiptPlugin);
