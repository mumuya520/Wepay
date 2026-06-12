#include "OrderService.h"
#include "WepayV3Config.h"
#include "SecurityValidator.h"
#include "EmailService.h"
#include "common/NotifyTaskService.h"
#include <drogon/drogon.h>
#include "common/PayDb.h"
#include "common/ChannelService.h"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <map>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <mutex>
#ifdef ENABLE_ROCKETMQ
#include <rocketmq/DefaultMQProducer.h>
#endif

namespace wepay {
namespace v3 {

namespace {
std::mutex g_v3QrDiagMutex;

void appendQrSelectionDiag(const nlohmann::json& payload) {
    try {
        std::filesystem::create_directories("logs");
        std::lock_guard<std::mutex> lock(g_v3QrDiagMutex);
        std::ofstream out("logs/v3-qrcode-select.jsonl", std::ios::app);
        if (!out.is_open()) {
            return;
        }
        out << payload.dump() << '\n';
    } catch (...) {
    }
}
}

OrderService::OrderService() {
    auto& config = WepayV3Config::getInstance();
    auto redis = std::make_shared<sw::redis::Redis>("tcp://" + config.redis.host + ":" +
                                                     std::to_string(config.redis.port));
    deviceManager_ = std::make_shared<DeviceManager>(redis);
    emailService_  = EmailService::globalInstance();
}

bool OrderService::createOrder(const OrderInfo& order) {
    try {
        // 1. 幂等性检查
        if (!checkOrderIdempotent(order.orderId)) {
            return false; // 订单已存在
        }

        // 2. 保存订单到数据库
        auto& db = PayDb::instance();
        db.exec(
            "INSERT INTO v3_order(order_id,merchant_order_id,merchant_id,device_id,"
            "qr_id,qr_code_type,qr_code_name,qr_code_content,"
            "amount,pay_type,status,screenshot_url,idempotent_flag,notify_email,"
            "created_at,expire_time,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,'PENDING','',0,?,?,?,?)",
            {order.orderId, order.merchantOrderId, order.merchantId, order.deviceId,
             order.qrId, order.qrCodeType, order.qrCodeName, order.qrCodeContent,
             std::to_string(order.amount), order.payType,
             order.notifyEmail,
             std::to_string(order.createTime),
             std::to_string(order.expireTime),
             std::to_string(order.createTime)});

        // 3. 同步写入 pay_order（统一订单管理 / 监听端共享可见）
        try {
            // 查商户 integer id
            auto mchRow = db.queryOne(
                "SELECT id,notify_url FROM merchant WHERE mch_no=? LIMIT 1",
                {order.merchantId});
            std::string mchIdStr = "0";
            std::string notifyUrl;
            if (!mchRow.empty()) {
                mchIdStr = mchRow.count("id") ? mchRow.at("id") : "0";
                notifyUrl = mchRow.count("notify_url") ? mchRow.at("notify_url") : "";
            }
            // 查 v3 商户配置中的 notify_url（优先用这个）
            if (notifyUrl.empty()) {
                auto v3Cfg = db.queryOne(
                    "SELECT callback_url FROM v3_merchant_config WHERE merchant_id=? LIMIT 1",
                    {order.merchantId});
                if (!v3Cfg.empty() && v3Cfg.count("callback_url"))
                    notifyUrl = v3Cfg.at("callback_url");
            }

            // 映射支付类型：ALIPAY→alipay, WECHAT→wxpay, QQ→qqpay
            std::string pt = order.payType;
            if (pt == "ALIPAY" || pt == "WECHAT" || pt == "QQ") {
                std::transform(pt.begin(), pt.end(), pt.begin(),
                    [](unsigned char c){ return std::tolower(c); });
            }

            db.exec(
                "INSERT INTO pay_order(order_id,mch_id,mch_order_no,channel_id,pay_type,"
                "amount,real_amount,subject,body,notify_url,client_ip,state,notify_state,"
                "expire_time,created_at,updated_at) "
                "VALUES(?,?,?,0,?,?,?,?,?,?,'',0,0,?,?,?)",
                {order.orderId, mchIdStr, order.merchantOrderId, pt,
                 ChannelService::fmtAmount(order.amount),
                 ChannelService::fmtAmount(order.amount),
                 order.merchantOrderId, order.orderId, notifyUrl,
                 std::to_string(order.expireTime),
                 std::to_string(order.createTime),
                 std::to_string(order.createTime)});
        } catch (const std::exception& e) {
            LOG_ERROR << "[V3] 同步 pay_order 失败: " << e.what();
        }

        // 5. 设置订单幂等锁（Redis）
        auto& config = WepayV3Config::getInstance();
        auto redis = sw::redis::Redis("tcp://" + config.redis.host + ":" +
                                     std::to_string(config.redis.port));

        std::string lockKey = "wepay:v3:order:lock:" + order.orderId;
        redis.setex(lockKey, 3600, "1"); // 1小时过期

        // 6. 推送订单到设备
        pushOrderToDevice(order.orderId);

        // 7. 发送订单超时延迟消息
        nlohmann::json timeoutMsg;
        timeoutMsg["orderId"] = order.orderId;
        timeoutMsg["createTime"] = order.createTime;
        sendToRocketMQ(config.rocketmq.orderTimeoutTopic, timeoutMsg);

        return true;
    } catch (...) {
        return false;
    }
}

bool OrderService::pushOrderToDevice(const std::string& orderId) {
    try {
        // 1. 查询订单信息
        auto order = getOrder(orderId);
        if (!order) {
            return false;
        }

        // 2. 选择设备
        std::string deviceId = order->deviceId.empty() ? selectDevice(order->merchantId) : order->deviceId;
        if (deviceId.empty()) {
            return false; // 没有在线设备
        }

        // 2.1 根据商户绑定/通道配置解析收款码偏好
        auto qrPreference = resolveQrPreference(order->merchantId, order->payType);
        std::string preferredCodeType = order->qrCodeType;
        std::string requestedCodeType = preferredCodeType;
        bool fallbackFromBusinessSupport = false;
        if ((preferredCodeType.empty() || preferredCodeType == "AUTO") && !qrPreference.codeType.empty()) {
            preferredCodeType = qrPreference.codeType;
        }
        if (!qrPreference.supportBusinessCode && preferredCodeType == "BUSINESS") {
            preferredCodeType = "PERSONAL";
            fallbackFromBusinessSupport = true;
        }

        // 2.2 选择设备下的收款码（按设备+支付类型+码类型偏好轮询）
        auto qr = selectQrCode(order->merchantId, deviceId, order->payType, preferredCodeType);
        nlohmann::json qrDiag = {
            {"ts", std::time(nullptr)},
            {"orderId", order->orderId},
            {"merchantId", order->merchantId},
            {"deviceId", deviceId},
            {"payType", order->payType},
            {"requestedCodeType", requestedCodeType},
            {"resolvedCodeType", qrPreference.codeType},
            {"preferredCodeType", preferredCodeType},
            {"supportBusinessCode", qrPreference.supportBusinessCode},
            {"fallbackFromBusinessSupport", fallbackFromBusinessSupport}
        };
        if (!qr) {
            qrDiag["matched"] = false;
            qrDiag["reason"] = "no_qrcode_available_after_preference_fallback";
            appendQrSelectionDiag(qrDiag);
            LOG_WARN << "[V3] no qrcode available, merchant=" << order->merchantId
                     << ", device=" << deviceId << ", payType=" << order->payType;
            return false;
        }

        order->deviceId = deviceId;
        order->qrId = qr->id;
        order->qrCodeType = qr->codeType;
        order->qrCodeName = qr->codeName;
        order->qrCodeContent = qr->codeContent;
        qrDiag["matched"] = true;
        qrDiag["selectedQrId"] = qr->id;
        qrDiag["selectedQrCodeType"] = qr->codeType;
        qrDiag["selectedQrName"] = qr->codeName;
        appendQrSelectionDiag(qrDiag);

        try {
            auto& db = PayDb::instance();
            long now = std::time(nullptr);
            db.exec(
                "UPDATE v3_order SET device_id=?,qr_id=?,qr_code_type=?,qr_code_name=?,qr_code_content=?,updated_at=? WHERE order_id=?",
                {order->deviceId, order->qrId, order->qrCodeType, order->qrCodeName, order->qrCodeContent,
                 std::to_string(now), orderId});
            db.exec("UPDATE v3_device_qrcode SET last_used_at=?,updated_at=? WHERE id=?",
                    {std::to_string(now), std::to_string(now), qr->id});
        } catch (const std::exception& e) {
            LOG_WARN << "[V3] persist qrcode info failed: " << e.what();
        }

        // 3. 更新订单状态为"推送中"
        updateOrderStatus(orderId, "PUSHING");

        // 4. 构建推送消息
        nlohmann::json orderData;
        orderData["orderId"] = order->orderId;
        orderData["merchantOrderId"] = order->merchantOrderId;
        orderData["amount"] = order->amount;
        orderData["payType"] = order->payType;
        orderData["expireTime"] = order->expireTime;
        orderData["deviceId"] = order->deviceId;
        orderData["qrId"] = order->qrId;
        orderData["qrCodeType"] = order->qrCodeType;
        orderData["qrCodeName"] = order->qrCodeName;
        orderData["qrCodeContent"] = order->qrCodeContent;

        // 5. 通过WebSocket推送
        bool wsSuccess = WebSocketManager::getInstance().pushOrderToDevice(deviceId, orderData);

        if (wsSuccess) {
            return true;
        }

        // 6. WebSocket推送失败，发送到RocketMQ重试
        nlohmann::json mqMsg;
        mqMsg["orderId"] = orderId;
        mqMsg["deviceId"] = deviceId;
        mqMsg["orderData"] = orderData;
        sendToRocketMQ(WepayV3Config::getInstance().rocketmq.orderPushTopic, mqMsg);

        return true;
    } catch (...) {
        return false;
    }
}

bool OrderService::updateOrderStatus(const std::string& orderId, const std::string& status) {
    try {
        auto& db = PayDb::instance();
        long now = std::time(nullptr);

        // 查旧状态（用于日志）
        std::string oldStatus;
        auto rows = db.query("SELECT status FROM v3_order WHERE order_id=? LIMIT 1", {orderId});
        if (!rows.empty()) oldStatus = rows[0].count("status") ? rows[0].at("status") : "";

        db.exec("UPDATE v3_order SET status=?,updated_at=? WHERE order_id=?",
                {status, std::to_string(now), orderId});
        db.exec("INSERT INTO v3_order_status_log(order_id,old_status,new_status,remark,created_at) "
                "VALUES(?,?,?,?,?)",
                {orderId, oldStatus, status, "handlePush/handleTimeout", std::to_string(now)});

        // 每笔 FAILED 订单发邮件（含手动回调按钮）
        if (emailService_ && (status == "FAILED" || status == "TIMEOUT")) {
            sendOrderEmail(orderId, status);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_WARN << "[V3] updateOrderStatus error: " << e.what();
        return false;
    }
}

std::optional<OrderInfo> OrderService::getOrder(const std::string& orderId) {
    try {
        auto& db = PayDb::instance();
        auto rows = db.query(
            "SELECT order_id,merchant_order_id,merchant_id,device_id,"
            "qr_id,qr_code_type,qr_code_name,qr_code_content,"
            "amount,pay_type,status,screenshot_url,notify_email,created_at,pay_time,expire_time "
            "FROM v3_order WHERE order_id=? LIMIT 1", {orderId});
        if (rows.empty()) return std::nullopt;
        auto& r = rows[0];
        OrderInfo info;
        info.orderId         = orderId;
        info.merchantOrderId = r.count("merchant_order_id") ? r.at("merchant_order_id") : "";
        info.merchantId      = r.count("merchant_id")       ? r.at("merchant_id")       : "";
        info.deviceId        = r.count("device_id")         ? r.at("device_id")         : "";
        info.qrId            = r.count("qr_id")             ? r.at("qr_id")             : "";
        info.qrCodeType      = r.count("qr_code_type")      ? r.at("qr_code_type")      : "PERSONAL";
        info.qrCodeName      = r.count("qr_code_name")      ? r.at("qr_code_name")      : "";
        info.qrCodeContent   = r.count("qr_code_content")   ? r.at("qr_code_content")   : "";
        info.amount          = r.count("amount") && !r.at("amount").empty() ? std::stod(r.at("amount")) : 0.0;
        info.payType         = r.count("pay_type")          ? r.at("pay_type")          : "";
        info.status          = r.count("status")            ? r.at("status")            : "";
        info.screenshotUrl   = r.count("screenshot_url")    ? r.at("screenshot_url")    : "";
        info.notifyEmail     = r.count("notify_email")      ? r.at("notify_email")      : "";
        info.createTime      = r.count("created_at") && !r.at("created_at").empty() ? std::stoll(r.at("created_at")) : 0;
        info.payTime         = r.count("pay_time")   && !r.at("pay_time").empty()   ? std::stoll(r.at("pay_time"))   : 0;
        info.expireTime      = r.count("expire_time") && !r.at("expire_time").empty()? std::stoll(r.at("expire_time")): 0;
        return info;
    } catch (const std::exception& e) {
        LOG_WARN << "[V3] getOrder error: " << e.what();
        return std::nullopt;
    }
}

std::vector<OrderInfo> OrderService::getPendingOrders(const std::string& deviceId) {
    std::vector<OrderInfo> orders;

    try {
        auto& db = PayDb::instance();
        long now = std::time(nullptr);
        auto rows = db.query(
            "SELECT order_id,merchant_order_id,merchant_id,device_id,"
            "qr_id,qr_code_type,qr_code_name,qr_code_content,"
            "amount,pay_type,status,created_at,pay_time,expire_time "
            "FROM v3_order WHERE device_id=? AND status IN ('PENDING','PUSHING') "
            "AND expire_time>? ORDER BY created_at ASC",
            {deviceId, std::to_string(now)});
        for (auto& r : rows) {
            OrderInfo info;
            info.orderId         = r["order_id"];
            info.merchantOrderId = r.count("merchant_order_id") ? r.at("merchant_order_id") : "";
            info.merchantId      = r.count("merchant_id")       ? r.at("merchant_id")       : "";
            info.deviceId        = deviceId;
            info.qrId            = r.count("qr_id")           ? r.at("qr_id")           : "";
            info.qrCodeType      = r.count("qr_code_type")    ? r.at("qr_code_type")    : "PERSONAL";
            info.qrCodeName      = r.count("qr_code_name")    ? r.at("qr_code_name")    : "";
            info.qrCodeContent   = r.count("qr_code_content") ? r.at("qr_code_content") : "";
            info.amount          = r.count("amount") && !r.at("amount").empty() ? std::stod(r.at("amount")) : 0.0;
            info.payType         = r.count("pay_type") ? r.at("pay_type") : "";
            info.status          = r.count("status")   ? r.at("status")   : "";
            info.createTime      = r.count("created_at")  && !r.at("created_at").empty()  ? std::stoll(r.at("created_at"))  : 0;
            info.expireTime      = r.count("expire_time") && !r.at("expire_time").empty() ? std::stoll(r.at("expire_time")) : 0;
            orders.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        LOG_WARN << "[V3] getPendingOrders error: " << e.what();
    }

    return orders;
}

bool OrderService::confirmOrderPaid(const std::string& orderId, const std::string& screenshotUrl) {
    try {
        auto& db = PayDb::instance();
        long now = std::time(nullptr);

        // 1. 查询订单
        auto order = getOrder(orderId);
        if (!order) return false;

        // 2. 更新订单状态
        db.exec("UPDATE v3_order SET status='PAID',pay_time=?,updated_at=?,screenshot_url=? "
                "WHERE order_id=?",
                {std::to_string(now), std::to_string(now), screenshotUrl, orderId});
        db.exec("INSERT INTO v3_order_status_log(order_id,old_status,new_status,remark,created_at) "
                "VALUES(?,?,?,?,?)",
                {orderId, order->status, "PAID", "handlePush", std::to_string(now)});
        order->status  = "PAID";
        order->payTime = now;

        // 3. 触发商户回调
        bool callbackSuccess = triggerMerchantCallback(*order);

        // 4. 每笔订单无论回调成功与否都发邮件通知（商户可手动回调）
        if (emailService_) {
            sendOrderEmail(orderId, "PAID", !callbackSuccess);
        }

        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "[V3] confirmOrderPaid error: " << e.what();
        return false;
    }
}

void OrderService::handleOrderTimeout(const std::string& orderId) {
    try {
        auto order = getOrder(orderId);
        if (!order) return;
        if (order->status != "PENDING" && order->status != "PUSHING") return;

        updateOrderStatus(orderId, "TIMEOUT"); // 内部已发 TIMEOUT 邮件

        // 同时走 RocketMQ（如已启用）
        nlohmann::json msg; msg["type"] = "ORDER_TIMEOUT"; msg["orderId"] = orderId;
        sendToRocketMQ(WepayV3Config::getInstance().rocketmq.emailNotifyTopic, msg);
    } catch (...) {}
}

bool OrderService::checkOrderIdempotent(const std::string& orderId) {
    try {
        auto& config = WepayV3Config::getInstance();
        auto redis = sw::redis::Redis("tcp://" + config.redis.host + ":" +
                                     std::to_string(config.redis.port));

        std::string lockKey = "wepay:v3:order:lock:" + orderId;
        return !redis.exists(lockKey);
    } catch (...) {
        return false;
    }
}

std::string OrderService::selectDevice(const std::string& merchantId) {
    // 获取商户的在线设备列表
    auto devices = deviceManager_->getOnlineDevices(merchantId);

    if (devices.empty()) {
        return "";
    }

    // 简单轮询策略：选择第一个设备
    // TODO: 可以实现更复杂的负载均衡策略
    return devices[0];
}

OrderService::QrSelectionPreference OrderService::resolveQrPreference(
    const std::string& merchantNo,
    const std::string& payType) {
    QrSelectionPreference pref;
    try {
        auto& db = PayDb::instance();
        auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=? LIMIT 1", {merchantNo});
        if (mch.empty() || !mch.count("id") || mch.at("id").empty()) {
            return pref;
        }

        const std::string mchId = mch.at("id");
        int mchIdInt = 0;
        try {
            mchIdInt = std::stoi(mchId);
        } catch (...) {
            return pref;
        }

        auto channel = ChannelService::selectChannel(mchIdInt, payType, 0.01);
        if (channel.channelId <= 0) {
            return pref;
        }

        auto row = db.queryOne(
            "SELECT c.code_type,c.support_business_code,mc.code_type AS mch_code_type "
            "FROM pay_channel c "
            "LEFT JOIN merchant_channel mc ON mc.channel_id=c.id AND mc.mch_id=? "
            "WHERE c.id=? LIMIT 1",
            {mchId, std::to_string(channel.channelId)});
        if (row.empty()) {
            return pref;
        }

        if (row.count("support_business_code") && !row.at("support_business_code").empty()) {
            try {
                pref.supportBusinessCode = std::stoi(row.at("support_business_code")) != 0;
            } catch (...) {}
        }

        const std::string mchCodeType = row.count("mch_code_type") ? row.at("mch_code_type") : "";
        const std::string chCodeType = row.count("code_type") ? row.at("code_type") : "";
        pref.codeType = !mchCodeType.empty() ? mchCodeType : chCodeType;
        if (!pref.supportBusinessCode && pref.codeType == "BUSINESS") {
            pref.codeType = "PERSONAL";
        }
    } catch (const std::exception& e) {
        LOG_WARN << "[V3] resolveQrPreference error: " << e.what();
    } catch (...) {
    }
    return pref;
}

std::optional<OrderService::DeviceQrCode> OrderService::selectQrCode(
    const std::string& merchantId,
    const std::string& deviceId,
    const std::string& payType,
    const std::string& preferredCodeType) {
    try {
        auto& db = PayDb::instance();
        std::string sql =
            "SELECT id,device_id,merchant_id,pay_type,code_type,code_name,code_content "
            "FROM v3_device_qrcode WHERE device_id=? AND state=1 AND LOWER(pay_type)=LOWER(?) ";
        std::vector<std::string> params = {deviceId, payType};

        if (!merchantId.empty()) {
            sql += "AND (merchant_id='' OR merchant_id=?) ";
            params.push_back(merchantId);
        }
        if (!preferredCodeType.empty()) {
            sql += "AND LOWER(code_type)=LOWER(?) ";
            params.push_back(preferredCodeType);
        }

        sql += "ORDER BY last_used_at ASC, sort_order ASC, id ASC LIMIT 1";
        auto rows = db.query(sql, params);
        if (rows.empty() && !preferredCodeType.empty()) {
            return selectQrCode(merchantId, deviceId, payType, "");
        }
        if (rows.empty()) {
            return std::nullopt;
        }

        auto& row = rows[0];
        DeviceQrCode qr;
        qr.id = row.count("id") ? row.at("id") : "";
        qr.deviceId = row.count("device_id") ? row.at("device_id") : "";
        qr.merchantId = row.count("merchant_id") ? row.at("merchant_id") : "";
        qr.payType = row.count("pay_type") ? row.at("pay_type") : "";
        qr.codeType = row.count("code_type") ? row.at("code_type") : "PERSONAL";
        qr.codeName = row.count("code_name") ? row.at("code_name") : "";
        qr.codeContent = row.count("code_content") ? row.at("code_content") : "";
        return qr;
    } catch (const std::exception& e) {
        LOG_WARN << "[V3] selectQrCode error: " << e.what();
        return std::nullopt;
    }
}

bool OrderService::sendToRocketMQ(const std::string& topic, const nlohmann::json& message) {
#ifdef ENABLE_ROCKETMQ
    try {
        auto& config = WepayV3Config::getInstance();

        rocketmq::DefaultMQProducer producer("wepay_v3_producer");
        producer.setNamesrvAddr(config.rocketmq.namesrvAddr);
        producer.start();

        rocketmq::Message msg(topic, message.dump());
        auto result = producer.send(msg);

        producer.shutdown();

        return result.getSendStatus() == rocketmq::SendStatus::SEND_OK;
    } catch (...) {
        return false;
    }
#else
    LOG_INFO << "[V3] RocketMQ disabled, skipping sendToRocketMQ topic=" << topic;
    return false;
#endif
}

void OrderService::sendOrderEmail(const std::string& orderId,
                                   const std::string& statusType,
                                   bool needCallbackBtn) {
    if (!emailService_) return;
    try {
        auto& db = PayDb::instance();

        // 查订单
        auto orderOpt = getOrder(orderId);
        if (!orderOpt) return;
        auto& ord = *orderOpt;

        // 查商户配置
        auto cfgs = db.query(
            "SELECT notify_email,email_notify_enabled,notify_on_success,notify_on_fail,"
            "hmac_secret,callback_url FROM v3_merchant_config "
            "WHERE merchant_id=? AND status=1 LIMIT 1", {ord.merchantId});
        if (cfgs.empty()) return;
        auto& cfg = cfgs[0];

        if (cfg.count("email_notify_enabled") && cfg.at("email_notify_enabled") == "0") return;
        // 优先用下单时指定的邮箱，否则使用开发者（SMTP 发件账号）邮箱
        std::string toEmail = ord.notifyEmail;
        if (toEmail.empty()) {
            auto svc = EmailService::globalInstance();
            if (svc) toEmail = svc->getFromEmail();
        }
        if (toEmail.empty()) return;

        // 检查各状态开关
        if (statusType == "PAID") {
            if (cfg.count("notify_on_success") && cfg.at("notify_on_success") == "0") return;
        } else {
            if (cfg.count("notify_on_fail") && cfg.at("notify_on_fail") == "0") return;
        }

        EmailService::EmailData data;
        data.orderId         = ord.orderId;
        data.merchantOrderId = ord.merchantOrderId;
        data.merchantId      = ord.merchantId;
        data.toEmail         = toEmail;

        // 格式化金额
        std::ostringstream amtSs;
        amtSs << std::fixed << std::setprecision(2) << ord.amount;
        data.money   = amtSs.str();
        data.payType = ord.payType;

        // 格式化时间
        time_t pt = (time_t)ord.payTime;
        char timeBuf[32];
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &pt);
#else
        localtime_r(&pt, &tm);
#endif
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm);
        data.payTime = timeBuf;

        time_t ct = (time_t)ord.createTime;
        struct tm ctm;
#ifdef _WIN32
        localtime_s(&ctm, &ct);
#else
        localtime_r(&ct, &ctm);
#endif
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &ctm);
        data.createTime = timeBuf;
        data.deviceId   = ord.deviceId;

        if (statusType == "PAID" && !needCallbackBtn) {
            emailService_->sendPaySuccessEmail(data);
        } else if (statusType == "MANUAL_CALLBACK") {
            // 管理员手动回调 → 发送成功邮件，告知已由管理员手动触发
            data.failReason = "管理员手动触发回调";
            data.callbackUrl = "";
            emailService_->sendPaySuccessEmail(data);
        } else {
            // FAILED / TIMEOUT / PAID+callback失败 → 带手动回调按钮
            data.failReason = (statusType == "TIMEOUT") ? "订单超时未支付" :
                              (statusType == "FAILED")  ? "支付失败" :
                                                          "自动回调失败，需手动触发";

            // 生成手动回调 URL
            std::string secret  = cfg.count("hmac_secret")  ? cfg.at("hmac_secret")  : "";
            auto& wconfig = WepayV3Config::getInstance();
            CallbackTokenGenerator::TokenData td;
            td.orderId    = orderId;
            td.merchantId = ord.merchantId;
            td.expireTime = std::time(nullptr) + 86400; // 24h
            data.callbackUrl = CallbackTokenGenerator::generateCallbackUrl(
                wconfig.baseUrl, td, secret);
            if (!CallbackTokenGenerator::isSafePublicUrl(data.callbackUrl)) {
                LOG_WARN << "[V3-Email] unsafe manual callback URL, baseUrl="
                         << wconfig.baseUrl << " orderId=" << orderId;
                data.callbackUrl.clear();
            }

            emailService_->sendPayFailEmail(data);
        }

        LOG_INFO << "[V3-Email] Sent " << statusType << " email to " << toEmail
                 << " orderId=" << orderId;
    } catch (const std::exception& e) {
        LOG_WARN << "[V3-Email] sendOrderEmail error: " << e.what();
    }
}

bool OrderService::triggerMerchantCallback(const OrderInfo& order) {
    try {
        auto& db = PayDb::instance();
        auto cfgs = db.query(
            "SELECT callback_url,hmac_secret FROM v3_merchant_config "
            "WHERE merchant_id=? AND status=1 LIMIT 1", {order.merchantId});
        if (cfgs.empty() || cfgs[0].at("callback_url").empty()) {
            LOG_WARN << "[V3] No callback_url for merchant=" << order.merchantId;
            return false;
        }
        std::string callbackUrl = cfgs[0].at("callback_url");
        std::string secret      = cfgs[0].count("hmac_secret") ? cfgs[0].at("hmac_secret") : "";

        // 构建 payload
        long ts = std::time(nullptr);
        nlohmann::json body;
        body["orderId"]         = order.orderId;
        body["merchantOrderId"] = order.merchantOrderId;
        body["merchantId"]      = order.merchantId;
        body["amount"]          = std::to_string(order.amount);
        body["payType"]         = order.payType;
        body["status"]          = order.status;
        body["payTime"]         = std::to_string(order.payTime);
        body["timestamp"]       = std::to_string(ts);

        // sorted-qs HMAC-SHA256 签名
        std::map<std::string,std::string> params;
        for (auto& [k,v] : body.items())
            params[k] = v.is_string() ? v.get<std::string>() : v.dump();
        std::ostringstream qs;
        for (auto& [k,v] : params) qs << k << "=" << v << "&";
        std::string toSign = qs.str();
        if (!toSign.empty()) toSign.pop_back();
        unsigned char hash[SHA256_DIGEST_LENGTH];
        HMAC(EVP_sha256(), secret.c_str(), secret.size(),
             (unsigned char*)toSign.c_str(), toSign.size(), hash, nullptr);
        std::ostringstream sig;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            sig << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        body["sign"] = sig.str();

        std::string payload = body.dump();
        bool success = false;
        CURL* curl = curl_easy_init();
        if (curl) {
            struct curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_URL, callbackUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                long code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
                success = (code >= 200 && code < 300);
            }
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(curl);
        }
        LOG_INFO << "[V3] triggerMerchantCallback " << (success?"OK":"FAIL")
                 << " order=" << order.orderId << " url=" << callbackUrl;

        // 写通知任务记录（幂等：不存在才插入，让运维面板可见）
        if (!callbackUrl.empty()) {
            NotifyTaskService::createTaskAndSend(order.orderId, callbackUrl, "WepayV3Plugin");
        }

        return success;
    } catch (const std::exception& e) {
        LOG_ERROR << "[V3] triggerMerchantCallback error: " << e.what();
        return false;
    }
}

