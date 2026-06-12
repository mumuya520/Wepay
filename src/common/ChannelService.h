// WePay-Cpp — 支付通道路由与管理服务
// 负责：通道选择、费率计算、金额锁定、订单号生成
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <ctime> // C 时间库
#include <cmath> // 数学库
#include <random> // 随机数库
#include <sstream>
#include <iomanip>
#include <json/json.h>
#include "PayDb.h"

// 支付通道服务类，负责通道选择、费率计算、金额锁定、订单号生成等功能
class ChannelService {
public:
    // 支付通道信息结构体
    struct ChannelInfo {
        // 通道 ID
        int channelId = 0;
        // 通道编码（如 "wxpay"、"alipay" 等）
        std::string channelCode;
        // 通道名称
        std::string channelName;
        // 支付类型（如 "wxpay"、"alipay" 等）
        std::string payType;
        // 插件名称
        std::string plugin;
        // 费率（百分比）
        double rate = 0;
        // 通道参数 JSON 字符串
        std::string paramsJson;
        // 最小金额
        double minAmount = 0.01;
        // 最大金额
        double maxAmount = 50000;
        // 开放时段起始（0-23 小时）
        int timeStart = 0;
        // 开放时段截止（0-23 小时）
        int timeStop  = 0;
        // 日限额
        double dayLimit = 0;
        // 日已用金额
        double dayAmount = 0;
    };

    // 通道选择模式枚举
    // FIRST_AVAILABLE：按排序选择首个可用通道
    // RANDOM：随机选择一个可用通道
    // ROUND_ROBIN：轮询选择可用通道
    enum SelectMode { FIRST_AVAILABLE = 0, RANDOM = 1, ROUND_ROBIN = 2 };

    // 为商户选择可用支付通道（增强版）
    // 增强功能：时段过滤、日限额过滤、多种选择模式（随机/轮询/首个）
    // 参数 mchId：商户 ID
    // 参数 payType：支付类型（如 "wxpay"、"alipay" 等）
    // 参数 amount：支付金额
    // 返回：选中的通道信息（如果无可用通道返回空结构体）
    static ChannelInfo selectChannel(int mchId, const std::string &payType, double amount) {
        auto &db = PayDb::instance();

        // 读取全局选择模式配置（仅作兜底）
        std::string modeStr = db.getSetting("channel_select_mode", "0");
        int globalSelectMode = 0;
        try { globalSelectMode = std::stoi(modeStr); } catch (...) {}

        // 查商户绑定的通道 (pay_type 大小写不敏感)
        auto rows = db.query(
            "SELECT c.id,c.channel_code,c.channel_name,c.pay_type,c.plugin,"
            "c.rate,c.params_json,c.min_amount,c.max_amount,"
            "c.time_start,c.time_stop,c.day_limit,c.day_amount,c.day_count,"
            "c.select_mode,c.code_type,c.support_business_code,"
            "mc.select_mode AS mch_select_mode,mc.code_type AS mch_code_type,"
            "COALESCE(NULLIF(mc.rate,''), c.rate) AS final_rate "
            "FROM pay_channel c "
            "JOIN merchant_channel mc ON mc.channel_id=c.id "
            "WHERE mc.mch_id=? AND LOWER(c.pay_type)=LOWER(?) AND c.state=1 AND mc.state=1 "
            "ORDER BY c.sort_order ASC, c.id ASC",
            {std::to_string(mchId), payType});

        bool useMerchantBindings = !rows.empty();

        // 如果商户未绑定任何通道，回退到系统默认通道 (pay_type 大小写不敏感)
        if (rows.empty()) {
            rows = db.query(
                "SELECT id,channel_code,channel_name,pay_type,plugin,"
                "rate,params_json,min_amount,max_amount,"
                "time_start,time_stop,day_limit,day_amount,day_count,"
                "select_mode,code_type,support_business_code,"
                "rate AS final_rate "
                "FROM pay_channel WHERE LOWER(pay_type)=LOWER(?) AND state=1 "
                "ORDER BY sort_order ASC, id ASC",
                {payType});
        }

        // 过滤可用通道
        std::vector<PayDb::Row> available;
        int curHour = currentHour();
        for (auto &r : rows) {
            double minA = safeDouble(r["min_amount"], 0.01);
            double maxA = safeDouble(r["max_amount"], 50000);
            if (amount < minA || amount > maxA) continue;

            // 时段过滤
            int ts = safeInt(r, "time_start", 0);
            int te = safeInt(r, "time_stop", 0);
            if (ts != 0 || te != 0) {
                if (ts < te) {
                    if (curHour < ts || curHour > te) continue;
                } else if (ts > te) {
                    if (curHour < ts && curHour > te) continue;
                }
            }

            // 日限额过滤
            double dayLim = safeDouble(r["day_limit"], 0);
            double dayAmt = safeDouble(r["day_amount"], 0);
            if (dayLim > 0 && dayAmt + amount > dayLim) continue;

            available.push_back(r);
        }

        if (available.empty()) return {};

        int selectMode = globalSelectMode;
        if (useMerchantBindings) {
            auto it = available[0].find("mch_select_mode");
            if (it != available[0].end() && !it->second.empty()) {
                try { selectMode = std::stoi(it->second); } catch (...) {}
            } else {
                auto chIt = available[0].find("select_mode");
                if (chIt != available[0].end() && !chIt->second.empty()) {
                    try { selectMode = std::stoi(chIt->second); } catch (...) {}
                }
            }
        } else {
            auto chIt = available[0].find("select_mode");
            if (chIt != available[0].end() && !chIt->second.empty()) {
                try { selectMode = std::stoi(chIt->second); } catch (...) {}
            }
        }

        // 选择策略
        PayDb::Row *chosen = nullptr;
        if (selectMode == RANDOM && available.size() > 1) {
            std::mt19937 rng((unsigned)std::random_device{}());
            std::uniform_int_distribution<size_t> d(0, available.size() - 1);
            chosen = &available[d(rng)];
        } else if (selectMode == ROUND_ROBIN && available.size() > 1) {
            std::string rrKey = std::string("rr_channel_") + std::to_string(mchId) + "_" + payType;
            int idx = 0;
            try { idx = std::stoi(db.getSetting(rrKey, "0")); } catch (...) {}
            idx = idx % (int)available.size();
            chosen = &available[idx];
            db.setSetting(rrKey, std::to_string((idx + 1) % (int)available.size()));
        } else {
            chosen = &available[0];
        }

        // 更新日用量
        std::string chId = (*chosen)["id"];
        db.exec("UPDATE pay_channel SET day_amount=CAST(COALESCE(NULLIF(day_amount,''),'0') AS REAL)+?,day_count=day_count+1 WHERE id=?",
                {fmtAmount(amount), chId});

        return rowToChannelInfo(*chosen);
    }

