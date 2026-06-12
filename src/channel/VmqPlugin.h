#pragma once
#include "ChannelPlugin.h"
#include "../common/ChannelService.h"
#include "../common/EpaySign.h"
#include "../common/Md5Utils.h"
#include "../common/NotifyTaskService.h"
#include "../common/PayDb.h"
#include "../common/WsBus.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

class VmqPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "vmq"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt; if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("key", "监听密钥", "password", "", "留空使用 setting.key 平台密钥");
        add("require_online", "要求监听端在线才允许下单", "switch", "false",
            "true 时若 jkstate=0 直接拒单");
        add("lock_price", "金额浮动锁", "switch", "true",
            "并发同金额订单时自动浮动 0.01 避免冲突");
        add("payQf", "金额浮动方向", "select", "1",
            "1=递增 2=递减 (与 V免签 fox 配置项 payQf 一致)");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        int typeInt = payTypeToInt(req.payType);
        if (typeInt == 0) { result.errMsg = "不支持的VMQ支付类型"; return result; }
        auto &db = PayDb::instance();
        Json::Value p = req.channelParams;
        bool requireOnline = p.get("require_online", false).asBool();
        if (requireOnline && db.getSetting("jkstate", "0") != "1") {
            result.errMsg = "监听端离线";
            return result;
        }
        bool lockPrice = p.get("lock_price", db.getSetting("payQf", "1") == "1").asBool();
        double realAmount = req.amount;
        if (lockPrice && !lockUniqueAmount(req.orderId, req.amount, realAmount)) {
            result.errMsg = "无法锁定唯一金额";
            return result;
        }
        auto qr = selectQrcode(typeInt, realAmount, p);
        if (qr.empty()) {
            PayDb::instance().exec("DELETE FROM tmp_price WHERE oid=?", {req.orderId});
            result.errMsg = "暂无可用收款码";
            return result;
        }
        std::string payUrl = qr.count("pay_url") ? qr["pay_url"] : "";
        db.exec("UPDATE pay_order SET real_amount=?,pay_url=?,qrcode=?,raw_response=? WHERE order_id=?",
                {ChannelService::fmtAmount(realAmount), payUrl, payUrl, rowJson(qr), req.orderId});
        result.success = true;
        result.payUrl = payUrl;
        result.qrCode = payUrl;
        result.channelOrderNo = req.orderId;
        result.rawResponse = rowJson(qr);
        result.extra["real_amount"] = ChannelService::fmtAmount(realAmount);
        result.extra["qrcode_id"] = qr.count("id") ? qr.at("id") : "0";
        return result;
    }

    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &) override {
        ChannelQueryResult r;
        auto row = PayDb::instance().queryOne("SELECT state,real_amount,channel_order_no FROM pay_order WHERE order_id=?", {orderId});
        if (row.empty()) return r;
        r.success = true;
        r.tradeState = row["state"] == "1" ? 1 : (row["state"] == "-1" ? -1 : 0);
        r.channelOrderNo = row.count("channel_order_no") ? row["channel_order_no"] : orderId;
        try { r.paidAmount = std::stod(row.count("real_amount") ? row["real_amount"] : "0"); } catch (...) {}
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        std::string type = get(params, "type");
        std::string price = get(params, "price");
        std::string t = get(params, "t");
        std::string sign = get(params, "sign");
        std::string key = channelParams.get("key", PayDb::instance().getSetting("key", "")).asString();
        if (type.empty() || price.empty()) return r;
        if (!sign.empty() && !t.empty() && Md5Utils::pushSign(type, price, t, key) != sign) return r;
        auto order = findPendingOrder(type, fmtPrice(price));
        if (order.empty()) return r;
        r.verified = true;
        r.paid = true;
        r.orderId = order["order_id"];
        r.channelOrderNo = order["order_id"];
        r.paidAmount = toDouble(order.count("real_amount") ? order.at("real_amount") : price);
        r.responseText = "success";
        return r;
    }

    // 尝试用平台 key 与所有商户 vmq_key 校验签名，返回匹配的 (mchId,key)
    // mchId == -1 表示平台 key 命中。
    // 多 V免签 fork 心跳签名公式枚举
    static std::vector<std::string> heartSignVariants(const std::string &t,
                                                      const std::string &k) {
        return {
            Md5Utils::md5(t + k),                    // 标准 V免签 fox
            Md5Utils::md5(k + t),                    // 部分 fork
            Md5Utils::md5(t + k + t),                // 罕见
            Md5Utils::md5(k + t + k),                // 罕见
            Md5Utils::md5("t=" + t + "&key=" + k),   // url 风格
            Md5Utils::md5("t=" + t + "key=" + k)
        };
    }

    static bool resolveHeartKey(const std::string &t, const std::string &sign,
                                int &outMchId, std::string &outKey) {
        if (t.empty() || sign.empty()) return false;
        auto &db = PayDb::instance();

        auto tryKey = [&](const std::string &key) -> bool {
            for (auto &v : heartSignVariants(t, key)) {
                if (v == sign) return true;
            }
            return false;
        };

        std::string sysKey = db.getSetting("key", "");
        if (!sysKey.empty()) {
            auto vs = heartSignVariants(t, sysKey);
            LOG_INFO << "[heartKey] try platform key=" << sysKey
                     << " v1=" << vs[0] << " v2=" << vs[1] << " got=" << sign;
            if (tryKey(sysKey)) { outMchId = -1; outKey = sysKey; return true; }
        }
        auto rows = db.query(
            "SELECT id,vmq_key FROM merchant WHERE state=1 AND vmq_key<>''", {});
        LOG_INFO << "[heartKey] merchants with vmq_key=" << rows.size();
        for (auto &r : rows) {
            const std::string &mk = r.at("vmq_key");
            if (tryKey(mk)) {
                try { outMchId = std::stoi(r.at("id")); } catch (...) { outMchId = 0; }
                outKey = mk; return true;
            }
        }
        auto rows2 = db.query(
            "SELECT id,mch_key FROM merchant WHERE state=1 AND mch_key<>''", {});
        for (auto &r : rows2) {
            const std::string &mk = r.at("mch_key");
            if (tryKey(mk)) {
                LOG_INFO << "[heartKey] matched mch_key for mch=" << r.at("id");
                try { outMchId = std::stoi(r.at("id")); } catch (...) { outMchId = 0; }
                outKey = mk; return true;
            }
        }
        return false;
    }

    static bool resolvePushKey(const std::string &type, const std::string &price,
                               const std::string &t, const std::string &sign,
                               int &outMchId, std::string &outKey) {
        if (sign.empty() || t.empty()) {
            // 监听端可能用未签名直推（旧 mpay 风格），允许平台 key 兜底
            outMchId = -1; outKey = PayDb::instance().getSetting("key", ""); return true;
        }
        auto &db = PayDb::instance();
        std::string sysKey = db.getSetting("key", "");
        if (!sysKey.empty()) {
            std::string c = Md5Utils::pushSign(type, price, t, sysKey);
            LOG_INFO << "[pushKey] try platform key=" << sysKey
                     << " expected=" << c << " got=" << sign;
            if (c == sign) { outMchId = -1; outKey = sysKey; return true; }
        }
        auto rows = db.query(
            "SELECT id,vmq_key FROM merchant WHERE state=1 AND vmq_key<>''", {});
        LOG_INFO << "[pushKey] merchants with vmq_key=" << rows.size();
        for (auto &r : rows) {
            const std::string &mk = r.at("vmq_key");
            std::string c = Md5Utils::pushSign(type, price, t, mk);
            LOG_INFO << "[pushKey] try mch=" << r.at("id") << " key=" << mk
                     << " expected=" << c;
            if (c == sign) {
                try { outMchId = std::stoi(r.at("id")); } catch (...) { outMchId = 0; }
                outKey = mk; return true;
            }
        }
        // 兜底再试 mch_key
        auto rows2 = db.query(
            "SELECT id,mch_key FROM merchant WHERE state=1 AND mch_key<>''", {});
        for (auto &r : rows2) {
            const std::string &mk = r.at("mch_key");
            if (Md5Utils::pushSign(type, price, t, mk) == sign) {
                LOG_INFO << "[pushKey] matched mch_key for mch=" << r.at("id");
                try { outMchId = std::stoi(r.at("id")); } catch (...) { outMchId = 0; }
                outKey = mk; return true;
            }
        }
        return false;
    }

    // key 参数保留但已不强制使用：内部会尝试平台 key 与所有商户 vmq_key
    static bool handleHeart(const std::string &t, const std::string &sign, const std::string & /*hint*/) {
        int mchId = 0; std::string matched;
        if (!resolveHeartKey(t, sign, mchId, matched)) return false;
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        // 与 V免签 原版一致：始终更新平台级 lastheart/jkstate
        // 商户级心跳额外刷一份，便于多租户面板展示
        if (mchId > 0) {
            db.exec("UPDATE merchant SET vmq_last_heart=?,vmq_state=1 WHERE id=?",
                    {std::to_string(now), std::to_string(mchId)});
        }
        db.setSetting("lastheart", std::to_string(now));
        db.setSetting("jkstate",   "1");
        return true;
    }

    // 返回 1=签名 OK 已匹配并标记支付成功
    //      0=签名 OK 但暂无匹配订单
    //     -1=签名校验失败
    static int handlePaymentPushEx(const std::string &type, const std::string &price,
                                   const std::string &keyOrEmpty,
                                   const std::string &t = "",
                                   const std::string &sign = "") {
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        int mchId = 0; std::string matched = keyOrEmpty;
        if (!sign.empty() && !t.empty()) {
            if (!resolvePushKey(type, price, t, sign, mchId, matched)) return -1;
        }
        // 心跳/收款时间标记 (商户级 + 平台级)
        if (mchId > 0) {
            db.exec("UPDATE merchant SET vmq_last_pay=? WHERE id=?",
                    {std::to_string(now), std::to_string(mchId)});
        }
        db.setSetting("lastpay", std::to_string(now));
        // 与 V免签 原版一致：订单匹配不按 mch_id 过滤
        // (单租户匹配语义: state=0 AND type=? AND really_price=?)
        auto order = findPendingOrder(type, fmtPrice(price), "");
        if (order.empty()) return 0;
        // 回调签名 key: 优先用订单所属商户 mch_key, 兜底平台 key
        std::string callKey = matched;
        std::string oMch = order.count("mch_id") ? order.at("mch_id") : "";
        if (!oMch.empty() && oMch != "0") {
            auto m = db.queryOne("SELECT mch_key FROM merchant WHERE id=?", {oMch});
            if (!m.empty() && !m["mch_key"].empty()) callKey = m["mch_key"];
        }
        if (callKey.empty()) callKey = db.getSetting("key", "");
        markOrderPaid(order, type, callKey);
        return 1;
    }

    static bool handlePaymentPush(const std::string &type, const std::string &price,
                                  const std::string &keyOrEmpty,
                                  const std::string &t = "",
                                  const std::string &sign = "") {
        return handlePaymentPushEx(type, price, keyOrEmpty, t, sign) >= 0;
    }

    static Json::Value pendingOrders() {
        auto rows = PayDb::instance().query(
            "SELECT order_id,pay_type,COALESCE(NULLIF(real_amount,''),amount) AS display_amount "
            "FROM pay_order WHERE state=0 ORDER BY created_at ASC LIMIT 50", {});
        Json::Value arr(Json::arrayValue);
        for (auto &o : rows) {
            Json::Value item;
            item["order_id"] = o["order_id"];
            item["type"] = o["pay_type"];
            item["really_price"] = o["display_amount"];
            arr.append(item);
        }
        return arr;
    }

    static std::string extractAmount(const std::string &text) {
        std::regex re(R"((\d+\.\d{1,2})(?:\s*元|¥)?)");
        std::smatch m;
        if (std::regex_search(text, m, re)) return m[1].str();
        return "";
    }

    static int payTypeToInt(const std::string &payType) {
        if (payType == "1" || payType == "wxpay" || payType == "wx" || payType == "wechat") return 1;
        if (payType == "2" || payType == "alipay" || payType == "ali" || payType == "zfb") return 2;
        if (payType == "3" || payType == "qqpay" || payType == "qq") return 3;
        return 0;
    }

    static std::string intToPayType(const std::string &type) {
        if (type == "1") return "wxpay";
        if (type == "2") return "alipay";
        if (type == "3") return "qqpay";
        return type;
    }

    static std::string fmtPrice(const std::string &s) {
        try { return ChannelService::fmtAmount(std::stod(s)); } catch (...) { return s; }
    }

