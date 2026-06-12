// WePay-Cpp — 对账与自动结算服务
// 职责：
//   1. 日结对账：系统订单 vs 通道上游数据，检测少报/金额差异/重复回调
//   2. T+1 自动结算触发：根据商户冻结余额自动生成结算单
//   3. 差异告警：发现异常时写入 reconcile_diff 表并触发告警
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include "PayDb.h"
#include "ChannelService.h"

// 对账差异类型枚举
enum class ReconcileDiffType {
    // 平台已支付但未收到回调
    MISSING_CALLBACK = 1,
    // 金额不匹配
    AMOUNT_MISMATCH  = 2,
    // 重复回调
    DUPLICATE_CALLBACK= 3,
    // 订单被撤销（上游退票）
    REVERSAL         = 4,
    // 未知差异
    UNKNOWN          = 99
};

// 单条对账差异记录
struct ReconcileDiff {
    // 差异记录 ID
    int64_t id = 0;
    // 对账日期（YYYY-MM-DD 格式）
    std::string reconcile_date;
    // 商户 ID
    int mch_id = 0;
    // 支付通道代码
    std::string channel_code;
    // 平台订单 ID
    std::string order_id;
    // 通道订单号
    std::string channel_order_no;
    // 差异类型（ReconcileDiffType）
    int diff_type = 0;
    // 平台记录的金额
    double platform_amount = 0;
    // 上游记录的金额
    double upstream_amount = 0;
    // 差异描述
    std::string diff_detail;
    // 处理状态（0=待处理 1=已确认 2=已忽略）
    int state = 0;
    // 创建时间戳
    int64_t created_at = 0;
    // 更新时间戳
    int64_t updated_at = 0;
};

// 对账与自动结算服务类
// 职责：
//   1. 日结对账：系统订单 vs 通道上游数据，检测少报/金额差异/重复回调
//   2. T+1 自动结算触发：根据商户冻结余额自动生成结算单
//   3. 差异告警：发现异常时写入 reconcile_diff 表并触发告警
class ReconcileService {
public:
    // 初始化对账差异表
    // 创建 reconcile_diff 表和相关索引（幂等操作）
    static void initTable() {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 创建对账差异表
        db.execSqliteDirect(
            "CREATE TABLE IF NOT EXISTS reconcile_diff("
            // 主键 ID
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            // 对账日期（YYYY-MM-DD）
            "reconcile_date TEXT NOT NULL,"
            // 商户 ID
            "mch_id INTEGER NOT NULL,"
            // 支付通道代码
            "channel_code TEXT NOT NULL DEFAULT '',"
            // 平台订单 ID
            "order_id TEXT NOT NULL DEFAULT '',"
            // 通道订单号
            "channel_order_no TEXT NOT NULL DEFAULT '',"
            // 差异类型
            "diff_type INTEGER NOT NULL DEFAULT 0,"
            // 平台记录的金额
            "platform_amount TEXT NOT NULL DEFAULT '0.00',"
            // 上游记录的金额
            "upstream_amount TEXT NOT NULL DEFAULT '0.00',"
            // 差异描述
            "diff_detail TEXT NOT NULL DEFAULT '',"
            // 处理状态
            "state INTEGER NOT NULL DEFAULT 0,"
            // 创建时间戳
            "created_at INTEGER NOT NULL DEFAULT 0,"
            // 更新时间戳
            "updated_at INTEGER NOT NULL DEFAULT 0)");
        // 创建索引以加快查询
        db.execSqliteDirect(
            "CREATE INDEX IF NOT EXISTS idx_recon_diff_date ON reconcile_diff(reconcile_date, mch_id)");
    }

