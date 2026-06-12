// WePay-Cpp — 许可证客户端
// 授权模式：
//   1. 本地 license.lic 校验（HMAC签名 + 硬件指纹 + 有效期）
//   2. 可选远程验证（config.json 配置 server + key）
//   3. 守护线程定期重检，到期自动终止服务
//   4. 无有效授权 → 禁止启动
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include <drogon/drogon.h>
#include "SyncHttp.h"
#include "HardwareFingerprint.h"

class LicenseClient {
public:
    enum class Status {
        VALID,              // 验证通过
        EXPIRING_SOON,      // 即将到期（30天内）
        EXPIRED,            // 已过期
        HARDWARE_MISMATCH,  // 硬件指纹不匹配
        HARDWARE_UPGRADED,  // 主因子匹配（硬件升级宽容）
        INVALID_SIGNATURE,  // 签名无效（文件被篡改）
        FILE_NOT_FOUND,     // license.lic 不存在
        PARSE_ERROR,        // 文件格式错误
        REJECTED,           // 远程服务器拒绝
        SERVER_ERROR,       // 远程服务器错误
    };

    struct LicenseInfo {
        Status      status = Status::FILE_NOT_FOUND;
        std::string licensee;       // 被授权方名称
        std::string expireDate;     // "PERPETUAL" 或 "YYYY-MM-DD"
        int         daysLeft = 0;
        int         maxUsers = 0;   // 0=不限
        std::string features;       // 功能模块，逗号分隔
        int         graceDays = 7;  // 离线宽限天数
        std::string message;        // 服务器消息
        int64_t     lastOnline = 0; // 上次成功联网时间戳
    };

    static LicenseClient& instance() {
        static LicenseClient inst;
        return inst;
    }

    // 从 config.json 读取可选的远程服务器配置
    void configure(const Json::Value& cfg) {
        if (!cfg.isMember("license")) return;
        auto& lc = cfg["license"];
        serverUrl_    = lc.get("server", "").asString();
        licenseKey_   = lc.get("key", "").asString();
        graceDays_    = lc.get("grace_days", 7).asInt();
        heartbeatMin_ = lc.get("heartbeat_minutes", 60).asInt();
        if (!serverUrl_.empty() && !licenseKey_.empty())
            remoteConfigured_ = true;
    }

    // 启动验证（阻塞，main中调用）
    // 必须有有效授权才能启动：
    //   1. 先读本地 license.lic（license_tool 签发的）
    //   2. 本地无效时尝试远程验证（如果配置了 server+key）
    //   3. 都无效 → 拒绝启动
    bool verify() {
        // 采集硬件指纹
        auto fac = HardwareFingerprint::computeFactors();
        fpHash_    = HardwareFingerprint::sha256hex(fac.full);
        fpPrimary_ = HardwareFingerprint::sha256hex(fac.primary);

        // ── 第一步：本地 license.lic 校验 ──
        if (verifyLocalFile()) {
            LOG_INFO << "[License] 本地授权验证通过: " << info_.licensee;
            startHeartbeat(); // 守护线程：定期重检有效期，到期自动终止
            return isAllowed();
        }

        // ── 第二步：远程验证（可选）──
        if (remoteConfigured_) {
            if (remoteVerify()) {
                saveCacheFile();
                startHeartbeat();
                return isAllowed();
            }
            // 远程不可达，尝试本地缓存
            if (loadCacheFile()) {
                int64_t now = std::time(nullptr);
                int offlineDays = (int)((now - info_.lastOnline) / 86400);
                if (offlineDays <= graceDays_) {
                    LOG_WARN << "[License] 使用远程缓存（离线第" << offlineDays << "天）";
                    startHeartbeat();
                    return true;
                }
            }
        }

        // 无有效授权
        return false;
    }

    bool isAllowed() const {
        switch (info_.status) {
            case Status::VALID:
            case Status::EXPIRING_SOON:
            case Status::HARDWARE_UPGRADED:
                return true;
            default:
                return false;
        }
    }

