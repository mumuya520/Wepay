// WePay-Cpp — 飞鹅云打印任务服务
// 飞鹅云打印任务队列服务
// state: 0=排队中, 1=打印成功, -1=打印失败
// 失败重试间隔（指数退避）：30s → 2min → 5min → 15min → 终止
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include <drogon/drogon.h> // Drogon 框架
#include <drogon/HttpClient.h> // Drogon HTTP 客户端
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>
#include "../common/PayDb.h"

// 飞鹅云打印任务服务类
// 职责：
//   1. 管理打印任务队列（state: 0=排队中, 1=打印成功, -1=打印失败）
//   2. 实现失败重试机制（指数退避：30s → 2min → 5min → 15min → 终止）
//   3. 与飞鹅云 API 集成（HMAC-SHA256 签名、HTTP 请求）
class PrintTaskService {
public:
    // 最大重试次数
    static constexpr int MAX_RETRY = 5;

    // 定时器调用：每 30s 处理失败任务的重试
    // 功能：
    //   1. 查询失败任务（state=-1）并重新发送
    //   2. 查询超时任务（state=0 且超过 5 分钟）并重新发送
    static void processPending() {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取当前时间戳
        long long now = std::time(nullptr);

        // ── 第 1 步：处理失败任务（state=-1）──────────────────────────────
        // 查询失败且未超限的任务（最多 20 条）
        auto tasks = db.query(
            "SELECT id,task_id,printer_sn,printer_key,content,copies,order_id,retry_cnt"
            " FROM print_task"
            " WHERE state=-1 AND retry_cnt<?"
            " ORDER BY updated_at ASC"
            " LIMIT 20",
            {std::to_string(MAX_RETRY)});

        // 遍历所有失败任务
        for (auto &t : tasks) {
            // 获取任务 ID
            std::string taskId   = t.at("task_id");
            // 获取打印机序列号
            std::string printerSn = t.at("printer_sn");
            // 获取打印机密钥
            std::string printerKey = t.at("printer_key");
            // 获取打印内容
            std::string content   = t.at("content");
            // 获取打印份数
            int copies            = std::stoi(t.at("copies"));
            // 获取订单 ID
            std::string orderId   = t.at("order_id");
            // 获取重试次数
            int retryCnt          = std::stoi(t.at("retry_cnt"));

            // ── 乐观锁：防止并发重复重试 ──────────────────────────────
            // 将任务状态改为 0（排队中）
            db.exec(
                "UPDATE print_task SET state=0,updated_at=? WHERE task_id=? AND state=-1",
                {std::to_string(now), taskId});

            // 发送到飞鹅云
            sendToFeie(taskId, printerSn, printerKey, content, copies, orderId, retryCnt);
        }

        // ── 第 2 步：处理超时任务（state=0 但超过 5 分钟）──────────────────────────────
        // 计算 5 分钟前的时间戳
        long long staleThreshold = now - 300;
        // 查询超时且未超限的任务（最多 10 条）
        auto staleTasks = db.query(
            "SELECT id,task_id,printer_sn,printer_key,content,copies,order_id,retry_cnt"
            " FROM print_task"
            " WHERE state=0 AND updated_at<? AND retry_cnt<?"
            " ORDER BY updated_at ASC"
            " LIMIT 10",
            {std::to_string(staleThreshold), std::to_string(MAX_RETRY)});

        // 遍历所有超时任务
        for (auto &t : staleTasks) {
            // 获取任务 ID
            std::string taskId   = t.at("task_id");
            // 获取打印机序列号
            std::string printerSn = t.at("printer_sn");
            // 获取打印机密钥
            std::string printerKey = t.at("printer_key");
            // 获取打印内容
            std::string content   = t.at("content");
            // 获取打印份数
            int copies            = std::stoi(t.at("copies"));
            // 获取订单 ID
            std::string orderId   = t.at("order_id");
            // 获取重试次数
            int retryCnt          = std::stoi(t.at("retry_cnt"));
            // 发送到飞鹅云
            sendToFeie(taskId, printerSn, printerKey, content, copies, orderId, retryCnt);
        }
    }

// 私有区域
private:
    // 计算指数退避重试间隔（秒）
    // 参数 retryCnt：当前重试次数（0-based）
    // 返回：下次重试的等待秒数
    // 重试间隔：30s → 2min → 5min → 15min
    static long long backoffSeconds(int retryCnt) {
        // 根据重试次数返回不同的等待时间
        switch (retryCnt) {
            // 第 1 次重试：等待 30 秒
            case 0:
                return 30;
            // 第 2 次重试：等待 2 分钟
            case 1:
                return 120;
            // 第 3 次重试：等待 5 分钟
            case 2:
                return 300;
            // 第 4 次及以后：等待 15 分钟
            default:
                return 900;
        }
    }