    // 日结对账（定时任务调用）
    // 比对日期范围内平台已支付订单与上游记录是否一致
    // 检测少报、金额差异、重复回调等异常
    // 参数 date：对账日期（YYYY-MM-DD 格式）
    // 参数 upstreamData：上游数据（channel_code -> channel_order_no -> amount）
    // 返回：统计信息（total、ok、missing、amount_mismatch）
    static std::map<std::string, int> reconcileDay(
        const std::string &date,
        const std::map<std::string, std::map<std::string, double>> &upstreamData
    ) {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 计算日期范围的开始时间戳
        long long dayStart = parseDate(date);
        // 计算日期范围的结束时间戳
        long long dayEnd   = dayStart + 86400;
        // 创建统计信息字典
        std::map<std::string, int> stats;
        // 初始化统计计数
        stats["total"] = 0;
        stats["missing"] = 0;
        stats["amount_mismatch"] = 0;
        stats["ok"] = 0;

        // ── 第 1 步：取平台已支付订单 ──────────────────────────────
        // 查询指定日期范围内所有已支付的订单
        auto orders = db.query(
            "SELECT order_id,mch_id,channel_order_no,amount,real_amount,pay_time,pay_type "
            "FROM pay_order WHERE state=1 AND pay_time>=? AND pay_time<?",
            {std::to_string(dayStart), std::to_string(dayEnd)});

        // 已处理订单集合（用于去重）
        std::set<std::string> handled;
        // 遍历所有已支付订单
        for (auto &o : orders) {
            // 获取订单 ID
            std::string oid = o.at("order_id");
            // 获取通道订单号
            std::string cono = o.count("channel_order_no") ? o.at("channel_order_no") : "";
            // 获取平台记录的金额
            double platAmt = safeDouble(o.at("amount"), 0);
            // 获取商户 ID
            int mchId = safeInt(o, "mch_id", 0);
            // 获取支付类型
            std::string payType = o.count("pay_type") ? o.at("pay_type") : "";

            // 增加总订单计数
            stats["total"]++;

            // ── 查找对应上游通道码 ──────────────────────────────
            std::string chCode;
            // 如果有通道订单号，查询对应的通道代码
            if (!cono.empty()) {
                auto ch = db.queryOne(
                    "SELECT c.channel_code FROM pay_channel c "
                    "JOIN pay_order o2 ON o2.channel_id=c.id "
                    "WHERE o2.order_id=?", {oid});
                // 如果查询成功，获取通道代码
                if (!ch.empty())
                    chCode = ch.at("channel_code");
            }

            // ── 第 2 步：检查上游是否有该订单记录 ──────────────────────────────
            bool upstreamFound = false;
            // 如果有通道代码且上游有该通道的数据
            if (!chCode.empty() && upstreamData.count(chCode) && !cono.empty()) {
                // 在上游数据中查找该订单
                auto it = upstreamData.at(chCode).find(cono);
                // 如果找到该订单
                if (it != upstreamData.at(chCode).end()) {
                    // 标记为已找到
                    upstreamFound = true;
                    // 获取上游记录的金额
                    double upAmt = it->second;
                    // 比较金额（允许 0.01 元的误差）
                    if (std::abs(platAmt - upAmt) > 0.01) {
                        // 金额不匹配，记录差异
                        insertDiff(db, date, mchId, chCode, oid, cono,
                            ReconcileDiffType::AMOUNT_MISMATCH, platAmt, upAmt,
                            "金额不匹配: 平台=" + fmtAmt(platAmt) + " 上游=" + fmtAmt(upAmt));
                        // 增加金额不匹配计数
                        stats["amount_mismatch"]++;
                    } else {
                        // 金额匹配，增加成功计数
                        stats["ok"]++;
                    }
                }
            }

            // ── 第 3 步：检查平台有但上游未找到的订单 ──────────────────────────────
            // 如果平台有通道订单号但上游未找到，视为少报
            if (!cono.empty() && !upstreamFound && !upstreamData.empty()) {
                // 有上游数据但没有此订单，说明可能是掉单
                insertDiff(db, date, mchId, chCode, oid, cono,
                    ReconcileDiffType::MISSING_CALLBACK, platAmt, 0,
                    "平台已支付但上游未查到订单，可能掉单");
                // 增加少报计数
                stats["missing"]++;
            }

            // 标记订单已处理
            handled.insert(oid);
        }

        // ── 第 4 步：检查上游有多出订单（平台没有但上游有） ──────────────────────────────
        // 遍历所有上游数据
        for (auto &[chCode, ordersMap] : upstreamData) {
            // 遍历该通道的所有订单
            for (auto &[cono, upAmt] : ordersMap) {
                // 在平台数据中查找该订单
                auto exist = db.queryOne(
                    "SELECT order_id FROM pay_order WHERE channel_order_no=? AND state=1",
                    {cono});
                // 如果平台没有该订单
                if (exist.empty()) {
                    // 在支付通道表中查找通道信息
                    auto chRow = db.queryOne(
                        "SELECT c.id FROM pay_channel c WHERE LOWER(c.channel_code)=LOWER(?) LIMIT 1",
                        {chCode});
                    // 记录差异：上游有但平台没有
                    insertDiff(db, date, 0, chCode, "", cono,
                        ReconcileDiffType::MISSING_CALLBACK, 0, upAmt,
                        "上游有订单但平台未处理，上游金额=" + fmtAmt(upAmt));
                    // 增加少报计数
                    stats["missing"]++;
                }
            }
        }

        // 记录对账完成日志
        LOG_INFO << "[Reconcile] " << date << " 对账完成: total=" << stats["total"]
                 << " ok=" << stats["ok"] << " missing=" << stats["missing"]
                 << " amount_mismatch=" << stats["amount_mismatch"];
        // 返回统计信息
        return stats;
    }

