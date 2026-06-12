#pragma once
#include "ChannelPlugin.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include "../common/RsaUtils.h"
#include "../common/SyncHttp.h"

class HnaPayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "hnapay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t, const std::string &dflt = "", const std::string &help = "") {
            Json::Value v; v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt; if (!help.empty()) v["help"] = help; arr.append(v);
        };
        add("appid", "商户ID", "input", "", "新生用户ID");
        add("appkey", "新生公钥(新收款密钥)", "textarea");
        add("appsecret", "商户私钥(新收款密钥)", "textarea");
        add("appmchid", "报备编号", "input", "", "支付宝/微信需要填写");
        add("appswitch", "接口类型", "select", "2", "0=公众号/生活号支付 1=支付宝H5 2=扫码支付");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        if (p.get("appid", "").asString().empty() || p.get("appkey", "").asString().empty() || p.get("appsecret", "").asString().empty()) {
            r.errMsg = "新生支付参数不完整(appid/appkey/appsecret)";
            return r;
        }
        std::string mode = p.get("appswitch", "2").asString();
        if (mode == "2" || req.payType == "bank") return createScan(req);
        if (mode == "1" || req.payType == "ali_wap") return createH5(req);
        return createJsapi(req);
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params, const std::string &, const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "fail";
        std::string pub = normalizePublicKey(channelParams.get("appkey", "").asString());
        bool oldScan = !get(params, "signMsg").empty();
        bool ok = false;
        if (oldScan) {
            ok = rsaSha1VerifyHex(signByOrder(params, {"tranCode","version","merId","merOrderNum","tranAmt","submitTime","hnapayOrderId","tranFinishTime","respCode","charset","signType"}), get(params, "signMsg"), pub);
            r.paid = get(params, "respCode") == "0000";
            r.orderId = get(params, "merOrderNum");
            r.channelOrderNo = get(params, "hnapayOrderId");
            try { r.paidAmount = std::stod(get(params, "tranAmt")) / 100.0; } catch (...) {}
        } else {
            ok = RsaUtils::verifySha1(signByOrder(params, {"version","tranCode","merOrderId","merId","merAttach","charset","signType","hnapayOrderId","resultCode","tranAmt","submitTime","tranFinishTime"}), get(params, "signValue"), pub);
            r.paid = get(params, "resultCode") == "0000";
            r.orderId = get(params, "merOrderId");
            r.channelOrderNo = get(params, "hnapayOrderId");
            try { r.paidAmount = std::stod(get(params, "tranAmt")); } catch (...) {}
        }
        r.verified = ok;
        if (ok) r.responseText = r.paid ? "success" : "fail";
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        Json::Value biz;
        biz["oriMerOrderId"] = req.orderId;
        biz["refundAmt"] = fmtCent(req.refundAmount);
        biz["notifyUrl"] = req.notifyUrl;
        std::map<std::string, std::string> params;
        params["version"] = "2.0";
        params["tranCode"] = "EXP09";
        params["merId"] = req.channelParams.get("appid", "").asString();
        params["merOrderId"] = req.refundNo;
        params["submitTime"] = now();
        params["signType"] = "1";
        params["charset"] = "1";
        params["msgCiphertext"] = encryptJson(biz, req.channelParams.get("appkey", "").asString());
        params["signValue"] = RsaUtils::signSha1(signByOrder(params, {"version","tranCode","merId","merOrderId","submitTime","msgCiphertext"}), normalizePrivateKey(req.channelParams.get("appsecret", "").asString()));
        auto resp = SyncHttp::postForm("https://gateway.hnapay.com/exp/refund.do", buildQuery(params));
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "新生退款响应解析失败"; return r; }
        if (j.get("resultCode", "").asString() != "0000") { r.errMsg = j.get("errorMsg", "新生退款失败").asString(); return r; }
        r.success = true; r.state = 1; r.channelRefundNo = req.refundNo;
        return r;
    }

