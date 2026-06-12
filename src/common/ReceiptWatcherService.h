// WePay-Cpp — 网页流水监听服务
//
// 1. HTTP API 供 Python receipt_watcher 拉取账号+订单
//    GET  /receipt-watcher/accounts
//    GET  /receipt-watcher/orders/{accountKey}
//    POST /receipt-watcher/status
// 2. MQ 消费: receipt_flow_notify → 自动匹配订单
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h>
#include <json/json.h>
#include <string>
#include <sstream>
#include <ctime>
#include <iostream>
#include "PayDb.h"
#include "MqService.h"
#include "PaymentService.h"
#include "../channel/ReceiptPlugin.h"

// 网页流水监听服务类
// 职责：
//   1. HTTP API 供 Python receipt_watcher 拉取账号+订单
//   2. MQ 消费: receipt_flow_notify → 自动匹配订单
//   3. 流水日志记录和订单匹配
class ReceiptWatcherService : public drogon::HttpController<ReceiptWatcherService> {
public:
    // 注册 HTTP 路由
    METHOD_LIST_BEGIN
        // GET /receipt-watcher/accounts - 获取所有监听账号
        ADD_METHOD_TO(ReceiptWatcherService::getAccounts,
                      "/receipt-watcher/accounts", drogon::Get);
        // GET /receipt-watcher/orders/{accountKey} - 获取待付订单列表
        ADD_METHOD_TO(ReceiptWatcherService::getOrders,
                      "/receipt-watcher/orders/{1}", drogon::Get);
        // POST /receipt-watcher/status - 报告监听状态
        ADD_METHOD_TO(ReceiptWatcherService::reportStatus,
                      "/receipt-watcher/status", drogon::Post);
    METHOD_LIST_END

    // ── 初始化(main.cc 调用) ────────────────────────────────
    // 初始化网页流水监听服务
    // 创建 API Key 配置、订阅 MQ 消息、启动监听
    static void setup() {
        // 获取数据库实例
        auto &db = PayDb::instance();

        // 初始化 API Key 配置（如果不存在）
        if (db.getSetting("receipt_watcher_api_key").empty())
            db.setSetting("receipt_watcher_api_key", "");

        // ── 订阅 MQ 消息 ──────────────────────────────
        // 如果 MQ 服务已启用
        if (MqService::instance().enabled()) {
            // 订阅 receipt_flow_notify 主题
            // Python watcher 推过来的流水消息
            MqService::instance().subscribe("receipt_flow_notify",
                [](const std::string &, const std::string &body) {
                    // 处理流水消息
                    handleFlowMessage(body);
                });
            // 记录消费者注册日志
            std::cout << "[ReceiptWatcher] MQ 消费者已注册" << std::endl;
        }

        // 记录服务启动日志
        std::cout << "[ReceiptWatcher] 服务已启动" << std::endl;
    }

    // ── GET /receipt-watcher/accounts ────────────────────────
    // 获取所有网页流水监听账号
    // 返回按 plugin_api_config_id 聚合的账号列表
    void getAccounts(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // 检查 API Key
        if (!checkKey(req)) {
            deny(cb);
            return;
        }

        // 获取数据库实例
        auto &db = PayDb::instance();
        // 查询所有启用的 receipt_monitor 通道
        auto channels = db.query(
            "SELECT id,channel_code,channel_name,plugin,pay_type,"
            "params_json,COALESCE(api_config_id,0) AS api_config_id "
            "FROM pay_channel WHERE state=1 AND plugin='receipt_monitor' "
            "ORDER BY id", {});

        // ── 按 plugin_api_config_id 聚合账号 ──────────────────────────────
        // 账号字典（key = plugin_api_config_id）
        std::map<std::string, Json::Value> accs;
        // 遍历所有通道
        for (auto &ch : channels) {
            // 获取 API 配置 ID
            std::string cfgId = ch.count("api_config_id") ? ch["api_config_id"] : "0";
            // 生成账号 key
            std::string key = ch["plugin"] + "_" + cfgId;

            // 如果账号不存在，创建新账号
            if (!accs.count(key)) {
                // 创建账号对象
                Json::Value a;
                // 设置账号 key
                a["account_key"] = key;
                // 设置插件代码
                a["plugin_code"] = ch["plugin"];
                // 设置 API 配置 ID
                a["api_config_id"] = std::atoi(cfgId.c_str());

                // ── 解析通道参数 ──────────────────────────────
                // 通道参数 JSON
                Json::Value cfg;
                // 如果有参数 JSON
                if (!ch["params_json"].empty()) {
                    // 创建 JSON 读取器
                    Json::CharReaderBuilder rb;
                    // 创建字符串流
                    std::istringstream ss(ch["params_json"]);
                    // 错误信息
                    std::string e;
                    // 解析 JSON
                    Json::parseFromStream(rb, ss, &cfg, &e);
                }
                // 设置配置
                a["config"] = cfg;
                // 设置查询间隔（秒）
                a["query_interval_seconds"] =
                    cfg.get("receipt_watcher_query_interval_seconds", 5).asInt();
                // 初始化通道列表
                a["channels"] = Json::Value(Json::arrayValue);
                // 添加到账号字典
                accs[key] = a;
            }

            // ── 添加通道信息 ──────────────────────────────
            // 创建通道信息对象
            Json::Value ci;
            // 设置通道 ID
            ci["channel_id"] = std::atoi(ch["id"].c_str());
            // 设置通道代码
            ci["channel_code"] = ch["channel_code"];
            // 设置支付类型
            ci["pay_type"] = ch["pay_type"];
            // 添加到通道列表
            accs[key]["channels"].append(ci);
        }

        // ── 构建响应 ──────────────────────────────
        // 创建响应对象
        Json::Value result;
        // 设置响应码
        result["code"] = 0;
        // 创建数组
        Json::Value arr(Json::arrayValue);
        // 遍历所有账号
        for (auto &[_, a] : accs)
            // 添加到数组
            arr.append(a);
        // 设置数据
        result["data"] = arr;
        // 返回 JSON 响应
        cb(drogon::HttpResponse::newHttpJsonResponse(result));
    }