protected:
    static bool lockUniqueAmount(const std::string &orderId, double amount, double &realAmount) {
        auto &db = PayDb::instance();
        long long cent = (long long)std::round(amount * 100);
        // 先尝试锁住原始金额（tmp_price 防止并发撞单）
        db.exec("INSERT OR IGNORE INTO tmp_price(price,oid) VALUES(?,?)",
                {std::to_string(cent), orderId});
        auto chk0 = db.queryOne("SELECT oid FROM tmp_price WHERE price=?", {std::to_string(cent)});
        if (!chk0.empty() && chk0["oid"] == orderId) {
            realAmount = (double)cent / 100.0;
            return true;
        }
        // 原始金额已被其他订单占用
        // 查 pay_order 里是否有同金额待支付订单，没有则直接用原始金额
        auto exist = db.queryOne(
            "SELECT 1 FROM pay_order WHERE state=0 "
            "AND ABS(CAST(amount AS REAL)-?)<0.001 LIMIT 1",
            {std::to_string((double)cent / 100.0)});
        if (exist.empty()) {
            // 没有同金额待支付订单 → 直接用原始金额（避免无谓加价）
            realAmount = (double)cent / 100.0;
            return true;
        }
        // 有同金额待支付 → +0.01 避撞
        for (int delta = 1; delta <= 20; ++delta) {
            long long tryCent = cent + delta;
            db.exec("INSERT OR IGNORE INTO tmp_price(price,oid) VALUES(?,?)",
                    {std::to_string(tryCent), orderId});
            auto chk = db.queryOne("SELECT oid FROM tmp_price WHERE price=?", {std::to_string(tryCent)});
            if (!chk.empty() && chk["oid"] == orderId) {
                realAmount = (double)tryCent / 100.0;
                return true;
            }
        }
        return false;
    }

    static PayDb::Row selectQrcode(int typeInt, double amount, const Json::Value &p) {
        auto &db = PayDb::instance();
        std::vector<std::string> params{std::to_string(typeInt), ChannelService::fmtAmount(amount)};
        std::string sql = "SELECT * FROM pay_qrcode WHERE type=? AND state=0 AND (price='0' OR price='0.00' OR price='' OR price IS NULL OR ABS(CAST(price AS REAL)-?)<0.001)";
        std::string account = p.get("account", "").asString();
        if (!account.empty()) { sql += " AND (account=? OR account='' OR account IS NULL)"; params.push_back(account); }
        std::string pluginCode = p.get("plugin_code", "").asString();
        if (!pluginCode.empty()) { sql += " AND (plugin_code=? OR plugin_code='' OR plugin_code IS NULL)"; params.push_back(pluginCode); }
        int channelId = p.get("channel_id", 0).asInt();
        if (channelId > 0) {
            sql += " AND (channel_id=? OR channel_id=0 OR channel_id IS NULL)";
            params.push_back(std::to_string(channelId));
        }
        int mchId = p.get("mch_id", 0).asInt();
        if (mchId > 0) {
            sql += " AND (mch_id=? OR mch_id=0 OR mch_id IS NULL)";
            params.push_back(std::to_string(mchId));
        }
        sql += " ORDER BY CAST(price AS REAL) DESC,id ASC LIMIT 1";
        auto qr = db.queryOne(sql, params);
        if (!qr.empty()) return qr;
        std::string fallback = p.get("qrcode_url", "").asString();
        if (!fallback.empty()) {
            PayDb::Row r; r["id"] = "0"; r["pay_url"] = fallback; r["type"] = std::to_string(typeInt); return r;
        }
        return {};
    }

    static PayDb::Row findPendingOrder(const std::string &type, const std::string &price,
                                       const std::string &mchId = "") {
        std::string sql =
            "SELECT * FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
            "AND (ABS(CAST(real_amount AS REAL)-?)<0.001 OR ABS(CAST(amount AS REAL)-?)<0.001)";
        std::vector<std::string> params{intToPayType(type), price, price};
        if (!mchId.empty()) { sql += " AND mch_id=?"; params.push_back(mchId); }
        sql += " ORDER BY created_at ASC LIMIT 1";
        return PayDb::instance().queryOne(sql, params);
    }

