#pragma once
#include "ChannelPlugin.h"
#include <cmath>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <openssl/evp.h>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class Fuiou2Plugin : public ChannelPlugin {
public:
    std::string name() const override { return "fuiou2"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appid", "机构号", "input");
        add("appmchid", "商户号", "input");
        add("appsecret", "商户私钥", "textarea");
        add("appkey", "富友公钥", "textarea");
        add("appurl", "订单号前缀", "input");
        add("entrykey", "代理进件密钥", "input", "", "不使用进件或投诉接口可不填写");
        add("appswitch", "环境选择", "select", "0", "0=生产环境 1=测试环境");
        add("apptype", "支付方式", "input", "1", "1=扫码支付 2=公众号/小程序支付");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appmchid", "").asString().empty() ||
            p.get("appkey", "").asString().empty() || p.get("appsecret", "").asString().empty()) {
            result.errMsg = "富友参数不完整(appid/appmchid/appkey/appsecret)";
            return result;
        }
        bool js = req.payType == "wx_jsapi" || req.payType == "ali_jsapi" || p.get("apptype", "1").asString() == "2";
        std::map<std::string, std::string> biz;
        std::string orderNo = p.get("appurl", "").asString() + req.orderId;
        biz[js ? "trade_type" : "order_type"] = js ? mapTradeType(req.payType) : mapOrderType(req.payType);
        biz["order_amt"] = std::to_string((long long)std::llround(req.amount * 100.0));
        biz["mchnt_order_no"] = orderNo;
        biz["txn_begin_ts"] = currentTs();
        biz["goods_des"] = req.subject.empty() ? "商品" : req.subject;
        biz["term_ip"] = req.clientIp;
        biz["notify_url"] = req.notifyUrl;
        biz["goods_detail"] = "";
        biz["addn_inf"] = "";
        biz["curr_type"] = "CNY";
        biz["goods_tag"] = "";
        if (js) {
            biz["limit_pay"] = "";
            biz["product_id"] = "";
            biz["openid"] = "";
            biz["sub_openid"] = p.get("openid", p.get("sub_openid", "").asString()).asString();
            biz["sub_appid"] = p.get("sub_appid", "").asString();
        }
        auto xmlResp = submit(p, js ? "/wxPreCreate" : "/preCreate", biz, result.errMsg);
        result.rawResponse = mapToJson(xmlResp);
        if (!result.errMsg.empty()) return result;
        result.success = true;
        result.channelOrderNo = xmlResp["mchnt_order_no"];
        if (js) {
            Json::Value pack;
            pack["appId"] = xmlResp["sdk_appid"];
            pack["timeStamp"] = xmlResp["sdk_timestamp"];
            pack["nonceStr"] = xmlResp["sdk_noncestr"];
            pack["package"] = xmlResp["sdk_package"];
            pack["signType"] = xmlResp["sdk_signtype"];
            pack["paySign"] = xmlResp["sdk_paysign"];
            Json::StreamWriterBuilder wb; wb["indentation"] = "";
            result.extra["payinfo"] = Json::writeString(wb, pack);
            result.payUrl = xmlResp["reserved_transaction_id"];
        } else {
            result.payUrl = xmlResp["qr_code"];
            result.qrCode = result.payUrl;
        }
        return result;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "0";
        std::string xml = urlDecode(get(params, "req"));
        if (xml.empty()) return r;
        auto m = parseXml(xml);
        if (!verify(m, channelParams.get("appkey", "").asString())) return r;
        r.verified = true;
        r.paid = m["result_code"] == "000000";
        r.orderId = stripPrefix(m["mchnt_order_no"], channelParams.get("appurl", "").asString());
        r.channelOrderNo = m["mchnt_order_no"];
        r.buyerId = m["user_id"];
        try { r.paidAmount = std::stod(m["order_amt"]) / 100.0; } catch (...) {}
        r.responseText = r.paid ? "1" : "0";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        std::map<std::string, std::string> biz;
        biz["mchnt_order_no"] = req.channelOrderNo.empty() ? req.channelParams.get("appurl", "").asString() + req.orderId : req.channelOrderNo;
        biz["refund_order_no"] = req.refundNo;
        biz["order_type"] = req.channelParams.get("refund_order_type", "ALIPAY").asString();
        biz["total_amt"] = std::to_string((long long)std::llround(req.paidAmount * 100.0));
        biz["refund_amt"] = std::to_string((long long)std::llround(req.refundAmount * 100.0));
        biz["operator_id"] = "";
        auto resp = submit(req.channelParams, "/commonRefund", biz, r.errMsg);
        r.rawResponse = mapToJson(resp);
        if (!r.errMsg.empty()) return r;
        r.success = true;
        r.state = 1;
        r.channelRefundNo = resp["mchnt_order_no"];
        return r;
    }

