// WePay-Cpp — 邮件通知免签插件 (alipay_email)
//
// 原理:
//   1. 商户配置个人支付宝收款码 + 接收收款通知的邮箱 IMAP 信息
//   2. 下单时分配唯一金额(+0~99 分偏移) → 展示收款码
//   3. 用户扫码付款 → 支付宝发收款通知邮件到指定邮箱
//   4. 后台定时 IMAP 轮询 → 解析邮件中的金额 → 匹配待支付订单 → 自动确认
//
// 无需登录支付宝后台 / 无需浏览器 / 无需 API 资质 / 极其稳定
//
// 通道参数(params_json):
//   qrcode_url           : 收款码内容(必填)
//   qrcode_image         : 收款码图片 URL(可选)
//   amount_offset_max    : 最大偏移分数(默认99)
//   receipt_valid_seconds : 订单有效期秒(默认300)
//   email_imap_host      : IMAP 服务器(必填, 如 imap.qq.com)
//   email_imap_port      : IMAP 端口(默认993)
//   email_username       : 邮箱地址(必填)
//   email_password       : 邮箱密码/授权码(必填)
//   email_search_minutes : 搜索最近 N 分钟邮件(默认30)
#pragma once
#include "ChannelPlugin.h"
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../common/PaymentService.h"
#include <mutex>
#include <cmath>
#include <random>
#include <regex>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <curl/curl.h>

class AlipayEmailPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "alipay_email"; }

    Json::Value paramSchema() const override {
        Json::Value s(Json::arrayValue);
        auto add = [&](const char* k, const char* l, const char* t,
                       const char* d, const char* h, bool req = false) {
            Json::Value f;
            f["key"]=k; f["label"]=l; f["type"]=t;
            f["default"]=d; f["help"]=h; f["required"]=req;
            s.append(f);
        };
        add("qrcode_url", "收款码内容", "input", "",
            "支付宝个人收款码 URL", true);
        add("qrcode_image", "收款码图片", "image", "",
            "展示给用户的收款码图片地址", false);
        add("amount_offset_max", "最大偏移(分)", "number", "99",
            "金额偏移范围 0~N 分", false);
        add("receipt_valid_seconds", "有效期(秒)", "number", "300",
            "订单有效期", false);
        add("email_imap_host", "IMAP服务器", "input", "imap.qq.com",
            "如 imap.qq.com / imap.163.com / imap.gmail.com", true);
        add("email_imap_port", "IMAP端口", "number", "993", "", false);
        add("email_username", "邮箱地址", "input", "", "", true);
        add("email_password", "邮箱密码/授权码", "password", "",
            "QQ邮箱填授权码", true);
        add("email_search_minutes", "搜索范围(分钟)", "number", "30",
            "搜索最近 N 分钟的邮件", false);
        return s;
    }

    // ── 下单: 分配唯一金额 ──
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        auto &db = PayDb::instance();

        // 从 pay_qrcode 表读取收款码
        int typeInt = (req.payType == "alipay") ? 2 :
                      (req.payType == "qqpay") ? 3 : 1;
        auto qr = db.queryOne(
            "SELECT * FROM pay_qrcode WHERE type=? AND state=0 "
            "AND (plugin_code='alipay_email' OR plugin_code='' OR plugin_code IS NULL) "
            "ORDER BY id ASC LIMIT 1",
            {std::to_string(typeInt)});
        // 回退到 params_json 配置
        std::string qrcodeUrl = qr.empty() ? p.get("qrcode_url", "").asString()
                                           : (qr.count("pay_url") ? qr["pay_url"] : "");
        std::string qrcodeImage = qr.empty() ? p.get("qrcode_image", "").asString() : "";
        if (qrcodeUrl.empty()) {
            result.errMsg = "未配置收款码，请在收款码/监控码Tab添加";
            return result;
        }

        int maxOffset = p.get("amount_offset_max", 99).asInt();
        if (maxOffset <= 0) maxOffset = 99;
        int validSec = p.get("receipt_valid_seconds", 300).asInt();
        if (validSec <= 0) validSec = 300;

        // 分配唯一金额
        double realAmount = -1;
        {
            static std::mutex mtx;
            std::lock_guard<std::mutex> lock(mtx);

            long long cutoff = std::time(nullptr) - validSec;
            long long baseCent = (long long)std::round(req.amount * 100);

            for (int off = 0; off <= maxOffset; ++off) {
                double candidate = (baseCent + off) / 100.0;
                std::string cStr = ChannelService::fmtAmount(candidate);

                auto dup = db.queryOne(
                    "SELECT order_id FROM pay_order "
                    "WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
                    "AND ABS(CAST(real_amount AS REAL)-CAST(? AS REAL))<0.001 "
                    "AND created_at>?",
                    {req.payType, cStr, std::to_string(cutoff)});

                if (dup.empty()) {
                    realAmount = candidate;
                    break;
                }
            }
        }

        if (realAmount < 0) {
            result.errMsg = "同金额订单过多, 请稍后再试";
            return result;
        }

        // ext_json
        Json::Value ext;
        ext["plugin"] = "alipay_email";
        ext["original_amount"] = ChannelService::fmtAmount(req.amount);
        ext["receipt_amount"] = ChannelService::fmtAmount(realAmount);
        ext["auto_match"] = true;
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string extJson = Json::writeString(wb, ext);

        db.exec("UPDATE pay_order SET real_amount=?,channel_data=? WHERE order_id=?",
                {ChannelService::fmtAmount(realAmount), extJson, req.orderId});

        // 金额锁
        db.exec("INSERT INTO tmp_price(oid,price) VALUES(?,?)",
                {req.orderId, ChannelService::fmtAmount(realAmount)});

        result.success = true;
        result.payUrl = qrcodeUrl;
        result.qrCode = qrcodeUrl;
        result.extra["real_amount"] = ChannelService::fmtAmount(realAmount);
        result.extra["need_confirm"] = false; // 全自动，用户无需手动确认
        result.extra["qrcode_image"] = qrcodeImage;
        return result;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &,
        const std::string &,
        const Json::Value &) override
    {
        ChannelNotifyResult r;
        r.verified = true;
        r.paid = true;
        r.responseText = "success";
        return r;
    }

    // ══════════════════════════════════════════════════════════
    //  IMAP 邮件轮询: 从指定邮箱搜索支付宝收款通知邮件
    // ══════════════════════════════════════════════════════════

    struct MailPayment {
        std::string amount;   // "12.34"
        std::string subject;  // 邮件主题
        std::string uid;      // 邮件 UID (去重用)
    };

    // 对所有使用 alipay_email 插件的已安装通道执行 IMAP 轮询
    static void pollAllChannels() {
        auto &db = PayDb::instance();
        auto channels = db.query(
            "SELECT id,params_json FROM pay_channel "
            "WHERE plugin='alipay_email' AND state=1", {});

        for (auto &ch : channels) {
            try {
                Json::Value params;
                {
                    Json::CharReaderBuilder rb;
                    std::istringstream ss(ch["params_json"]);
                    std::string errs;
                    Json::parseFromStream(rb, ss, &params, &errs);
                }
                pollChannel(ch["id"], params);
            } catch (const std::exception &e) {
                LOG_ERROR << "[AlipayEmail] channel " << ch["id"]
                          << " poll error: " << e.what();
            }
        }
    }

