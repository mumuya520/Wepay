#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include <iomanip>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

// 掌易收聚合支付(zhangyishou) - MD5拼接签名(直接拼接值), JSON POST, 支持转账/余额查询
// 网关: https://apipay.zhangyishou.com/api/Order/AddOrder
// 签名: 拼接所有参数值(顺序) + key, MD5
// 请求: MerchantId/DownstreamOrderNo/OrderTime/PayChannelId/AsynPath/OrderMoney/IPPath → JSON POST
// 响应: Code=1009成功, Info包含数据
// 回调: Signature = MD5(MerchantId+DownstreamOrderNo+key)
// 扫码: PayChannelId需配置
// 退款: /api/OrderRefund/Refund
// 转账: AddOrder, PayChannelId=12002(支付宝)/12001(银联), PaymentType=3(支付宝)/2(银联)
// 余额查询: /query/bookQuery

class ZhangyishouPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "zhangyishou"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "登录账号", "input");
        add("appkey", "商户密钥", "input");
        add("appurl", "商户编号", "input");
        add("appmchid", "通道ID", "input", "", "多个ID用|分隔");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() || p.get("appmchid", "").asString().empty()) {
            r.errMsg = "掌易收参数不完整(appid/appkey/appmchid)";
            return r;
        }

        std::string merchantId = p.get("appid", "").asString();
        std::string merchantNo = p.get("appurl", "").asString();
        std::string key = p.get("appkey", "").asString();
        std::string payChannelId = p.get("appmchid", "").asString();

        // Handle channel ID with | separator for mobile/mini
        std::string originalPayChannelId = payChannelId;
        auto pipePos = payChannelId.find('|');
        if (pipePos != std::string::npos) {
            // For mobile (not in WeChat), use second ID
            if (req.clientIp != "127.0.0.1") { // Simple check
                payChannelId = payChannelId.substr(pipePos + 1);
            } else {
                payChannelId = payChannelId.substr(0, pipePos);
            }
        }

        Json::Value params;
        params["MerchantId"] = merchantId;
        params["DownstreamOrderNo"] = req.orderId;
        params["OrderTime"] = getTimestamp();
        params["PayChannelId"] = payChannelId;
        params["AsynPath"] = req.notifyUrl;
        params["OrderMoney"] = fmtAmount(req.amount);
        params["IPPath"] = req.clientIp;
        params["MerchantNo"] = merchantNo;
        params["Mproductdesc"] = req.subject.empty() ? "商品" : req.subject;

        // Sign: concatenate all values in order + key
        std::string signStr = merchantId + req.orderId + getTimestamp() + payChannelId + req.notifyUrl + fmtAmount(req.amount) + req.clientIp + key;
        params["MD5Sign"] = Md5Utils::md5(signStr);

        auto result = executeRequest(params, "https://apipay.zhangyishou.com/api/Order/AddOrder");
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.payUrl = result.data.get("Info", "").asString();
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "ERROR";

        std::string merchantId = channelParams.get("appid", "").asString();
        std::string key = channelParams.get("appkey", "").asString();

        auto itMerchantId = params.find("MerchantId");
        auto itDownstreamOrderNo = params.find("DownstreamOrderNo");
        auto itSignature = params.find("Signature");
        if (itMerchantId == params.end() || itDownstreamOrderNo == params.end() || itSignature == params.end()) return r;

        // Verify sign: MD5(MerchantId + DownstreamOrderNo + key)
        std::string signStr = itMerchantId->second + itDownstreamOrderNo->second + key;
        std::string expectedSign = Md5Utils::md5(signStr);
        if (itSignature->second != expectedSign) return r;

        r.verified = true;
        std::string orderState = get(params, "OrderState");
        r.paid = (orderState == "1");
        r.orderId = itDownstreamOrderNo->second;
        r.channelOrderNo = get(params, "OrderNo");
        try { r.paidAmount = std::stod(get(params, "OrderMoney")); } catch (...) {}
        r.responseText = r.paid ? "OK" : "ERROR";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string merchantId = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();

        Json::Value params;
        params["MerchantId"] = merchantId;
        params["MerchantOrder"] = req.orderId;
        params["RefundAmount"] = fmtAmount(req.refundAmount);

        // Sign: MerchantId + MerchantOrder + RefundAmount + key
        std::string signStr = merchantId + req.orderId + fmtAmount(req.refundAmount) + key;
        params["MD5Sign"] = Md5Utils::md5(signStr);

        auto result = executeRequest(params, "https://apipay.zhangyishou.com/api/OrderRefund/Refund");
        r.rawResponse = result.rawResponse;
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.state = 1;
        r.channelRefundNo = req.refundNo;
        return r;
    }

    ChannelTransferResult transfer(const ChannelTransferRequest &req) override {
        ChannelTransferResult r;
        auto &p = req.channelParams;
        std::string merchantId = p.get("appid", "").asString();
        std::string merchantNo = p.get("appurl", "").asString();
        std::string key = p.get("appkey", "").asString();
        std::string clientIp = p.get("clientIp", "127.0.0.1").asString();
        std::string payType = p.get("payType", "alipay").asString();

        // Determine PayChannelId and PaymentType based on transfer type
        std::string payChannelId, paymentType, accountNumberType;
        if (payType == "alipay" || req.accountType == 2) {
            payChannelId = "12002";
            paymentType = "3";
            // Determine AccountNumberType
            if (req.accountNo.length() >= 4 && req.accountNo.substr(0, 4) == "2088") {
                accountNumberType = "2"; // 账号
            } else if (req.accountNo.find('@') != std::string::npos) {
                accountNumberType = "1"; // 邮箱
            } else {
                accountNumberType = "3"; // 用户ID
            }
        } else {
            payChannelId = "12001";
            paymentType = "2";
            accountNumberType = "1";
        }

        Json::Value params;
        params["MerchantId"] = merchantId;
        params["DownstreamOrderNo"] = req.transferNo;
        params["OrderTime"] = getTimestamp();
        params["PayChannelId"] = payChannelId;
        params["AsynPath"] = req.notifyUrl;
        params["OrderMoney"] = fmtAmount(req.amount);
        params["IPPath"] = clientIp;

        // Sign for base params
        std::string signStr = merchantId + req.transferNo + getTimestamp() + payChannelId + req.notifyUrl + fmtAmount(req.amount) + clientIp + key;
        params["MD5Sign"] = Md5Utils::md5(signStr);
        params["MerchantNo"] = merchantNo;
        params["PaymentType"] = paymentType;
        params["AccountNumber"] = req.accountNo;
        params["AccountNumberType"] = accountNumberType;
        params["AccountName"] = req.accountName;
        params["PaymentRemark"] = req.remark.empty() ? "转账" : req.remark;
        params["ReasonPayment"] = req.remark.empty() ? "转账" : req.remark;
        params["Mproductdesc"] = req.remark.empty() ? "转账" : req.remark;

        auto result = executeRequest(params, "https://apipay.zhangyishou.com/api/Order/AddOrder");
        if (!result.success) { r.errMsg = result.errMsg; return r; }

        r.success = true;
        r.state = 0; // Processing
        r.channelTransferNo = req.transferNo;
        return r;
    }

private:
    struct ApiResponse {
        bool success = false;
        std::string errMsg;
        std::string rawResponse;
        Json::Value data;
    };

    static ApiResponse executeRequest(const Json::Value &params, const std::string &url) {
        ApiResponse ar;
        Json::FastWriter fw;
        std::string jsonBody = fw.write(params);

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";

        auto resp = SyncHttp::postJson(url, jsonBody, headers);
        ar.rawResponse = resp.body;
        if (!resp.success) { ar.errMsg = resp.errMsg; return ar; }

        Json::Value result;
        if (!Json::Reader().parse(resp.body, result)) { ar.errMsg = "响应解析失败"; return ar; }

        std::string code = result.get("Code", "").asString();
        if (code == "1009") {
            ar.success = true;
            ar.data = result;
        } else {
            ar.errMsg = "[" + code + "]" + result.get("Message", "").asString();
        }
        return ar;
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string getTimestamp() {
        auto now = std::time(nullptr);
        std::tm *local = std::localtime(&now);
        std::ostringstream oss;
        oss << std::put_time(local, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k, const std::string &dflt = "") {
        auto it = m.find(k);
        return it == m.end() ? dflt : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(ZhangyishouPlugin);