    // ── 查询对账差异列表 ─────────────────────────────────────
    static std::vector<ReconcileDiff> listDiffs(const std::string &date,
                                                  int mchId = 0,
                                                  int diffType = -1,
                                                  int page = 1,
                                                  int size = 50) {
        auto &db = PayDb::instance();
        std::string where = "WHERE reconcile_date=?";
        std::vector<std::string> params{date};
        if (mchId > 0) { where += " AND mch_id=?"; params.push_back(std::to_string(mchId)); }
        if (diffType >= 0) { where += " AND diff_type=?"; params.push_back(std::to_string(diffType)); }

        int offset = (page - 1) * size;
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string(offset));

        auto rows = db.query(
            "SELECT * FROM reconcile_diff " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", pp);

        std::vector<ReconcileDiff> result;
        for (auto &r : rows) {
            ReconcileDiff d;
            d.id = safeInt(r, "id", 0);
            d.reconcile_date = r.at("reconcile_date");
            d.mch_id = safeInt(r, "mch_id", 0);
            d.channel_code = r.count("channel_code") ? r.at("channel_code") : "";
            d.order_id = r.count("order_id") ? r.at("order_id") : "";
            d.channel_order_no = r.count("channel_order_no") ? r.at("channel_order_no") : "";
            d.diff_type = safeInt(r, "diff_type", 0);
            d.platform_amount = safeDouble(r, "platform_amount", 0);
            d.upstream_amount = safeDouble(r, "upstream_amount", 0);
            d.diff_detail = r.count("diff_detail") ? r.at("diff_detail") : "";
            d.state = safeInt(r, "state", 0);
            d.created_at = safeInt64(r, "created_at", 0);
            d.updated_at = safeInt64(r, "updated_at", 0);
            result.push_back(d);
        }
        return result;
    }

    // ── 处理差异（确认/忽略） ─────────────────────────────────
    static bool resolveDiff(int64_t diffId, int state, const std::string &remark = "") {
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        std::string sql;
        std::vector<std::string> params;
        if (remark.empty()) {
            sql = "UPDATE reconcile_diff SET state=?,updated_at=? WHERE id=?";
            params = {std::to_string(state), std::to_string(now), std::to_string(diffId)};
        } else {
            sql = "UPDATE reconcile_diff SET state=?,updated_at=?,diff_detail=diff_detail||? WHERE id=?";
            params = {std::to_string(state), std::to_string(now),
                       " | " + remark, std::to_string(diffId)};
        }
        return db.exec(sql, params);
    }

    // ── T+1 自动结算触发 ─────────────────────────────────────
    // 检查每个商户的冻结余额，达到起结金额则自动生成结算单
    static int autoSettle(const std::string &date,   // T日，即昨日
                          double minSettleAmount = 100.0) {
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);

