// WePay-Cpp — webpay (CEF) 浏览器抓包推送接收端
//
// 配套外部进程: cef_binary/tests/webpay/webpay.exe
//   - 登录 Cookie         POST /notify/cef/cookie  (Content-Type: text/plain)
//   - 账单接口原始 JSON   POST /notify/cef/bill    (Content-Type: application/json)
//   - 登录过期通知       POST /notify/cef/expired (Content-Type: application/json)
//
// 行为:
//   1) 收到账单 → publish "bill.alipay.raw"
//   2) 解析账单 JSON 抽 (amount, payType) 列表 → 逐条调
//      PaymentService::processPayment 自动匹配未支付订单 + 入账 + 商户回调 + publish "order.paid"
//   3) 收到 Cookie → 落 setting + publish "cookie.alipay.updated"
//   4) 收到过期通知 → 设备离线 + 清除 Cookie + 发送告警

#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <string> // 字符串库
#include <utility> // 工具库
#include <vector> // 向量容器
#include <json/json.h> // JSON 库

#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/MqService.h" // 消息队列服务
#include "../common/PayDb.h" // 数据库操作
#include "../common/PaymentService.h" // 支付服务

// CEF 浏览器账单接收控制器类
class CefBillCtrl : public drogon::HttpController<CefBillCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(CefBillCtrl::receiveBill,    "/notify/cef/bill",    drogon::Post); // 账单接收路由
        ADD_METHOD_TO(CefBillCtrl::receiveCookie,  "/notify/cef/cookie",  drogon::Post); // Cookie 接收路由
        ADD_METHOD_TO(CefBillCtrl::receiveExpired, "/notify/cef/expired", drogon::Post); // 过期通知路由
    METHOD_LIST_END // 路由列表结束

    // ═══════════════════════════════════════════════════════════════
    // POST /notify/cef/bill — 接收 CEF 浏览器账单
    // ═══════════════════════════════════════════════════════════════
    // body: {"url":"https://...","body":"<原始账单 JSON, 已剥 JSONP>"}
    void receiveBill(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb)
    {
        auto root = req->getJsonObject(); // 解析 JSON 请求体
        if (!root) { RESP_ERR(cb, "body 不是合法 JSON"); return; } // 检查请求体

        const std::string sourceUrl = (*root).get("url",  "").asString(); // 获取来源 URL
        const std::string rawBody   = (*root).get("body", "").asString(); // 获取原始账单 JSON

        // 1. 发 "bill.alipay.raw" 供审计 / 风控订阅
        PaymentService::publishCefBillRaw(sourceUrl, rawBody); // 发布原始账单事件

        // 2. 解析账单条目 → 调 PaymentService::processPayment 自动匹配
        std::vector<std::pair<std::string, std::string>> entries; // 账单条目列表
        Json::Value matched(Json::arrayValue); // 匹配的订单列表
        std::string matchedOidsStr; // 匹配的订单号字符串

        if (extractEntries(rawBody, entries)) { // 解析账单条目
            for (auto &[payType, amount] : entries) { // 遍历每个条目
                Json::Value extra; extra["url"] = sourceUrl; // 添加来源 URL
                PaymentService::publishRawIncoming("cef", payType, amount, extra); // 发布原始收入事件
                std::string oid = PaymentService::processPayment(payType, amount); // 处理支付
                if (!oid.empty()) { // 如果匹配到订单
                    matched.append(oid); // 添加到匹配列表
                    if (!matchedOidsStr.empty()) matchedOidsStr += ","; // 添加分隔符
                    matchedOidsStr += oid; // 添加订单号
                }
            }
        }

        // 3. 账单原文落库, 便于事后重放 / 对账 (cef_bill_log 由 DatabaseInit v400 创建)
        const long long now = std::time(nullptr); // 获取当前时间
        auto &db = PayDb::instance(); // 获取数据库实例
        db.exec("INSERT INTO cef_bill_log(captured_at,source_url,raw_body,"
                "entries_cnt,matched_oids,created_at) VALUES(?,?,?,?,?,?)",
                {std::to_string(now), sourceUrl, rawBody,
                 std::to_string((int)entries.size()), matchedOidsStr,
                 std::to_string(now)}); // 保存账单日志

        // 4. 更新 CEF "设备"的 last_pay / last_heart (device 行由 v402 预注册)
        db.exec("UPDATE device SET last_heart=?,last_pay=?,updated_at=?,state=1 "
                "WHERE device_no='cef_alipay_001'",
                {std::to_string(now), std::to_string(now), std::to_string(now)}); // 更新设备心跳

        Json::Value data; // 创建响应
        data["entries_count"] = (int)entries.size(); // 条目数
        data["matched"]       = matched; // 匹配的订单
        RESP_OK(cb, data); // 返回成功
    }

    // ═══════════════════════════════════════════════════════════════
    // POST /notify/cef/expired — 接收登录过期通知
    // ═══════════════════════════════════════════════════════════════
    // body: {"url":"https://..."}  浏览器检测到登录已过期时 POST 过来
    // 行为:
    //   1) 发 MQ "cookie.alipay.expired"  (订阅方: 发送告警 / 切流)
    //   2) 把 cef_alipay_001 设备 state 置 0 (离线), 方便管理员在后台看到红点
    //   3) 清掉 setting.cef_alipay_cookie, 强制下次登录后重新灌入
    void receiveExpired(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb)
    {
        std::string sourceUrl; // 初始化来源 URL
        if (auto root = req->getJsonObject()) { // 解析 JSON 请求体
            sourceUrl = (*root).get("url", "").asString(); // 获取来源 URL
        }

        auto &db = PayDb::instance(); // 获取数据库实例
        const long long now = std::time(nullptr); // 获取当前时间

        // 1. 设备置离线
        db.exec("UPDATE device SET state=0,updated_at=? WHERE device_no='cef_alipay_001'",
                {std::to_string(now)}); // 设置设备为离线

        // 2. 清除过期 Cookie
        db.setSetting("cef_alipay_cookie", ""); // 清除 Cookie
        db.setSetting("cef_alipay_cookie_expired_at", std::to_string(now)); // 记录过期时间

        // 3. 发 MQ 告警事件
        Json::Value ev; // 创建事件对象
        ev["source_url"]  = sourceUrl; // 来源 URL
        ev["captured_at"] = (Json::Int64)now; // 捕获时间
        MqService::instance().publish("cookie.alipay.expired",
                                      Json::FastWriter().write(ev)); // 发布过期事件

        LOG_WARN << "[CefBillCtrl] 支付宝登录态过期, url=" << sourceUrl; // 记录警告日志

        Json::Value data; data["acknowledged"] = true; // 创建响应
        RESP_OK(cb, data); // 返回成功
    }

    // ═══════════════════════════════════════════════════════════════
    // POST /notify/cef/cookie — 接收登录 Cookie
    // ═══════════════════════════════════════════════════════════════
    // body: 整段 Cookie 字符串 "k1=v1; k2=v2; ..."
    void receiveCookie(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb)
    {
        std::string cookie(req->getBody()); // 获取请求体作为 Cookie
        if (cookie.empty()) { RESP_ERR(cb, "Cookie 为空"); return; } // 检查 Cookie 是否为空

        auto &db = PayDb::instance(); // 获取数据库实例
        const long long now = std::time(nullptr); // 获取当前时间
        db.setSetting("cef_alipay_cookie", cookie); // 保存 Cookie
        db.setSetting("cef_alipay_cookie_at", std::to_string(now)); // 记录更新时间

        // 更新 CEF 设备心跳 (state=1 表示在线)
        db.exec("UPDATE device SET last_heart=?,updated_at=?,state=1 "
                "WHERE device_no='cef_alipay_001'",
                {std::to_string(now), std::to_string(now)}); // 更新设备状态为在线

        PaymentService::publishCookieUpdated(cookie.size()); // 发布 Cookie 更新事件

        Json::Value data; data["cookie_len"] = (int)cookie.size(); // 创建响应
        RESP_OK(cb, data); // 返回成功
    }

