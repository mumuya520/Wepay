// WePay-Cpp — 定时任务服务 ( cron.php)
// 功能:
//   1. 订单过期关闭
//   2. 自动结算生成
//   3. 每日订单统计
//   4. 通知重试 (已由 NotifyTaskService 实现)
//   5. 过期数据清理
//   6. 通道日额度重置
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include <iostream> // 输入输出库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式化库
#include "PayDb.h" // 数据库操作
#include "ChannelService.h" // 通道服务
#include "OpsService.h" // 运维服务
#include "WepayV3EmailService.h" // V3 邮件服务

// 定时任务服务类
class CronService {
public:
    // ── 1. 关闭过期未支付订单 ──────────────────────────────
    // 建议每 60s 调用一次
    // 静态方法：关闭过期订单
    static void closeExpiredOrders() {
        auto &db = PayDb::instance(); // 获取数据库实例
        long long now = std::time(nullptr); // 获取当前时间戳
        auto rows = db.query( // 查询过期订单
            "SELECT order_id FROM pay_order WHERE state=0 AND expire_time>0 AND expire_time<?", // SQL 查询
            {std::to_string(now)}); // 参数：当前时间
        int cnt = 0; // 计数器
        for (auto &r : rows) { // 遍历每个过期订单
            db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=? AND state=0", // 更新订单状态为已关闭
                    {std::to_string(now), r.at("order_id")}); // 参数：当前时间、订单 ID
            // 释放锁定金额
            db.exec("DELETE FROM tmp_price WHERE oid=?", {r.at("order_id")}); // 删除临时金额锁定
            ++cnt; // 计数加 1
        }
        if (cnt > 0) // 如果有关闭的订单
            std::cout << "[Cron] 关闭过期订单 " << cnt << " 笔" << std::endl; // 输出日志
    }

    // ── 2. 自动结算 ───────────────────────────────────────
    // 建议每日 02:00 执行一次
    // 为启用自动结算的商户生成结算单
    // 静态方法：自动生成结算单
    static void autoSettle() {
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string autoSettleEnabled = db.getSetting("auto_settle", "0"); // 读取自动结算启用标志
        if (autoSettleEnabled != "1") return; // 如果未启用则返回

        double minAmount = 0; // 最小结算金额
        try { minAmount = std::stod(db.getSetting("settle_min_amount", "100")); } catch (...) {} // 读取最小结算金额

        long long now = std::time(nullptr); // 获取当前时间戳
        std::string today = fmtDate(now); // 格式化当前日期

        // 查询所有启用自动结算的商户
        auto merchants = db.query( // 查询商户
            "SELECT id,balance,settle_type,settle_account,settle_name " // 选择字段
            "FROM merchant WHERE state=1 AND auto_settle=1 AND balance>=?", // 条件：启用且余额足够
            {ChannelService::fmtAmount(minAmount)}); // 参数：最小金额

        int cnt = 0; // 计数器
        for (auto &m : merchants) { // 遍历每个商户
            double balance = 0; // 余额
            try { balance = std::stod(m.at("balance")); } catch (...) {} // 转换余额为浮点数
            if (balance < minAmount) continue; // 如果余额不足则跳过

            int mchId = std::stoi(m.at("id")); // 获取商户 ID
            std::string settleNo = ChannelService::generateSettleNo(); // 生成结算单号

            // 计算手续费
            double fee = 0; // 手续费
            std::string feeStr = db.getSetting("settle_fee", "0"); // 读取手续费比例
            try { fee = std::stod(feeStr); } catch (...) {} // 转换为浮点数
            double actualAmount = balance - fee; // 实际结算金额 = 余额 - 手续费
            if (actualAmount <= 0) continue; // 如果实际金额不足则跳过

            // 冻结余额
            if (!ChannelService::changeMchBalance(mchId, 3, balance, "settle", settleNo, "自动结算冻结")) { // 冻结商户余额
                continue; // 如果冻结失败则跳过
            }

            db.exec( // 插入结算单
                "INSERT INTO settle_order(settle_no,mch_id,amount,fee,actual_amount," // 表名和字段
                "settle_type,account,account_name,state,created_at,updated_at) " // 更多字段
                "VALUES(?,?,?,?,?,?,?,?,0,?,?)", // 占位符
                {settleNo, std::to_string(mchId), // 结算单号、商户 ID
                 ChannelService::fmtAmount(balance), ChannelService::fmtAmount(fee), // 余额、手续费
                 ChannelService::fmtAmount(actualAmount), // 实际金额
                 m.count("settle_type") ? m.at("settle_type") : "", // 结算类型
                 m.count("settle_account") ? m.at("settle_account") : "", // 结算账户
                 m.count("settle_name") ? m.at("settle_name") : "", // 结算名称
                 std::to_string(now), std::to_string(now)}); // 创建时间、更新时间
            ++cnt; // 计数加 1
        }
        if (cnt > 0) // 如果有生成的结算单
            std::cout << "[Cron] 自动结算 " << cnt << " 笔" << std::endl; // 输出日志
    }

    // ── 3. 每日订单统计 ───────────────────────────────────
    // 建议每日 00:30 执行
    // 静态方法：每日订单统计
    static void dailyOrderStats() {
        auto &db = PayDb::instance(); // 获取数据库实例
        long long now = std::time(nullptr); // 获取当前时间戳
        std::string yesterday = fmtDate(now - 86400); // 格式化昨天日期

        // 检查幂等
        std::string key = "stats_" + yesterday; // 生成统计键
        if (!db.getSetting(key).empty()) return; // 如果已统计则返回

        // 总订单数/金额
        auto total = db.queryOne( // 查询总订单数和金额
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS amt " // 选择计数和总金额
            "FROM pay_order WHERE DATE(created_at)=?", // 条件：昨天的订单
            {yesterday}); // 参数：昨天日期
        auto paid = db.queryOne( // 查询已支付订单数和金额
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS amt " // 选择计数和总金额
            "FROM pay_order WHERE DATE(created_at)=? AND state=1", // 条件：昨天且已支付
            {yesterday}); // 参数：昨天日期
        auto mchFee = db.queryOne( // 查询商户手续费
            "SELECT COALESCE(SUM(CAST(mch_fee_amount AS REAL)),0) AS amt " // 选择手续费总额
            "FROM pay_order WHERE DATE(created_at)=? AND state=1", // 条件：昨天且已支付
            {yesterday}); // 参数：昨天日期

        // 写入统计表
        db.exec( // 执行插入或替换
            "INSERT OR REPLACE INTO daily_stats(stat_date,total_orders,total_amount," // 表名和字段
            "paid_orders,paid_amount,fee_amount,created_at) VALUES(?,?,?,?,?,?,?)", // 占位符
            {yesterday, // 统计日期
             total.empty() ? "0" : total.at("cnt"), // 总订单数
             total.empty() ? "0" : total.at("amt"), // 总金额
             paid.empty() ? "0" : paid.at("cnt"), // 已支付订单数
             paid.empty() ? "0" : paid.at("amt"), // 已支付金额
             mchFee.empty() ? "0" : mchFee.at("amt"), // 手续费总额
             std::to_string(now)}); // 创建时间

        db.setSetting(key, std::to_string(now)); // 设置统计标记
        std::cout << "[Cron] " << yesterday << " 统计完成: " // 输出日志
                  << (paid.empty() ? "0" : paid.at("cnt")) << " 笔已支付" << std::endl; // 已支付笔数
    }

    // ── 4. 清理过期数据 ──────────────────────────────────
    // 建议每日 03:00 执行
    // 静态方法：清理过期数据
    static void cleanupExpiredData() {
        auto &db = PayDb::instance(); // 获取数据库实例
        long long now = std::time(nullptr); // 获取当前时间戳
        long long threshold30d = now - 30 * 86400; // 30 天前的时间戳
        long long threshold7d  = now - 7 * 86400; // 7 天前的时间戳

        // 清理 30 天前的已关闭/过期订单
        std::string keepDays = db.getSetting("order_keep_days", "30"); // 读取订单保留天数
        long long keepTs = now - std::stoi(keepDays) * 86400; // 计算保留期限时间戳
        int cnt = 0; // 计数器
        auto closedOrders = db.query( // 查询已关闭的订单
            "SELECT order_id FROM pay_order WHERE state IN (-1,-2) AND created_at<?", // 条件：已关闭且超期
            {std::to_string(keepTs)}); // 参数：保留期限时间戳
        for (auto &r : closedOrders) { // 遍历每个已关闭订单
            db.exec("DELETE FROM pay_order WHERE order_id=?", {r.at("order_id")}); // 删除订单
            db.exec("DELETE FROM tmp_price WHERE oid=?", {r.at("order_id")}); // 删除临时金额锁定
            ++cnt; // 计数加 1
        }

        // 清理 7 天前的临时金额锁定
        db.exec("DELETE FROM tmp_price WHERE ctime<?", {std::to_string(threshold7d)}); // 删除过期的临时锁定

        // 清理 30 天前的操作日志
        db.exec("DELETE FROM oplog WHERE created_at<?", {std::to_string(threshold30d)}); // 删除过期的操作日志

        // 清理过期 setting 标记
        db.exec("DELETE FROM sys_setting WHERE k LIKE 'bill_gen_%' AND CAST(v AS INTEGER)<?", // 删除过期的账单生成标记
                {std::to_string(threshold30d)}); // 参数：30 天前时间戳
        db.exec("DELETE FROM sys_setting WHERE k LIKE 'stats_%' AND CAST(v AS INTEGER)<?", // 删除过期的统计标记
                {std::to_string(threshold30d)}); // 参数：30 天前时间戳

        if (cnt > 0) // 如果有删除的订单
            std::cout << "[Cron] 清理过期数据: 订单 " << cnt << " 条" << std::endl; // 输出日志
    }

    // ── 5. 通道日额度重置 ────────────────────────────────
    // 建议每日 00:01 执行
    // 静态方法：重置通道日额度
    static void resetChannelDayQuota() {
        auto &db = PayDb::instance(); // 获取数据库实例
        long long now = std::time(nullptr); // 获取当前时间戳

        // 重置所有通道的日累计
        db.exec("UPDATE pay_channel SET day_amount=0,day_count=0,updated_at=? WHERE day_limit>0", // 重置日累计
                {std::to_string(now)}); // 参数：当前时间戳

        // 重启被日限额暂停的通道
        db.exec("UPDATE pay_channel SET state=1,updated_at=? WHERE state=2 AND day_limit>0", // 恢复暂停的通道
                {std::to_string(now)}); // 参数：当前时间戳

        std::cout << "[Cron] 通道日额度已重置" << std::endl; // 输出日志
    }

    // ── 6. 订单补单检查 ──────────────────────────────────
    // 建议每 5 分钟执行
    // 查找状态仍为 0 但接近过期的订单，向上游查询实际支付状态
    // 静态方法：检查待支付订单
    static void checkPendingOrders() {
        auto &db = PayDb::instance(); // 获取数据库实例
        long long now = std::time(nullptr); // 获取当前时间戳
        long long soon = now + 120; // 2 分钟内过期的时间戳

        auto rows = db.query( // 查询接近过期的待支付订单
            "SELECT order_id,channel_id FROM pay_order WHERE state=0 AND expire_time>? AND expire_time<?", // 条件：待支付且接近过期
            {std::to_string(now), std::to_string(soon)}); // 参数：当前时间、2 分钟后时间

        // 这里只做标记，实际上游查询需要插件支持
        for (auto &r : rows) { // 遍历每个接近过期的订单
            db.exec("UPDATE pay_order SET updated_at=? WHERE order_id=? AND state=0", // 更新订单时间戳
                    {std::to_string(now), r.at("order_id")}); // 参数：当前时间、订单 ID
        }
    }

    // ── 7. 通道日统计同步 ────────────────────────────
    // 建议每日 00:35 执行
    // 静态方法：同步通道日统计
    static void syncChannelDailyStat() {
        OpsService::syncChannelDailyStat(); // 调用运维服务同步通道日统计
        std::cout << "[Cron] 通道日统计同步完成" << std::endl; // 输出日志
    }

    // ── 8. V3 每日汇总邮件 ─────────────────────────────────
    // 建议每日 10:00 执行（精确到分钟：hour==10 && min<2）
    // 静态方法：发送 V3 每日汇总邮件
    static void v3DailySummaryEmail() {
        auto &db = PayDb::instance(); // 获取数据库实例
        if (db.getSetting("v3_daily_email_enabled", "0") != "1") return; // 如果未启用则返回

        std::string baseUrl = db.getSetting("site_url", "http://localhost"); // 读取网站 URL
        // 遍历所有开启了邮件通知的商户
        auto mchs = db.query( // 查询启用邮件通知的商户
            "SELECT id,notify_email FROM merchant WHERE state=1" // 条件：启用
            " AND notify_email!='' AND email_notify_enabled=1", {}); // 条件：有邮箱且启用邮件通知
        for (auto &m : mchs) { // 遍历每个商户
            WepayV3EmailService::instance().sendDailySummary( // 发送每日汇总邮件
                m.at("id"), m.at("notify_email"), baseUrl); // 参数：商户 ID、邮箱、网站 URL
        }
        std::cout << "[Cron] V3 每日汇总邮件已触发，商户数=" << mchs.size() << std::endl; // 输出日志
    }

    // ── 9. 自动转账结算 ────────────────────────────────────
    // 对审核通过(state=1)的结算单执行批量转账标记
    // 实际转账需要通道插件支持，这里做状态推进和记录
    // 建议每日 04:00 执行
    // 静态方法：自动转账结算
    static void autoTransfer() {
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string autoTransfer = db.getSetting("auto_transfer", "0"); // 读取自动转账启用标志
        if (autoTransfer != "1") return; // 如果未启用则返回

        long long now = std::time(nullptr); // 获取当前时间戳

        // 查找已审核通过但未转账的结算单
        auto rows = db.query( // 查询待转账的结算单
            "SELECT * FROM settle_order WHERE state=1 ORDER BY created_at ASC LIMIT 50", {}); // 条件：已审核，限制 50 条

        int cnt = 0; // 计数器
        for (auto &r : rows) { // 遍历每个结算单
            std::string settleNo = r.at("settle_no"); // 获取结算单号
            // 标记为转账中 state=2
            db.exec("UPDATE settle_order SET state=2,updated_at=? WHERE settle_no=? AND state=1", // 更新状态为转账中
                    {std::to_string(now), settleNo}); // 参数：当前时间、结算单号

            // 记录转账日志
            db.exec("INSERT INTO transfer_log(settle_no,mch_id,amount,settle_type,account," // 插入转账日志
                    "account_name,state,created_at) VALUES(?,?,?,?,?,?,0,?)", // 占位符
                    {settleNo, r.at("mch_id"), // 结算单号、商户 ID
                     r.count("actual_amount") ? r.at("actual_amount") : "0", // 实际金额
                     r.count("settle_type") ? r.at("settle_type") : "", // 结算类型
                     r.count("account") ? r.at("account") : "", // 账户
                     r.count("account_name") ? r.at("account_name") : "", // 账户名
                     std::to_string(now)}); // 创建时间
            ++cnt; // 计数加 1
        }
        if (cnt > 0) // 如果有转账的结算单
            std::cout << "[Cron] 批量转账 " << cnt << " 笔结算单" << std::endl; // 输出日志
    }

    // ── 初始化数据库表 ───────────────────────────────────
    // 静态方法：初始化数据库表
    static void initTables() {
        auto &db = PayDb::instance(); // 获取数据库实例
        db.exec( // 创建每日统计表
            "CREATE TABLE IF NOT EXISTS daily_stats (" // 表名：daily_stats
            "  stat_date TEXT PRIMARY KEY," // 统计日期（主键）
            "  total_orders INTEGER DEFAULT 0," // 总订单数
            "  total_amount REAL DEFAULT 0," // 总金额
            "  paid_orders INTEGER DEFAULT 0," // 已支付订单数
            "  paid_amount REAL DEFAULT 0," // 已支付金额
            "  fee_amount REAL DEFAULT 0," // 手续费总额
            "  created_at INTEGER" // 创建时间
            ")", {}); // 参数：无
        db.exec( // 创建转账日志表
            "CREATE TABLE IF NOT EXISTS transfer_log (" // 表名：transfer_log
            "  id INTEGER PRIMARY KEY AUTOINCREMENT," // ID（自增主键）
            "  settle_no TEXT," // 结算单号
            "  mch_id INTEGER DEFAULT 0," // 商户 ID
            "  amount TEXT DEFAULT '0'," // 转账金额
            "  settle_type TEXT," // 结算类型
            "  account TEXT," // 账户
            "  account_name TEXT," // 账户名
            "  state INTEGER DEFAULT 0," // 转账状态
            "  error_msg TEXT," // 错误信息
            "  created_at INTEGER" // 创建时间
            ")", {}); // 参数：无
        db.exec( // 创建商户域名表
            "CREATE TABLE IF NOT EXISTS merchant_domain (" // 表名：merchant_domain
            "  id INTEGER PRIMARY KEY AUTOINCREMENT," // ID（自增主键）
            "  mch_id INTEGER DEFAULT 0," // 商户 ID
            "  domain TEXT," // 域名
            "  state INTEGER DEFAULT 1," // 状态
            "  created_at INTEGER" // 创建时间
            ")", {}); // 参数：无
    }

private:
    // 私有辅助方法
    // 静态方法：格式化日期
    static std::string fmtDate(long long ts) {
        time_t t = (time_t)ts; // 转换为 time_t
        struct tm tm; // 时间结构体
#ifdef _WIN32 // Windows 平台
        localtime_s(&tm, &t); // 转换为本地时间（Windows 版本）
#else // Unix/Linux 平台
        localtime_r(&t, &tm); // 转换为本地时间（Unix 版本）
#endif
        char buf[12]; // 缓冲区
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm); // 格式化为 YYYY-MM-DD
        return std::string(buf); // 返回格式化的日期字符串
    }

    // 静态方法：获取当前小时
    static int currentHour() {
        time_t t = std::time(nullptr); // 获取当前时间戳
        struct tm tm; // 时间结构体
#ifdef _WIN32 // Windows 平台
        localtime_s(&tm, &t); // 转换为本地时间（Windows 版本）
#else // Unix/Linux 平台
        localtime_r(&t, &tm); // 转换为本地时间（Unix 版本）
#endif
        return tm.tm_hour; // 返回小时（0-23）
    }
}; // 类定义结束