        // 取所有有冻结余额的商户
        auto mchRows = db.query(
            "SELECT id,mch_no,mch_name,frozen FROM merchant WHERE frozen>='0.01' AND state=1", {});
        int count = 0;
        for (auto &m : mchRows) {
            double frozen = safeDouble(m.at("frozen"), 0);
            if (frozen < minSettleAmount) continue;

            int mchId = safeInt(m, "id", 0);
            std::string mchNo = m.count("mch_no") ? m.at("mch_no") : "";

            // 查询结算账户
            auto acc = db.queryOne(
                "SELECT account_no,bank_name,account_name FROM mch_account "
                "WHERE mch_id=? AND state=1 ORDER BY id DESC LIMIT 1",
                {std::to_string(mchId)});

            // 查询 T 日收入（只结算已确收的冻结金额）
            // 冻结金额来源：订单入账（change_type=3，即冻结）
            // 结算时从 frozen 中扣减（change_type=5）
            std::string settleNo = ChannelService::generateSettleNo();

            Json::Value accInfo;
            if (!acc.empty()) {
                accInfo["account_no"] = acc.count("account_no") ? acc.at("account_no") : "";
                accInfo["bank_name"]  = acc.count("bank_name") ? acc.at("bank_name") : "";
                accInfo["account_name"] = acc.count("account_name") ? acc.at("account_name") : "";
            }
            Json::StreamWriterBuilder wb; wb["indentation"] = "";
            std::string accJson = Json::writeString(wb, accInfo);

            // 计算结算手续费（默认 0，即平台不收手续费）
            double fee = 0;
            std::string feeStr = db.getSetting("settle_fee", "0");
            try { fee = std::stod(feeStr); } catch (...) {}
            double realAmt = frozen - fee;

            // 生成结算单
            bool ok = db.exec(
                "INSERT INTO settle_order(settle_no,mch_id,amount,fee,real_amount,"
                "account_info,state,admin_remark,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,0,?,?,?)",
                {settleNo, std::to_string(mchId),
                 ChannelService::fmtAmount(frozen),
                 ChannelService::fmtAmount(fee),
                 ChannelService::fmtAmount(realAmt),
                 accJson,
                 "系统自动结算 (" + date + ")",
                 std::to_string(now), std::to_string(now)});
            if (ok) {
                LOG_INFO << "[Reconcile] 自动结算单已生成: " << settleNo
                         << " mchId=" << mchId << " amount=" << frozen;
                count++;
            }
        }
        return count;
    }

    // ── 日汇总报表 ──────────────────────────────────────────
    struct DayReport {
        std::string date;
        int mch_id = 0;
        int total_orders = 0;
        double total_amount = 0;
        double total_fee = 0;
        double total_income = 0;  // 商户净收入
        int refund_count = 0;
        double refund_amount = 0;
        double settle_amount = 0;
    };

    static DayReport genDayReport(const std::string &date, int mchId = 0) {
        auto &db = PayDb::instance();
        long long dayStart = parseDate(date);
        long long dayEnd = dayStart + 86400;
        DayReport r;
        r.date = date;
        r.mch_id = mchId;

        std::string mchFilter = (mchId > 0) ? "AND mch_id=" + std::to_string(mchId) : "";

        auto o = db.queryOne(
            "SELECT COUNT(*) as cnt,SUM(CAST(amount AS REAL)) as sum_amt,"
            "SUM(CAST(mch_fee AS REAL)) as sum_fee "
            "FROM pay_order WHERE state=1 AND pay_time>=? AND pay_time<? " + mchFilter,
            {std::to_string(dayStart), std::to_string(dayEnd)});
        if (!o.empty()) {
            r.total_orders = safeInt(o, "cnt", 0);
            r.total_amount = safeDouble(o, "sum_amt", 0);
            r.total_fee    = safeDouble(o, "sum_fee", 0);
            r.total_income = r.total_amount - r.total_fee;
        }

        auto ref = db.queryOne(
            "SELECT COUNT(*) as cnt,SUM(CAST(refund_amount AS REAL)) as sum_ref "
            "FROM refund_order WHERE state=1 AND updated_at>=? AND updated_at<? " + mchFilter,
            {std::to_string(dayStart), std::to_string(dayEnd)});
        if (!ref.empty()) {
            r.refund_count  = safeInt(ref, "cnt", 0);
            r.refund_amount = safeDouble(ref, "sum_ref", 0);
        }

        auto st = db.queryOne(
            "SELECT SUM(CAST(amount AS REAL)) as sum_st "
            "FROM settle_order WHERE state=2 AND updated_at>=? AND updated_at<? " + mchFilter,
            {std::to_string(dayStart), std::to_string(dayEnd)});
        if (!st.empty()) {
            r.settle_amount = safeDouble(st, "sum_st", 0);
        }

        return r;
    }

private:
    static void insertDiff(PayDb &db, const std::string &date,
                          int mchId, const std::string &chCode,
                          const std::string &oid, const std::string &cono,
                          ReconcileDiffType dtype, double platAmt, double upAmt,
                          const std::string &detail) {
        long long now = std::time(nullptr);
        // 幂等：同日期+同商户+同通道+同订单号 不重复插入
        std::string existSql = "SELECT id FROM reconcile_diff WHERE reconcile_date=? "
                               "AND mch_id=? AND channel_code=? AND order_id=? AND diff_type=?";
        auto exist = db.queryOne(existSql,
            {date, std::to_string(mchId), chCode, oid, std::to_string((int)dtype)});
        if (!exist.empty()) return;

        db.exec(
            "INSERT INTO reconcile_diff(reconcile_date,mch_id,channel_code,order_id,channel_order_no,"
            "diff_type,platform_amount,upstream_amount,diff_detail,state,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,0,?,?)",
            {date, std::to_string(mchId), chCode, oid, cono,
             std::to_string((int)dtype),
             ChannelService::fmtAmount(platAmt),
             ChannelService::fmtAmount(upAmt),
             detail, std::to_string(now), std::to_string(now)});
    }

    static long long parseDate(const std::string &date) {
        int y = 0, m = 0, d = 0;
        char c1 = 0, c2 = 0;
        std::istringstream iss(date);
        iss >> y >> c1 >> m >> c2 >> d;
        struct tm t; t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
        t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
        return (long long)std::mktime(&t);
    }

    static double safeDouble(const std::string &s, double def) {
        try { return std::stod(s); } catch (...) { return def; }
    }
    static int safeInt(const std::map<std::string, std::string> &r,
                       const std::string &k, int def) {
        auto it = r.find(k);
        if (it == r.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }
    static int64_t safeInt64(const std::map<std::string, std::string> &r,
                             const std::string &k, int64_t def) {
        auto it = r.find(k);
        if (it == r.end() || it->second.empty()) return def;
        try { return std::stoll(it->second); } catch (...) { return def; }
    }
    static std::string fmtAmt(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }
};
