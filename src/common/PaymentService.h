// WePay-Cpp — 支付匹配 / 入账 / 通知 / MQ 公共服务
//
// 多个入口共享同一份"收到一笔到账金额 → 匹配未支付订单 → 标记已付 → 入账 →
// 触发商户回调 / 发布 MQ"的逻辑:
//   - DeviceCtrl       (/device/push, V免签 /appPush, 码支付, mpayNotify)
//   - CefBillCtrl      (webpay 浏览器拦截支付宝账单接口的回调)
//   - NotifyReceiveCtrl(上游通道异步回调)
//
// 收敛在这里, 既避免代码重复, 也保证 MQ 事件和 notify_url 行为完全一致.
//
// MQ 主题 (exchange = "wepay"):
//   payment.raw            入口收到的原始金额 (匹配前)
//   payment.unmatched      匹配未命中
//   order.paid             订单匹配成功 + 已入账
//   bill.alipay.raw        CEF 推过来的支付宝账单 JSON 原文
//   cookie.alipay.updated  CEF 推过来的登录 Cookie 已更新

#pragma once // 防止头文件重复包含
#include <ctime> // C 时间库
#include <map> // 映射容器
#include <string> // 字符串库
#include <unordered_map> // 哈希表
#include <json/json.h> // JSON 库

#include "ChannelService.h" // 通道服务
#include "EpaySign.h" // 易支付签名
#include "MqService.h" // 消息队列服务
#include "NotifyTaskService.h" // 通知任务服务
#include "PayDb.h" // 数据库操作

// 支付服务类
// 负责支付匹配、入账、通知和 MQ 事件发布
// 多个入口共享同一份"收到金额 → 匹配订单 → 标记已付 → 入账 → 触发回调 → 发布 MQ"的逻辑
class PaymentService {
public:
    // 处理支付：匹配未支付订单并标记已支付
    // 工作流程：
    //   1. 按 (pay_type, real_amount) 模糊匹配最早一笔 state=0 订单
    //   2. 命中 → 标记 state=1，删除 tmp_price 锁
    //   3. 商户余额入账 (ChannelService::changeMchBalance)
    //   4. 触发商户 notify_url 异步回调 (NotifyTaskService)
    //   5. 发布 MQ "order.paid" 事件；未命中发布 "payment.unmatched"
    // 参数 payType：支付类型（"wxpay"、"alipay"、"qqpay" 等）
    // 参数 price：支付金额（已格式化为 "12.34" 字符串）
    // 返回：匹配到的订单 ID（未匹配返回空字符串）
    static std::string processPayment(const std::string &payType, const std::string &price) {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取当前时间戳
        const long long now = std::time(nullptr);

        // 查询未支付订单
        // 条件：待支付状态、支付类型匹配、金额接近（允许 0.001 元误差）
        auto orders = db.query(
            "SELECT * FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
            "AND (ABS(CAST(real_amount AS REAL) - CAST(? AS REAL)) < 0.001 "
            "OR ABS(CAST(amount AS REAL) - CAST(? AS REAL)) < 0.001) "
            "ORDER BY created_at ASC LIMIT 1",
            {payType, price, price});

        // 如果未找到匹配的订单
        if (orders.empty()) {
            // 发布未匹配事件到 MQ
            publishUnmatched(payType, price, now);
            // 返回空字符串
            return "";
        }

        // 获取第一个匹配的订单
        auto &order = orders[0];
        // 获取订单 ID
        const std::string oid = order["order_id"];

        // 更新订单状态为已支付
        db.exec("UPDATE pay_order SET state=1,pay_time=?,updated_at=? "
                "WHERE order_id=? AND state=0",
                {std::to_string(now), std::to_string(now), oid});
        // 删除临时金额锁定记录
        db.exec("DELETE FROM tmp_price WHERE oid=?", {oid});

        // 商户余额入账
        // 获取商户 ID
        int mchId = 0;
        try {
            mchId = std::stoi(order["mch_id"]);
        } catch (...) {
        }
        // 如果商户 ID 有效
        if (mchId > 0) {
            // 获取订单金额和手续费
            double amount = 0, mchFee = 0;
            try {
                amount = std::stod(order["amount"]);
            } catch (...) {
            }
            try {
                mchFee = std::stod(order["mch_fee_amount"]);
            } catch (...) {
            }
            // 计算商户收入（订单金额 - 手续费）
            const double income = amount - mchFee;
            // 如果收入大于 0，更新商户余额
            if (income > 0) {
                ChannelService::changeMchBalance(mchId, 1, income, "order", oid, "订单收入");
            }
        }

        // 触发商户 notify_url 异步回调
        triggerMerchantNotify(order, mchId, payType, oid);

        // 发布 MQ "order.paid" 事件
        publishOrderPaid(order, mchId, payType, oid, now);

        // 返回订单 ID
        return oid;
    }