    // ── GET /receipt-watcher/orders/{accountKey} ─────────────
    // 获取待付订单列表
    // 参数 accountKey：账号 key（plugin_api_config_id）
    // 返回：待付订单列表（最近 N 分钟内创建的订单）
    void getOrders(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                   std::string accountKey) {
        // 检查 API Key
        if (!checkKey(req)) {
            deny(cb);
            return;
        }

        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取订单关闭时间（分钟）
        int closeMin = 10;
        // 尝试从配置读取
        try {
            closeMin = std::stoi(db.getSetting("close_minutes", "5"));
        } catch (...) {
        }
        // 计算截止时间戳（N 分钟前）
        long long cutoff = std::time(nullptr) - closeMin * 60;

        // ── 查询待付订单 ──────────────────────────────
        // 查询所有 receipt_monitor 通道的待付订单
        auto rows = db.query(
            "SELECT o.order_id,o.pay_type,o.real_amount,o.amount,"
            "o.subject,o.created_at,o.expire_time,o.ext_json "
            "FROM pay_order o "
            "JOIN pay_channel c ON c.id=CAST(o.channel_id AS INTEGER) "
            "WHERE o.state=0 AND c.plugin='receipt_monitor' "
            "AND o.created_at>? "
            "ORDER BY o.created_at ASC LIMIT 50",
            {std::to_string(cutoff)});

        // ── 构建响应 ──────────────────────────────
        // 创建响应对象
        Json::Value result;
        // 设置响应码
        result["code"] = 0;
        // 创建数组
        Json::Value arr(Json::arrayValue);
        // 遍历所有订单
        for (auto &r : rows) {
            // 创建订单项
            Json::Value item;
            // 设置订单 ID
            item["order_id"] = r["order_id"];
            // 设置支付类型
            item["pay_type"] = r["pay_type"];
            // 设置实际金额
            item["real_amount"] = r["real_amount"];
            // 设置订单金额
            item["amount"] = r["amount"];
            // 设置订单主题
            item["subject"] = r.count("subject") ? r["subject"] : "";
            // 设置创建时间
            item["created_at"] = r["created_at"];
            // 添加到数组
            arr.append(item);
        }
        // 设置数据
        result["data"] = arr;
        // 返回 JSON 响应
        cb(drogon::HttpResponse::newHttpJsonResponse(result));
    }