private:
    static std::map<std::string, std::string> submit(const Json::Value &p, const std::string &path,
                                                     std::map<std::string, std::string> params, std::string &err) {
        params["version"] = "1.0";
        params["ins_cd"] = p.get("appid", "").asString();
        params["mchnt_cd"] = p.get("appmchid", "").asString();
        params["term_id"] = "88888888";
        params["random_str"] = randomStr(16);
        params["sign"] = rsaMd5Sign(signString(params), normalizePrivateKey(p.get("appsecret", "").asString()));
        std::string xml = "<?xml version=\"1.0\" encoding=\"GBK\" standalone=\"yes\"?><xml>" + toXml(params) + "</xml>";
        auto resp = SyncHttp::postForm(gateway(p) + path, "req=" + urlEncode(urlEncode(xml)));
        if (!resp.success) { err = resp.errMsg; return {}; }
        std::string decoded = urlDecode(resp.body);
        auto m = parseXml(decoded);
        if (m["result_code"] != "000000" && m["result_code"] != "030010") {
            err = m["result_msg"].empty() ? "富友返回失败" : m["result_msg"];
            return m;
        }
        if (!verify(m, p.get("appkey", "").asString())) {
            err = "富友返回数据验签失败";
            return m;
        }
        return m;
    }

    static std::string signString(const std::map<std::string, std::string> &params) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.first.rfind("reserved", 0) == 0) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        return s;
    }

    static bool verify(const std::map<std::string, std::string> &m, const std::string &pubKey) {
        auto it = m.find("sign");
        if (it == m.end() || pubKey.empty()) return false;
        return rsaMd5Verify(signString(m), it->second, normalizePublicKey(pubKey));
    }

    static std::string rsaMd5Sign(const std::string &data, const std::string &pem) {
        EVP_PKEY *pkey = RsaUtils::loadPrivateKey(pem);
        if (!pkey) return "";
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        std::string out;
        if (ctx && EVP_DigestSignInit(ctx, nullptr, EVP_md5(), nullptr, pkey) == 1 && EVP_DigestSignUpdate(ctx, data.data(), data.size()) == 1) {
            size_t len = 0; EVP_DigestSignFinal(ctx, nullptr, &len);
            std::vector<unsigned char> sig(len);
            if (EVP_DigestSignFinal(ctx, sig.data(), &len) == 1) out = RsaUtils::base64Encode(sig.data(), len);
        }
        if (ctx) EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return out;
    }

    static bool rsaMd5Verify(const std::string &data, const std::string &signB64, const std::string &pem) {
        EVP_PKEY *pkey = RsaUtils::loadPublicKey(pem);
        if (!pkey) return false;
        auto sig = RsaUtils::base64Decode(signB64);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        bool ok = false;
        if (ctx && !sig.empty() && EVP_DigestVerifyInit(ctx, nullptr, EVP_md5(), nullptr, pkey) == 1 && EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1) {
            ok = EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1;
        }
        if (ctx) EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return ok;
    }

    static std::string toXml(const std::map<std::string, std::string> &m) {
        std::string s;
        for (auto &kv : m) s += "<" + kv.first + ">" + xmlEscape(kv.second) + "</" + kv.first + ">";
        return s;
    }

    static std::map<std::string, std::string> parseXml(const std::string &xml) {
        std::map<std::string, std::string> m;
        size_t pos = 0;
        while ((pos = xml.find('<', pos)) != std::string::npos) {
            size_t e = xml.find('>', pos + 1); if (e == std::string::npos) break;
            std::string tag = xml.substr(pos + 1, e - pos - 1);
            if (tag.empty() || tag[0] == '?' || tag == "xml" || tag[0] == '/') { pos = e + 1; continue; }
            std::string close = "</" + tag + ">";
            size_t c = xml.find(close, e + 1); if (c == std::string::npos) { pos = e + 1; continue; }
            m[tag] = xml.substr(e + 1, c - e - 1);
            pos = c + close.size();
        }
        return m;
    }

    static std::string gateway(const Json::Value &p) { return p.get("appswitch", "0").asString() == "1" ? "https://fundwx.fuiou.com" : "https://spay-mc.fuioupay.com"; }
    static std::string mapOrderType(const std::string &payType) { if (payType == "wxpay" || payType == "wx_jsapi") return "WECHAT"; if (payType == "bank") return "UNIONPAY"; return "ALIPAY"; }
    static std::string mapTradeType(const std::string &payType) { if (payType == "ali_jsapi" || payType == "alipay") return "FWC"; if (payType == "wx_lite") return "LETPAY"; return "JSAPI"; }
    static std::string currentTs() {
        auto t = std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char b[16];
        std::strftime(b, sizeof(b), "%Y%m%d%H%M%S", &tmv);
        return b;
    }
    static std::string randomStr(int n) { static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; std::mt19937 rng((unsigned)std::random_device{}()); std::string s; for (int i = 0; i < n; ++i) s += cs[rng() % (sizeof(cs)-1)]; return s; }
    static std::string normalizePrivateKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b += key.substr(i,64)+"\n"; return "-----BEGIN RSA PRIVATE KEY-----\n"+b+"-----END RSA PRIVATE KEY-----\n"; }
    static std::string normalizePublicKey(const std::string &key) { if (key.find("-----BEGIN") != std::string::npos) return key; std::string b; for (size_t i=0;i<key.size();i+=64) b += key.substr(i,64)+"\n"; return "-----BEGIN PUBLIC KEY-----\n"+b+"-----END PUBLIC KEY-----\n"; }
    static std::string stripPrefix(const std::string &s, const std::string &p) { return !p.empty() && s.rfind(p,0)==0 ? s.substr(p.size()) : s; }
    static std::string xmlEscape(std::string s) { return s; }
    static std::string mapToJson(const std::map<std::string,std::string> &m) { Json::Value j; for (auto &kv:m) j[kv.first]=kv.second; Json::StreamWriterBuilder wb; wb["indentation"]=""; return Json::writeString(wb,j); }
    static std::string urlEncode(const std::string &s) { std::ostringstream oss; for(unsigned char c:s){ if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') oss<<c; else oss<<'%'<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)c; } return oss.str(); }
    static std::string urlDecode(const std::string &s) { std::string o; for(size_t i=0;i<s.size();++i){ if(s[i]=='%'&&i+2<s.size()){ char h[3]={s[i+1],s[i+2],0}; o+=(char)strtol(h,nullptr,16); i+=2; } else if(s[i]=='+') o+=' '; else o+=s[i]; } return o; }
    static std::string get(const std::map<std::string,std::string> &m,const std::string &k){auto it=m.find(k);return it==m.end()?"":it->second;}
};

REGISTER_CHANNEL_PLUGIN(Fuiou2Plugin);
