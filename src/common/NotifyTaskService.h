// WePay-Cpp — 异步通知任务服务
// 支持指数退避重试策略 1m→5m→15m→1hr→1hr
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include <drogon/drogon.h> // Drogon 框架
#include "PayDb.h" // 数据库操作
#include "HttpCaller.h"

// 商户通知任务队列服务
// 负责异步发送商户回调通知，支持指数退避重试策略
// 任务状态：0=PENDING（待发送）、1=SUCCESS（成功）、2=FAIL_MAX_RETRY（失败达到最大重试次数）
// 重试间隔（指数退避）：1分钟 → 5分钟 → 15分钟 → 1小时 → 1小时 → 终止
class NotifyTaskService {
public:
    // 最大重试次数常量
    static constexpr int MAX_RETRY = 5;

    // 创建通知任务并立即触发首次发送
    // 幂等操作：同一 order_id 只建一条任务记录
    // 参数 orderId：订单 ID
    // 参数 notifyFullUrl：完整的回调通知 URL
    // 参数 plugin：插件名称，用于运维面板过滤（如 "WepayV3Plugin"、"VmqPlugin"）
    static void createTaskAndSend(const std::string &orderId,
                                  const std::string &notifyFullUrl,
                                  const std::string &plugin = "") {
        // 检查参数有效性
        if (notifyFullUrl.empty() || orderId.empty())
            return;
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取当前时间戳
        long long now = std::time(nullptr);

        // 检查是否已存在该订单的通知任务
        auto exist = db.queryOne("SELECT id FROM pay_notify_task WHERE order_id=?", {orderId});
        // 如果不存在，创建新任务
        if (exist.empty()) {
            db.exec(
                "INSERT INTO pay_notify_task"
                "(order_id,plugin,notify_full_url,status,retry_cnt,next_retry_at,created_at,updated_at)"
                " VALUES(?,?,?,0,0,?,?,?)",
                {orderId, plugin, notifyFullUrl,
                 std::to_string(now), std::to_string(now), std::to_string(now)});
        }

        // 查询任务信息
        auto rows = db.query(
            "SELECT id,retry_cnt,plugin FROM pay_notify_task WHERE order_id=?", {orderId});
        // 如果查询失败，直接返回
        if (rows.empty())
            return;
        // 获取任务 ID
        std::string taskId = rows[0].at("id");
        // 获取插件名称
        std::string pluginName  = rows[0].count("plugin") ? rows[0].at("plugin") : "";
        // 获取重试次数
        int retryCnt = std::stoi(rows[0].at("retry_cnt"));
        // 发送任务
        sendTask(taskId, orderId, notifyFullUrl, retryCnt, pluginName);
    }

    // 定时器调用：每 30 秒处理到期未成功的任务
    // 由 CronService 定时调用
    static void processPending() {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取当前时间戳
        long long now = std::time(nullptr);

        // 查询所有待发送且已到期的任务（限制 20 条）
        auto tasks = db.query(
            "SELECT id,order_id,plugin,notify_full_url,retry_cnt"
            " FROM pay_notify_task"
            " WHERE status=0 AND next_retry_at<=?"
            " LIMIT 20",
            {std::to_string(now)});

        // 遍历所有待发送任务
        for (auto &t : tasks) {
            // 获取任务 ID
            std::string taskId  = t.at("id");
            // 获取订单 ID
            std::string orderId = t.at("order_id");
            // 获取完整的回调 URL
            std::string fullUrl = t.at("notify_full_url");
            // 获取插件名称
            std::string pluginName = t.count("plugin") ? t.at("plugin") : "";
            // 获取重试次数
            int retryCnt        = std::stoi(t.at("retry_cnt"));

            // 使用乐观锁：先把 next_retry_at 推后 60 秒，避免重复发送
            long long lockUntil = now + 60;
            db.exec(
                "UPDATE pay_notify_task SET next_retry_at=? WHERE id=? AND next_retry_at<=?",
                {std::to_string(lockUntil), taskId, std::to_string(now)});

            // 发送任务
            sendTask(taskId, orderId, fullUrl, retryCnt, pluginName);
        }
    }

    // 手动重试：重置任务状态为 PENDING 并立即触发发送
    // 参数 orderId：订单 ID
    static void retryNow(const std::string &orderId) {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 查询任务信息
        auto rows = db.query(
            "SELECT id,notify_full_url,retry_cnt,plugin FROM pay_notify_task WHERE order_id=?",
            {orderId});
        // 如果查询失败，直接返回
        if (rows.empty())
            return;
        // 获取任务 ID
        std::string taskId  = rows[0].at("id");
        // 获取完整的回调 URL
        std::string fullUrl = rows[0].at("notify_full_url");
        // 获取插件名称
        std::string pluginName = rows[0].count("plugin") ? rows[0].at("plugin") : "";
        // 获取重试次数
        int retryCnt = std::stoi(rows[0].at("retry_cnt"));
        // 重置任务状态为 PENDING，立即重试
        db.exec(
            "UPDATE pay_notify_task SET status=0,next_retry_at=?,updated_at=? WHERE id=?",
            {std::to_string(now), std::to_string(now), taskId});
        // 发送任务
        sendTask(taskId, orderId, fullUrl, retryCnt, pluginName);
    }