    // 发布原始到账金额事件
    // 在入口收到原始金额时调用（匹配前）
    // 参数 source：来源（"device"、"vsign"、"mpay"、"cef" 等）
    // 参数 payType：支付类型
    // 参数 price：金额
    // 参数 extra：额外信息（可选）
    static void publishRawIncoming(const std::string &source,
                                   const std::string &payType,
                                   const std::string &price,
                                   const Json::Value &extra = Json::Value()) {
        // 创建事件对象
        Json::Value ev;
        // 设置来源
        ev["source"]      = source;
        // 设置支付类型
        ev["pay_type"]    = payType;
        // 设置金额
        ev["amount"]      = price;
        // 设置捕获时间
        ev["captured_at"] = (Json::Int64)std::time(nullptr);
        // 如果有额外信息，添加到事件
        if (!extra.isNull())
            ev["extra"] = extra;
        // 发布到 MQ 的 payment.raw 主题
        MqService::instance().publish("payment.raw", Json::FastWriter().write(ev));
    }

    // 发布 CEF 账单原文事件
    // CefBillCtrl 收到 webpay 浏览器抓包的账单 JSON 时调用
    // 参数 url：请求 URL
    // 参数 body：响应体
    static void publishCefBillRaw(const std::string &url, const std::string &body) {
        // 创建事件对象
        Json::Value ev;
        // 设置 URL
        ev["url"]         = url;
        // 设置响应体
        ev["body"]        = body;
        // 设置捕获时间
        ev["captured_at"] = (Json::Int64)std::time(nullptr);
        // 发布到 MQ 的 bill.alipay.raw 主题
        MqService::instance().publish("bill.alipay.raw", Json::FastWriter().write(ev));
    }

    // 发布 Cookie 更新事件
    // CefBillCtrl 收到登录 Cookie 时调用
    // 参数 cookieLen：Cookie 长度
    static void publishCookieUpdated(std::size_t cookieLen) {
        // 创建事件对象
        Json::Value ev;
        // 设置 Cookie 长度
        ev["len"]         = (int)cookieLen;
        // 设置捕获时间
        ev["captured_at"] = (Json::Int64)std::time(nullptr);
        // 发布到 MQ 的 cookie.alipay.updated 主题
        MqService::instance().publish("cookie.alipay.updated", Json::FastWriter().write(ev));
    }

// 私有区域
private:
    // 发布未匹配事件
    // 当支付金额无法匹配任何订单时调用
    // 参数 payType：支付类型
    // 参数 price：支付金额
    // 参数 now：当前时间戳
    static void publishUnmatched(const std::string &payType,
                                 const std::string &price,
                                 long long now) {
        // 创建事件对象
        Json::Value ev;
        // 设置支付类型
        ev["pay_type"]    = payType;
        // 设置金额
        ev["amount"]      = price;
        // 设置捕获时间
        ev["captured_at"] = (Json::Int64)now;
        // 发布到 MQ 的 payment.unmatched 主题
        MqService::instance().publish("payment.unmatched", Json::FastWriter().write(ev));
    }

