// WePay-Cpp — 标准支付接口（兼容主流聚合支付）
// POST /api/pay/submit        页面跳转支付
// POST /api/pay/create        API 创建订单
// POST /api/pay/query         查询订单
// POST /api/pay/close         关闭订单
// POST /api/pay/refund        申请退款
// POST /api/pay/refundquery   查询退款
// POST /api/pay/notify        异步通知回调
#pragma once
#include <drogon/HttpController.h>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../common/DeviceKeyUtils.h"
#include "../channel/ChannelPlugin.h"
#include <json/json.h>
#include <ctime>
#include <sstream>

using namespace drogon;

class StandardPayCtrl : public drogon::HttpController<StandardPayCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(StandardPayCtrl::submit,       "/api/pay/submit",       drogon::Post);
        ADD_METHOD_TO(StandardPayCtrl::create,       "/api/pay/create",       drogon::Post);
        ADD_METHOD_TO(StandardPayCtrl::query,        "/api/pay/query",        drogon::Post);
        ADD_METHOD_TO(StandardPayCtrl::close,        "/api/pay/close",        drogon::Post);
        ADD_METHOD_TO(StandardPayCtrl::refund,       "/api/pay/refund",       drogon::Post);
        ADD_METHOD_TO(StandardPayCtrl::refundQuery,  "/api/pay/refundquery",  drogon::Post);
    METHOD_LIST_END

    // 页面跳转支付（用户在商户网站点击支付按钮后跳转）
    void submit(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) {
            RESP_ERR(cb, "参数格式错误");
            return;
        }

        // 提取参数
        std::string mchNo      = (*j).get("mch_no", "").asString();
        std::string outTradeNo = (*j).get("out_trade_no", "").asString();
        std::string amountStr  = (*j).get("amount", "").asString();
        std::string subject    = (*j).get("subject", "").asString();
        std::string payType    = (*j).get("pay_type", "").asString();
        std::string notifyUrl  = (*j).get("notify_url", "").asString();
        std::string returnUrl  = (*j).get("return_url", "").asString();
        std::string timestamp  = (*j).get("timestamp", "").asString();
        std::string sign       = (*j).get("sign", "").asString();

        // 参数验证
        if (mchNo.empty() || outTradeNo.empty() || amountStr.empty() ||
            subject.empty() || payType.empty() || timestamp.empty() || sign.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        // 查询商户
        auto mch = db.queryOne(
            "SELECT id,mch_no,username,mch_name,state FROM merchant WHERE mch_no=?",
            {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }
        if (mch["state"] == "0") {
            RESP_ERR(cb, "商户已被禁用");
            return;
        }

        int mchId = 0;
        try { mchId = std::stoi(mch["id"]); } catch (...) {}

        // 验证签名（使用商户的 RSA 公钥）
        auto device = db.queryOne(
            "SELECT public_key,key_type FROM device_keys "
            "WHERE user_id=? AND user_type=2 AND state=1 AND key_type IN ('rsa2048','rsa4096') "
            "ORDER BY id DESC LIMIT 1",
            {mch["id"]});

        if (!device.empty()) {
            // 有 RSA 密钥，验证签名
            std::string signStr = buildSignString({
                {"amount", amountStr},
                {"mch_no", mchNo},
                {"notify_url", notifyUrl},
                {"out_trade_no", outTradeNo},
                {"pay_type", payType},
                {"return_url", returnUrl},
                {"subject", subject},
                {"timestamp", timestamp}
            });

            DeviceKeyUtils::KeyType kt = (device["key_type"] == "rsa2048")
                ? DeviceKeyUtils::KeyType::RSA_2048
                : DeviceKeyUtils::KeyType::RSA_4096;

            bool verified = DeviceKeyUtils::verifySignature(signStr, sign, device["public_key"], kt);
            if (!verified) {
                RESP_ERR(cb, "签名验证失败");
                return;
            }
        }

        // 金额验证
        double amount = 0;
        try { amount = std::stod(amountStr); } catch (...) {}
        if (amount <= 0) {
            RESP_ERR(cb, "金额错误");
            return;
        }

        // 检查订单是否已存在
        auto exist = db.queryOne(
            "SELECT order_id,pay_url FROM pay_order WHERE mch_id=? AND mch_order_no=?",
            {std::to_string(mchId), outTradeNo});

        if (!exist.empty()) {
            // 订单已存在，直接返回支付链接
            Json::Value data;
            data["order_id"] = exist["order_id"];
            data["pay_url"] = exist["pay_url"];
            RESP_JSON(cb, AjaxResult::success("订单已存在", data));
            return;
        }

        // 选择通道
        auto channel = ChannelService::selectChannel(mchId, payType, amount);
        if (channel.channelId == 0) {
            RESP_ERR(cb, "暂无可用支付通道");
            return;
        }

        // 计算费率
        double mchRate    = ChannelService::getMchRate(mchId, channel.channelId);
        double mchFee     = ChannelService::calcFee(amount, mchRate);
        double channelFee = ChannelService::calcFee(amount, channel.rate);

        // 生成订单号
        std::string orderId = ChannelService::generateOrderId(
            db.getSetting("order_prefix", "W"));

        long long now = std::time(nullptr);
        int closeMin = 5;
        try { closeMin = std::stoi(db.getSetting("close_minutes", "5")); } catch (...) {}
        long long expireTime = now + closeMin * 60;

        std::string clientIp = req->getPeerAddr().toIp();

        // 创建订单
        bool ok = db.exec(
            "INSERT INTO pay_order("
            "order_id,mch_id,mch_order_no,channel_id,pay_type,amount,mch_fee,channel_fee,"
            "subject,notify_url,return_url,client_ip,state,expire_time,created_at,updated_at"
            ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?)",
            {orderId, std::to_string(mchId), outTradeNo, std::to_string(channel.channelId),
             payType, ChannelService::fmtAmount(amount), ChannelService::fmtAmount(mchFee),
             ChannelService::fmtAmount(channelFee), subject, notifyUrl, returnUrl, clientIp,
             std::to_string(expireTime), std::to_string(now), std::to_string(now)});

        if (!ok) {
            RESP_ERR(cb, "订单创建失败");
            return;
        }

        // 调用支付通道
        std::string payUrl, qrCode, channelOrderNo;

        auto plugin = ChannelPluginRegistry::instance().create(channel.plugin);
        if (plugin) {
            ChannelOrderRequest creq;
            creq.orderId = orderId;
            creq.mchOrderNo = outTradeNo;
            creq.amount = amount;
            creq.subject = subject;
            creq.notifyUrl = notifyUrl;
            creq.returnUrl = returnUrl;
            creq.clientIp = clientIp;
            creq.payType = payType;

            auto channelRow = db.queryOne("SELECT params_json FROM pay_channel WHERE id=?",
                                         {std::to_string(channel.channelId)});
            if (!channelRow.empty() && channelRow.count("params_json")) {
                try {
                    Json::CharReaderBuilder builder;
                    std::string errs;
                    std::istringstream iss(channelRow["params_json"]);
                    Json::parseFromStream(builder, iss, &creq.channelParams, &errs);
                } catch (...) {}
            }

            auto cres = plugin->createOrder(creq);
            if (cres.success) {
                payUrl = cres.payUrl;
                qrCode = cres.qrCode;
                channelOrderNo = cres.channelOrderNo;

                db.exec("UPDATE pay_order SET pay_url=?,qrcode=?,channel_order_no=?,updated_at=? WHERE order_id=?",
                        {payUrl, qrCode, channelOrderNo, std::to_string(std::time(nullptr)), orderId});
            } else {
                RESP_ERR(cb, cres.errMsg);
                return;
            }
        }

        // 返回支付链接（前端跳转）
        Json::Value data;
        data["order_id"] = orderId;
        data["pay_url"] = payUrl;
        data["qr_code"] = qrCode;

        RESP_JSON(cb, AjaxResult::success("success", data));
    }

    // API 创建订单（别名，指向 submit）
    void create(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        submit(req, std::move(cb));
    }

    // 查询订单
    void query(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) {
            RESP_ERR(cb, "参数格式错误");
            return;
        }

        std::string mchNo      = (*j).get("mch_no", "").asString();
        std::string outTradeNo = (*j).get("out_trade_no", "").asString();

        if (mchNo.empty() || outTradeNo.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=?", {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }

        auto order = db.queryOne(
            "SELECT order_id,mch_order_no,pay_type,amount,real_amount,state,pay_time,created_at "
            "FROM pay_order WHERE mch_id=? AND mch_order_no=?",
            {mch["id"], outTradeNo});

        if (order.empty()) {
            RESP_ERR(cb, "订单不存在");
            return;
        }

        Json::Value data;
        data["order_id"] = order["order_id"];
        data["out_trade_no"] = order["mch_order_no"];
        data["pay_type"] = order["pay_type"];
        data["amount"] = order["amount"];
        data["real_amount"] = order["real_amount"];
        data["state"] = std::stoi(order["state"]);
        data["pay_time"] = (Json::Int64)std::stoll(order["pay_time"]);
        data["created_at"] = (Json::Int64)std::stoll(order["created_at"]);

        RESP_JSON(cb, AjaxResult::success("success", data));
    }

    // 关闭订单
    void close(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) {
            RESP_ERR(cb, "参数格式错误");
            return;
        }

        std::string mchNo      = (*j).get("mch_no", "").asString();
        std::string outTradeNo = (*j).get("out_trade_no", "").asString();

        if (mchNo.empty() || outTradeNo.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=?", {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }

        auto order = db.queryOne(
            "SELECT order_id,state FROM pay_order WHERE mch_id=? AND mch_order_no=?",
            {mch["id"], outTradeNo});

        if (order.empty()) {
            RESP_ERR(cb, "订单不存在");
            return;
        }

        int state = std::stoi(order["state"]);
        if (state == 1) {
            RESP_ERR(cb, "订单已支付，无法关闭");
            return;
        }
        if (state == 2) {
            Json::Value data;
            data["order_id"] = order["order_id"];
            data["state"] = 2;
            RESP_JSON(cb, AjaxResult::success("订单已关闭", data));
            return;
        }

        bool ok = db.exec(
            "UPDATE pay_order SET state=2,updated_at=? WHERE order_id=?",
            {std::to_string(std::time(nullptr)), order["order_id"]});

        if (!ok) {
            RESP_ERR(cb, "关闭订单失败");
            return;
        }

        Json::Value data;
        data["order_id"] = order["order_id"];
        data["state"] = 2;

        RESP_JSON(cb, AjaxResult::success("订单已关闭", data));
    }

    // 申请退款
    void refund(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) {
            RESP_ERR(cb, "参数格式错误");
            return;
        }

        std::string mchNo        = (*j).get("mch_no", "").asString();
        std::string outTradeNo   = (*j).get("out_trade_no", "").asString();
        std::string outRefundNo  = (*j).get("out_refund_no", "").asString();
        std::string refundAmount = (*j).get("refund_amount", "").asString();
        std::string reason       = (*j).get("reason", "退款").asString();

        if (mchNo.empty() || outTradeNo.empty() || outRefundNo.empty() || refundAmount.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=?", {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }

        auto order = db.queryOne(
            "SELECT order_id,amount,state FROM pay_order WHERE mch_id=? AND mch_order_no=?",
            {mch["id"], outTradeNo});

        if (order.empty()) {
            RESP_ERR(cb, "订单不存在");
            return;
        }

        if (order["state"] != "1") {
            RESP_ERR(cb, "订单未支付或已关闭");
            return;
        }

        double orderAmount = 0, refundAmt = 0;
        try {
            orderAmount = std::stod(order["amount"]);
            refundAmt = std::stod(refundAmount);
        } catch (...) {
            RESP_ERR(cb, "金额格式错误");
            return;
        }

        if (refundAmt <= 0 || refundAmt > orderAmount) {
            RESP_ERR(cb, "退款金额错误");
            return;
        }

        auto existRefund = db.queryOne(
            "SELECT refund_id FROM refund_order WHERE mch_id=? AND mch_refund_no=?",
            {mch["id"], outRefundNo});
        if (!existRefund.empty()) {
            Json::Value data;
            data["refund_id"] = existRefund["refund_id"];
            RESP_JSON(cb, AjaxResult::success("退款单号已存在", data));
            return;
        }

        std::string refundId = "R" + std::to_string(std::time(nullptr)) +
                               std::to_string(rand() % 10000);

        long long now = std::time(nullptr);

        bool ok = db.exec(
            "INSERT INTO refund_order("
            "refund_id,order_id,mch_id,mch_refund_no,refund_amount,reason,state,created_at,updated_at"
            ") VALUES(?,?,?,?,?,?,0,?,?)",
            {refundId, order["order_id"], mch["id"], outRefundNo,
             ChannelService::fmtAmount(refundAmt), reason,
             std::to_string(now), std::to_string(now)});

        if (!ok) {
            RESP_ERR(cb, "退款申请失败");
            return;
        }

        Json::Value data;
        data["refund_id"] = refundId;
        data["order_id"] = order["order_id"];
        data["out_refund_no"] = outRefundNo;
        data["refund_amount"] = ChannelService::fmtAmount(refundAmt);
        data["state"] = 0;

        RESP_JSON(cb, AjaxResult::success("退款申请已提交", data));
    }

    // 查询退款
    void refundQuery(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) {
            RESP_ERR(cb, "参数格式错误");
            return;
        }

        std::string mchNo       = (*j).get("mch_no", "").asString();
        std::string outRefundNo = (*j).get("out_refund_no", "").asString();

        if (mchNo.empty() || outRefundNo.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=?", {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }

        auto refund = db.queryOne(
            "SELECT refund_id,order_id,mch_refund_no,refund_amount,reason,state,created_at,updated_at "
            "FROM refund_order WHERE mch_id=? AND mch_refund_no=?",
            {mch["id"], outRefundNo});

        if (refund.empty()) {
            RESP_ERR(cb, "退款单不存在");
            return;
        }

        Json::Value data;
        data["refund_id"] = refund["refund_id"];
        data["order_id"] = refund["order_id"];
        data["out_refund_no"] = refund["mch_refund_no"];
        data["refund_amount"] = refund["refund_amount"];
        data["reason"] = refund["reason"];
        data["state"] = std::stoi(refund["state"]);
        data["created_at"] = (Json::Int64)std::stoll(refund["created_at"]);
        data["updated_at"] = (Json::Int64)std::stoll(refund["updated_at"]);

        RESP_JSON(cb, AjaxResult::success("success", data));
    }

private:
    static std::string buildSignString(const std::map<std::string, std::string> &params) {
        std::vector<std::string> keys;
        for (auto &[k, v] : params) {
            if (!v.empty()) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());

        std::string result;
        for (auto &k : keys) {
            if (!result.empty()) result += "&";
            result += k + "=" + params.at(k);
        }
        return result;
    }
};
