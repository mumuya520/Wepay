#pragma once
#include "ChannelPlugin.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include "../common/Md5Utils.h"
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class EasypayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "easypay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("reqtype", "接入模式", "select", "2", "2=机构模式 1=商户模式");
        add("appid", "机构号/商户号", "input", "", "reqId");
        add("appmchid", "子商户号", "input", "", "机构模式填写子商户号，商户模式留空");
        add("appkey", "易生公钥", "textarea", "", "不能有换行和标签");
        add("appsecret", "商户私钥", "textarea", "", "不能有换行和标签");
        add("appswitch", "环境选择", "select", "0", "0=生产 1=测试");
        add("apptype", "支付方式", "input", "1", "支付宝/云闪付：1主扫 2JSAPI");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string reqId = p.get("appid", "").asString();
        std::string priKey = p.get("appsecret", "").asString();
        if (reqId.empty() || priKey.empty()) {
            result.errMsg = "易生参数不完整(appid/appsecret)";
            return result;
        }
        bool js = shouldJs(req.payType, p.get("apptype", "1").asString());
        std::string payType = mapPayType(req.payType, js);
        Json::Value body = buildBody(req, p, payType);
        Json::Value respBody = execute(p, js ? "/trade/jsapi" : "/trade/native", body, result.rawResponse, result.errMsg);
        if (!result.errMsg.empty()) return result;
        auto &state = respBody["respStateInfo"];
        if (state.get("respCode", "").asString() != "000000") {
            result.errMsg = state.get("respDesc", "易生下单失败").asString();
            return result;
        }
        if (state.get("transState", "").asString() == "X") {
            result.errMsg = state.get("appendRetMsg", state.get("transStatusDesc", "易生交易失败").asString()).asString();
            return result;
        }
        result.success = true;
        result.channelOrderNo = respBody["respOrderInfo"].get("outTrace", "").asString();
        if (!js) {
            result.qrCode = respBody["respOrderInfo"].get("qrCode", "").asString();
            result.payUrl = result.qrCode;
        } else if (payType.rfind("AliPay", 0) == 0) {
            result.payUrl = respBody["aliRespParamInfo"].get("tradeNo", "").asString();
            result.extra["trade_no"] = result.payUrl;
        } else if (payType.rfind("WeChat", 0) == 0) {
            result.extra["payinfo"] = respBody["wxRespParamInfo"].get("wcPayData", "").asString();
        } else {
            result.payUrl = respBody["qrRespParamInfo"].get("qrRedirectUrl", "").asString();
        }
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &,
                                     const std::string &rawBody,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "{\"code\":\"100001\",\"msg\":\"sign error\"}";
        Json::Value j;
        if (!Json::Reader().parse(rawBody, j)) {
            r.responseText = "{\"code\":\"400001\",\"msg\":\"no data\"}";
            return r;
        }
        r.verified = verifySign(j["reqHeader"], j["reqBody"], j.get("reqSign", "").asString(), channelParams.get("appkey", "").asString());
        if (!r.verified) return r;
        Json::Value data = j["reqBody"];
        std::string state = data["respStateInfo"].get("transState", "").asString();
        r.paid = (state == "0" || state == "1");
        r.orderId = data["respOrderInfo"].get("orgTrace", "").asString();
        r.channelOrderNo = data["respOrderInfo"].get("outTrace", "").asString();
        r.buyerId = data["respOrderInfo"].get("userId", "").asString();
        try { r.paidAmount = data["respOrderInfo"].get("transAmount", 0).asInt64() / 100.0; } catch (...) {}
        r.responseText = "{\"code\":\"000000\",\"msg\":\"Success\"}";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        Json::Value body;
        auto &p = req.channelParams;
        body["reqInfo"]["mchtCode"] = mchtCode(p);
        body["reqOrderInfo"]["orgTrace"] = req.refundNo;
        body["reqOrderInfo"]["oriOutTrace"] = req.channelOrderNo;
        body["reqOrderInfo"]["oriTransDate"] = req.orderId.size() >= 8 ? req.orderId.substr(0, 8) : currentDate();
        body["reqOrderInfo"]["refundAmount"] = (Json::Int64)std::llround(req.refundAmount * 100.0);
        body["payInfo"]["transDate"] = currentDate();
        Json::Value respBody = execute(p, "/trade/refund/apply", body, r.rawResponse, r.errMsg);
        if (!r.errMsg.empty()) return r;
        if (respBody["respStateInfo"].get("respCode", "").asString() != "000000") {
            r.errMsg = respBody["respStateInfo"].get("respDesc", "易生退款失败").asString();
            return r;
        }
        if (respBody["respStateInfo"].get("transState", "").asString() == "X") {
            r.errMsg = respBody["respStateInfo"].get("appendRetMsg", respBody["respStateInfo"].get("transStatusDesc", "易生退款失败").asString()).asString();
            return r;
        }
        r.success = true;
        r.state = 1;
        r.channelRefundNo = respBody.get("outTrace", "").asString();
        return r;
    }