    bool hasFeature(const std::string& feature) const {
        if (!isAllowed()) return false;
        if (info_.features == "FULL" || info_.features.find("FULL") != std::string::npos)
            return true;
        std::istringstream ss(info_.features);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            // trim
            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            if (tok == feature) return true;
        }
        return false;
    }

    const LicenseInfo& info() const { return info_; }

    // 输出硬件指纹到 license.lic，供用户发给厂商申请授权
    // 若文件已存在则不覆盖（可能是厂商下发的有效授权）
    static void exportFingerprintFile(const std::string& path = "license.lic") {
        if (std::ifstream(path).good()) return; // 已有文件，不覆盖

        auto fac = HardwareFingerprint::computeFactors();
        std::string fpFull    = HardwareFingerprint::sha256hex(fac.full);
        std::string fpPrimary = HardwareFingerprint::sha256hex(fac.primary);

        time_t now = std::time(nullptr);
        char buf[20];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

        std::ofstream f(path, std::ios::trunc);
        if (!f) return;
        f << "# WePay 授权申请文件\n"
          << "# 请将此文件发送给厂商以获取正式授权\n"
          << "# 厂商签发后会替换此文件内容\n"
          << "fp_hash=" << fpFull << "\n"
          << "fp_primary=" << fpPrimary << "\n"
          << "machine_id=" << fac.primary << "\n"
          << "mac=" << fac.mac << "\n"
          << "cpu=" << fac.cpu << "\n"
          << "generated=" << buf << "\n";
        f.close();
        LOG_INFO << "[License] 已生成 " << path << "，请发送给厂商申请授权";
    }

    // 控制台输出授权信息（彩色）
    void printPanel() const {
        const char* LINE = "============================================================";
        std::cout << "\n\x1b[1;36m" << LINE << "\x1b[0m\n";
        std::cout << "  \x1b[1;32m[WePay]\x1b[0m 许可证信息\n";
        std::cout << "\x1b[1;36m" << LINE << "\x1b[0m\n";

        auto statusStr = [](Status s) -> std::string {
            switch (s) {
                case Status::VALID:             return "\x1b[1;32m✅ 有效\x1b[0m";
                case Status::EXPIRING_SOON:     return "\x1b[1;33m⚠️  即将到期\x1b[0m";
                case Status::EXPIRED:           return "\x1b[1;31m❌ 已过期\x1b[0m";
                case Status::HARDWARE_MISMATCH: return "\x1b[1;31m❌ 硬件不匹配\x1b[0m";
                case Status::HARDWARE_UPGRADED: return "\x1b[1;33m⚠️  硬件已升级（宽容）\x1b[0m";
                case Status::INVALID_SIGNATURE: return "\x1b[1;31m❌ 签名无效（文件被篡改）\x1b[0m";
                case Status::FILE_NOT_FOUND:    return "\x1b[1;31m❌ license.lic 未找到\x1b[0m";
                case Status::PARSE_ERROR:       return "\x1b[1;31m❌ 文件未签发\x1b[0m";
                case Status::REJECTED:          return "\x1b[1;31m❌ 远程授权被拒绝\x1b[0m";
                case Status::SERVER_ERROR:      return "\x1b[1;31m❌ 远程服务器错误\x1b[0m";
                default:                        return "未知";
            }
        };

        if (isAllowed()) {
            std::cout << "  被授权方: \x1b[1;97m" << info_.licensee << "\x1b[0m\n";
            std::cout << "  有效期  : \x1b[1;97m" << info_.expireDate << "\x1b[0m\n";
            if (info_.expireDate != "PERPETUAL") {
                if (info_.daysLeft > 0)
                    std::cout << "  剩余天数: \x1b[1;33m" << info_.daysLeft << " 天\x1b[0m\n";
                else
                    std::cout << "  已过期  : \x1b[1;31m" << -info_.daysLeft << " 天前\x1b[0m\n";
            }
            std::cout << "  功能模块: \x1b[38;2;148;0;211m" << info_.features << "\x1b[0m\n";
            if (info_.maxUsers > 0)
                std::cout << "  最大用户: " << info_.maxUsers << "\n";
        }
        std::cout << "  状态    : " << statusStr(info_.status) << "\n";
        if (!info_.message.empty())
            std::cout << "  消息    : " << info_.message << "\n";
        std::cout << "\x1b[1;36m" << LINE << "\x1b[0m\n";
        if (!isAllowed()) {
            std::cout << "\n  \x1b[1;33m请将 license.lic 发送给厂商获取正式授权\x1b[0m\n";
            std::cout << "  \x1b[1;33m或在 config.json 配置远程授权服务器\x1b[0m\n\n";
        }
    }

    void stop() {
        heartbeatRunning_ = false;
        if (heartbeatThread_.joinable()) heartbeatThread_.join();
    }

