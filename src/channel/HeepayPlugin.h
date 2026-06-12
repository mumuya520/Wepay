// WePay-Cpp — 汇付宝支付插件 (完整实现)
// 参考 PHP: mpay_v2_webman/app/common/payment/HeepayApiPayment.php
// 参考 SDK: mpay_v2_webman/app/common/sdk/heepay/HeepayClient.php
//
// 支付产品:
//   - 22: 支付宝支付
//   - 30: 微信支付
//   - 34: 银联 H5
//   - 20: 银联网页
//   - 64: 银联扫码
//
// 签名: MD5 签名
#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#ifndef _WIN32
#include <openssl/md5.h>
#endif
#include "../common/SyncHttp.h"

class HeepayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "heepay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("agent_id",    "商户编号",     "input",    "", "汇付宝商户编号 (agent_id)");
        add("ref_agent_id","二级商户号",   "input",    "", "二级商户号 (可选)");
        add("bank_id",     "上游商户BankId", "input", "", "上游商户 BankId (可选)");
        add("pay_key",     "支付密钥",     "password", "", "汇付宝支付密钥");
        add("refund_key",  "退款密钥",     "password", "", "汇付宝退款密钥 (可选)");
        add("pay_method",   "支付方式",     "select",   "jump", "jump=跳转/h5=H5/qrcode=扫码/web=网页");
        return arr;
    }

    // ═══ 下单接口 ═══════════════════════════════════════════════════
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;

        std::string agentId  = p.get("agent_id", "").asString();
        std::string payKey   = p.get("pay_key", "").asString();
        std::string payMethod = p.get("pay_method", "jump").asString();
        std::string wayCode  = req.payType;

        if (agentId.empty() || payKey.empty()) {
            r.errMsg = "汇付宝参数不完整(agent_id/pay_key)";
            return r;
        }

        // 根据支付方式选择 pay_type
        std::string payType;
        bool isBank = false;  // 是否网银支付

        if (payMethod == "qrcode" || wayCode == "bank_qr" || wayCode == "unionpay_qr") {
            // 银联扫码
            payType = "64";
        } else if (payMethod == "h5" || wayCode == "ali_h5" || wayCode == "wx_h5") {
            // H5 支付
            if (wayCode == "bank" || wayCode == "bank_h5") {
                payType = "34";  // 银联 H5
            } else if (wayCode == "wxpay" || wayCode == "wx_h5") {
                payType = "30";  // 微信 H5
            } else {
                payType = "22";  // 支付宝 H5
            }
        } else if (payMethod == "web" || wayCode == "bank_web") {
            // 网银支付
            payType = "20";
            isBank = true;
        } else {
            // 默认跳转支付
            if (wayCode == "bank") {
                payType = "34";  // 银联 H5
            } else if (wayCode == "wxpay") {
                payType = "30";  // 微信
            } else {
                payType = "22";  // 支付宝
            }
        }

        // 构造支付跳转 URL
        r.payUrl = buildPayUrl(req, p, payType, isBank);
        r.success = true;
        r.channelOrderNo = req.orderId;

        return r;
    }

    // ═══ 查单接口 (不支持) ═══════════════════════════════════════════
    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        r.success = false;
        r.errMsg = "汇付宝插件暂不支持主动查单";
        return r;
    }

    // ═══ 退款接口 ═══════════════════════════════════════════════════
    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string agentId   = p.get("agent_id", "").asString();
        std::string refundKey = p.get("refund_key", "").asString();
        if (refundKey.empty()) refundKey = p.get("pay_key", "").asString();

        if (agentId.empty() || refundKey.empty()) {
            r.errMsg = "汇付宝参数不完整(agent_id/refund_key)"; return r;
        }

        // 构造退款请求
        std::string refundDetails;
        if (req.refundAmount >= req.paidAmount) {
            // 全额退款
            std::map<std::string, std::string> params;
            params["version"] = "1";
            params["agent_id"] = agentId;
            params["agent_bill_id"] = req.orderId;
            params["notify_url"] = req.notifyUrl;
            params["sign_type"] = "MD5";
            std::string signContent = "agent_bill_id=" + req.orderId + "&agent_id=" + agentId +
                                     "&key=" + refundKey + "&notify_url=" + req.notifyUrl +
                                     "&version=" + params["version"];
            params["sign"] = md5(signContent);
        } else {
            // 部分退款
            refundDetails = req.orderId + "," + fmtAmount(req.refundAmount) + "," + req.refundNo;
            std::map<std::string, std::string> params;
            params["version"] = "1";
            params["agent_id"] = agentId;
            params["refund_details"] = refundDetails;
            params["notify_url"] = req.notifyUrl;
            params["sign_type"] = "MD5";
            std::string signContent = "agent_id=" + agentId + "&key=" + refundKey +
                                     "&notify_url=" + req.notifyUrl + "&refund_details=" + refundDetails +
                                     "&version=" + params["version"];
            params["sign"] = md5(signContent);
        }

        // 发送退款请求 (汇付宝退款使用 XML 响应, GBK 编码)
        std::map<std::string, std::string> params;
        if (req.refundAmount >= req.paidAmount) {
            params["version"] = "1";
            params["agent_id"] = agentId;
            params["agent_bill_id"] = req.orderId;
            params["notify_url"] = req.notifyUrl;
            params["sign_type"] = "MD5";
            std::string signContent = "agent_bill_id=" + req.orderId + "&agent_id=" + agentId +
                                     "&key=" + refundKey + "&notify_url=" + req.notifyUrl +
                                     "&version=" + params["version"];
            params["sign"] = md5(signContent);
        } else {
            refundDetails = req.orderId + "," + fmtAmount(req.refundAmount) + "," + req.refundNo;
            params["version"] = "1";
            params["agent_id"] = agentId;
            params["refund_details"] = refundDetails;
            params["notify_url"] = req.notifyUrl;
            params["sign_type"] = "MD5";
            std::string signContent = "agent_id=" + agentId + "&key=" + refundKey +
                                     "&notify_url=" + req.notifyUrl + "&refund_details=" + refundDetails +
                                     "&version=" + params["version"];
            params["sign"] = md5(signContent);
        }

        auto resp = SyncHttp::postForm("https://pay.heepay.com/API/Payment/PaymentRefund.aspx", buildQuery(params));
        r.rawResponse = resp.body;

        // 汇付宝返回 GBK 编码的 XML，需要解析
        // 简化处理: 检查返回是否包含成功标记
        if (!resp.success) {
            r.errMsg = "退款请求失败: " + resp.errMsg; return r;
        }

        // 解析 XML 响应
        std::string body = resp.body;
        std::string retCode = extractXmlValue(body, "ret_code");
        std::string retMsg = extractXmlValue(body, "ret_msg");

        if (retCode == "0000") {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = req.refundNo;
            r.refundAmount = req.refundAmount;
        } else {
            r.errMsg = retMsg.empty() ? "退款失败" : retMsg;
        }

        return r;
    }

    // ═══ 关闭订单 (不支持) ═══════════════════════════════════════════
    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        r.success = false;
        r.errMsg = "汇付宝插件暂不支持关单";
        return r;
    }

    // ═══ 回调验证 ═══════════════════════════════════════════════════
    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "ok";

        std::string payKey = channelParams.get("pay_key", "").asString();
        if (payKey.empty()) {
            r.verified = false; r.errMsg = "缺少支付密钥"; return r;
        }

        // 构造验签数据
        std::string signContent = "result=" + getParam(params, "result") +
                                 "&agent_id=" + getParam(params, "agent_id") +
                                 "&jnet_bill_no=" + getParam(params, "jnet_bill_no") +
                                 "&agent_bill_id=" + getParam(params, "agent_bill_id") +
                                 "&pay_type=" + getParam(params, "pay_type") +
                                 "&pay_amt=" + getParam(params, "pay_amt") +
                                 "&remark=" + getParam(params, "remark") +
                                 "&key=" + payKey;

        std::string sign = getParam(params, "sign");
        r.verified = (sign == md5(signContent));

        if (!r.verified) {
            r.errMsg = "验签失败"; return r;
        }

        r.paid = (getParam(params, "result") == "1");
        r.orderId = getParam(params, "agent_bill_id");
        r.channelOrderNo = getParam(params, "jnet_bill_no");

        try {
            r.paidAmount = std::stod(getParam(params, "pay_amt"));
        } catch (...) {}

        return r;
    }