private:
    static Json::Value execute(const Json::Value &p, const std::string &path, const Json::Value &reqBody,
                               std::string &raw, std::string &err) {
        Json::Value req;
        req["reqBody"] = reqBody;
        req["reqHeader"]["transTime"] = currentTs();
        req["reqHeader"]["reqId"] = p.get("appid", "").asString();
        req["reqHeader"]["reqType"] = p.get("reqtype", "2").asString();
        req["reqSign"] = sign(req["reqHeader"], req["reqBody"], p.get("appsecret", "").asString());
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string body = Json::writeString(wb, req);
        auto resp = SyncHttp::postJson(gateway(p) + path, body, {{"Content-Type", "application/json; charset=utf-8"}});
        raw = resp.body;
        if (!resp.success) { err = resp.errMsg; return Json::Value(); }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { err = "易生响应解析失败"; return Json::Value(); }
        if (j["rspHeader"].get("rspCode", "").asString() != "000000") {
            err = "[" + j["rspHeader"].get("rspCode", "").asString() + "]" + j["rspHeader"].get("rspInfo", "易生请求失败").asString();
            return Json::Value();
        }
        if (!verifySign(j["rspHeader"], j["rspBody"], j.get("rspSign", "").asString(), p.get("appkey", "").asString())) {
            err = "易生返回数据验签失败";
            return Json::Value();
        }
        return j["rspBody"];
    }

    static Json::Value buildBody(const ChannelOrderRequest &req, const Json::Value &p, const std::string &payType) {
        Json::Value body;
        body["reqInfo"]["mchtCode"] = mchtCode(p);
        body["reqOrderInfo"]["orgTrace"] = req.orderId;
        body["reqOrderInfo"]["transAmount"] = (Json::Int64)std::llround(req.amount * 100.0);
        body["reqOrderInfo"]["orderSub"] = req.subject.empty() ? "商品" : req.subject;
        body["reqOrderInfo"]["backUrl"] = req.notifyUrl;
        body["payInfo"]["payType"] = payType;
        body["payInfo"]["transDate"] = currentDate();
        body["settleParamInfo"]["delaySettleFlag"] = "0";
        body["settleParamInfo"]["patnerSettleFlag"] = "0";
        body["settleParamInfo"]["splitSettleFlag"] = "0";
        body["riskData"]["customerIp"] = req.clientIp;
        if (payType.rfind("UnionPay", 0) == 0) {
            body["qrBizParam"]["transType"] = "10";
            body["qrBizParam"]["areaInfo"] = "1561000";
        }
        std::string openid = p.get("openid", "").asString();
        std::string appid = p.get("sub_appid", "").asString();
        if (!openid.empty() && payType.rfind("AliPay", 0) == 0) body["aliBizParam"]["buyerId"] = openid;
        if (!openid.empty() && payType.rfind("WeChat", 0) == 0) { body["wxBizParam"]["subAppid"] = appid; body["wxBizParam"]["subOpenId"] = openid; }
        return body;
    }

    static std::string mchtCode(const Json::Value &p) {
        return p.get("reqtype", "2").asString() == "2" ? p.get("appmchid", "").asString() : p.get("appid", "").asString();
    }

    static std::string mapPayType(const std::string &payType, bool js) {
        if (payType == "wxpay" || payType == "wx_jsapi") return js ? "WeChatJsapi" : "WeChatNative";
        if (payType == "bank" || payType == "ysf_qr") return js ? "UnionPayJsapi" : "UnionPayNative";
        return js ? "AliPayJsapi" : "AliPayNative";
    }

    static bool shouldJs(const std::string &payType, const std::string &apptype) {
        return payType == "ali_jsapi" || payType == "wx_jsapi" || ("," + apptype + ",").find(",2,") != std::string::npos;
    }

    static std::string sign(const Json::Value &header, const Json::Value &body, const std::string &privateKey) {
        return RsaUtils::signSha256(signContent(header, body), normalizePrivateKey(privateKey));
    }

    static bool verifySign(const Json::Value &header, const Json::Value &body, const std::string &sig, const std::string &publicKey) {
        if (sig.empty() || publicKey.empty()) return false;
        return RsaUtils::verifySha256(signContent(header, body), sig, normalizePublicKey(publicKey));
    }

    static std::string signContent(const Json::Value &header, const Json::Value &body) {
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        return Json::writeString(wb, sortJson(header)) + upper(Md5Utils::md5(Json::writeString(wb, sortJson(body))));
    }

    static Json::Value sortJson(const Json::Value &v) {
        if (v.isArray()) {
            Json::Value a(Json::arrayValue);
            for (auto &x : v) a.append(sortJson(x));
            return a;
        }
        if (v.isObject()) {
            Json::Value o(Json::objectValue);
            std::vector<std::string> names = v.getMemberNames();
            std::sort(names.begin(), names.end());
            for (auto &n : names) o[n] = sortJson(v[n]);
            return o;
        }
        return v;
    }

    static std::string gateway(const Json::Value &p) {
        return p.get("appswitch", "0").asString() == "1" ? "https://d-phoenix-gap.easypay.com.cn:24443/yqt" : "https://phoenix.eycard.cn/yqt";
    }

    static std::string normalizePrivateKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string body;
        for (size_t i = 0; i < key.size(); i += 64) body += key.substr(i, 64) + "\n";
        return "-----BEGIN RSA PRIVATE KEY-----\n" + body + "-----END RSA PRIVATE KEY-----\n";
    }

    static std::string normalizePublicKey(const std::string &key) {
        if (key.find("-----BEGIN") != std::string::npos) return key;
        std::string body;
        for (size_t i = 0; i < key.size(); i += 64) body += key.substr(i, 64) + "\n";
        return "-----BEGIN PUBLIC KEY-----\n" + body + "-----END PUBLIC KEY-----\n";
    }

    static std::string currentTs() { return timeFmt("%Y%m%d%H%M%S"); }
    static std::string currentDate() { return timeFmt("%Y%m%d"); }
    static std::string timeFmt(const char *fmt) {
        auto t = std::time(nullptr); struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[32]; std::strftime(buf, sizeof(buf), fmt, &tmv); return buf;
    }

    static std::string upper(std::string s) {
        for (char &c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }
};

REGISTER_CHANNEL_PLUGIN(EasypayPlugin);