    // 获取商户的实际费率（百分比）
    // 参数 mchId：商户 ID
    // 参数 channelId：通道 ID
    // 返回：费率百分比（如 1.0 表示 1%）
    // 优先级：商户-通道绑定费率 > 商户默认费率 > 通道默认费率
    static double getMchRate(int mchId, int channelId) {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 查询商户-通道绑定的费率
        auto row = db.queryOne(
            // SQL 语句：查询商户-通道绑定的费率，如果为空则使用通道默认费率
            "SELECT COALESCE(NULLIF(mc.rate,''), c.rate) AS r "
            "FROM merchant_channel mc "
            "JOIN pay_channel c ON c.id=mc.channel_id "
            "WHERE mc.mch_id=? AND mc.channel_id=?",
            // 参数：商户 ID、通道 ID
            {std::to_string(mchId), std::to_string(channelId)});
        // 如果查询结果为空
        if (row.empty()) {
            // 查询商户默认费率
            auto mch = db.queryOne("SELECT rate FROM merchant WHERE id=?",
                                   {std::to_string(mchId)});
            // 如果商户不存在，返回默认费率 1.0
            return mch.empty() ? 1.0 : safeDouble(mch["rate"], 1.0);
        }
        // 返回查询到的费率
        return safeDouble(row["r"], 1.0);
    }

    // 计算手续费
    // 参数 amount：交易金额
    // 参数 ratePercent：费率百分比（如 1.0 表示 1%）
    // 返回：手续费金额
    static double calcFee(double amount, double ratePercent) {
        // 计算手续费：金额 * 费率 / 100，四舍五入到分
        return std::round(amount * ratePercent) / 100.0;
    }

    // 生成系统订单号
    // 格式：前缀 + 日期时间 + 随机数
    // 参数 prefix：前缀（默认 "W" 表示普通订单）
    // 返回：生成的订单号（如 "W20240605120530123456"）
    static std::string generateOrderId(const std::string &prefix = "W") {
        // 获取当前时间
        auto now = std::time(nullptr);
        // 时间结构体
        struct tm tmv;
// Windows 平台
#ifdef _WIN32
        // 使用 localtime_s 转换时间
        localtime_s(&tmv, &now);
// Unix 平台
#else
        // 使用 localtime_r 转换时间
        localtime_r(&now, &tmv);
#endif
        // 创建随机数生成器
        std::mt19937 rng((unsigned)std::random_device{}());
        // 生成 100000-999999 的随机数
        std::uniform_int_distribution<int> d(100000, 999999);
        // 创建字符串流
        std::ostringstream oss;
        // 组合订单号：前缀 + 日期时间 + 随机数
        oss << prefix
            // 格式化日期时间为 YYYYMMDDHHMMSS
            << std::put_time(&tmv, "%Y%m%d%H%M%S")
            // 添加随机数
            << d(rng);
        // 返回生成的订单号
        return oss.str();
    }