private:
    // ═══ 构造支付跳转 URL ════════════════════════════════════════════
    std::string buildPayUrl(const ChannelOrderRequest &req, const Json::Value &p,
                            const std::string &payType, bool isBank) {
        std::string agentId   = p.get("agent_id", "").asString();
        std::string payKey    = p.get("pay_key", "").asString();
        std::string refAgentId = p.get("ref_agent_id", "").asString();
        std::string bankId    = p.get("bank_id", "").asString();

        std::map<std::string, std::string> params;
        params["version"] = isBank ? "3" : "1";
        params["agent_id"] = agentId;
        params["agent_bill_id"] = req.orderId;
        params["agent_bill_time"] = formatTimestampFull(std::time(nullptr));
        params["pay_type"] = payType;
        params["pay_amt"] = fmtAmount(req.amount);
        params["notify_url"] = req.notifyUrl;
        params["return_url"] = req.returnUrl;
        // 用户 IP: 转换 . 为 _
        std::string userIp = req.clientIp.empty() ? "127_0_0_1" : req.clientIp;
        std::replace(userIp.begin(), userIp.end(), '.', '_');
        params["user_ip"] = userIp;
        params["goods_name"] = req.subject.empty() ? "商品" : req.subject;
        params["sign_type"] = "MD5";

        if (!refAgentId.empty()) params["ref_agent_id"] = refAgentId;
        if (isBank && !bankId.empty()) {
            params["bank_id"] = pickBankId(bankId);
            params["pay_code"] = "0";
            params["bank_card_type"] = "-1";
        }

        // 构造签名内容
        std::string signContent = "version=" + params["version"] +
                                 "&agent_id=" + params["agent_id"] +
                                 "&agent_bill_id=" + params["agent_bill_id"] +
                                 "&agent_bill_time=" + params["agent_bill_time"] +
                                 "&pay_type=" + params["pay_type"] +
                                 "&pay_amt=" + params["pay_amt"] +
                                 "&notify_url=" + params["notify_url"] +
                                 "&return_url=" + params["return_url"] +
                                 "&user_ip=" + params["user_ip"] +
                                 "&key=" + payKey;
        if (!refAgentId.empty()) {
            signContent += "&ref_agent_id=" + refAgentId;
        }
        if (isBank) {
            signContent += "&bank_card_type=" + params["bank_card_type"];
        }

        params["sign"] = md5(signContent);

        return "https://pay.Heepay.com/Payment/Index.aspx?" + buildQuery(params);
    }

    // ═══ MD5 签名 ═════════════════════════════════════════════════
    static std::string md5(const std::string &input) {
        unsigned char digest[16];
#ifdef _WIN32
        HCRYPTPROV hProv;
        HCRYPTHASH hHash;
        CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, 0);
        CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash);
        CryptHashData(hHash, (BYTE*)input.c_str(), input.length(), 0);
        CryptGetHashParam(hHash, HP_HASHVAL, digest, NULL, 0);
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
#else
        MD5((unsigned char*)input.c_str(), input.length(), digest);