    // ── POST /receipt-watcher/status ─────────────────────────
    // 报告监听状态
    // 接收 Python watcher 的状态报告
    void reportStatus(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // 检查 API Key
        if (!checkKey(req)) {
            deny(cb);
            return;
        }

        // 获取请求 JSON 对象
        auto body = req->getJsonObject();
        // 如果 JSON 对象存在
        if (body) {
            // 获取账号 key
            std::string key = (*body).get("account_key", "").asString();
            // 获取状态字符串
            std::string status = (*body).get("status", "").asString();
            // 如果 key 不为空
            if (!key.empty()) {
                // 保存状态到数据库配置
                PayDb::instance().setSetting(
                    "rw_status_" + key, status);
            }
        }

        // ── 构建响应 ──────────────────────────────
        // 创建响应对象
        Json::Value r;
        // 设置响应码
        r["code"] = 0;
        // 设置响应消息
        r["msg"] = "ok";
        // 返回 JSON 响应
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    //  MQ 消费: 处理 Python 推过来的归一化流水
    // ══════════════════════════════════════════════════════════

    // 处理流水消息
    // 参数 body：MQ 消息体（JSON 格式）
    // 功能：
    //   1. 解析流水消息
    //   2. 检查幂等性（是否已处理）
    //   3. 记录流水日志
    //   4. 匹配订单
    //   5. 更新流水状态
    static void handleFlowMessage(const std::string &body) {
        // ── 解析 JSON 消息 ──────────────────────────────
        // 创建 JSON 对象
        Json::Value msg;
        // 创建 JSON 读取器
        Json::CharReaderBuilder rb;
        // 创建字符串流
        std::istringstream ss(body);
        // 错误信息
        std::string errs;
        // 解析 JSON
        if (!Json::parseFromStream(rb, ss, &msg, &errs)) {
            // 记录解析失败日志
            std::cerr << "[ReceiptWatcher] JSON 解析失败: " << errs << std::endl;
            // 返回
            return;
        }

        // ── 提取流水信息 ──────────────────────────────
        // 获取流水记录
        Json::Value record = msg.get("record", Json::Value());
        // 如果记录为空，返回
        if (record.isNull())
            return;

        // 获取订单号
        std::string orderNo = record.get("order_no", "").asString();
        // 获取金额
        std::string price   = record.get("price", "").asString();
        // 获取支付类型
        std::string payType = record.get("pay_type", "alipay").asString();

        // 如果订单号或金额为空，返回
        if (orderNo.empty() || price.empty())
            return;

        // ── 检查幂等性 ──────────────────────────────
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 查询流水日志中是否已存在
        auto seen = db.queryOne(
            "SELECT id FROM receipt_flow_log WHERE flow_no=?", {orderNo});
        // 如果已存在，返回（幂等）
        if (!seen.empty())
            return;

        // ── 记录流水日志 ──────────────────────────────
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 插入流水日志（初始状态为 0=待匹配）
        db.exec(
            "INSERT INTO receipt_flow_log(flow_no,amount,pay_type,raw_json,status,created_at) "
            "VALUES(?,?,?,?,0,?)",
            {orderNo, price, payType, body, std::to_string(now)});

        // ── 匹配订单 ──────────────────────────────
        // 调用 ReceiptPlugin 匹配订单
        std::string matchedOid = ReceiptPlugin::matchByAmount(payType, price, orderNo);

        // 如果匹配成功
        if (!matchedOid.empty()) {
            // 更新流水日志为已匹配（状态为 1）
            db.exec("UPDATE receipt_flow_log SET status=1,matched_order=?,updated_at=? "
                    "WHERE flow_no=?",
                    {matchedOid, std::to_string(now), orderNo});

            // 记录匹配成功日志
            std::cout << "[ReceiptWatcher] 匹配成功 flow=" << orderNo
                      << " → order=" << matchedOid
                      << " amount=" << price << std::endl;
        } else {
            // 更新流水日志为未匹配（状态为 2）
            db.exec("UPDATE receipt_flow_log SET status=2,updated_at=? WHERE flow_no=?",
                    {std::to_string(now), orderNo});

            // 记录匹配失败日志
            std::cerr << "[ReceiptWatcher] 未匹配 flow=" << orderNo
                      << " amount=" << price << " type=" << payType << std::endl;
        }
    }

// 私有区域
private:
    // 检查 API Key
    // 参数 req：HTTP 请求对象
    // 返回：API Key 是否有效
    // 说明：
    //   1. 从数据库读取配置的 API Key
    //   2. 如果未配置，则放行（返回 true）
    //   3. 从请求头或参数中获取 API Key
    //   4. 比较是否相等
    static bool checkKey(const drogon::HttpRequestPtr &req) {
        // 从数据库读取配置的 API Key
        std::string cfgKey = PayDb::instance().getSetting("receipt_watcher_api_key");
        // 如果未配置密钥，则放行
        if (cfgKey.empty())
            return true;
        // 从请求头中获取 API Key
        std::string reqKey = req->getHeader("X-Watcher-Key");
        // 如果请求头中没有，从参数中获取
        if (reqKey.empty())
            reqKey = req->getParameter("api_key");
        // 比较是否相等
        return reqKey == cfgKey;
    }

    // 返回 API Key 无效错误
    // 参数 cb：响应回调函数
    static void deny(std::function<void(const drogon::HttpResponsePtr &)> &cb) {
        // 创建错误响应对象
        Json::Value r;
        // 设置错误码
        r["code"] = -1;
        // 设置错误消息
        r["msg"] = "API Key invalid";
        // 创建 JSON 响应
        auto resp = drogon::HttpResponse::newHttpJsonResponse(r);
        // 设置 HTTP 状态码为 401 Unauthorized
        resp->setStatusCode(drogon::k401Unauthorized);
        // 返回响应
        cb(resp);
    }
};