private:
    LicenseClient() = default;
    ~LicenseClient() { stop(); }

    bool remoteConfigured_ = false;
    std::string serverUrl_;
    std::string licenseKey_;
    std::string fpHash_;
    std::string fpPrimary_;
    int graceDays_ = 7;
    int heartbeatMin_ = 60;
    LicenseInfo info_;
    std::mutex mtx_;
    std::thread heartbeatThread_;
    std::atomic<bool> heartbeatRunning_{false};

    // ── HMAC 密钥（与 license_tool 一致）─────────────────────
    static std::string _licKey() {
        static const uint8_t a[] = {0x4D,0xA3,0x7F,0xC1,0x98,0x2E,0x5B,0x60};
        static const uint8_t b[] = {0xE7,0x14,0x8D,0x3A,0xF6,0x51,0x29,0xBC};
        static const uint8_t c[] = {0x07,0x9E,0x4C,0xD5,0x72,0xAB,0x1F,0x83};
        static const uint8_t d[] = {0x6E,0x30,0xC4,0x57,0x8A,0x1D,0x95,0x42};
        std::vector<uint8_t> k;
        k.insert(k.end(), std::begin(a), std::end(a));
        k.insert(k.end(), std::begin(b), std::end(b));
        k.insert(k.end(), std::begin(c), std::end(c));
        k.insert(k.end(), std::begin(d), std::end(d));
        std::string r(reinterpret_cast<const char*>(k.data()), k.size());
        std::fill(k.begin(), k.end(), 0);
        return r;
    }

    static int _daysUntil(const std::string& ds) {
        if (ds == "PERPETUAL") return 99999;
        int y=0, m=0, d=0;
        if (sscanf(ds.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -99999;
        std::tm t = {};
        t.tm_year = y-1900; t.tm_mon = m-1; t.tm_mday = d;
        t.tm_hour = 23; t.tm_min = 59; t.tm_sec = 59;
        return (int)((mktime(&t) - time(nullptr)) / 86400);
    }

    // ── 本地 license.lic 校验 ─────────────────────────────────
    bool verifyLocalFile(const std::string& path = "license.lic") {
        std::ifstream f(path);
        if (!f.is_open()) {
            info_.status = Status::FILE_NOT_FOUND;
            return false;
        }

        // 解析 key=value
        std::string line, body, sig;
        std::string licensee, fpHash, fpPrimary, issueDate, expireDate, features;
        int maxUsers = 0, graceDays = 7;

        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);

            if      (k == "licensee")    { licensee   = v; body += line + "\n"; }
            else if (k == "fp_hash")     { fpHash     = v; body += line + "\n"; }
            else if (k == "fp_primary")  { fpPrimary  = v; body += line + "\n"; }
            else if (k == "issue_date")  { issueDate  = v; body += line + "\n"; }
            else if (k == "expire_date") { expireDate = v; body += line + "\n"; }
            else if (k == "max_users")   { try { maxUsers = std::stoi(v); } catch(...){} body += line + "\n"; }
            else if (k == "features")    { features   = v; body += line + "\n"; }
            else if (k == "grace_days")  { try { graceDays = std::stoi(v); } catch(...){} body += line + "\n"; }
            else if (k == "machine_id")  { body += line + "\n"; }
            else if (k == "mac")         { body += line + "\n"; }
            else if (k == "cpu")         { body += line + "\n"; }
            else if (k == "sig")         { sig = v; }
        }

        // 无签名 = 未签发的申请文件
        if (sig.empty()) {
            info_.status = Status::PARSE_ERROR;
            LOG_WARN << "[License] license.lic 无签名（未签发的申请文件）";
            return false;
        }

        // 验签
        std::string key = _licKey();
        std::string expected = HardwareFingerprint::hmacHex(body, key);
        std::fill(key.begin(), key.end(), 0);
        if (expected != sig) {
            info_.status = Status::INVALID_SIGNATURE;
            LOG_ERROR << "[License] license.lic 签名无效（文件被篡改）";
            return false;
        }

        // 硬件指纹校验
        if (fpHash != fpHash_) {
            if (fpPrimary == fpPrimary_) {
                info_.status = Status::HARDWARE_UPGRADED;
                LOG_WARN << "[License] 硬件指纹部分变化（主因子匹配，宽容通过）";
            } else {
                info_.status = Status::HARDWARE_MISMATCH;
                LOG_ERROR << "[License] 硬件指纹不匹配，此授权不属于本机";
                return false;
            }
        }

        // 有效期
        int daysLeft = _daysUntil(expireDate);
        if (daysLeft < -graceDays) {
            info_.status = Status::EXPIRED;
            LOG_ERROR << "[License] 授权已过期 " << -daysLeft << " 天";
            return false;
        }

        // 填充 info
        info_.licensee  = licensee;
        info_.expireDate = expireDate;
        info_.daysLeft  = daysLeft;
        info_.maxUsers  = maxUsers;
        info_.features  = features;
        info_.graceDays = graceDays;

        if (info_.status != Status::HARDWARE_UPGRADED) {
            info_.status = (daysLeft < 30) ? Status::EXPIRING_SOON : Status::VALID;
        }
        return true;
    }

    // ── 远程验证 ─────────────────────────────────────────────
    // POST {license_key, fp_hash, fp_primary, action:"verify"}
    // 响应 {code:0, licensee, expire_date, days_left, max_users, features, message}
    bool remoteVerify() {
        Json::Value req;
        req["license_key"] = licenseKey_;
        req["fp_hash"]     = fpHash_;
        req["fp_primary"]  = fpPrimary_;
        req["action"]      = "verify";
        req["version"]     = "2.0";

        Json::StreamWriterBuilder wb;
        std::string body = Json::writeString(wb, req);
        std::string url  = serverUrl_ + "/api/license/verify";

        auto resp = SyncHttp::postJson(url, body, {}, 10);
        if (!resp.success || resp.status != 200) {
            LOG_WARN << "[License] 远程验证请求失败: "
                     << (resp.success ? std::to_string(resp.status) : resp.errMsg);
            return false;
        }

        return parseResponse(resp.body);
    }

    // ── 心跳 ─────────────────────────────────────────────────
    // POST {license_key, fp_hash, action:"heartbeat"}
    bool remoteHeartbeat() {
        Json::Value req;
        req["license_key"] = licenseKey_;
        req["fp_hash"]     = fpHash_;
        req["action"]      = "heartbeat";

        Json::StreamWriterBuilder wb;
        std::string body = Json::writeString(wb, req);
        std::string url  = serverUrl_ + "/api/license/heartbeat";

        auto resp = SyncHttp::postJson(url, body, {}, 10);
        if (!resp.success || resp.status != 200) return false;
        return parseResponse(resp.body);
    }

    bool parseResponse(const std::string& body) {
        Json::Value root;
        Json::CharReaderBuilder rb;
        std::istringstream ss(body);
        std::string errs;
        if (!Json::parseFromStream(rb, ss, &root, &errs)) return false;

        int code = root.get("code", -1).asInt();
        if (code != 0) {
            info_.status  = Status::REJECTED;
            info_.message = root.get("message", "授权被拒绝").asString();
            return true; // 通信成功，但被拒
        }

        std::lock_guard<std::mutex> lk(mtx_);
        info_.licensee  = root.get("licensee", "").asString();
        info_.expireDate = root.get("expire_date", "PERPETUAL").asString();
        info_.daysLeft  = root.get("days_left", 99999).asInt();
        info_.maxUsers  = root.get("max_users", 0).asInt();
        info_.features  = root.get("features", "FULL").asString();
        info_.graceDays = root.get("grace_days", graceDays_).asInt();
        info_.message   = root.get("message", "").asString();
        info_.lastOnline = std::time(nullptr);

        if (info_.daysLeft < 0)
            info_.status = Status::EXPIRED;
        else if (info_.daysLeft < 30)
            info_.status = Status::EXPIRING_SOON;
        else
            info_.status = Status::VALID;

        return true;
    }

    void startHeartbeat() {
        if (heartbeatRunning_.load()) return;
        heartbeatRunning_ = true;
        heartbeatThread_ = std::thread([this] {
            LOG_INFO << "[License] 授权守护线程启动，检查间隔 " << heartbeatMin_ << " 分钟";
            while (heartbeatRunning_.load()) {
                std::this_thread::sleep_for(std::chrono::minutes(heartbeatMin_));
                if (!heartbeatRunning_.load()) break;

                // ── 重检本地 license.lic 有效期 ──
                if (!verifyLocalFile()) {
                    LOG_ERROR << "[License] 授权已失效（" << _statusName(info_.status)
                              << "），服务将在 10 秒后终止...";
                    std::cerr << "\n[License] 授权已失效，服务即将终止！请续期 license.lic\n" << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                    drogon::app().quit();
                    return;
                }

                // ── 远程心跳（可选）──
                if (remoteConfigured_) {
                    if (remoteHeartbeat()) {
                        saveCacheFile();
                    } else {
                        LOG_WARN << "[License] 远程心跳失败，下次重试";
                    }
                }
            }
        });
    }

    static const char* _statusName(Status s) {
        switch (s) {
            case Status::EXPIRED:           return "已过期";
            case Status::HARDWARE_MISMATCH: return "硬件不匹配";
            case Status::INVALID_SIGNATURE: return "签名无效";
            case Status::FILE_NOT_FOUND:    return "文件缺失";
            case Status::PARSE_ERROR:       return "格式错误";
            default:                        return "无效";
        }
    }

    // ── 本地缓存文件 (.license_cache) ────────────────────────
    // 存储: JSON + HMAC签名，防篡改
    static std::string cachePath() { return ".license_cache"; }

    void saveCacheFile() {
        Json::Value j;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            j["licensee"]    = info_.licensee;
            j["expire_date"] = info_.expireDate;
            j["days_left"]   = info_.daysLeft;
            j["max_users"]   = info_.maxUsers;
            j["features"]    = info_.features;
            j["grace_days"]  = info_.graceDays;
            j["last_online"] = (Json::Int64)info_.lastOnline;
            j["fp_hash"]     = fpHash_;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string payload = Json::writeString(wb, j);
        std::string sig = HardwareFingerprint::hmacHex(payload,
            HardwareFingerprint::sha256hex(licenseKey_ + fpHash_));

        std::ofstream f(cachePath(), std::ios::trunc);
        if (f) {
            f << payload << "\n" << sig << "\n";
            f.close();
        }
    }

    bool loadCacheFile() {
        std::ifstream f(cachePath());
        if (!f) return false;
        std::string payload, sig;
        std::getline(f, payload);
        std::getline(f, sig);
        f.close();
        if (payload.empty() || sig.empty()) return false;

        // 验签
        std::string expected = HardwareFingerprint::hmacHex(payload,
            HardwareFingerprint::sha256hex(licenseKey_ + fpHash_));
        if (expected != sig) {
            LOG_WARN << "[License] 本地缓存签名无效，已忽略";
            return false;
        }

        Json::Value j;
        Json::CharReaderBuilder rb;
        std::istringstream ss(payload);
        std::string errs;
        if (!Json::parseFromStream(rb, ss, &j, &errs)) return false;

        // 验证指纹匹配
        if (j.get("fp_hash", "").asString() != fpHash_) return false;

        std::lock_guard<std::mutex> lk(mtx_);
        info_.licensee  = j.get("licensee", "").asString();
        info_.expireDate = j.get("expire_date", "PERPETUAL").asString();
        info_.daysLeft  = j.get("days_left", 0).asInt();
        info_.maxUsers  = j.get("max_users", 0).asInt();
        info_.features  = j.get("features", "FULL").asString();
        info_.graceDays = j.get("grace_days", graceDays_).asInt();
        info_.lastOnline = j.get("last_online", (Json::Int64)0).asInt64();
        return true;
    }
};

// 控制器中检查功能模块的宏
#define CHECK_LICENSE_FEATURE(cb, feature) \
    if (!LicenseClient::instance().hasFeature(feature)) { \
        Json::Value _r; _r["code"] = 403; \
        _r["msg"] = std::string("当前许可证未授权功能: ") + feature; \
        auto _resp = drogon::HttpResponse::newHttpJsonResponse(_r); \
        (cb)(_resp); return; \
    }
