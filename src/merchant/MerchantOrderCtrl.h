// WePay-Cpp — 商户后台: 订单与数据统计
// GET  /merchant/api/order/list       订单列表（分页、筛选）
// GET  /merchant/api/order/detail     订单详情
// GET  /merchant/api/dashboard        数据面板（余额、统计、待结算）
// GET  /merchant/api/money/logs       资金日志（分页）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户订单与统计控制器类
class MerchantOrderCtrl : public drogon::HttpController<MerchantOrderCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantOrderCtrl::list,      "/merchant/api/order/list",   drogon::Get, "MerchantAuthFilter"); // 订单列表
        ADD_METHOD_TO(MerchantOrderCtrl::detail,    "/merchant/api/order/detail", drogon::Get, "MerchantAuthFilter"); // 订单详情
        ADD_METHOD_TO(MerchantOrderCtrl::dashboard, "/merchant/api/dashboard",    drogon::Get, "MerchantAuthFilter"); // 数据面板
        ADD_METHOD_TO(MerchantOrderCtrl::moneyLogs, "/merchant/api/money/logs",   drogon::Get, "MerchantAuthFilter"); // 资金日志
    METHOD_LIST_END // 路由列表结束

    // 订单列表方法（分页、筛选）
    void list(const drogon::HttpRequestPtr &req, // HTTP 请求对象
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        int page = safeInt(req->getParameter("page"), 1); // 获取页码（默认 1）
        int size = safeInt(req->getParameter("size"), 20); // 获取每页数量（默认 20）
        if (page < 1) page = 1; // 页码最小为 1
        if (size > 100) size = 20; // 每页最多 100 条，超过则重置为 20
        int offset = (page - 1) * size; // 计算数据库偏移量

        auto &db = PayDb::instance(); // 获取数据库单例
        std::string where = "mch_id=?"; // WHERE 子句初始化
        std::vector<std::string> params = {mchId}; // 参数列表

        std::string stateP = req->getParameter("state"); // 获取订单状态筛选参数
        std::string typeP  = req->getParameter("pay_type"); // 获取支付类型筛选参数
        if (!stateP.empty()) { where += " AND state=?"; params.push_back(stateP); } // 如果有状态筛选，添加到 WHERE
        if (!typeP.empty())  { where += " AND pay_type=?"; params.push_back(typeP); } // 如果有支付类型筛选，添加到 WHERE

        auto cntR = db.query("SELECT COUNT(*) AS c FROM pay_order WHERE " + where, params); // 查询总数
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]); // 获取总数

        auto pp = params; // 复制参数列表
        pp.push_back(std::to_string(size)); // 添加 LIMIT 参数
        pp.push_back(std::to_string(offset)); // 添加 OFFSET 参数
        auto rows = db.query( // 查询订单列表
            "SELECT order_id,mch_order_no,pay_type,amount,real_amount," // 查询订单基本信息
            "mch_fee_amount,subject,state,notify_state,pay_time,created_at " // 查询费用和状态信息
            "FROM pay_order WHERE " + where + " ORDER BY id DESC LIMIT ? OFFSET ?", pp); // 按 ID 倒序，分页查询

        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每条订单记录
            Json::Value item; // 创建订单项
            item["order_id"]       = r["order_id"]; // 订单 ID
            item["mch_order_no"]   = r["mch_order_no"]; // 商户订单号
            item["pay_type"]       = r["pay_type"]; // 支付类型
            item["amount"]         = r["amount"]; // 订单金额
            item["real_amount"]    = r["real_amount"]; // 实际支付金额
            item["fee"]            = r["mch_fee_amount"]; // 商户手续费
            item["subject"]        = r["subject"]; // 订单主题
            item["state"]          = std::stoi(r["state"]); // 订单状态（转为整数）
            item["notify_state"]   = std::stoi(r["notify_state"]); // 通知状态（转为整数）
            item["pay_time"]       = (Json::Int64)std::stoll(r["pay_time"]); // 支付时间戳
            item["created_at"]     = (Json::Int64)std::stoll(r["created_at"]); // 创建时间戳
            arr.append(item); // 添加到数组
        }
        Json::Value data; // 响应数据
        data["list"] = arr; // 订单列表
        data["total"] = total; // 总数
        data["page"] = page; // 当前页码
        data["size"] = size; // 每页数量
        RESP_OK(cb, data); // 返回成功响应
    }

    // 订单详情方法
    void detail(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId   = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        std::string orderId = req->getParameter("order_id"); // 获取订单 ID 参数
        auto row = PayDb::instance().queryOne( // 查询订单详情
            "SELECT * FROM pay_order WHERE order_id=? AND mch_id=?", // 查询所有订单字段
            {orderId, mchId}); // 按订单 ID 和商户 ID 查询
        if (row.empty()) { RESP_ERR(cb, "订单不存在"); return; } // 订单不存在
        Json::Value data; // 响应数据
        for (auto &[k, v] : row) data[k] = v; // 将查询结果转换为 JSON
        data["state"] = std::stoi(row["state"]); // 将状态转为整数
        RESP_OK(cb, data); // 返回订单详情
    }

    // 数据面板方法（显示余额、统计、待结算）
    void dashboard(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto &db = PayDb::instance(); // 获取数据库单例

        // 商户余额
        auto mch = db.queryOne("SELECT balance,frozen,total_income FROM merchant WHERE id=?", // 查询商户余额信息
                               {mchId}); // 按商户 ID 查询

        // 今日统计
        long long dayStart = todayStart(); // 获取今天开始的时间戳
        auto todayCnt = db.queryOne( // 查询今日统计
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS total " // 查询订单数和总金额
            "FROM pay_order WHERE mch_id=? AND state=1 AND pay_time>=?", // 已支付的订单
            {mchId, std::to_string(dayStart)}); // 商户 ID 和今日开始时间

        // 总订单统计
        auto allCnt = db.queryOne( // 查询总订单统计
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS total " // 查询订单数和总金额
            "FROM pay_order WHERE mch_id=? AND state=1", // 所有已支付的订单
            {mchId}); // 按商户 ID 查询

        // 待结算
        auto pendSettle = db.queryOne( // 查询待结算金额
            "SELECT COALESCE(SUM(CAST(amount AS REAL)),0) AS total " // 查询待结算总金额
            "FROM settle_order WHERE mch_id=? AND state IN (0,1)", // 状态为待审核或审核中的结算单
            {mchId}); // 按商户 ID 查询

        Json::Value data; // 响应数据
        data["balance"]       = mch.empty() ? "0.00" : mch["balance"]; // 可用余额
        data["frozen"]        = mch.empty() ? "0.00" : mch["frozen"]; // 冻结金额
        data["total_income"]  = mch.empty() ? "0.00" : mch["total_income"]; // 总收入
        data["today_count"]   = todayCnt.empty() ? 0 : std::stoi(todayCnt["cnt"]); // 今日订单数
        data["today_amount"]  = todayCnt.empty() ? "0.00" : todayCnt["total"]; // 今日交易额
        data["total_count"]   = allCnt.empty() ? 0 : std::stoi(allCnt["cnt"]); // 总订单数
        data["total_amount"]  = allCnt.empty() ? "0.00" : allCnt["total"]; // 总交易额
        data["pending_settle"]= pendSettle.empty() ? "0.00" : pendSettle["total"]; // 待结算金额
        RESP_OK(cb, data); // 返回数据面板
    }

    // 资金日志方法（分页查询）
    void moneyLogs(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        int page = safeInt(req->getParameter("page"), 1); // 获取页码（默认 1）
        int size = safeInt(req->getParameter("size"), 20); // 获取每页数量（默认 20）
        int offset = (page - 1) * size; // 计算数据库偏移量

        auto &db = PayDb::instance(); // 获取数据库单例
        auto cntR = db.query("SELECT COUNT(*) AS c FROM money_log WHERE mch_id=?", {mchId}); // 查询总数
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]); // 获取总数

        auto rows = db.query( // 查询资金日志
            "SELECT * FROM money_log WHERE mch_id=? ORDER BY id DESC LIMIT ? OFFSET ?", // 查询所有字段，按 ID 倒序
            {mchId, std::to_string(size), std::to_string(offset)}); // 商户 ID、分页参数

        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每条日志记录
            Json::Value item; // 创建日志项
            item["id"]            = std::stoi(r["id"]); // 日志 ID
            item["change_type"]   = std::stoi(r["change_type"]); // 变动类型（1=收入，2=支出）
            item["change_amount"] = r["change_amount"]; // 变动金额
            item["before_amount"] = r["before_amount"]; // 变动前余额
            item["after_amount"]  = r["after_amount"]; // 变动后余额
            item["biz_type"]      = r["biz_type"]; // 业务类型（支付、退款等）
            item["biz_no"]        = r["biz_no"]; // 业务单号
            item["remark"]        = r["remark"]; // 备注
            item["created_at"]    = (Json::Int64)std::stoll(r["created_at"]); // 创建时间戳
            arr.append(item); // 添加到数组
        }
        Json::Value data; // 响应数据
        data["list"] = arr; // 日志列表
        data["total"] = total; // 总数
        RESP_OK(cb, data); // 返回成功响应
    }

private:
    // 安全字符串转整数方法
    static int safeInt(const std::string &s, int def) { // 字符串和默认值
        try { return std::stoi(s); } catch (...) { return def; } // 尝试转换，失败返回默认值
    }
    // 获取今天开始时间戳方法
    static long long todayStart() { // 返回今天 00:00:00 的时间戳
        auto now = std::time(nullptr); // 获取当前时间戳
        struct tm t; // 时间结构体
#ifdef _WIN32
        localtime_s(&t, &now); // Windows 本地时间转换
#else
        localtime_r(&now, &t); // Linux 本地时间转换
#endif
        t.tm_hour = 0; // 设置小时为 0
        t.tm_min = 0; // 设置分钟为 0
        t.tm_sec = 0; // 设置秒为 0
        return mktime(&t); // 转换回时间戳
    }
};