    // 计算 HMAC-SHA256 签名（十六进制格式）
    // 参数 key：密钥
    // 参数 data：待签名数据
    // 返回：HMAC-SHA256 签名（十六进制字符串）
    static std::string hmacSha256Hex(const std::string &key, const std::string &data) {
        // 创建 SHA256 哈希缓冲区
        unsigned char hash[SHA256_DIGEST_LENGTH];
        // 计算 HMAC-SHA256
        HMAC(EVP_sha256(), key.data(), key.size(),
             (unsigned char*)data.data(), data.size(), hash, nullptr);
        // 创建输出流
        std::ostringstream oss;
        // 将哈希值转换为十六进制字符串
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        // 返回十六进制字符串
        return oss.str();
    }

    // URL 编码
    // 参数 s：待编码字符串
    // 返回：URL 编码后的字符串
    // 规则：字母数字和 -_.~ 保留，其他字符转换为 %XX 格式
    static std::string urlEncode(const std::string &s) {
        // 十六进制字符表
        static const char *hex = "0123456789ABCDEF";
        // 结果字符串
        std::string r;
        // 遍历每个字符
        for (unsigned char c : s) {
            // 如果是保留字符，直接添加
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                r += c;
            } else {
                // 否则转换为 %XX 格式
                r += '%';
                // 高 4 位
                r += hex[(c >> 4) & 0x0F];
                // 低 4 位
                r += hex[c & 0x0F];
            }
        }
        // 返回编码后的字符串
        return r;
    }

    // 飞鹅云 XML 转义
    // 参数 s：待转义字符串
    // 返回：转义后的字符串
    // 转义规则：< → &lt;, > → &gt;, & → &amp;
    static std::string escapeFeie(const std::string &s) {
        // 结果字符串
        std::string r;
        // 遍历每个字符
        for (char c : s) {
            // 转义 <
            if (c == '<')
                r += "&lt;";
            // 转义 >
            else if (c == '>')
                r += "&gt;";
            // 转义 &
            else if (c == '&')
                r += "&amp;";
            // 其他字符直接添加
            else
                r += c;
        }
        // 返回转义后的字符串
        return r;
    }

    // 发送打印任务到飞鹅云
    // 参数 taskId：任务 ID
    // 参数 printerSn：打印机序列号
    // 参数 printerKey：打印机密钥
    // 参数 content：打印内容
    // 参数 copies：打印份数
    // 参数 orderId：订单 ID
    // 参数 retryCnt：当前重试次数
    static void sendToFeie(const std::string &taskId,
                           const std::string &printerSn,
                           const std::string &printerKey,
                           const std::string &content,
                           int copies,
                           const std::string &orderId,
                           int retryCnt) {
        // ── 获取配置 ──────────────────────────────
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取当前时间戳
        long long now = std::time(nullptr);

        // 获取飞鹅云 API URL
        std::string apiUrl = db.getSetting("feie_api_url", "http://api.feieyun.com/Api/Order");
        // 获取飞鹅云用户名
        std::string feieUser = db.getSetting("feie_user");
        // 获取时间戳字符串
        std::string stime = std::to_string(now);

        // ── 计算签名 ──────────────────────────────
        // 飞鹅云签名公式：sig = HMAC-SHA256(UKEY, user+sn+UKEY+stime)
        // 构建签名数据
        std::string sigData = feieUser + printerSn + printerKey + stime;
        // 计算 HMAC-SHA256 签名
        std::string sig = hmacSha256Hex(printerKey, sigData);

        // ── 构建打印内容 ──────────────────────────────
        // 创建输出流
        std::ostringstream contentoss;
        // 构建飞鹅云格式的打印内容：<script>sz=份数|sj=内容;</script>
        contentoss << "<script>sz=" << copies << "|sj=" << escapeFeie(content) << ";</script>";

        // ── 构建 POST 请求数据 ──────────────────────────────
        // 构建 POST 数据
        std::string postData =
            // 用户名
            "user=" + urlEncode(feieUser) +
            // 时间戳
            "&stime=" + stime +
            // 签名
            "&sig=" + sig +
            // API 名称
            "&apiname=PrintSchoolNotice" +
            // 打印机序列号
            "&sn=" + printerSn +
            // 打印内容
            "&content=" + urlEncode(contentoss.str()) +
            // 打印份数
            "&times=" + std::to_string(copies);

        // ── 创建 HTTP 请求 ──────────────────────────────
        // 创建 HTTP 客户端
        auto client = drogon::HttpClient::newHttpClient(apiUrl);
        // 创建 HTTP 请求
        auto req = drogon::HttpRequest::newHttpRequest();
        // 设置请求方法为 POST
        req->setMethod(drogon::Post);
        // 设置请求路径
        req->setPath("/");
        // 设置 Content-Type
        req->addHeader("Content-Type", "application/x-www-form-urlencoded");
        // 设置请求体
        req->setBody(postData);

        // ── 发送异步请求 ──────────────────────────────
        // 保存任务 ID（用于异步回调）
        std::string savedTaskId = taskId;
        // 发送请求并设置回调
        client->sendRequest(req,
            [savedTaskId, orderId, retryCnt](drogon::ReqResult result,
                                               const drogon::HttpResponsePtr &resp) {
                // 获取数据库实例
                auto &db2 = PayDb::instance();
                // 获取当前时间戳
                long long ts = std::time(nullptr);
                // 请求是否成功
                bool ok = false;
                // 错误消息
                std::string errMsg;

                // ── 检查响应 ──────────────────────────────
                // 如果 HTTP 请求成功且有响应
                if (result == drogon::ReqResult::Ok && resp) {
                    // 获取响应体
                    auto bodyView = resp->getBody();
                    // 转换为字符串
                    std::string bodyStr(bodyView.begin(), bodyView.end());
                    // 检查是否包含 "ok":true
                    if (bodyStr.find("\"ok\":true") != std::string::npos ||
                        bodyStr.find("\"ok\": true") != std::string::npos) {
                        // 标记为成功
                        ok = true;
                    } else {
                        // 提取错误消息（最多 256 字符）
                        errMsg = bodyStr.substr(0, 256);
                    }
                } else {
                    // 记录 HTTP 错误
                    errMsg = std::string("HTTP ") + std::to_string((int)result);
                }

                // ── 更新任务状态 ──────────────────────────────
                // 如果请求成功
                if (ok) {
                    // 更新任务为成功状态
                    db2.exec(
                        "UPDATE print_task SET state=1,retry_cnt=?,updated_at=? WHERE task_id=?",
                        {std::to_string(retryCnt), std::to_string(ts), savedTaskId});
                } else {
                    // 计算新的重试次数
                    int newRetry = retryCnt + 1;
                    // 计算下次重试时间
                    long long nextRetry = ts + backoffSeconds(retryCnt);
                    // 如果超过最大重试次数
                    if (newRetry >= MAX_RETRY) {
                        // 更新任务为失败状态（不再重试）
                        db2.exec(
                            "UPDATE print_task SET state=-1,retry_cnt=?,error_msg=?,updated_at=? WHERE task_id=?",
                            {std::to_string(newRetry), errMsg, std::to_string(ts), savedTaskId});
                    } else {
                        // 更新任务为失败状态（等待重试）
                        db2.exec(
                            "UPDATE print_task SET state=-1,retry_cnt=?,error_msg=?,updated_at=? WHERE task_id=?",
                            {std::to_string(newRetry), errMsg, std::to_string(ts), savedTaskId});
                    }
                }
            });
    }
};
