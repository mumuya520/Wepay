// WePay-Cpp — 网页流水监听收款插件
// 配合 Python receipt_watcher 实现个人免签收款(无需挂机手机)
//
// 流程:
//   1. 下单 → 分配唯一金额(+0~99分偏移) → 展示收款码
//   2. 用户扫码付指定金额, 无需任何额外操作
//   3. Python receipt_watcher 自动抓取支付宝流水 → RabbitMQ
//   4. WePay-Cpp 消费队列 → 按金额匹配订单 → 确认支付
//
// 通道参数(params_json):
//   qrcode_url           : 收款码内容(必填)
//   qrcode_image         : 收款码图片URL(可选)
//   amount_offset_max    : 最大偏移分数(默认99)
//   receipt_valid_seconds : 订单有效期秒(默认300)
#pragma once
#include "ChannelPlugin.h"
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../common/PaymentService.h"
#include <mutex>
#include <cmath>
#include <sstream>

class ReceiptPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "receipt_monitor"; }

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
            "支付宝/微信个人收款码URL", true);
        add("qrcode_image", "收款码图片", "input", "",
            "展示给用户的收款码图片地址", false);
        add("amount_offset_max", "最大偏移(分)", "number", "99",
            "金额偏移范围, 99=最多+0.99元", false);
        add("receipt_valid_seconds", "有效期(秒)", "number", "300",
            "订单有效期", false);
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

        // 分配唯一金额(互斥锁防并发冲突)
        double realAmount = -1;
        {
            static std::mutex mtx;
            std::lock_guard<std::mutex> lock(mtx);

            long long cutoff = std::time(nullptr) - validSec;
            long long baseCent = (long long)std::round(req.amount * 100);

            for (int off = 0; off <= maxOffset; ++off) {
                double candidate = (baseCent + off) / 100.0;
                std::string cStr = ChannelService::fmtAmount(candidate);

                // 查有效期内同金额同支付方式的未付订单
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

        // 保存原始金额 → ext_json, 更新 real_amount
        std::string extJson =
            R"({"plugin":"receipt_monitor","original_amount":")"
            + ChannelService::fmtAmount(req.amount)
            + R"(","receipt_amount":")"
            + ChannelService::fmtAmount(realAmount) + R"("})";

        db.exec("UPDATE pay_order SET real_amount=?,ext_json=? WHERE order_id=?",
                {ChannelService::fmtAmount(realAmount), extJson, req.orderId});

        // 金额锁(CronService 过期清理)
        int typeInt = (req.payType == "alipay") ? 2 :
                      (req.payType == "qqpay") ? 3 : 1;
        db.exec("INSERT INTO tmp_price(oid,type,price) VALUES(?,?,?)",
                {req.orderId, std::to_string(typeInt),
                 ChannelService::fmtAmount(realAmount)});

        result.success = true;
        result.payUrl = qrcodeUrl;
        result.qrCode = qrcodeUrl;
        result.extra["real_amount"] = ChannelService::fmtAmount(realAmount);
        result.extra["qrcode_image"] = p.get("qrcode_image", "").asString();
        return result;
    }

    // ── 通知验签: 来自 ReceiptWatcherService 内部调用 ────────

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override
    {
        ChannelNotifyResult r;
        r.verified = true;
        r.paid = true;
        r.responseText = "success";
        // orderId 和 paidAmount 由 ReceiptWatcherService 填充
        auto it = params.find("order_id");
        if (it != params.end()) r.orderId = it->second;
        it = params.find("channel_order_no");
        if (it != params.end()) r.channelOrderNo = it->second;
        it = params.find("paid_amount");
        if (it != params.end()) {
            try { r.paidAmount = std::stod(it->second); } catch (...) {}
        }
        return r;
    }

    // ── 查询: 无法主动查上游, 返回当前状态 ──────────────────

    ChannelQueryResult queryOrder(const std::string &orderId,
                                  const Json::Value &channelParams) override {
        ChannelQueryResult r;
        auto row = PayDb::instance().queryOne(
            "SELECT state FROM pay_order WHERE order_id=?", {orderId});
        if (!row.empty()) {
            int st = 0;
            try { st = std::stoi(row["state"]); } catch (...) {}
            r.success = true;
            r.tradeState = st;
        }
        return r;
    }

    // ══════════════════════════════════════════════════════════
    //  静态: 流水匹配(被 ReceiptWatcherService 调用)
    // ══════════════════════════════════════════════════════════

    // 按金额匹配待付订单, 返回 order_id (未匹配返回空)
    // 完整流程: 匹配 → 标记paid → 恢复原始金额 → 入账 → 商户通知
    static std::string matchByAmount(const std::string &payType,
                                      const std::string &price,
                                      const std::string &tradeNo = "")
    {
        auto &db = PayDb::instance();

        // 先写入 channel_order_no(如有), 再让 processPayment 做全流程
        if (!tradeNo.empty()) {
            auto pre = db.queryOne(
                "SELECT order_id FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
                "AND ABS(CAST(real_amount AS REAL)-CAST(? AS REAL))<0.001 "
                "ORDER BY created_at ASC LIMIT 1",
                {payType, price});
            if (!pre.empty()) {
                db.exec("UPDATE pay_order SET channel_order_no=? WHERE order_id=?",
                        {tradeNo, pre["order_id"]});
            }
        }

        // processPayment: 按(payType, real_amount)匹配 → state=1 → 删tmp_price
        //                  → 入账 → 商户notify → MQ "order.paid"
        std::string matched = PaymentService::processPayment(payType, price);
        if (matched.empty()) return "";

        // 恢复原始金额(偏移前)— 保证业务统计准确
        auto row = db.queryOne(
            "SELECT ext_json FROM pay_order WHERE order_id=?", {matched});
        if (!row.empty() && !row["ext_json"].empty()) {
            Json::Value ext;
            Json::CharReaderBuilder rb;
            std::istringstream ss(row["ext_json"]);
            std::string errs;
            if (Json::parseFromStream(rb, ss, &ext, &errs)) {
                std::string origAmt = ext.get("original_amount", "").asString();
                if (!origAmt.empty()) {
                    db.exec("UPDATE pay_order SET real_amount=? WHERE order_id=?",
                            {origAmt, matched});
                }
            }
        }

        return matched;
    }
};

REGISTER_CHANNEL_PLUGIN(ReceiptPlugin);