// OrderPushController实现
OrderPushController::OrderPushController() {
    auto& config = WepayV3Config::getInstance();

    validator_ = std::make_shared<SecurityValidator>(
        config.security.hmacSecret,
        config.security.rsaPublicKey
    );

    orderService_ = std::make_shared<OrderService>();
}

void OrderPushController::handlePush(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        auto jsonBody = req->getJsonObject();
        if (!jsonBody) {
            callback(buildResponse(400, "Invalid JSON body"));
            return;
        }

        // 解析参数
        std::string deviceId = (*jsonBody)["deviceId"].asString();
        std::string timestamp = (*jsonBody)["timestamp"].asString();
        std::string nonce = (*jsonBody)["nonce"].asString();
        std::string sign = (*jsonBody)["sign"].asString();
        std::string orderId = (*jsonBody)["orderId"].asString();
        std::string status = (*jsonBody)["status"].asString(); // "PAID" / "FAILED"

        // 动态从 DB 读取 hmac_secret
        std::string clientIp = req->getPeerAddr().toIp();
        auto dynValidator = std::make_shared<SecurityValidator>(
            SecurityValidator::resolveHmacSecret(deviceId), "");

        // 安全校验
        auto validationResult = dynValidator->validateRequest(
            deviceId, timestamp, nonce, sign, clientIp
        );

        if (!validationResult.success) {
            callback(buildResponse(403, validationResult.errorMsg));
            return;
        }

        // 更新订单状态
        if (status == "PAID") {
            std::string screenshotUrl = (*jsonBody)["screenshotUrl"].isString() ? (*jsonBody)["screenshotUrl"].asString() : "";
            orderService_->confirmOrderPaid(orderId, screenshotUrl);
        } else if (status == "FAILED") {
            orderService_->updateOrderStatus(orderId, "FAILED");
        }

        callback(buildResponse(200, "success"));

    } catch (const std::exception& e) {
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

void OrderPushController::handlePending(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        auto jsonBody = req->getJsonObject();
        if (!jsonBody) {
            callback(buildResponse(400, "Invalid JSON body"));
            return;
        }

        std::string deviceId = (*jsonBody)["deviceId"].asString();
        std::string timestamp = (*jsonBody)["timestamp"].asString();
        std::string nonce = (*jsonBody)["nonce"].asString();
        std::string sign = (*jsonBody)["sign"].asString();

        // 安全校验（动态取密钥）
        std::string clientIp = req->getPeerAddr().toIp();
        auto dynValidator = std::make_shared<SecurityValidator>(
            SecurityValidator::resolveHmacSecret(deviceId), "");
        auto validationResult = dynValidator->validateRequest(
            deviceId, timestamp, nonce, sign, clientIp
        );

        if (!validationResult.success) {
            callback(buildResponse(403, validationResult.errorMsg));
            return;
        }

        // 查询待推送订单
        auto orders = orderService_->getPendingOrders(deviceId);

        nlohmann::json ordersJson = nlohmann::json::array();
        for (const auto& order : orders) {
            nlohmann::json orderJson;
            orderJson["orderId"] = order.orderId;
            orderJson["amount"] = order.amount;
            orderJson["payType"] = order.payType;
            orderJson["expireTime"] = order.expireTime;
            orderJson["deviceId"] = order.deviceId;
            orderJson["qrId"] = order.qrId;
            orderJson["qrCodeType"] = order.qrCodeType;
            orderJson["qrCodeName"] = order.qrCodeName;
            orderJson["qrCodeContent"] = order.qrCodeContent;
            ordersJson.push_back(orderJson);
        }

        nlohmann::json responseData;
        responseData["orders"] = ordersJson;

        callback(buildResponse(200, "success", responseData));

    } catch (const std::exception& e) {
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// 通知栏监听上报接口  POST /api/wepay/v3/notify
// 安卓 App 通过 AccessibilityService / 通知监听服务捕获微信/支付宝收款通知后调用。
// 请求参数：
//   deviceId   设备ID
//   payType    "WECHAT" / "ALIPAY" / "QQ"
//   amount     收款金额（字符串，精度 0.01）
//   remark     收款备注/付款方（可选，用于多笔相同金额去歧义）
//   timestamp  Unix秒
//   nonce      随机字符串（防重放）
//   sign       HMAC-SHA256(deviceId+payType+amount+timestamp+nonce, key)
// ──────────────────────────────────────────────────────────────────────────────
std::optional<OrderInfo> OrderService::matchOrderByAmount(
    const std::string& merchantId,
    double amount,
    const std::string& payType) {

    try {
        auto& db = PayDb::instance();
        long nowTs = std::time(nullptr);
        // 找该商户当前仍有效、金额匹配、状态为未支付且未关闭的最旧订单（FIFO匹配）
        std::string sql =
            "SELECT order_id, merchant_order_id, merchant_id, device_id, "
            "       amount, pay_type, status, created_at, expire_time "
            "FROM v3_order "
            "WHERE merchant_id=? AND status IN ('PENDING','PUSHING') "
            "  AND ABS(CAST(amount AS REAL) - ?)< 0.001 "
            "  AND expire_time > ? ";

        std::vector<std::string> args = {merchantId, std::to_string(amount), std::to_string(nowTs)};

        if (!payType.empty()) {
            sql += "AND LOWER(pay_type)=LOWER(?) ";
            args.push_back(payType);
        }
        sql += "ORDER BY created_at ASC LIMIT 1";

        auto rows = db.query(sql, args);
        if (rows.empty()) return std::nullopt;

        auto& r = rows[0];
        OrderInfo info;
        info.orderId         = r["order_id"];
        info.merchantOrderId = r["merchant_order_id"];
        info.merchantId      = r["merchant_id"];
        info.deviceId        = r["device_id"];
        info.amount          = r["amount"].empty() ? 0.0 : std::stod(r["amount"]);
        info.payType         = r["pay_type"];
        info.status          = r["status"];
        info.createTime      = r["created_at"].empty()   ? 0 : std::stoll(r["created_at"]);
        info.expireTime      = r["expire_time"].empty()  ? 0 : std::stoll(r["expire_time"]);
        return info;

    } catch (const std::exception& e) {
        LOG_WARN << "[V3-Notify] matchOrderByAmount error: " << e.what();
        return std::nullopt;
    }
}

void OrderPushController::handleNotify(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        auto jsonBody = req->getJsonObject();
        if (!jsonBody) {
            callback(buildResponse(400, "Invalid JSON body"));
            return;
        }

        std::string deviceId  = (*jsonBody)["deviceId"].asString();
        std::string payType   = (*jsonBody)["payType"].asString();   // WECHAT/ALIPAY/QQ
        std::string amountStr = (*jsonBody)["amount"].asString();
        std::string remark    = (*jsonBody).get("remark", "").asString();
        std::string timestamp = (*jsonBody)["timestamp"].asString();
        std::string nonce     = (*jsonBody)["nonce"].asString();
        std::string sign      = (*jsonBody)["sign"].asString();

        if (deviceId.empty() || amountStr.empty() || payType.empty()) {
            callback(buildResponse(400, "Missing required fields: deviceId/payType/amount"));
            return;
        }

        double amount = 0.0;
        try { amount = std::stod(amountStr); } catch (...) {
            callback(buildResponse(400, "Invalid amount format"));
            return;
        }
        if (amount <= 0.0) {
            callback(buildResponse(400, "Amount must be positive"));
            return;
        }

        // 1. HMAC 签名验证（动态取密钥，包含 amount/payType，与客户端一致）
        std::string clientIp = req->getPeerAddr().toIp();
        auto dynValidator = std::make_shared<SecurityValidator>(
            SecurityValidator::resolveHmacSecret(deviceId), "");
        auto vr = dynValidator->validateRequest(deviceId, timestamp, nonce, sign, clientIp,
            {{"amount", amountStr}, {"payType", payType}});
        if (!vr.success) {
            callback(buildResponse(403, vr.errorMsg));
            return;
        }

        // 2. 查询该设备绑定的商户（merchant.id）
        auto& db = PayDb::instance();
        auto mchRows = db.query(
            "SELECT merchant_id FROM v3_device_merchant WHERE device_id=?", {deviceId});

        if (mchRows.empty()) {
            nlohmann::json d; d["matched"] = false; d["reason"] = "device not bound to any merchant";
            callback(buildResponse(200, "no match", d));
            return;
        }

        // 2. 将手机端上报的 payType 转换为小写（数据库存储格式）
        std::string payTypeLower = payType;
        for (auto& c : payTypeLower) c = std::tolower(c);

        // 3. 在 pay_order 中按金额匹配最旧的待支付订单
        long payTs = std::time(nullptr);
        PayDb::Row order;
        std::string mchId;
        for (auto& mr : mchRows) {
            mchId = mr["merchant_id"];
            order = db.queryOne(
                "SELECT * FROM pay_order WHERE state=0 AND mch_id=? AND LOWER(pay_type)=LOWER(?) "
                "AND ABS(CAST(amount AS FLOAT) - ?) < 0.001 "
                "AND (expire_time=0 OR expire_time>?) "
                "ORDER BY created_at ASC LIMIT 1",
                {mchId, payTypeLower, std::to_string(amount), std::to_string(payTs)});
            if (!order.empty()) break;
        }

        if (order.empty()) {
            LOG_INFO << "[V3-Notify] No pending order matched: device=" << deviceId
                     << " pay_type=" << payTypeLower << " amount=" << amount;
            nlohmann::json d;
            d["matched"] = false;
            d["reason"]  = "no pending order matches amount=" + amountStr + " payType=" + payTypeLower;
            callback(buildResponse(200, "no match", d));
            return;
        }

        std::string matchedOrderId = order.count("order_id") ? order["order_id"] : "";

        // 4. 标记 pay_order 为已支付
        db.exec(
            "UPDATE pay_order SET state=1, pay_time=? WHERE order_id=?",
            {std::to_string(payTs), matchedOrderId});

        // 5. 触发商户回调（优先用订单自己的 notify_url，没有再查 merchant 表）
        bool cbOk = false;
        std::string orderNotifyUrl = order.count("notify_url") ? order.at("notify_url") : "";
        if (orderNotifyUrl.empty()) {
            auto mchRow = db.queryOne(
                "SELECT mch_key, notify_url FROM merchant WHERE id=?", {mchId});
            if (!mchRow.empty()) {
                if (!mchRow["notify_url"].empty())
                    orderNotifyUrl = mchRow["notify_url"];
            }
        }
        if (!orderNotifyUrl.empty()) {
            std::string callKey;
            auto mchRow = db.queryOne("SELECT mch_key FROM merchant WHERE id=?", {mchId});
            if (!mchRow.empty()) callKey = mchRow["mch_key"];
            std::string notifyUrl = orderNotifyUrl;
            std::string realAmount = order.count("real_amount") ? order["real_amount"] : amountStr;
            long ts = std::time(nullptr);
            std::map<std::string,std::string> params = {
                {"order_id", matchedOrderId},
                {"pay_type", payTypeLower},
                {"price",    realAmount},
                {"timestamp", std::to_string(ts)}
            };
            std::ostringstream qs;
            for (auto& [k,v] : params) qs << k << "=" << v << "&";
            std::string toSign = qs.str(); if (!toSign.empty()) toSign.pop_back();
            toSign += "&key=" + callKey;
            unsigned char hash[SHA256_DIGEST_LENGTH];
            HMAC(EVP_sha256(), callKey.c_str(), callKey.size(),
                 (unsigned char*)toSign.c_str(), toSign.size(), hash, nullptr);
            std::ostringstream sig;
            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
                sig << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
            params["sign"] = sig.str();
            std::ostringstream postQs;
            for (auto& [k,v] : params) postQs << k << "=" << v << "&";
            std::string postBody = postQs.str(); if (!postBody.empty()) postBody.pop_back();

            CURL* curl = curl_easy_init();
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, notifyUrl.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.c_str());
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                CURLcode res = curl_easy_perform(curl);
                if (res == CURLE_OK) {
                    long code = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
                    cbOk = (code >= 200 && code < 300);
                }
                curl_easy_cleanup(curl);
            }
        }

        // 7. WebSocket 广播到所有管理员连接（实时更新面板）
        {
            nlohmann::json wsMsg;
            wsMsg["type"]       = "ORDER_PAID";
            wsMsg["orderId"]    = matchedOrderId;
            wsMsg["merchantId"] = mchId;
            wsMsg["amount"]     = amount;
            wsMsg["payType"]    = payType;
            wsMsg["payTime"]    = payTs;
            wsMsg["deviceId"]   = deviceId;
            try { WebSocketManager::getInstance().broadcastMessage(wsMsg); } catch (...) {}
        }

        // 8. 发邮件通知（回调失败时含手动回调按钮）
        try { orderService_->sendOrderEmail(matchedOrderId, "PAID", !cbOk); } catch (...) {}

        LOG_INFO << "[V3-Notify] Matched order=" << matchedOrderId
                 << " amount=" << amount << " payType=" << payTypeLower
                 << " device=" << deviceId << " cb=" << (cbOk ? "OK" : "FAIL");

        nlohmann::json d;
        d["matched"]  = true;
        d["orderId"]  = matchedOrderId;
        d["amount"]   = amount;
        d["payTime"]  = payTs;
        callback(buildResponse(200, "success", d));

    } catch (const std::exception& e) {
        LOG_ERROR << "[V3-Notify] handleNotify exception: " << e.what();
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

drogon::HttpResponsePtr OrderPushController::buildResponse(
    int code,
    const std::string& message,
    const nlohmann::json& data) {

    nlohmann::json response;
    response["code"] = code;
    response["message"] = message;

    if (!data.empty()) {
        response["data"] = data;
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeString("application/json");
    resp->setBody(response.dump());

    if (code == 200) {
        resp->setStatusCode(drogon::k200OK);
    } else if (code == 400) {
        resp->setStatusCode(drogon::k400BadRequest);
    } else if (code == 403) {
        resp->setStatusCode(drogon::k403Forbidden);
    } else {
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    return resp;
}

} // namespace v3
} // namespace wepay
