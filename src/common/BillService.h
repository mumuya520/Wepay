// WePay-Cpp — 账单生成服务
// 由 main.cc 注册到 Drogon 定时任务，每天凌晨自动生成昨日账单
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include <iostream> // 输入输出库
#include <iomanip> // 输入输出格式化库
#include <sstream> // 字符串流库
#include "PayDb.h" // 数据库操作

// 账单生成服务类
// 由 main.cc 注册到 Drogon 定时任务，每天凌晨自动生成昨日账单
class BillService {
public:
    // 生成昨日全局日账单 + 每个商户的日账单
    // 该方法会生成两类账单：
    // 1. 全局日账单（mch_id=0）：汇总所有商户的交易数据
    // 2. 商户日账单：每个商户的单独账单
    static void generateYesterdayBills() {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 获取当前时间戳
        time_t now = std::time(nullptr);
        // 计算昨日 00:00 的时间戳（86400 秒 = 1 天）
        time_t yStart = (now - (now % 86400)) - 86400;
        // 计算昨日 23:59:59 的时间戳
        time_t yEnd   = yStart + 86400;
        // 将昨日日期格式化为 YYYY-MM-DD
        std::string billDate = formatDate(yStart, "%Y-%m-%d");

        // 生成全局日账单（mch_id=0 表示全局）
        generateOneBill(db, billDate, 1, 0, yStart, yEnd);

        // 查询所有已启用的商户
        auto mchs = db.query("SELECT id FROM merchant WHERE state=1", {});
        // 为每个商户生成日账单
        for (auto &m : mchs) {
            // 获取商户 ID
            int mchId = 0;
            // 尝试将商户 ID 字符串转换为整数
            try {
                mchId = std::stoi(m["id"]);
            } catch(...) {
                // 如果转换失败，跳过该商户
                continue;
            }
            // 生成该商户的日账单
            generateOneBill(db, billDate, 1, mchId, yStart, yEnd);
        }
        // 记录账单生成完成的日志
        std::cout << "[BillService] 昨日 " << billDate << " 账单生成完毕，商户数="
                  << mchs.size() << std::endl;
    }

// 私有辅助函数区域
private:
    // 生成单个账单
    // 参数 db：数据库实例
    // 参数 billDate：账单日期（YYYY-MM-DD 格式）
    // 参数 billType：账单类型（1=日账单）
    // 参数 mchId：商户 ID（0 表示全局账单）
    // 参数 start：统计时间范围起始（秒级时间戳）
    // 参数 end：统计时间范围结束（秒级时间戳）
    static void generateOneBill(PayDb &db, const std::string &billDate,
                                int billType, int mchId,
                                long long start, long long end) {
        // 构建商户过滤条件（如果 mchId > 0，则只统计该商户的数据）
        std::string mchFilter = (mchId > 0) ? " AND mch_id=" + std::to_string(mchId) : "";

        // 查询订单统计数据（已支付的订单）
        auto orderR = db.queryOne(
            // SQL 语句：统计订单数量、总金额、手续费
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(amount AS REAL)),0) AS amt, "
            "COALESCE(SUM(CAST(mch_fee_amount AS REAL)),0) AS fee "
            // 从 pay_order 表查询已支付的订单
            "FROM pay_order WHERE state=1 AND pay_time>=? AND pay_time<?" + mchFilter,
            // 参数：时间范围起始、时间范围结束
            {std::to_string(start), std::to_string(end)});
        // 查询退款统计数据（已完成的退款）
        auto refundR = db.queryOne(
            // SQL 语句：统计退款数量、退款总金额
            "SELECT COUNT(*) AS cnt, COALESCE(SUM(CAST(refund_amount AS REAL)),0) AS amt "
            // 从 refund_order 表查询已完成的退款
            "FROM refund_order WHERE state=1 AND finished_at>=? AND finished_at<?" + mchFilter,
            // 参数：时间范围起始、时间范围结束
            {std::to_string(start), std::to_string(end)});

        // 获取订单数量
        int orderCnt = orderR.empty() ? 0 : safeInt(orderR["cnt"]);
        // 如果是商户账单且当日无订单，则跳过生成
        if (orderCnt == 0 && mchId > 0)
            return;

        // 获取订单总金额
        double totalAmt  = orderR.empty() ? 0 : safeDouble(orderR["amt"]);
        // 获取手续费总额
        double feeAmt    = orderR.empty() ? 0 : safeDouble(orderR["fee"]);
        // 获取退款数量
        int refundCnt    = refundR.empty() ? 0 : safeInt(refundR["cnt"]);
        // 获取退款总金额
        double refundAmt = refundR.empty() ? 0 : safeDouble(refundR["amt"]);
        // 计算净金额（订单金额 - 退款金额 - 手续费）
        double netAmt    = totalAmt - refundAmt - feeAmt;

        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 插入或更新账单记录
        db.exec("INSERT OR REPLACE INTO account_bill(bill_date,bill_type,mch_id,"
                "order_count,total_amount,refund_count,refund_amount,fee_amount,net_amount,"
                "generated_at) VALUES(?,?,?,?,?,?,?,?,?,?)",
                // 参数：账单日期、账单类型、商户 ID、订单数量、订单总金额、
                //      退款数量、退款总金额、手续费、净金额、生成时间
                {billDate, std::to_string(billType), std::to_string(mchId),
                 std::to_string(orderCnt), fmtAmount(totalAmt),
                 std::to_string(refundCnt), fmtAmount(refundAmt),
                 fmtAmount(feeAmt), fmtAmount(netAmt),
                 std::to_string(now)});
    }

    // 安全的字符串转整数
    // 参数 s：字符串
    // 返回：转换后的整数（转换失败返回 0）
    static int safeInt(const std::string &s) {
        // 尝试将字符串转换为整数
        try {
            return std::stoi(s);
        } catch(...) {
            // 转换失败返回 0
            return 0;
        }
    }

    // 安全的字符串转浮点数
    // 参数 s：字符串
    // 返回：转换后的浮点数（转换失败返回 0.0）
    static double safeDouble(const std::string &s) {
        // 尝试将字符串转换为浮点数
        try {
            return std::stod(s);
        } catch(...) {
            // 转换失败返回 0.0
            return 0.0;
        }
    }

    // 格式化金额为字符串
    // 参数 v：金额数值
    // 返回：格式化后的金额字符串（保留两位小数）
    static std::string fmtAmount(double v) {
        // 创建字符串流
        std::ostringstream oss;
        // 设置固定小数点格式，精度为 2
        oss << std::fixed << std::setprecision(2) << v;
        // 返回格式化后的字符串
        return oss.str();
    }

    // 格式化时间戳为日期字符串
    // 参数 ts：时间戳（秒级）
    // 参数 fmt：格式字符串（如 "%Y-%m-%d"）
    // 返回：格式化后的日期字符串
    static std::string formatDate(time_t ts, const char *fmt) {
        // 时间结构体
        struct tm t;
// Windows 平台
#ifdef _WIN32
        // 使用 localtime_s 转换时间戳
        localtime_s(&t, &ts);
// Unix 平台
#else
        // 使用 localtime_r 转换时间戳
        localtime_r(&ts, &t);
#endif
        // 格式化缓冲区
        char buf[32];
        // 使用 strftime 格式化时间
        std::strftime(buf, sizeof(buf), fmt, &t);
        // 返回格式化后的字符串
        return buf;
    }
// 类定义结束
};