    // 更新通知 URL
    // 用于修改回调地址，同时重置任务状态为待发送
    // 参数 orderId：订单 ID
    // 参数 newUrl：新的回调 URL
    static void updateUrl(const std::string &orderId, const std::string &newUrl) {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 更新通知 URL，重置状态为 PENDING，立即重试
        db.exec(
            "UPDATE pay_notify_task SET notify_full_url=?,status=0,next_retry_at=0,updated_at=?"
            " WHERE order_id=?",
            {newUrl, std::to_string(std::time(nullptr)), orderId});
    }

// 私有区域
private:
    // 计算指数退避延迟时间（秒）
    // 根据重试次数返回下次重试的延迟时间
    // 参数 retryCnt：当前重试次数
    // 返回：延迟时间（秒）
    static long long backoffSeconds(int retryCnt) {
        // 根据重试次数选择延迟时间
        switch (retryCnt) {
            // 第 0 次重试：延迟 1 分钟
            case 0:
                return 60;
            // 第 1 次重试：延迟 5 分钟
            case 1:
                return 300;
            // 第 2 次重试：延迟 15 分钟
            case 2:
                return 900;
            // 第 3 次及以后重试：延迟 1 小时
            case 3:
                return 3600;
            // 默认：延迟 1 小时
            default:
                return 3600;
        }
    }

    // 发送通知任务
    // 异步发送商户回调通知，并根据响应结果更新任务状态
    // 参数 taskId：任务 ID
    // 参数 orderId：订单 ID
    // 参数 fullUrl：完整的回调 URL
    // 参数 retryCnt：当前重试次数
    // 参数 pluginName：插件名称
    static void sendTask(const std::string &taskId,
                         const std::string &orderId,
                         const std::string &fullUrl,
                         int retryCnt,
                         const std::string &pluginName = "") {
        // 判断是否为 POST 请求（根据 URL 中是否包含 /mpayNotify）
        bool isPost = fullUrl.find("/mpayNotify") != std::string::npos;

        // 定义异步回调函数，处理 HTTP 响应
        auto onResult = [taskId, orderId, retryCnt, pluginName, fullUrl](bool ok, int httpStatus, const std::string &body) {
            // 获取数据库实例
            auto &db = PayDb::instance();
            // 获取当前时间戳
            long long now = std::time(nullptr);
            // 记录回调日志到 pay_callback_log 表
            db.exec(
                "INSERT INTO pay_callback_log(order_id,plugin,notify_url,http_status,response,success,created_at)"
                " VALUES(?,?,?,?,?,?,?)",
                {orderId, pluginName, fullUrl, std::to_string(httpStatus),
                 body.substr(0, 1000), ok && body.find("success") != std::string::npos ? "1" : "0",
                 std::to_string(now)});

            // 判断业务是否成功
            // 条件：HTTP 请求成功 AND (响应包含 "success" OR HTTP 200 且响应体很短)
            bool businessOk = ok && (body.find("success") != std::string::npos
                                     || (httpStatus == 200 && body.size() < 5));
            // 如果业务成功
            if (businessOk) {
                // 更新任务状态为 SUCCESS，记录最后响应
                db.exec(
                    "UPDATE pay_notify_task SET status=1,last_response=?,updated_at=? WHERE id=?",
                    {body.substr(0, 500), std::to_string(now), taskId});
            } else {
                // 业务失败，增加重试次数
                int newRetry = retryCnt + 1;
                // 如果已达到最大重试次数
                if (newRetry >= MAX_RETRY) {
                    // 更新任务状态为 FAIL_MAX_RETRY，标记为失败
                    db.exec(
                        "UPDATE pay_notify_task SET status=2,retry_cnt=?,last_response=?,updated_at=? WHERE id=?",
                        {std::to_string(newRetry), body.substr(0, 500),
                         std::to_string(now), taskId});
                } else {
                    // 还有重试机会，计算下次重试时间
                    long long next = now + backoffSeconds(newRetry);
                    // 更新任务状态为 PENDING，设置下次重试时间
                    db.exec(
                        "UPDATE pay_notify_task SET status=0,retry_cnt=?,next_retry_at=?,"
                        "last_response=?,updated_at=? WHERE id=?",
                        {std::to_string(newRetry), std::to_string(next),
                         body.substr(0, 500), std::to_string(now), taskId});
                }
            }
        };

        // 根据请求类型发送异步请求
        if (isPost) {
            // 发送 POST 请求（用于 mpayNotify）
            HttpCaller::asyncPost(fullUrl, "", "application/json", onResult);
        } else {
            // 发送 GET 请求
            HttpCaller::asyncGet(fullUrl, onResult);
        }
    }
// 类定义结束
};