private:
    ChannelOrderResult createScan(const ChannelOrderRequest &req) const {
        ChannelOrderResult r;
        auto &p = req.channelParams;
        std::map<std::string, std::string> m;
        m["tranCode"] = "WS01"; m["version"] = "2.1"; m["merId"] = p.get("appid", "").asString(); m["payType"] = "QRCODE_B2C"; m["charset"] = "1"; m["signType"] = "1";
        m["merOrderNum"] = req.orderId; m["tranAmt"] = fmtCent(req.amount); m["submitTime"] = submitTime(req.orderId); m["orgCode"] = mapOrg(req.payType); m["goodsName"] = req.subject.empty()?"商品":req.subject; m["tranIP"] = req.clientIp; m["notifyUrl"] = req.notifyUrl; m["weChatMchId"] = p.get("appmchid", "").asString();
        m["signMsg"] = rsaSha1SignHex(signByOrder(m, {"tranCode","version","merId","submitTime","merOrderNum","tranAmt","payType","orgCode","notifyUrl","charset","signType"}), normalizePrivateKey(p.get("appsecret", "").asString()));
        auto resp = SyncHttp::postForm("https://gateway.hnapay.com/website/scanPay.do", buildQuery(m));
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j; if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "新生扫码响应解析失败"; return r; }
        if (j.get("resultCode", "").asString() != "0000") { r.errMsg = j.get("msgExt", "新生扫码下单失败").asString(); return r; }
        r.success = true; r.channelOrderNo = j.get("hnapayOrderId", "").asString(); r.qrCode = extractQr(j.get("qrCodeUrl", "").asString()); r.payUrl = r.qrCode; return r;
    }

    ChannelOrderResult createJsapi(const ChannelOrderRequest &req) const { return createEncryptedPay(req, "https://gateway.hnapay.com/ita/inCharge.do", "ITA10"); }
    ChannelOrderResult createH5(const ChannelOrderRequest &req) const { return createEncryptedPay(req, "https://gateway.hnapay.com/multipay/h5.do", "MUP11"); }

    ChannelOrderResult createEncryptedPay(const ChannelOrderRequest &req, const std::string &url, const std::string &tranCode) const {
        ChannelOrderResult r; auto &p = req.channelParams;
        Json::Value biz; biz["tranAmt"] = req.amount; biz["orgCode"] = mapOrg(req.payType); biz["notifyServerUrl"] = req.notifyUrl; biz["notifyUrl"] = req.notifyUrl; biz["frontUrl"] = req.returnUrl; biz["merUserIp"] = req.clientIp; biz["goodsInfo"] = req.subject; biz["orderSubject"] = req.subject; biz["merchantId"] = p.get("appmchid", "").asString();
        std::string openid = p.get("openid", p.get("sub_openid", "").asString()).asString(); if (!openid.empty()) biz[mapOrg(req.payType)=="WECHATPAY" ? "openId" : "buyerId"] = openid;
        std::map<std::string, std::string> m; m["version"]="2.0"; m["tranCode"]=tranCode; m["merId"]=p.get("appid", "").asString(); m["merOrderId"]=req.orderId; m["submitTime"]=submitTime(req.orderId); m["signType"]="1"; m["charset"]="1"; m["msgCiphertext"] = encryptJson(biz, p.get("appkey", "").asString());
        std::vector<std::string> order = tranCode == "MUP11" ? std::vector<std::string>{"version","tranCode","merId","merOrderId","submitTime","signType","charset","msgCiphertext"} : std::vector<std::string>{"version","tranCode","merId","merOrderId","submitTime","msgCiphertext"};
        m["signValue"] = RsaUtils::signSha1(signByOrder(m, order), normalizePrivateKey(p.get("appsecret", "").asString()));
        if (tranCode == "MUP11") { r.success = true; r.payUrl = url + "?" + buildQuery(m); r.rawResponse = buildQuery(m); return r; }
        auto resp = SyncHttp::postForm(url, buildQuery(m)); r.rawResponse = resp.body; if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j; if (!Json::Reader().parse(resp.body, j)) { r.errMsg="新生JSAPI响应解析失败"; return r; }
        if (j.get("resultCode", "").asString() != "0000") { r.errMsg = j.get("errorMsg", "新生JSAPI下单失败").asString(); return r; }
        r.success = true; r.channelOrderNo = j.get("hnapayOrderId", "").asString(); r.payUrl = j.get("payInfo", "").asString(); r.extra["payInfo"] = r.payUrl; return r;
    }

    static std::string signByOrder(const std::map<std::string,std::string> &m, const std::vector<std::string> &order) { std::string s; for (auto &k: order) { auto it=m.find(k); s += k + "=[" + (it==m.end()?"":it->second) + "]"; } return s; }
    static std::string buildQuery(const std::map<std::string,std::string> &m) { std::string q; for (auto &kv:m) { if(!q.empty()) q+='&'; q += kv.first + "=" + urlEncode(kv.second); } return q; }
    static std::string encryptJson(const Json::Value &v, const std::string &pubKey) { Json::StreamWriterBuilder wb; wb["indentation"]=""; return rsaPublicEncrypt(Json::writeString(wb, v), normalizePublicKey(pubKey)); }
    static std::string rsaPublicEncrypt(const std::string &data, const std::string &pem) { BIO *bio=BIO_new_mem_buf(pem.data(), (int)pem.size()); EVP_PKEY *pkey=PEM_read_bio_PUBKEY(bio,nullptr,nullptr,nullptr); BIO_free(bio); if(!pkey) return ""; RSA *rsa=EVP_PKEY_get1_RSA(pkey); EVP_PKEY_free(pkey); if(!rsa) return ""; std::string out; int size=RSA_size(rsa); for(size_t i=0;i<data.size();i+=117){ std::vector<unsigned char> buf(size); int n=RSA_public_encrypt((int)std::min<size_t>(117,data.size()-i),(const unsigned char*)data.data()+i,buf.data(),rsa,RSA_PKCS1_PADDING); if(n>0) out.append((char*)buf.data(),n); } RSA_free(rsa); return RsaUtils::base64Encode(out); }
    static std::string rsaSha1SignHex(const std::string &data, const std::string &pri) { auto b64=RsaUtils::signSha1(data,pri); auto bin=RsaUtils::base64Decode(b64); std::ostringstream oss; for(auto c:bin) oss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)c; return oss.str(); }
    static bool rsaSha1VerifyHex(const std::string &data, const std::string &hex, const std::string &pub) { std::string bin; for(size_t i=0;i+1<hex.size();i+=2){ char h[3]={hex[i],hex[i+1],0}; bin.push_back((char)strtol(h,nullptr,16)); } return RsaUtils::verifySha1(data,RsaUtils::base64Encode(bin),pub); }
    static std::string normalizePrivateKey(const std::string &key){ if(key.find("-----BEGIN")!=std::string::npos)return key; std::string b; for(size_t i=0;i<key.size();i+=64)b+=key.substr(i,64)+"\n"; return "-----BEGIN RSA PRIVATE KEY-----\n"+b+"-----END RSA PRIVATE KEY-----\n"; }
    static std::string normalizePublicKey(const std::string &key){ if(key.find("-----BEGIN")!=std::string::npos)return key; std::string b; for(size_t i=0;i<key.size();i+=64)b+=key.substr(i,64)+"\n"; return "-----BEGIN PUBLIC KEY-----\n"+b+"-----END PUBLIC KEY-----\n"; }
    static std::string mapOrg(const std::string &payType){ if(payType=="wxpay"||payType=="wx_jsapi")return "WECHATPAY"; if(payType=="bank")return "UNIONPAY"; return "ALIPAY"; }
    static std::string fmtCent(double v){ return std::to_string((long long)std::llround(v*100.0)); }
    static std::string submitTime(const std::string &oid){ return oid.size()>=14?oid.substr(0,14):now(); }
    static std::string now(){
        auto t=std::time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv,&t);
#else
        localtime_r(&t,&tmv);
#endif
        char b[16]; std::strftime(b,sizeof(b),"%Y%m%d%H%M%S",&tmv); return b;
    }
    static std::string extractQr(const std::string &s){ auto p=s.find("qrContent="); if(p==std::string::npos)return s; p+=10; auto e=s.find("&sign=",p); return e==std::string::npos?s.substr(p):s.substr(p,e-p); }
    static std::string urlEncode(const std::string &s){ std::ostringstream oss; for(unsigned char c:s){ if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')oss<<c; else oss<<'%'<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)c; } return oss.str(); }
    static std::string get(const std::map<std::string,std::string>&m,const std::string&k){auto it=m.find(k);return it==m.end()?"":it->second;}
};

REGISTER_CHANNEL_PLUGIN(HnaPayPlugin);