    // 发布订单已支付事件
    // 订单匹配成功并已入账时调用
    // 参数 order：订单数据
    // 参数 mchId：商户 ID
    // 参数 payType：支付类型
    // 参数 oid：订单 ID
    // 参数 now：当前时间戳
    static void publishOrderPaid(const std::unordered_map<std::string, std::string> &order,
                                 int mchId,
                                 const std::string &payType,
                                 const std::string &oid,
                                 long long now) {
        // 创建事件对象
        Json::Value ev;
        // 设置订单 ID
        ev["order_id"]     = oid;
        // 设置商户 ID
        ev["mch_id"]       = mchId;
        // 设置商户订单号（如果存在）
        ev["mch_order_no"] = order.count("mch_order_no") ? order.at("mch_order_no") : "";
        // 设置支付类型
        ev["pay_type"]     = payType;
        // 设置订单金额（如果存在）
        ev["amount"]       = order.count("amount")      ? order.at("amount")      : "";
        // 设置实际支付金额（如果存在）
        ev["real_amount"]  = order.count("real_amount") ? order.at("real_amount") : "";
        // 设置支付时间
        ev["pay_time"]     = (Json::Int64)now;
        // 如果订单有主题/标题，添加到事件
        if (order.count("subject"))
            ev["subject"]  = order.at("subject");
        // 如果订单有自定义参数，添加到事件
        if (order.count("param") && !order.at("param").empty())
            ev["param"]    = order.at("param");
        // 发布到 MQ 的 order.paid 主题
        MqService::instance().publish("order.paid", Json::FastWriter().write(ev));
    }

    // 触发商户通知回调
    // 向商户的 notify_url 发送支付成功通知
    // 参数 order：订单数据
    // 参数 mchId：商户 ID
    // 参数 payType：支付类型
    // 参数 oid：订单 ID
    static void triggerMerchantNotify(const std::unordered_map<std::string, std::string> &order,
                                      int mchId,
                                      const std::string &payType,
                                      const std::string &oid) {
        // 获取商户通知 URL
        const std::string notifyUrl =
            order.count("notify_url") ? order.at("notify_url") : "";
        // 如果通知 URL 为空或商户 ID 无效，直接返回
        if (notifyUrl.empty() || mchId <= 0)
            return;

        // 获取数据库实例
        auto &db = PayDb::instance();
        // 查询商户密钥
        auto mch = db.queryOne("SELECT mch_key FROM merchant WHERE id=?",
                               {std::to_string(mchId)});
        // 获取商户密钥（如果查询失败则为空）
        const std::string mchKey = mch.empty() ? "" : mch["mch_key"];

        // 构建通知参数
        std::map<std::string, std::string> m;
        // 设置系统订单号
        m["trade_no"]     = oid;
        // 设置商户订单号（如果存在）
        m["out_trade_no"] = order.count("mch_order_no") ? order.at("mch_order_no") : "";
        // 设置支付类型
        m["type"]         = payType;
        // 设置支付金额（如果存在）
        m["money"]        = order.count("amount") ? order.at("amount") : "";
        // 设置交易状态为成功
        m["trade_status"] = "TRADE_SUCCESS";
        // 如果订单有自定义参数，添加到通知
        if (order.count("param") && !order.at("param").empty())
            m["param"] = order.at("param");
        // 如果订单有主题/标题，添加到通知
        if (order.count("subject"))
            m["name"] = order.at("subject");

        // 对参数进行签名
        const std::string signStr = EpaySign::sign(m, mchKey);
        // 添加签名到参数
        m["sign"] = signStr;
        // 设置签名类型为 MD5
        m["sign_type"] = "MD5";

        // 构建查询字符串
        std::string query;
        // 遍历参数，拼接成 URL 查询字符串
        for (auto &[k, v] : m) {
            // 如果不是第一个参数，添加 & 分隔符
            if (!query.empty())
                query += "&";
            // 添加参数
            query += k + "=" + v;
        }
        // 判断 URL 中是否已有查询参数，选择 ? 或 & 作为分隔符
        const std::string sep =
            (notifyUrl.find('?') == std::string::npos) ? "?" : "&";
        // 创建通知任务并发送
        NotifyTaskService::createTaskAndSend(oid, notifyUrl + sep + query, "PaymentService");
    }
// 类定义结束
};