private:
    // 轮询单个通道的邮箱
    static void pollChannel(const std::string &channelId,
                             const Json::Value &params)
    {
        std::string host = params.get("email_imap_host", "").asString();
        int port = params.get("email_imap_port", 993).asInt();
        std::string user = params.get("email_username", "").asString();
        std::string pass = params.get("email_password", "").asString();
        int searchMinutes = params.get("email_search_minutes", 30).asInt();
        if (searchMinutes <= 0) searchMinutes = 30;

        if (host.empty() || user.empty() || pass.empty()) return;

        // 构造 IMAP URL
        std::string imapUrl = "imaps://" + host + ":" + std::to_string(port) + "/INBOX";

        // 搜索最近 N 分钟内来自支付宝的未读邮件
        // IMAP SEARCH 命令: UNSEEN FROM "service@alipay.com" SINCE "18-May-2026"
        std::string sinceDate = imapDateStr(searchMinutes);
        std::string searchCmd = "UNSEEN FROM \"service@alipay.com\" SINCE \"" + sinceDate + "\"";

        // 获取匹配的邮件 UID 列表
        auto uids = imapSearch(imapUrl, user, pass, searchCmd);
        if (uids.empty()) return;

        auto &db = PayDb::instance();

        for (auto &uid : uids) {
            // 检查是否已处理过
            std::string uidKey = "email_uid_" + channelId + "_" + uid;
            if (!db.getSetting(uidKey).empty()) continue;

            // 获取邮件内容
            std::string body = imapFetchBody(imapUrl, user, pass, uid);
            if (body.empty()) continue;

            // 解析金额
            std::string amount = parseAlipayAmount(body);
            if (amount.empty()) continue;

            // 标记已处理
            db.setSetting(uidKey, std::to_string(std::time(nullptr)));

            // 尝试匹配订单
            std::string matched = PaymentService::processPayment("alipay", amount);
            if (!matched.empty()) {
                LOG_INFO << "[AlipayEmail] ch=" << channelId
                         << " 邮件匹配成功: " << amount << " → " << matched;
            } else {
                LOG_DEBUG << "[AlipayEmail] ch=" << channelId
                          << " 金额 " << amount << " 未匹配到订单";
            }
        }
    }

    // ── IMAP SEARCH: 返回 UID 列表 ──
    static std::vector<std::string> imapSearch(
        const std::string &imapUrl,
        const std::string &user,
        const std::string &pass,
        const std::string &searchCmd)
    {
        std::vector<std::string> uids;
        CURL *curl = curl_easy_init();
        if (!curl) return uids;

        std::string response;
        std::string url = imapUrl;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

        // IMAP SEARCH via custom request
        std::string custom = "SEARCH " + searchCmd;
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, custom.c_str());

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            LOG_ERROR << "[AlipayEmail] IMAP SEARCH error: "
                      << curl_easy_strerror(res);
            return uids;
        }

        // 解析搜索结果: "* SEARCH 1 2 3\r\n"
        // libcurl IMAP 返回的 UID 列表
        std::istringstream ss(response);
        std::string line;
        while (std::getline(ss, line)) {
            // 去除 \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // libcurl 返回格式: 每行一个 UID 或 "* SEARCH uid1 uid2 ..."
            if (line.find("* SEARCH") != std::string::npos) {
                std::istringstream ls(line.substr(line.find("SEARCH") + 7));
                std::string uid;
                while (ls >> uid) {
                    if (!uid.empty() && std::isdigit(uid[0]))
                        uids.push_back(uid);
                }
            } else {
                // libcurl 也可能直接返回以空格分隔的序号
                std::istringstream ls(line);
                std::string token;
                while (ls >> token) {
                    if (!token.empty() && std::isdigit(token[0]))
                        uids.push_back(token);
                }
            }
        }

        return uids;
    }

    // ── IMAP FETCH: 获取指定 UID 邮件正文 ──
    static std::string imapFetchBody(
        const std::string &imapUrl,
        const std::string &user,
        const std::string &pass,
        const std::string &uid)
    {
        CURL *curl = curl_easy_init();
        if (!curl) return "";

        std::string response;
        // FETCH 特定邮件: imaps://host:port/INBOX/;UID=<uid>
        std::string url = imapUrl + "/;UID=" + uid;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            LOG_ERROR << "[AlipayEmail] IMAP FETCH error uid=" << uid
                      << ": " << curl_easy_strerror(res);
            return "";
        }

        return response;
    }

    // ── 解析支付宝收款通知邮件中的金额 ──
    // 支付宝邮件典型格式:
    //   主题: "收款 xx.xx 元到支付宝"
    //   正文(HTML): 含 "收款金额" "xx.xx元" 等
    static std::string parseAlipayAmount(const std::string &mailBody) {
        // 先尝试从 Subject 解析
        // Subject: =?UTF-8?B?xxx?= 或 Subject: 收款 12.34 元到支付宝
        std::string decoded = decodeMailContent(mailBody);

        // 模式1: "收款 xx.xx 元" (Subject 或正文)
        {
            std::regex re(R"(收款\s*([\d]+\.[\d]{1,2})\s*元)");
            std::smatch m;
            if (std::regex_search(decoded, m, re)) {
                return normalizeAmount(m[1].str());
            }
        }

        // 模式2: "收款金额" 后跟金额
        {
            std::regex re(R"(收款金额[：:\s]*([\d]+\.[\d]{1,2}))");
            std::smatch m;
            if (std::regex_search(decoded, m, re)) {
                return normalizeAmount(m[1].str());
            }
        }

        // 模式3: 纯数字金额 "¥xx.xx" 或 "￥xx.xx"
        {
            std::regex re(R"([¥￥]\s*([\d]+\.[\d]{1,2}))");
            std::smatch m;
            if (std::regex_search(decoded, m, re)) {
                return normalizeAmount(m[1].str());
            }
        }

        // 模式4: HTML 中 amount 相关标签
        {
            std::regex re(R"(amount[^>]*>([\d]+\.[\d]{1,2}))");
            std::smatch m;
            if (std::regex_search(decoded, m, re)) {
                return normalizeAmount(m[1].str());
            }
        }

        return "";
    }

    // 解码邮件内容 (处理 Base64、Quoted-Printable 编码)
    static std::string decodeMailContent(const std::string &raw) {
        std::string result = raw;

        // 解码 Base64 编码的 Subject
        {
            std::regex re(R"(=\?[Uu][Tt][Ff]-8\?[Bb]\?([A-Za-z0-9+/=]+)\?=)");
            std::smatch m;
            std::string tmp = result;
            while (std::regex_search(tmp, m, re)) {
                std::string decoded = base64Decode(m[1].str());
                result.replace(result.find(m[0].str()), m[0].str().size(), decoded);
                tmp = m.suffix().str();
            }
        }

        // 解码 Quoted-Printable (=XX)
        {
            std::string decoded;
            decoded.reserve(result.size());
            for (size_t i = 0; i < result.size(); ++i) {
                if (result[i] == '=' && i + 2 < result.size()) {
                    char h = result[i+1], l = result[i+2];
                    if (h == '\r' || h == '\n') {
                        // 软换行
                        if (h == '\r' && i + 3 < result.size() && result[i+3] == '\n') ++i;
                        ++i;
                        continue;
                    }
                    if (std::isxdigit(h) && std::isxdigit(l)) {
                        int val = 0;
                        if (h >= '0' && h <= '9') val = (h - '0') << 4;
                        else if (h >= 'A' && h <= 'F') val = (h - 'A' + 10) << 4;
                        else if (h >= 'a' && h <= 'f') val = (h - 'a' + 10) << 4;
                        if (l >= '0' && l <= '9') val |= l - '0';
                        else if (l >= 'A' && l <= 'F') val |= l - 'A' + 10;
                        else if (l >= 'a' && l <= 'f') val |= l - 'a' + 10;
                        decoded += (char)val;
                        i += 2;
                        continue;
                    }
                }
                decoded += result[i];
            }
            result = std::move(decoded);
        }

        return result;
    }

    // Base64 解码
    static std::string base64Decode(const std::string &in) {
        static const std::string chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -8;
        for (unsigned char c : in) {
            if (c == '=') break;
            auto pos = chars.find((char)c);
            if (pos == std::string::npos) continue;
            val = (val << 6) | (int)pos;
            valb += 6;
            if (valb >= 0) {
                out.push_back((char)((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    // 格式化金额为 "xx.xx"
    static std::string normalizeAmount(const std::string &raw) {
        double v = 0;
        try { v = std::stod(raw); } catch (...) { return ""; }
        if (v <= 0) return "";
        return ChannelService::fmtAmount(v);
    }

    // 生成 IMAP SINCE 日期字符串: "18-May-2026"
    static std::string imapDateStr(int minutesAgo) {
        time_t t = std::time(nullptr) - minutesAgo * 60;
        struct tm tm;
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d-%s-%04d",
                 tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
        return std::string(buf);
    }

    // libcurl 写回调
    static size_t writeCallback(void *data, size_t size, size_t nmemb, void *userp) {
        auto *s = static_cast<std::string*>(userp);
        s->append(static_cast<char*>(data), size * nmemb);
        return size * nmemb;
    }
};

REGISTER_CHANNEL_PLUGIN(AlipayEmailPlugin);
