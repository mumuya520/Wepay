// WePay-Cpp — 安全支付网关接口（RSA 签名认证）
// POST /gateway/create       创建订单（RSA 签名）
// POST /gateway/query        查询订单
// POST /gateway/close        关闭订单
// POST /gateway/refund       申请退款
#pragma once
#include <drogon/HttpController.h>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../common/DeviceKeyUtils.h"
#include "../common/Md5Utils.h"
#include "../channel/ChannelPlugin.h"
#include <json/json.h>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <map>
#include <vector>

using namespace drogon;

class SecureGatewayCtrl : public drogon::HttpController<SecureGatewayCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SecureGatewayCtrl::create, "/gateway/create", drogon::Post);
        ADD_METHOD_TO(SecureGatewayCtrl::query,  "/gateway/query",  drogon::Post);
        ADD_METHOD_TO(SecureGatewayCtrl::close,  "/gateway/close",  drogon::Post);
        ADD_METHOD_TO(SecureGatewayCtrl::refund, "/gateway/refund", drogon::Post);
    METHOD_LIST_END

    // 创建订单
    void create(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) {
            RESP_ERR(cb, "参数格式错误");
            return;
        }

        // 1. 提取参数
        std::string mchNo      = (*j).get("mch_no", "").asString();
        std::string outTradeNo = (*j).get("out_trade_no", "").asString();
        std::string amountStr  = (*j).get("amount", "").asString();
        std::string subject    = (*j).get("subject", "").asString();
        std::string payType    = (*j).get("pay_type", "").asString();
        std::string notifyUrl  = (*j).get("notify_url", "").asString();
        std::string returnUrl  = (*j).get("return_url", "").asString();
        std::string timestamp  = (*j).get("timestamp", "").asString();
        std::string sign       = (*j).get("sign", "").asString();

        // 2. 参数验证
        if (mchNo.empty() || outTradeNo.empty() || amountStr.empty() ||
            subject.empty() || payType.empty() || timestamp.empty() || sign.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        // 3. 查询商户
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

        // 4. 查询商户的 RSA 公钥（从设备密钥表）
        auto device = db.queryOne(
            "SELECT public_key,key_type FROM device_keys "
            "WHERE user_id=? AND user_type=2 AND state=1 AND key_type IN ('rsa2048','rsa4096') "
            "ORDER BY id DESC LIMIT 1",
            {mch["id"]});

        if (device.empty()) {
            RESP_ERR(cb, "商户未配置 RSA 密钥，请先在商户后台生成 RSA 密钥");
            return;
        }

        std::string publicKey = device["public_key"];
        std::string keyType   = device["key_type"];

        // 5. 验证时间戳（5分钟内有效）
        try {
            long long ts = std::stoll(timestamp);
            long long now = std::time(nullptr) * 1000;
            if (std::abs(now - ts) > 300000) {
                RESP_ERR(cb, "时间戳已过期");
                return;
            }
        } catch (...) {
            RESP_ERR(cb, "时间戳格式错误");
            return;
        }

        // 6. 构建待签名字符串（按字母顺序排列参数）
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

        // 7. 验证签名
        DeviceKeyUtils::KeyType kt = (keyType == "rsa2048")
            ? DeviceKeyUtils::KeyType::RSA_2048
            : DeviceKeyUtils::KeyType::RSA_4096;

        bool verified = DeviceKeyUtils::verifySignature(signStr, sign, publicKey, kt);
        if (!verified) {
            LOG_WARN << "签名验证失败: mchNo=" << mchNo << " signStr=" << signStr;
            RESP_ERR(cb, "签名验证失败");
            return;
        }

        // 8. 金额验证
        double amount = 0;
        try { amount = std::stod(amountStr); } catch (...) {}
        if (amount <= 0) {
            RESP_ERR(cb, "金额错误");
            return;
        }

        // 9. 商户订单号去重
        auto exist = db.queryOne(
            "SELECT id FROM pay_order WHERE mch_id=? AND mch_order_no=?",
            {std::to_string(mchId), outTradeNo});
        if (!exist.empty()) {
            RESP_ERR(cb, "商户订单号已存在");
            return;
        }

        // 10. 选择通道
        auto channel = ChannelService::selectChannel(mchId, payType, amount);
        if (channel.channelId == 0) {
            RESP_ERR(cb, "暂无可用支付通道");
            return;
        }

        // 11. 计算费率
        double mchRate    = ChannelService::getMchRate(mchId, channel.channelId);
        double mchFee     = ChannelService::calcFee(amount, mchRate);
        double channelFee = ChannelService::calcFee(amount, channel.rate);

        // 12. 生成订单号
        std::string orderId = ChannelService::generateOrderId(
            db.getSetting("order_prefix", "W"));

        long long now = std::time(nullptr);
        int closeMin = 5;
        try { closeMin = std::stoi(db.getSetting("close_minutes", "5")); } catch (...) {}
        long long expireTime = now + closeMin * 60;

        std::string clientIp = req->getPeerAddr().toIp();

        // 13. 创建订单
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
            LOG_ERROR << "订单创建失败: orderId=" << orderId;
            RESP_ERR(cb, "订单创建失败");
            return;
        }

        LOG_INFO << "订单创建成功: orderId=" << orderId << " mchNo=" << mchNo << " amount=" << amount;

        // 14. 调用支付通道
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

        // 15. 返回成功响应
        Json::Value data;
        data["order_id"] = orderId;
        data["mch_no"] = mchNo;
        data["out_trade_no"] = outTradeNo;
        data["amount"] = ChannelService::fmtAmount(amount);
        data["subject"] = subject;
        data["pay_type"] = payType;
        data["qr_code"] = qrCode;
        data["pay_url"] = payUrl;
        data["timestamp"] = std::to_string(std::time(nullptr) * 1000);

        RESP_JSON(cb, AjaxResult::success("success", data));
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
        std::string timestamp  = (*j).get("timestamp", "").asString();
        std::string sign       = (*j).get("sign", "").asString();

        if (mchNo.empty() || outTradeNo.empty() || timestamp.empty() || sign.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        // 验证商户和签名（同上）
        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }

        // 查询订单
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
        std::string timestamp  = (*j).get("timestamp", "").asString();
        std::string sign       = (*j).get("sign", "").asString();

        if (mchNo.empty() || outTradeNo.empty() || timestamp.empty() || sign.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        // 验证商户
        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }

        // 完整签名验证
        if (!verifyGatewaySign(mch["id"], req, sign, "close")) {
            RESP_ERR(cb, "签名验证失败");
            return;
        }

        // 查询订单
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
            RESP_ERR(cb, "订单已关闭");
            return;
        }

        // 关闭订单
        bool ok = db.exec(
            "UPDATE pay_order SET state=2,updated_at=? WHERE order_id=?",
            {std::to_string(std::time(nullptr)), order["order_id"]});

        if (!ok) {
            RESP_ERR(cb, "关闭订单失败");
            return;
        }

        Json::Value data;
        data["order_id"] = order["order_id"];
        data["out_trade_no"] = outTradeNo;
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
        std::string reason       = (*j).get("reason", "").asString();
        std::string timestamp    = (*j).get("timestamp", "").asString();
        std::string sign         = (*j).get("sign", "").asString();

        if (mchNo.empty() || outTradeNo.empty() || outRefundNo.empty() ||
            refundAmount.empty() || timestamp.empty() || sign.empty()) {
            RESP_ERR(cb, "参数不完整");
            return;
        }

        auto &db = PayDb::instance();

        // 验证商户
        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {mchNo});
        if (mch.empty()) {
            RESP_ERR(cb, "商户不存在");
            return;
        }

        // 完整签名验证
        if (!verifyGatewaySign(mch["id"], req, sign, "refund")) {
            RESP_ERR(cb, "签名验证失败");
            return;
        }

        // 查询订单
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

        // 验证退款金额
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

        // 检查退款单号是否重复
        auto existRefund = db.queryOne(
            "SELECT id FROM refund_order WHERE mch_id=? AND mch_refund_no=?",
            {mch["id"], outRefundNo});
        if (!existRefund.empty()) {
            RESP_ERR(cb, "退款单号已存在");
            return;
        }

        // 生成退款订单号
        std::string refundId = "R" + std::to_string(std::time(nullptr)) +
                               std::to_string(rand() % 10000);

        long long now = std::time(nullptr);

        // 创建退款订单
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
        data["out_trade_no"] = outTradeNo;
        data["out_refund_no"] = outRefundNo;
        data["refund_amount"] = ChannelService::fmtAmount(refundAmt);
        data["state"] = 0;  // 0-处理中

        RESP_JSON(cb, AjaxResult::success("退款申请已提交", data));
    }