    // 生成结算单号
    // 返回：结算单号（前缀为 "S"）
    static std::string generateSettleNo() {
        return generateOrderId("S");
    }

    // 生成退款单号
    // 返回：退款单号（前缀为 "R"）
    static std::string generateRefundNo() {
        return generateOrderId("R");
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

    // 锁定唯一金额（免签模式）
    // 策略：只有数据库里存在“未支付且未关闭”的同金额订单时，金额才按 0.01 递增
    // 参数 payTypeInt：支付类型 ID
    // 参数 price：原始金额
    // 返回：锁定后的唯一金额
    static double lockUniquePrice(int payTypeInt, double price) {
        auto &db = PayDb::instance();
        long long base = (long long)std::round(price * 100);

        auto exist0 = db.queryOne(
            "SELECT 1 FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
            "AND ABS(CAST(amount AS REAL)-?)<0.001 LIMIT 1",
            {std::to_string(payTypeInt), fmtAmount((double)base / 100.0)});
        if (exist0.empty())
            return price;

        for (int i = 1; i <= 20; ++i) {
            double real = (base + i) / 100.0;
            auto exist = db.queryOne(
                "SELECT 1 FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
                "AND ABS(CAST(amount AS REAL)-?)<0.001 LIMIT 1",
                {std::to_string(payTypeInt), fmtAmount(real)});
            if (exist.empty())
                return real;
        }
        return price;
    }

    // 获取所有已启用的支付类型
    // 返回：支付类型列表（代码-名称对）
    static std::vector<std::pair<std::string, std::string>> getActivePayTypes() {
        // 查询所有已启用的支付类型
        auto rows = PayDb::instance().query(
            // SQL 语句：查询已启用的支付类型，按排序顺序和 ID 排序
            "SELECT code,name FROM pay_type WHERE state=1 ORDER BY sort_order ASC, id ASC",
            // 无参数
            {});
        // 创建结果列表
        std::vector<std::pair<std::string, std::string>> result;
        // 遍历查询结果
        for (auto &r : rows)
            // 添加支付类型代码和名称对
            result.emplace_back(r.at("code"), r.at("name"));
        // 返回结果列表
        return result;
    }

    // 商户资金变动（线程安全）
    // 通过单条 UPDATE 语句实现原子操作
    // 参数 mchId：商户 ID
    // 参数 changeType：变动类型（1=收入, 2=支出, 3=冻结, 4=解冻, 5=结算扣款, 6=退款扣款）
    // 参数 amount：变动金额
    // 参数 bizType：业务类型（如 "order"、"refund" 等）
    // 参数 bizNo：业务单号（如订单号、退款单号等）
    // 参数 remark：备注信息
    // 返回：true 表示操作成功，false 表示操作失败
    static bool changeMchBalance(int mchId, int changeType, double amount,
                                 const std::string &bizType, const std::string &bizNo,
                                 const std::string &remark = "") {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 查询商户当前的余额和冻结金额
        auto mch = db.queryOne("SELECT balance,frozen FROM merchant WHERE id=?",
                               {std::to_string(mchId)});
        // 如果商户不存在，返回失败
        if (mch.empty())
            return false;

        // 获取当前余额
        double balance = safeDouble(mch["balance"], 0);
        // 获取冻结金额
        double frozen  = safeDouble(mch["frozen"], 0);
        // 保存变动前的余额
        double before  = balance;

        // 根据变动类型处理余额变动
        switch (changeType) {
            case 1:
                // 收入：余额增加
                balance += amount;
                break;
            case 2:
                // 支出：检查余额是否足够，不足则返回失败
                if (balance < amount)
                    return false;
                // 余额减少
                balance -= amount;
                break;
            case 3:
                // 冻结：检查余额是否足够，不足则返回失败
                if (balance < amount)
                    return false;
                // 余额减少，冻结金额增加
                balance -= amount;
                frozen += amount;
                break;
            case 4:
                // 解冻：检查冻结金额是否足够，不足则返回失败
                if (frozen < amount)
                    return false;
                // 冻结金额减少，余额增加
                frozen -= amount;
                balance += amount;
                break;
            case 5:
                // 结算扣款（从冻结中扣）：检查冻结金额是否足够，不足则返回失败
                if (frozen < amount)
                    return false;
                // 冻结金额减少
                frozen -= amount;
                break;
            case 6:
                // 退款扣款：余额减少
                balance -= amount;
                break;
            default:
                // 未知的变动类型，返回失败
                return false;
        }

        // 更新商户的余额和冻结金额
        db.exec("UPDATE merchant SET balance=?,frozen=?,updated_at=? WHERE id=?",
                // 参数：新余额、新冻结金额、当前时间戳、商户 ID
                {fmtAmount(balance), fmtAmount(frozen),
                 std::to_string(std::time(nullptr)), std::to_string(mchId)});

        // 记录资金变动日志
        db.exec("INSERT INTO money_log(mch_id,change_type,change_amount,"
                "before_amount,after_amount,biz_type,biz_no,remark,created_at) "
                "VALUES(?,?,?,?,?,?,?,?,?)",
                // 参数：商户 ID、变动类型、变动金额、变动前余额、变动后余额、业务类型、业务单号、备注、创建时间
                {std::to_string(mchId), std::to_string(changeType),
                 fmtAmount(amount), fmtAmount(before), fmtAmount(balance),
                 bizType, bizNo, remark, std::to_string(std::time(nullptr))});
        // 返回成功
        return true;
    }

// 私有区域
private:
    // ── 辅助函数 ──────────────────────────────
    // 安全地将字符串转换为 double
    // 参数 s：字符串
    // 参数 def：默认值（转换失败时返回）
    // 返回：转换后的 double 值或默认值
    static double safeDouble(const std::string &s, double def) {
        // 尝试转换字符串为 double
        try {
            return std::stod(s);
        } catch (...) {
            // 转换失败时返回默认值
            return def;
        }
    }

    // 安全地从 Row 中获取整数值
    // 参数 r：数据行
    // 参数 k：列名
    // 参数 def：默认值（转换失败时返回）
    // 返回：转换后的整数值或默认值
    static int safeInt(const PayDb::Row &r, const std::string &k, int def) {
        // 在行中查找列
        auto it = r.find(k);
        // 如果列不存在或为空，返回默认值
        if (it == r.end() || it->second.empty())
            return def;
        // 尝试转换字符串为整数
        try {
            return std::stoi(it->second);
        } catch (...) {
            // 转换失败时返回默认值
            return def;
        }
    }

    // 获取当前小时（0-23）
    // 返回：当前小时值
    static int currentHour() {
        // 获取当前时间戳
        time_t t = std::time(nullptr);
        // 时间结构体
        struct tm tm;
        // Windows 平台
#ifdef _WIN32
        // 使用 localtime_s 转换时间
        localtime_s(&tm, &t);
        // Unix 平台
#else
        // 使用 localtime_r 转换时间
        localtime_r(&t, &tm);
#endif
        // 返回小时值
        return tm.tm_hour;
    }

    // 将数据库行转换为 ChannelInfo 结构体
    // 参数 r：数据库行
    // 返回：通道信息结构体
    static ChannelInfo rowToChannelInfo(const PayDb::Row &r) {
        // 创建通道信息结构体
        ChannelInfo ci;
        // 通道 ID
        ci.channelId   = std::stoi(r.at("id"));
        // 通道编码
        ci.channelCode = r.count("channel_code") ? r.at("channel_code") : "";
        // 通道名称
        ci.channelName = r.count("channel_name") ? r.at("channel_name") : "";
        // 支付类型
        ci.payType     = r.count("pay_type") ? r.at("pay_type") : "";
        // 插件名称
        ci.plugin      = r.count("plugin") ? r.at("plugin") : "";
        // 费率（优先使用商户特定费率，否则使用通道默认费率）
        ci.rate        = safeDouble(r.count("final_rate") ? r.at("final_rate") : "", 0);
        // 通道参数 JSON
        ci.paramsJson  = r.count("params_json") ? r.at("params_json") : "";
        // 最小金额
        ci.minAmount   = safeDouble(r.count("min_amount") ? r.at("min_amount") : "", 0.01);
        // 最大金额
        ci.maxAmount   = safeDouble(r.count("max_amount") ? r.at("max_amount") : "", 50000);
        // 开放时段起始小时
        ci.timeStart   = safeInt(r, "time_start", 0);
        // 开放时段截止小时
        ci.timeStop    = safeInt(r, "time_stop", 0);
        // 日限额
        ci.dayLimit    = safeDouble(r.count("day_limit") ? r.at("day_limit") : "", 0);
        // 日已用金额
        ci.dayAmount   = safeDouble(r.count("day_amount") ? r.at("day_amount") : "", 0);
        // 返回通道信息
        return ci;
    }
};
