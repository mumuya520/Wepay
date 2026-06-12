#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class HlpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "hlpay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "应用APPID", "input");
        add("appkey", "商户私钥", "textarea");
        add("appsecret", "平台公钥", "textarea");
        add("channelcode", "通道编码", "input", "", "可留空，留空为随机路由");
        add("appmchid", "子商户编码", "input", "", "仅服务商可传，普通商户请勿填写");
        add("appswitch", "场景类型", "select", "1", "1=线下 2=线上");
        add("apptype", "支付方式", "input", "1", "支付宝：1扫码 2JS 3PC 4H5；微信：1扫码 2公众号/小程序");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() || p.get("appsecret", "").asString().empty()) {
            r.errMsg = "汇联支付参数不完整(appid/appkey/appsecret)";
            return r;
        }
        Json::Value biz;
        biz["payType"] = mapPayType(req.payType);
        biz["paySubType"] = mapPaySubType(req.payType, p.get("apptype", "1").asString());
        biz["sceneType"] = p.get("appswitch", "1").asString();
        biz["mchOrderNo"] = req.orderId;
        biz["amount"] = fmtAmount(req.amount);
        biz["clientIp"] = req.clientIp;
        biz["subject"] = req.subject.empty() ? "商品" : req.subject;
        biz["notifyUrl"] = req.notifyUrl;
        biz["redirectUrl"] = req.returnUrl;
        std::string channelCode = p.get("channelcode", "").asString();
        if (!channelCode.empty()) biz["channelCode"] = channelCode;
        std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString();
        std::string subAppid = p.get("sub_appid", "").asString();
        if (!openid.empty()) biz["extra"]["userId"] = openid;
        if (!subAppid.empty()) biz["extra"]["subAppid"] = subAppid;
        if (biz["payType"].asString() == "WECHAT" && (biz["paySubType"].asString() == "H5" || biz["paySubType"].asString() == "APP")) {
            biz["extra"]["originalType"] = 0;
            biz["extra"]["appName"] = req.subject.empty() ? "在线商城" : req.subject;
        }
        Json::Value data = execute(p, "/openapi/pay/create", biz, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.channelOrderNo = data.get("payOrderNo", "").asString();
        r.payUrl = data.get("payInfo", "").asString();
        if (biz["paySubType"].asString() == "NATIVE") r.qrCode = r.payUrl;
        r.extra["payInfo"] = r.payUrl;
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &, const std::string &rawBody, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "sign fail";
        Json::Value j;
        if (!Json::Reader().parse(rawBody, j)) { r.responseText = "No data"; return r; }
        if (!verify(j, channelParams.get("appsecret", "").asString())) return r;
        r.verified = true;
        Json::Value data = j["data"];
        if (data.isString()) Json::Reader().parse(data.asString(), data);
        r.paid = data.get("state", "").asString() == "3" || data.get("state", 0).asInt() == 3;
        r.orderId = data.get("mchOrderNo", "").asString();
        r.channelOrderNo = data.get("payOrderNo", "").asString();
        r.responseText = r.paid ? "success" : "status fail";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        Json::Value biz;
        biz["payOrderNo"] = req.channelOrderNo;
        biz["mchRefundOrderNo"] = req.refundNo;
        biz["amount"] = fmtAmount(req.refundAmount);
        Json::Value data = execute(req.channelParams, "/openapi/pay/refund", biz, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.state = 1;
        r.channelRefundNo = data.get("instOrderNo", req.refundNo).asString();
        return r;
    }

private:
    static Json::Value execute(const Json::Value &p, const std::string &path, const Json::Value &biz, std::string &raw, std::string &err) {
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        Json::Value req;
        req["appId"] = p.get("appid", "").asString();
        req["subSn"] = p.get("appmchid", "").asString();
        req["timestamp"] = std::to_string(std::time(nullptr));
        req["requestId"] = std::to_string((long long)std::time(nullptr) * 1000);
        req["version"] = "1.01";
        req["signType"] = "RSA2";
        req["bizContent"] = Json::writeString(wb, biz);
        req["sign"] = RsaUtils::signSha256(signContent(jsonToMap(req)), normalizePrivateKey(p.get("appkey", "").asString()));
        auto resp = SyncHttp::postJson("https://api.huilianlink.com" + path, Json::writeString(wb, req), {{"Content-Type", "application/json"}});
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "汇联响应解析失败"; return Json::Value(); }
        if (j.get("code", 0).asInt() != 200) { err = j.get("msg", "汇联请求失败").asString(); return Json::Value(); }
        if (!verify(j, p.get("appsecret", "").asString())) { err = "汇联返回数据验签失败"; return Json::Value(); }
        return j["data"];
    }

    static bool verify(Json::Value j, const std::string &pubKey) {
        std::string sign = j.get("sign", "").asString();
        if (sign.empty() || pubKey.empty()) return false;
        if (j.isMember("data")) j["data"] = processData(j["data"]);
        return RsaUtils::verifySha256(signContent(jsonToMap(j)), sign, normalizePublicKey(pubKey));
    }

    static Json::Value processData(const Json::Value &data) {
        Json::Value d = data;
        if (d.isString()) Json::Reader().parse(d.asString(), d);
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        return Json::writeString(wb, sortJson(filterJson(d)));
    }

    static Json::Value filterJson(const Json::Value &v) {
        if (v.isArray()) { Json::Value a(Json::arrayValue); for (auto &x : v) { auto y = filterJson(x); if (!isEmpty(y)) a.append(y); } return a; }
        if (v.isObject()) { Json::Value o(Json::objectValue); for (auto &k : v.getMemberNames()) { auto y = filterJson(v[k]); if (!isEmpty(y)) o[k] = y; } return o; }
        return v;
    }

    static Json::Value sortJson(const Json::Value &v) {
        if (v.isArray()) { Json::Value a(Json::arrayValue); for (auto &x : v) a.append(sortJson(x)); return a; }
        if (v.isObject()) { Json::Value o(Json::objectValue); auto names = v.getMemberNames(); std::sort(names.begin(), names.end()); for (auto &n : names) o[n] = sortJson(v[n]); return o; }
        return v;
    }

    static bool isEmpty(const Json::Value &v) { return v.isNull() || (v.isString() && v.asString().empty()) || ((v.isArray() || v.isObject()) && v.empty()); }
    static std::map<std::string, std::string> jsonToMap(const Json::Value &j) { std::map<std::string, std::string> m; for (auto &k : j.getMemberNames()) if (k != "sign" && !isEmpty(j[k])) m[k] = j[k].asString(); return m; }
    static std::string signContent(const std::map<std::string, std::string> &m) { std::string s; for (auto &kv : m) { if (!s.empty()) s += "&"; s += kv.first + "=" + kv.second; } return s; }
    static std::string mapPayType(const std::string &payType) { if (payType == "wxpay" || payType == "wx_jsapi" || payType == "wx_lite") return "WECHAT"; if (payType == "bank") return "UNION_PAY"; return "ALIPAY"; }
    static std::string mapPaySubType(const std::string &payType, const std::string &apptype) { if (payType == "ali_jsapi" || payType == "wx_jsapi") return "JSAPI"; if (payType == "wx_lite") return "MINI_APP"; if (payType == "ali_wap" || apptype == "4") return "H5"; if (payType == "ali_pc" || apptype == "3") return "PC"; return "NATIVE"; }
    static std::string normalizePrivateKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b += key.substr(i,64)+"\n"; return "-----BEGIN RSA PRIVATE KEY-----\n"+b+"-----END RSA PRIVATE KEY-----\n"; }
    static std::string normalizePublicKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b += key.substr(i,64)+"\n"; return "-----BEGIN PUBLIC KEY-----\n"+b+"-----END PUBLIC KEY-----\n"; }
    static std::string fmtAmount(double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); }
};

REGISTER_CHANNEL_PLUGIN(HlpayPlugin);
