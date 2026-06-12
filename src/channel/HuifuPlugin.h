#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class HuifuPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "huifu"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "汇付系统号", "input", "", "主体为渠道商填渠道商ID，主体为直连商户填商户ID");
        add("appurl", "汇付产品号", "input");
        add("appsecret", "商户私钥", "textarea");
        add("appkey", "汇付公钥", "textarea");
        add("appmchid", "汇付子商户号", "input", "", "主体为渠道商时填写，直连商户可留空");
        add("project_id", "半支付托管项目号", "input", "", "仅托管H5/PC支付需要填写");
        add("seq_id", "托管小程序应用ID", "input", "", "仅托管小程序支付可填写");
        add("apptype", "支付方式", "input", "1", "支付宝:1扫码 2托管H5/PC 3托管小程序 4JS；微信:1自有JSAPI 2托管H5/PC 3托管小程序；银行卡:1银联扫码 2快捷 3网银 4银联JS");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appurl", "").asString().empty() || p.get("appsecret", "").asString().empty() || p.get("appkey", "").asString().empty()) {
            r.errMsg = "汇付参数不完整(appid/appurl/appsecret/appkey)";
            return r;
        }
        Json::Value data;
        data["req_date"] = reqDate(req.orderId);
        data["req_seq_id"] = req.orderId;
        data["huifu_id"] = huifuId(p);
        data["trade_type"] = mapTradeType(req.payType);
        data["trans_amt"] = fmtAmount(req.amount);
        data["goods_desc"] = req.subject.empty() ? "商品" : req.subject;
        data["notify_url"] = req.notifyUrl;
        Json::Value risk;
        risk["ip_addr"] = req.clientIp;
        data["risk_check_data"] = jsonCompact(risk);
        if (data["trade_type"].asString() == "A_NATIVE") {
            Json::Value ali; ali["subject"] = data["goods_desc"].asString(); data["alipay_data"] = jsonCompact(ali);
        } else if (data["trade_type"].asString() == "T_NATIVE") {
            Json::Value wx; wx["product_id"] = "01001"; wx["spbill_create_ip"] = req.clientIp; data["wx_data"] = jsonCompact(wx);
        } else if (data["trade_type"].asString() == "T_JSAPI" || data["trade_type"].asString() == "T_MINIAPP") {
            Json::Value wx; std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString(); wx["sub_openid"] = openid; wx["openid"] = openid; wx["device_info"] = "4"; wx["spbill_create_ip"] = req.clientIp; data["wx_data"] = jsonCompact(wx);
        } else if (data["trade_type"].asString() == "A_JSAPI") {
            Json::Value ali; ali["subject"] = data["goods_desc"].asString(); ali["buyer_id"] = p.get("buyer_id", p.get("openid", "").asString()).asString(); data["alipay_data"] = jsonCompact(ali);
        } else if (data["trade_type"].asString() == "U_JSAPI") {
            Json::Value up; up["qr_code"] = req.returnUrl; up["customer_ip"] = req.clientIp; up["user_id"] = p.get("user_id", p.get("openid", "").asString()).asString(); data["unionpay_data"] = jsonCompact(up);
        }
        Json::Value resp = requestApi(p, "/v3/trade/payment/jspay", data, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        std::string code = resp.get("resp_code", "").asString();
        if (code != "00000100" && code != "00000000") { r.errMsg = resp.get("resp_desc", "汇付下单失败").asString(); return r; }
        r.success = true;
        r.channelOrderNo = resp.get("hf_seq_id", "").asString();
        if (resp.isMember("pay_info")) r.payUrl = resp["pay_info"].asString();
        else r.payUrl = resp.get("qr_code", "").asString();
        r.qrCode = r.payUrl;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "sign fail";
        std::string dataStr = get(params, "resp_data");
        std::string sign = get(params, "sign");
        if (dataStr.empty() || sign.empty()) return r;
        r.verified = RsaUtils::verifySha256(dataStr, sign, normalizePublicKey(channelParams.get("appkey", "").asString()));
        if (!r.verified) return r;
        Json::Value data;
        if (!Json::Reader().parse(dataStr, data)) { r.responseText = "no data"; return r; }
        r.paid = data.get("trans_stat", "").asString() == "S";
        r.orderId = data.get("req_seq_id", "").asString();
        r.channelOrderNo = data.get("hf_seq_id", "").asString();
        r.responseText = r.paid ? ("RECV_ORD_ID_" + r.orderId) : "resp_code fail";
        if (data.isMember("alipay_response")) r.buyerId = data["alipay_response"].get("buyer_id", "").asString();
        else if (data.isMember("wx_response")) r.buyerId = data["wx_response"].get("sub_openid", "").asString();
        try { r.paidAmount = std::stod(data.get("trans_amt", "0").asString()); } catch (...) {}
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        Json::Value data;
        data["req_date"] = today();
        data["req_seq_id"] = req.refundNo;
        data["huifu_id"] = huifuId(req.channelParams);
        data["ord_amt"] = fmtAmount(req.refundAmount);
        data["org_req_date"] = reqDate(req.orderId);
        data["org_req_seq_id"] = req.orderId;
        Json::Value resp = requestApi(req.channelParams, "/v3/trade/payment/scanpay/refund", data, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        std::string code = resp.get("resp_code", "").asString();
        if (code != "00000000" && code != "00000100") { r.errMsg = resp.get("resp_desc", "汇付退款失败").asString(); return r; }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = resp.get("hf_seq_id", req.refundNo).asString();
        return r;
    }

    ChannelCloseResult close(const ChannelCloseRequest &req) override {
        ChannelCloseResult r;
        Json::Value data;
        data["req_date"] = today();
        data["req_seq_id"] = now() + "0001";
        data["huifu_id"] = huifuId(req.channelParams);
        data["org_req_date"] = reqDate(req.orderId);
        data["org_req_seq_id"] = req.orderId;
        std::string raw, err;
        Json::Value resp = requestApi(req.channelParams, "/v2/trade/payment/scanpay/close", data, raw, err);
        if (!err.empty()) { r.errMsg = err; return r; }
        std::string code = resp.get("resp_code", "").asString();
        if (code != "00000000" && code != "00000100") { r.errMsg = resp.get("resp_desc", "汇付关单失败").asString(); return r; }
        r.success = true;
        return r;
    }

private:
    static Json::Value requestApi(const Json::Value &p, const std::string &path, Json::Value data, std::string &raw, std::string &err) {
        Json::Value body;
        body["sys_id"] = p.get("appid", "").asString();
        body["product_id"] = p.get("appurl", "").asString();
        body["data"] = data;
        body["sign"] = RsaUtils::signSha256(sortedJson(data), normalizePrivateKey(p.get("appsecret", "").asString()));
        auto resp = SyncHttp::postJson("https://api.huifu.com" + path, jsonCompact(body), {{"Content-Type", "application/json; charset=utf-8"}});
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "汇付响应解析失败"; return Json::Value(); }
        if (!j.isMember("data") || !j.isMember("sign")) { err = "汇付响应缺少data/sign"; return Json::Value(); }
        if (!RsaUtils::verifySha256(sortedJson(j["data"]), j["sign"].asString(), normalizePublicKey(p.get("appkey", "").asString()))) { err = "汇付响应验签失败"; return Json::Value(); }
        return j["data"];
    }

    static Json::Value sortedFiltered(const Json::Value &v) {
        if (!v.isObject()) return v;
        Json::Value o(Json::objectValue);
        auto names = v.getMemberNames();
        std::sort(names.begin(), names.end());
        for (auto &n : names) if (!v[n].isNull()) o[n] = v[n];
        return o;
    }
    static std::string sortedJson(const Json::Value &v) { return jsonCompact(sortedFiltered(v)); }
    static std::string jsonCompact(const Json::Value &v) { Json::StreamWriterBuilder wb; wb["indentation"] = ""; return Json::writeString(wb, v); }
    static std::string huifuId(const Json::Value &p) { std::string sub = p.get("appmchid", "").asString(); return sub.empty() ? p.get("appid", "").asString() : sub; }
    static std::string mapTradeType(const std::string &payType) { if (payType == "wxpay") return "T_NATIVE"; if (payType == "wx_jsapi") return "T_JSAPI"; if (payType == "wx_lite") return "T_MINIAPP"; if (payType == "bank") return "U_NATIVE"; if (payType == "bank_jsapi") return "U_JSAPI"; if (payType == "ecny") return "D_NATIVE"; if (payType == "ali_jsapi") return "A_JSAPI"; return "A_NATIVE"; }
    static std::string normalizePrivateKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b += key.substr(i,64)+"\n"; return "-----BEGIN PRIVATE KEY-----\n"+b+"-----END PRIVATE KEY-----\n"; }
    static std::string normalizePublicKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b += key.substr(i,64)+"\n"; return "-----BEGIN PUBLIC KEY-----\n"+b+"-----END PUBLIC KEY-----\n"; }
    static std::string reqDate(const std::string &orderId) { return orderId.size() >= 8 ? orderId.substr(0,8) : today(); }
    static std::string today() { return now().substr(0,8); }
    static std::string now() {
        auto t=std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv,&t);
#else
        localtime_r(&t,&tmv);
#endif
        char b[16]; std::strftime(b,sizeof(b),"%Y%m%d%H%M%S",&tmv); return b;
    }
    static std::string fmtAmount(double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); }
    static std::string get(const std::map<std::string,std::string>&m,const std::string&k){auto it=m.find(k);return it==m.end()?"":it->second;}
};

REGISTER_CHANNEL_PLUGIN(HuifuPlugin);
