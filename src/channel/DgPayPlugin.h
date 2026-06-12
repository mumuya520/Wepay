// WePay-Cpp — 斗拱(汇付) DgPay 插件
// 汇付天下斗拱开放平台 https://paas.huifu.com/
// 签名: RSA-SHA1 / SHA256
//
// channelParams 必填:
//   sys_id         系统商编号
//   product_id     产品号
//   huifu_id       商户号
//   private_key    商户私钥 PEM
//   dg_pub_key     斗拱公钥 PEM (验签)
//   gateway        网关, 默认 https://api.huifu.com
//   pay_method     "qr"(聚合正扫) / "bar"(反扫) / "jsapi"
#pragma once
#include "ChannelPlugin.h"
#include <sstream>
#include <ctime>
#include <random>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class DgPayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "dgpay"; }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string sysId     = p.get("sys_id", "").asString();
        std::string productId = p.get("product_id", "").asString();
        std::string huifuId   = p.get("huifu_id", "").asString();
        std::string priKey    = p.get("private_key", "").asString();
        std::string gateway   = p.get("gateway", "https://api.huifu.com").asString();
        std::string method    = p.get("pay_method", "qr").asString();

        if (huifuId.empty() || priKey.empty()) {
            result.errMsg = "斗拱参数不完整(huifu_id/private_key)"; return result;
        }

        std::string apiPath;
        if (method == "qr")       apiPath = "/v2/trade/payment/jspay";      // 聚合正扫
        else if (method == "bar") apiPath = "/v2/trade/payment/micropay";    // 反扫
        else if (method == "jsapi") apiPath = "/v2/trade/payment/jspay";     // JSAPI
        else apiPath = "/v2/trade/payment/jspay";

        Json::Value data;
        data["req_seq_id"]  = req.orderId;
        data["req_date"]    = currentDate();
        data["huifu_id"]    = huifuId;
        data["trade_type"]  = (method == "bar") ? "A_NATIVE" : "T_MINIAPP";
        data["trans_amt"]   = fmtAmount(req.amount);
        data["goods_desc"]  = req.subject.empty() ? "商品" : req.subject;
        data["notify_url"]  = req.notifyUrl;
        if (method == "bar") {
            data["auth_code"] = p.get("auth_code", "").asString();
        }

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string dataStr = Json::writeString(wb, data);
        std::string sign = RsaUtils::signSha256(dataStr, priKey);

        Json::Value body;
        body["sys_id"]     = sysId;
        body["product_id"] = productId;
        body["data"]       = data;
        body["sign"]       = sign;
        std::string bodyStr = Json::writeString(wb, body);

        auto resp = SyncHttp::postJson(gateway + apiPath, bodyStr);
        result.rawResponse = resp.body;
        if (!resp.success) { result.errMsg = "斗拱请求失败: " + resp.errMsg; return result; }

        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { result.errMsg = "响应解析失败"; return result; }
        std::string respCode = j.get("resp_code", "").asString();
        if (respCode != "00000000" && respCode != "00000100") {  // 00000100=处理中
            result.errMsg = "斗拱: " + respCode + " " + j.get("resp_desc", "").asString();
            return result;
        }
        auto &d = j["data"];
        if (method == "bar") {
            std::string status = d.get("trans_stat", "").asString();
            result.success = true;
            result.extra["paid"] = (status == "S");
            result.channelOrderNo = d.get("hf_seq_id", "").asString();
        } else {
            std::string qrUrl = d.get("qr_code", d.get("pay_info", "").asString()).asString();
            result.success = !qrUrl.empty();
            result.payUrl = qrUrl; result.qrCode = qrUrl;
            result.channelOrderNo = d.get("hf_seq_id", "").asString();
        }
        return result;
    }

    ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "RECV_ORD_ID_" + std::to_string(std::time(nullptr));
        std::string pubKey = channelParams.get("dg_pub_key", "").asString();

        Json::Value body;
        if (Json::Reader().parse(rawBody, body)) {
            std::string data = body.get("data", "").asString();
            std::string sign = body.get("sign", "").asString();
            if (!pubKey.empty() && !sign.empty()) {
                r.verified = RsaUtils::verifySha256(data, sign, pubKey);
            } else r.verified = true;
            if (r.verified) {
                Json::Value d;
                if (Json::Reader().parse(data, d)) {
                    std::string stat = d.get("trans_stat", "").asString();
                    r.paid = (stat == "S");
                    r.orderId = d.get("req_seq_id", "").asString();
                    r.channelOrderNo = d.get("hf_seq_id", "").asString();
                    try { r.paidAmount = std::stod(d.get("trans_amt", "0").asString()); } catch(...){}
                }
            }
        }
        return r;
    }

private:
    static std::string currentDate() {
        auto now = std::time(nullptr); struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &t);
        return buf;
    }
    static std::string fmtAmount(double v) {
        std::ostringstream oss; oss.precision(2);
        oss << std::fixed << v; return oss.str();
    }
};

REGISTER_CHANNEL_PLUGIN(DgPayPlugin);
