// WePay-Cpp — 风控服务 ( api/Pay.php 风控逻辑)
// 功能:
//   1. IP 限频: 同一 IP 短时间内下单次数限制
//   2. 域名白名单: notify_url 域名须在商户授权列表内
//   3. 商品名黑名单: 过滤违规商品关键词
//   4. 每日支付限额: 商户/用户每日累计支付限额
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include <unordered_map> // 哈希表
#include <mutex> // 互斥锁
#include <vector> // 向量容器
#include <algorithm> // 算法库
#include "PayDb.h" // 数据库操作

class RiskControlService {
public:
    struct CheckResult {
        bool pass = true;
        std::string reason;
    };

    // 综合风控检查 (下单前调用)
    static CheckResult check(int mchId, const std::string &clientIp,
                              const std::string &notifyUrl,
                              const std::string &productName,
                              double amount) {
        CheckResult r;
        auto &db = PayDb::instance();

        // 1. IP 限频
        std::string ipLimitStr = db.getSetting("risk_ip_limit", "0");
        int ipLimit = 0;
        try { ipLimit = std::stoi(ipLimitStr); } catch (...) {}
        if (ipLimit > 0 && !clientIp.empty()) {
            if (!checkIpRate(clientIp, ipLimit)) {
                r.pass = false;
                r.reason = "操作过于频繁，请稍后再试";
                return r;
            }
        }

        // 2. 域名白名单
        std::string domainCheck = db.getSetting("risk_domain_check", "0");
        if (domainCheck == "1" && !notifyUrl.empty()) {
            std::string host = extractHost(notifyUrl);
            if (!host.empty() && !isDomainAllowed(db, mchId, host)) {
                r.pass = false;
                r.reason = "域名未授权，请到支付平台添加域名白名单";
                return r;
            }
        }

        // 3. 商品名黑名单
        std::string blacklist = db.getSetting("risk_product_blacklist", "");
        if (!blacklist.empty() && !productName.empty()) {
            auto keywords = split(blacklist, ',');
            std::string lowerName = toLower(productName);
            for (auto &kw : keywords) {
                std::string lkw = toLower(trim(kw));
                if (!lkw.empty() && lowerName.find(lkw) != std::string::npos) {
                    r.pass = false;
                    r.reason = "商品名称包含违规关键词";
                    return r;
                }
            }
        }

        // 4. 商户每日支付限额
        std::string dailyLimitStr = db.getSetting("risk_daily_limit", "0");
        double dailyLimit = 0;
        try { dailyLimit = std::stod(dailyLimitStr); } catch (...) {}
        if (dailyLimit > 0) {
            auto row = db.queryOne(
                "SELECT COALESCE(SUM(CAST(amount AS REAL)),0) AS total "
                "FROM pay_order WHERE mch_id=? AND state=1 AND DATE(created_at)=DATE('now','localtime')",
                {std::to_string(mchId)});
            double today = 0;
            if (!row.empty()) try { today = std::stod(row["total"]); } catch (...) {}
            if (today + amount > dailyLimit) {
                r.pass = false;
                r.reason = "已达到每日支付限额";
                return r;
            }
        }

        // 5. 用户每日支付限额 (按 IP)
        std::string userDailyStr = db.getSetting("risk_user_daily_limit", "0");
        double userDaily = 0;
        try { userDaily = std::stod(userDailyStr); } catch (...) {}
        if (userDaily > 0 && !clientIp.empty()) {
            auto row = db.queryOne(
                "SELECT COALESCE(SUM(CAST(amount AS REAL)),0) AS total "
                "FROM pay_order WHERE client_ip=? AND state=1 AND DATE(created_at)=DATE('now','localtime')",
                {clientIp});
            double today = 0;
            if (!row.empty()) try { today = std::stod(row["total"]); } catch (...) {}
            if (today + amount > userDaily) {
                r.pass = false;
                r.reason = "用户今日支付金额已达上限";
                return r;
            }
        }

        return r;
    }

    // 清理过期的 IP 记录 (定期调用)
    static void cleanup() {
        std::lock_guard<std::mutex> lk(mtx_);
        long long now = std::time(nullptr);
        for (auto it = ipCounter_.begin(); it != ipCounter_.end(); ) {
            if (now - it->second.windowStart > 60)
                it = ipCounter_.erase(it);
            else
                ++it;
        }
    }

private:
    struct IpRecord {
        int count = 0;
        long long windowStart = 0;
    };
    static inline std::unordered_map<std::string, IpRecord> ipCounter_;
    static inline std::mutex mtx_;

    // IP 限频: 60秒窗口内不超过 limit 次
    static bool checkIpRate(const std::string &ip, int limit) {
        std::lock_guard<std::mutex> lk(mtx_);
        long long now = std::time(nullptr);
        auto &rec = ipCounter_[ip];
        if (now - rec.windowStart > 60) {
            rec.count = 1;
            rec.windowStart = now;
            return true;
        }
        rec.count++;
        return rec.count <= limit;
    }

    // 域名白名单检查
    static bool isDomainAllowed(PayDb &db, int mchId, const std::string &host) {
        auto rows = db.query(
            "SELECT domain FROM merchant_domain WHERE mch_id=? AND state=1",
            {std::to_string(mchId)});
        if (rows.empty()) return true; // 未配置则不限制
        for (auto &r : rows) {
            std::string domain = r.at("domain");
            if (domain == host) return true;
            // 支持通配符 *.example.com
            if (domain.size() > 2 && domain[0] == '*' && domain[1] == '.') {
                std::string suffix = domain.substr(1);
                if (host.size() >= suffix.size() &&
                    host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
                    return true;
            }
        }
        return false;
    }

    static std::string extractHost(const std::string &url) {
        size_t start = url.find("://");
        if (start == std::string::npos) start = 0; else start += 3;
        size_t end = url.find('/', start);
        if (end == std::string::npos) end = url.size();
        std::string host = url.substr(start, end - start);
        // remove port
        size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
        return host;
    }

    static std::string toLower(const std::string &s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    }

    static std::string trim(const std::string &s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static std::vector<std::string> split(const std::string &s, char delim) {
        std::vector<std::string> r;
        std::string token;
        for (char c : s) {
            if (c == delim) { r.push_back(token); token.clear(); }
            else token += c;
        }
        if (!token.empty()) r.push_back(token);
        return r;
    }
};