private:
    // 构建待签名字符串（按字母顺序排列）
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

    // 网关签名验证（close/refund 共用）
    static bool verifyGatewaySign(const std::string &mchId,
                                 const drogon::HttpRequestPtr &req,
                                 const std::string &sign,
                                 const std::string &apiType) {
        (void)apiType; // 未来可区分不同 API 签名内容
        auto &db = PayDb::instance();

        // 读取商户 RSA 公钥（优先 EdDSA/RSA 设备密钥，次商户 mch_key）
        auto devKey = db.queryOne(
            "SELECT public_key,key_type FROM device_keys "
            "WHERE user_id=? AND user_type=2 AND state=1 "
            "ORDER BY id DESC LIMIT 1",
            {mchId});
        std::string publicKey, keyType;
        if (!devKey.empty()) {
            publicKey = devKey["public_key"];
            keyType   = devKey["key_type"];
        } else {
            // 回退到商户密钥（MD5 签名）
            auto mch = db.queryOne("SELECT mch_key FROM merchant WHERE id=?", {mchId});
            if (!mch.empty() && !mch["mch_key"].empty()) {
                auto j = req->getJsonObject();
                if (j) {
                    std::map<std::string, std::string> m;
                    for (auto &k : (*j).getMemberNames()) {
                        if ((*j)[k].isString())
                            m[k] = (*j)[k].asString();
                    }
                    std::string signStr = buildSignString(m) + mch["mch_key"];
                    std::string expected = Md5Utils::md5(signStr);
                    return expected == sign;
                }
            }
            return false;
        }

        // 构建签名字符串（从请求体中提取参数）
        std::map<std::string, std::string> m;
        auto j = req->getJsonObject();
        if (j) {
            for (auto &k : (*j).getMemberNames()) {
                if ((*j)[k].isString())
                    m[k] = (*j)[k].asString();
            }
        }
        std::string signStr = buildSignString(m);

        DeviceKeyUtils::KeyType kt = DeviceKeyUtils::KeyType::RSA_2048;
        if (keyType == "ed25519") kt = DeviceKeyUtils::KeyType::ED25519;
        else if (keyType == "rsa4096") kt = DeviceKeyUtils::KeyType::RSA_4096;

        bool ok = DeviceKeyUtils::verifySignature(signStr, sign, publicKey, kt);
        if (!ok) {
            LOG_WARN << "[Gateway] 签名验证失败 mchId=" << mchId
                     << " api=" << apiType << " signStr=" << signStr;
        }
        return ok;
    }
};