public:
    // 公开静态封装, 供其它控制器(MonitorCtrl 多租户路径) 复用
    static void markOrderPaidStatic(const PayDb::Row &order, const std::string &type,
                                     const std::string &key) {
        markOrderPaid(order, type, key);
    }

protected:
    static void markOrderPaid(const PayDb::Row &order, const std::string &type, const std::string &key) {
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        std::string orderId = order.at("order_id");
        db.exec("UPDATE pay_order SET state=1,pay_time=?,channel_order_no=?,updated_at=? WHERE order_id=? AND state=0",
                {std::to_string(now), orderId, std::to_string(now), orderId});
        db.exec("DELETE FROM tmp_price WHERE oid=?", {orderId});
        std::string notifyUrl = order.count("notify_url") ? order.at("notify_url") : "";
        if (!notifyUrl.empty()) {
            std::string payId = order.count("mch_order_no") ? order.at("mch_order_no") : "";
            std::string param = order.count("param") ? order.at("param") : "";
            std::string amount = order.count("amount") ? order.at("amount") : "0.00";
            std::string realAmount = order.count("real_amount") ? order.at("real_amount") : "0.00";
            std::string callSign = Md5Utils::notifySign(payId, param, type, amount, realAmount, key);
            std::string sep = notifyUrl.find('?') == std::string::npos ? "?" : "&";
            std::string fullUrl = notifyUrl + sep + "payId=" + urlEncode(payId) + "&param=" + urlEncode(param) +
                "&type=" + type + "&price=" + amount + "&reallyPrice=" + realAmount + "&sign=" + callSign;
            NotifyTaskService::createTaskAndSend(orderId, fullUrl, "VmqPlugin");
        }
    }

    static std::string rowJson(const PayDb::Row &row) {
        Json::Value j;
        for (auto &kv : row) j[kv.first] = kv.second;
        Json::StreamWriterBuilder wb;
        return Json::writeString(wb, j);
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }

    static double toDouble(const std::string &s) { try { return std::stod(s); } catch (...) { return 0.0; } }

    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(VmqPlugin);
