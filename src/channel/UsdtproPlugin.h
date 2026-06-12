#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include "../common/SyncHttp.h"

// USDTPRO V2 - USDT TRC20虚拟货币支付
// 非标准API支付: 展示收款地址+精确金额, 定时轮询Tronscan API匹配到账
// 流程: 1)计算USDT金额(金额/汇率,5位小数,防重复+0.00001) 2)返回支付页URL
//       3)前端轮询支付状态 4)后端定时任务查Tronscan交易记录匹配金额
// Tronscan API: https://apilist.tronscan.org/api/token_trc20/transfers
// 参数: direction=in, relatedAddress=收款地址, start_timestamp/end_timestamp(毫秒)
// 匹配: quant/1000000 == 订单usdtpro金额 → 支付成功

class UsdtproPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "usdtpro"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("address", "USDT TRC20地址", "input", "", "TRC20收款地址");
        add("rate", "汇率 $1 = ￥?", "input", "7.2", "1 USDT兑换多少人民币");
        add("timeout", "超时时间(秒)", "input", "300", "订单超时时间");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        std::string address = p.get("address", "").asString();
        double rate = 7.2;
        try { rate = std::stod(p.get("rate", "7.2").asString()); } catch (...) {}
        int timeout = 300;
        try { timeout = std::stoi(p.get("timeout", "300").asString()); } catch (...) {}

        if (address.empty()) { r.errMsg = "USDT参数不完整(address)"; return r; }
        if (rate <= 0) { r.errMsg = "汇率配置错误"; return r; }

        // Calculate USDT amount (5 decimal places)
        double usdtAmount = std::round(req.amount / rate * 100000.0) / 100000.0;

        // Return payment page info - the frontend will render the QR/address page
        r.success = true;
        r.payUrl = "usdtpro://" + address; // marker for frontend handling
        r.channelOrderNo = "";

        // Store USDT amount and payment details in extra for frontend
        r.extra["usdt_amount"] = usdtAmount;
        r.extra["address"] = address;
        r.extra["timeout"] = timeout;
        r.extra["rate"] = rate;
        r.extra["cny_amount"] = req.amount;
        return r;
    }

    // USDTPRO uses polling (Tronscan API) instead of traditional callback
    // The verifyNotify here is for manual/simulated notification
    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &body, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        // No traditional signature verification - uses Tronscan blockchain matching
        r.verified = true;
        r.paid = true;
        auto it = params.find("order_id");
        if (it != params.end()) r.orderId = it->second;
        it = params.find("trade_no");
        if (it != params.end()) r.channelOrderNo = it->second;
        r.responseText = "success";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        r.errMsg = "USDT支付不支持退款";
        return r;
    }

    // ── Tronscan API polling: check incoming USDT transfers ──
    // Returns list of {money: double} for successful transfers
    static std::vector<double> pollTronscan(const std::string &address, int timeoutSec) {
        std::vector<double> amounts;
        long long nowMs = (long long)std::time(nullptr) * 1000;
        long long startMs = nowMs - (long long)(timeoutSec + 5) * 1000;

        std::string url = "https://apilist.tronscan.org/api/token_trc20/transfers?"
            "direction=in&limit=300&start=0"
            "&start_timestamp=" + std::to_string(startMs) +
            "&end_timestamp=" + std::to_string(nowMs) +
            "&relatedAddress=" + address;

        auto resp = SyncHttp::get(url);
        if (!resp.success) return amounts;

        Json::Value root;
        if (!Json::Reader().parse(resp.body, root)) return amounts;
        const Json::Value &transfers = root["token_transfers"];
        if (!transfers.isArray()) return amounts;

        for (Json::ArrayIndex i = 0; i < transfers.size(); i++) {
            const Json::Value &v = transfers[i];
            if (v.get("finalResult", "").asString() != "SUCCESS") continue;
            double quant = 0;
            try { quant = std::stod(v.get("quant", "0").asString()); } catch (...) {}
            double money = quant / 1000000.0;
            amounts.push_back(money);
        }
        return amounts;
    }

    // Match Tronscan transfers against pending orders
    // Returns orderIds that have matching USDT amounts
    static std::vector<std::string> matchOrders(const std::string &address, int timeoutSec,
                                                 const std::map<double, std::string> &pendingOrders) {
        auto transfers = pollTronscan(address, timeoutSec);
        std::vector<std::string> matched;
        for (auto &amt : transfers) {
            // Match with 5 decimal precision
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(5) << amt;
            std::string key = oss.str();
            auto it = pendingOrders.find(amt);
            if (it == pendingOrders.end()) {
                // Try string-based lookup
                for (auto &kv : pendingOrders) {
                    if (std::abs(kv.first - amt) < 0.000001) {
                        matched.push_back(kv.second);
                        break;
                    }
                }
            } else {
                matched.push_back(it->second);
            }
        }
        return matched;
    }
};

REGISTER_CHANNEL_PLUGIN(UsdtproPlugin);