private:
    // 从支付宝个人交易记录返回的 JSON 中粗略抽取 (payType, amount) 列表.
    // 兼容常见结构: 顶层数组 / {data:[...]} / {data:{list:[...]}} 等.
    // 字段名容错 (transAmount/amount/tradeAmount/money/price).
    // 真实接口字段如有差异, 在此函数补充即可, 不影响其它代码.
    static bool extractEntries(const std::string &jsonText,
                               std::vector<std::pair<std::string, std::string>> &out)
    {
        if (jsonText.empty()) return false;

        Json::Reader reader;
        Json::Value root;
        if (!reader.parse(jsonText, root) || root.isNull()) return false;

        const Json::Value *list = nullptr;
        if (root.isArray()) list = &root;
        else if (root.isObject()) {
            for (const char *k : {"data", "list", "items", "records", "result"}) {
                if (root.isMember(k) && root[k].isArray()) { list = &root[k]; break; }
            }
            if (!list && root.isMember("data") && root["data"].isObject()) {
                const Json::Value &d = root["data"];
                for (const char *k : {"list", "items", "records", "result"}) {
                    if (d.isMember(k) && d[k].isArray()) { list = &d[k]; break; }
                }
            }
        }
        if (!list || !list->isArray() || list->empty()) return false;

        for (Json::ArrayIndex i = 0; i < list->size(); ++i) {
            const Json::Value &item = (*list)[i];
            if (!item.isObject()) continue;

            std::string amount;
            for (const char *k : {"transAmount", "amount", "tradeAmount", "money", "price"}) {
                if (item.isMember(k)) {
                    amount = stripCurrency(item[k].asString());
                    if (!amount.empty()) break;
                }
            }
            if (amount.empty()) continue;

            std::string payType = "alipay";  // CEF 来源默认是支付宝
            for (const char *k : {"payType", "channel", "businessTypeText"}) {
                if (item.isMember(k)) {
                    std::string v = item[k].asString();
                    if (v.find("微信") != std::string::npos
                        || v.find("wechat") != std::string::npos) payType = "wxpay";
                    break;
                }
            }
            out.emplace_back(payType, amount);
        }
        return !out.empty();
    }

    // 去掉金额可能带的 ¥/CNY/+/-/, 等装饰字符
    static std::string stripCurrency(const std::string &s) {
        std::string out;
        for (char c : s) {
            if ((c >= '0' && c <= '9') || c == '.') out += c;
        }
        return out;
    }
};
