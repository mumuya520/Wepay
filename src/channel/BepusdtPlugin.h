#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"
#include "../common/BepusdtProxy.h"

// BEpusdt 加密货币支付插件
// 通过 HTTP API 调用同进程内嵌的 BEpusdt 服务 (bepusdt.dll)
// 支持: USDT/USDC/ETH/BNB/TRX 等多链多币种
// API: POST /api/v1/order/create-transaction  创建交易
//      POST /api/v1/order/cancel-transaction  取消交易
// 签名: ksort → key=value&...拼接 → 追加 apiToken → MD5 小写
// 回调: POST /notify/channel/bepusdt  status=2 表示支付成功

class BepusdtPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "bepusdt"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl,
                       const std::string &t, const std::string &dflt = "",
                       const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t;
            v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("api_token", "API Token", "input", "",
            "BEpusdt 后台 → 系统管理 → 基本设置 → API 设置 → 对接令牌");
        add("trade_type", "交易类型", "select:usdt.trc20|USDT TRC20,usdt.erc20|USDT ERC20,"
            "usdt.polygon|USDT Polygon,usdt.bsc|USDT BSC,usdc.polygon|USDC Polygon,"
            "tron.trx|TRX,ethereum.eth|ETH,bsc.bnb|BNB",
            "usdt.trc20", "默认交易币种/网络");
        add("fiat", "法币类型", "select:CNY|人民币,USD|美元,EUR|欧元,GBP|英镑,JPY|日元",
            "CNY", "计价法币");
        add("timeout", "超时时间(秒)", "input", "600", "订单超时,最低120秒");
        return arr;
    }

    // ── 创建支付订单 ──────────────────────────────────────────
    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;

        std::string apiToken  = p.get("api_token", "").asString();
        std::string tradeType = p.get("trade_type", "usdt.trc20").asString();
        std::string fiat      = p.get("fiat", "CNY").asString();
        int timeout = 600;
        try { timeout = std::stoi(p.get("timeout", "600").asString()); } catch (...) {}
        if (timeout < 120) timeout = 120;

        if (apiToken.empty()) {
            r.errMsg = "BEpusdt API Token 未配置";
            return r;
        }

        // 回调地址: 由 GatewayCtrl 传入，已是完整 URL
        std::string notifyUrl = req.notifyUrl;
        std::string returnUrl = req.returnUrl;

        // 金额: BEpusdt 接受法币金额(浮点数), req.amount 已是元
        double amountYuan = req.amount;
        std::ostringstream amtOss;
        amtOss << std::fixed << std::setprecision(2) << amountYuan;
        std::string amountStr = amtOss.str();

        // 构造请求参数
        std::map<std::string, std::string> params;
        params["order_id"]     = req.orderId;
        params["amount"]       = amountStr;
        params["notify_url"]   = notifyUrl;
        params["redirect_url"] = returnUrl;
        params["trade_type"]   = tradeType;
        params["fiat"]         = fiat;
        params["timeout"]      = std::to_string(timeout);
        if (!req.subject.empty()) params["name"] = req.subject;

        // 签名: ksort → 排除空值和 signature → key=value& 拼接 → 末尾追加 apiToken → MD5 小写
        params["signature"] = makeSign(params, apiToken);

        // 序列化为 JSON
        Json::Value body(Json::objectValue);
        for (auto &kv : params) {
            // amount 转数字
            if (kv.first == "amount") {
                body[kv.first] = amountYuan;
            } else if (kv.first == "timeout") {
                body[kv.first] = timeout;
            } else {
                body[kv.first] = kv.second;
            }
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string jsonStr = Json::writeString(wb, body);

        // 调用 BEpusdt API
        std::string respBody;
        if (BepusdtProxy::isFFI()) {
            // FFI 模式: 直接调 gin engine（不经过 TCP）
            std::map<std::string, std::string> hdrs;
            hdrs["Content-Type"] = "application/json";
            auto ffiResp = BepusdtLib::instance().handleRequest(
                "POST", "/api/v1/order/create-transaction", "", hdrs, jsonStr);
            if (!ffiResp.ok) {
                r.errMsg = "BEpusdt FFI 调用失败: " + ffiResp.err;
                return r;
            }
            respBody = ffiResp.body;
        } else {
            // HTTP 模式: 走本地 loopback
            int bpPort = BepusdtProxy::internalPort();
            std::string url = "http://127.0.0.1:" + std::to_string(bpPort)
                            + "/api/v1/order/create-transaction";
            auto resp = SyncHttp::postJson(url, jsonStr);
            if (!resp.success) {
                r.errMsg = "BEpusdt API 请求失败: " + resp.errMsg;
                return r;
            }
            respBody = resp.body;
        }

        Json::Value root;
        if (!Json::Reader().parse(respBody, root)) {
            r.errMsg = "BEpusdt 响应解析失败";
            return r;
        }

        int statusCode = root.get("status_code", 0).asInt();
        if (statusCode != 200) {
            r.errMsg = "BEpusdt 下单失败: " + root.get("message", "unknown").asString();
            return r;
        }

        const Json::Value &data = root["data"];
        r.success = true;
        r.payUrl = data.get("payment_url", "").asString();
        r.channelOrderNo = data.get("trade_id", "").asString();

        // 存储额外信息
        r.extra["trade_id"]      = data.get("trade_id", "").asString();
        r.extra["actual_amount"] = data.get("actual_amount", "").asString();
        r.extra["token"]         = data.get("token", "").asString();
        r.extra["fiat"]          = data.get("fiat", "").asString();

        return r;
    }

    // ── 回调验签 ──────────────────────────────────────────────
    // BEpusdt 回调: JSON POST, status=2 表示支付成功
    // 签名验证: 同样的 MD5 签名算法
    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &body,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";

        std::string apiToken = channelParams.get("api_token", "").asString();
        if (apiToken.empty()) {
            r.responseText = "fail: no api_token";
            return r;
        }

        // 解析 JSON body
        Json::Value root;
        if (!Json::Reader().parse(body, root)) {
            r.responseText = "fail: invalid json";
            return r;
        }

        // 提取所有非空、非 signature 字段用于验签
        std::map<std::string, std::string> signParams;
        for (auto &key : root.getMemberNames()) {
            if (key == "signature") continue;
            std::string val;
            if (root[key].isString()) {
                val = root[key].asString();
            } else if (root[key].isInt() || root[key].isInt64()) {
                val = std::to_string(root[key].asInt64());
            } else if (root[key].isDouble()) {
                // BEpusdt 回调中 amount 和 actual_amount 可能是数字
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2) << root[key].asDouble();
                val = oss.str();
                // 去掉末尾多余的0 (如 28.80 → 28.8)
                if (val.find('.') != std::string::npos) {
                    while (val.back() == '0') val.pop_back();
                    if (val.back() == '.') val.pop_back();
                }
            } else if (root[key].isBool()) {
                continue; // 布尔值不参与签名
            } else {
                Json::StreamWriterBuilder wb; wb["indentation"] = "";
                val = Json::writeString(wb, root[key]);
            }
            if (!val.empty()) signParams[key] = val;
        }

        std::string expectedSign = makeSign(signParams, apiToken);
        std::string receivedSign = root.get("signature", "").asString();

        if (expectedSign != receivedSign) {
            r.responseText = "fail: signature mismatch";
            return r;
        }

        r.verified = true;

        // 提取订单信息
        r.orderId = root.get("order_id", "").asString();
        r.channelOrderNo = root.get("trade_id", "").asString();

        int status = root.get("status", 0).asInt();
        // status: 1=等待支付, 2=支付成功, 3=支付超时
        r.paid = (status == 2);
        r.responseText = "ok";

        return r;
    }

    // ── 退款 ──────────────────────────────────────────────────
    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        r.errMsg = "加密货币支付不支持退款";
        return r;
    }

private:
    // BEpusdt 签名算法:
    // 1. 筛选非空且非 signature 的参数
    // 2. 按参数名 ASCII 字典序排序 (std::map 天然有序)
    // 3. key=value 用 & 连接
    // 4. 末尾直接追加 apiToken (无 & 前缀)
    // 5. MD5 小写
    static std::string makeSign(const std::map<std::string, std::string> &params,
                                const std::string &apiToken) {
        std::string raw;
        for (auto &kv : params) {
            if (kv.first == "signature" || kv.second.empty()) continue;
            if (!raw.empty()) raw += "&";
            raw += kv.first + "=" + kv.second;
        }
        raw += apiToken;
        return Md5Utils::md5(raw); // 返回小写
    }
};

REGISTER_CHANNEL_PLUGIN(BepusdtPlugin);