#endif
        char hex[33];
        for (int i = 0; i < 16; ++i) {
            sprintf(hex + i * 2, "%02x", digest[i]);
        }
        hex[32] = '\0';
        return std::string(hex);
    }

    // ═══ 构造查询字符串 ════════════════════════════════════════════
    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::ostringstream oss;
        bool first = true;
        for (auto &[k, v] : params) {
            if (!first) oss << "&";
            first = false;
            oss << urlEncode(k) << "=" << urlEncode(v);
        }
        return oss.str();
    }

    // ═══ URL 编码 ════════════════════════════════════════════════════
    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                oss << c;
            } else {
                oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
            }
        }
        return oss.str();
    }

    // ═══ 格式化金额 ════════════════════════════════════════════════════
    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    // ═══ 格式化时间戳 (YYYYMMDDHHmmss) ═══════════════════════════════
    static std::string formatTimestampFull(time_t t) {
        struct tm tt;
#ifdef _WIN32
        localtime_s(&tt, &t);
#else
        localtime_r(&t, &tt);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tt);
        return buf;
    }

    // ═══ 选择银行 ID (随机) ══════════════════════════════════════════
    static std::string pickBankId(const std::string &bankIds) {
        size_t pos = bankIds.find(',');
        if (pos == std::string::npos) return bankIds;
        // 逗号分隔，随机选一个
        std::vector<std::string> items;
        std::string remaining = bankIds;
        while ((pos = remaining.find(',')) != std::string::npos) {
            std::string item = remaining.substr(0, pos);
            // 去除首尾空格
            size_t start = item.find_first_not_of(" \t");
            size_t end = item.find_last_not_of(" \t");
            if (start != std::string::npos) {
                items.push_back(item.substr(start, end - start + 1));
            }
            remaining = remaining.substr(pos + 1);
        }
        if (!remaining.empty()) {
            size_t start = remaining.find_first_not_of(" \t");
            size_t end = remaining.find_last_not_of(" \t");
            if (start != std::string::npos) {
                items.push_back(remaining.substr(start, end - start + 1));
            }
        }
        if (items.empty()) return bankIds;
        std::srand((unsigned)std::time(nullptr));
        return items[std::rand() % items.size()];
    }

    // ═══ 简单 XML 解析 ═══════════════════════════════════════════════
    static std::string extractXmlValue(const std::string &xml, const std::string &tag) {
        std::string openTag = "<" + tag + ">";
        std::string closeTag = "</" + tag + ">";
        size_t start = xml.find(openTag);
        if (start == std::string::npos) return "";
        start += openTag.length();
        size_t end = xml.find(closeTag, start);
        if (end == std::string::npos) return "";
        return xml.substr(start, end - start);
    }

    // ═══ 从 map 获取值 ═══════════════════════════════════════════════
    static std::string getParam(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(HeepayPlugin);
