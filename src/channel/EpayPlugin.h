#pragma once
#include "ChannelPlugin.h"
#include <iomanip>
#include <sstream>
#include <trantor/utils/Logger.h>
#include "../common/Md5Utils.h"
#include "../common/SyncHttp.h"

class EpayPlugin : public ChannelPlugin {
public:
    std::string name() const override { return "epay"; }

    Json::Value paramSchema() const override {
        Json::Value arr(Json::arrayValue);
        auto add = [&](const std::string &k, const std::string &lbl, const std::string &t,
                       const std::string &dflt = "", const std::string &help = "") {
            Json::Value v;
            v["key"] = k; v["label"] = lbl; v["type"] = t; v["default"] = dflt;
            if (!help.empty()) v["help"] = help;
            arr.append(v);
        };
        add("appurl", "接口地址", "input", "", "必须以 http:// 或 https:// 开头，以 / 结尾");
        add("appid", "商户ID", "input");
        add("appkey", "商户密钥", "input");
        add("appswitch", "是否使用mapi接口", "select", "1", "0=否，跳转 submit.php（旧版）；1=是，请求 mapi.php（推荐，兼容性更好）");
        return arr;
    }

    ChannelOrderResult createOrder(const ChannelOrderRequest &req) override {
        ChannelOrderResult result;
        auto &p = req.channelParams;
        std::string apiurl = normalizeBase(p.get("appurl", "").asString());
        std::string pid = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();
        if (apiurl.empty() || pid.empty() || key.empty()) {
            result.errMsg = "彩虹易支付参数不完整(appurl/appid/appkey)";
            return result;
        }
        std::map<std::string, std::string> params;
        params["pid"] = pid;
        params["type"] = mapPayType(req.payType);
        if (p.get("appswitch", "0").asString() == "1") {
            params["device"] = p.get("device", "pc").asString();
            params["clientip"] = req.clientIp;
        }
        params["notify_url"] = req.notifyUrl;
        params["return_url"] = req.returnUrl;
        // 部分易支付平台严格要求 out_trade_no 为纯数字（彩虹易标准格式 YmdHis+5位随机数）
        // 这里把 WePay 的 W 前缀去掉，回调时按 order_id 后缀匹配反查
        params["out_trade_no"] = stripPrefix(req.orderId);
        params["name"] = req.subject.empty() ? "商品" : req.subject;
        params["money"] = fmtAmount(req.amount);
        params["sign"] = sign(params, key);
        params["sign_type"] = "MD5";
        std::string body = buildQuery(params);
        result.rawResponse = body;

        if (p.get("appswitch", "0").asString() != "1") {
            // 彩虹易 submit.php 校验 POST 方法（原版 pagePay 用自动提交表单）
            // 这里在 URL 前加 [POST] 标记，由收银台生成自动提交表单
            result.success = true;
            result.payUrl = "[POST]" + apiurl + "submit.php?" + body;
            LOG_INFO << "[EpayPlugin] submit_url=" << apiurl << "submit.php"
                     << " body=" << body;
            return result;
        }

        LOG_INFO << "[EpayPlugin] mapi_url=" << apiurl << "mapi.php body=" << body;
        auto resp = SyncHttp::postForm(apiurl + "mapi.php", body);
        result.rawResponse = resp.body;
        LOG_INFO << "[EpayPlugin] mapi_response success=" << resp.success
                 << " status=" << resp.status << " body=" << resp.body;
        if (!resp.success) { result.errMsg = resp.errMsg; return result; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { result.errMsg = "彩虹易支付 mapi 响应解析失败: " + resp.body; return result; }
        if (j.get("code", 0).asInt() != 1) {
            result.errMsg = j.get("msg", "彩虹易支付下单失败").asString();
            return result;
        }
        result.success = true;
        std::string tradeNo = j.get("trade_no", "").asString();
        std::string payurlFromMapi = j.get("payurl", "").asString();
        std::string qrcodeFromMapi = j.get("qrcode", "").asString();

        // 优先级：
        // 1) mapi 直接返回了 payurl（有些上游会给 H5 跳转 URL） → 直接用
        // 2) 有 trade_no → 跳转到上游收银台（/Pay/console?trade_no=Y...），用户看到上游品牌页
        // 3) 否则只把原生 qrcode URL 作为 payUrl，由 WePay 自己的收银台展示二维码
        if (!payurlFromMapi.empty()) {
            result.payUrl = payurlFromMapi;
            result.qrCode = qrcodeFromMapi;
        } else if (!tradeNo.empty()) {
            result.payUrl = apiurl + "Pay/console?trade_no=" + urlEncode(tradeNo);
            result.qrCode.clear();   // 清空让收银台知道是跳转而不是显示二维码
        } else {
            result.payUrl = qrcodeFromMapi;
            result.qrCode = qrcodeFromMapi;
        }
        result.channelOrderNo = tradeNo;
        result.extra["urlscheme"] = j.get("urlscheme", "").asString();
        LOG_INFO << "[EpayPlugin] mapi_result trade_no=" << tradeNo
                 << " payUrl=" << result.payUrl
                 << " qrCode=" << result.qrCode;
        return result;
    }

    ChannelQueryResult queryOrder(const std::string &orderId, const Json::Value &channelParams) override {
        ChannelQueryResult r;
        std::string apiurl = normalizeBase(channelParams.get("appurl", "").asString());
        std::string pid = channelParams.get("appid", "").asString();
        std::string key = channelParams.get("appkey", "").asString();
        if (apiurl.empty() || pid.empty() || key.empty()) return r;
        std::string url = apiurl + "api.php?act=order&pid=" + urlEncode(pid) + "&key=" + urlEncode(key) + "&trade_no=" + urlEncode(orderId);
        auto resp = SyncHttp::get(url);
        if (!resp.success) return r;
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) return r;
        r.success = true;
        r.tradeState = j.get("status", 0).asInt() == 1 ? 1 : 0;
        r.channelOrderNo = j.get("trade_no", "").asString();
        try { r.paidAmount = std::stod(j.get("money", "0").asString()); } catch (...) {}
        return r;
    }

    ChannelNotifyResult verifyNotify(const std::map<std::string, std::string> &params,
                                     const std::string &,
                                     const Json::Value &channelParams) override {
        ChannelNotifyResult r;
        r.responseText = "success";
        auto signIt = params.find("sign");
        std::string key = channelParams.get("appkey", "").asString();
        if (signIt == params.end() || key.empty()) { r.verified = false; r.responseText = "fail"; return r; }
        r.verified = (sign(params, key) == signIt->second);
        if (!r.verified) { r.responseText = "fail"; return r; }
        r.paid = get(params, "trade_status") == "TRADE_SUCCESS";
        r.orderId = get(params, "out_trade_no");
        r.channelOrderNo = get(params, "trade_no");
        try { r.paidAmount = std::stod(get(params, "money")); } catch (...) {}
        return r;
    }

    ChannelRefundResult refund(const ChannelRefundRequest &req) override {
        ChannelRefundResult r;
        auto &p = req.channelParams;
        std::string apiurl = normalizeBase(p.get("appurl", "").asString());
        std::string pid = p.get("appid", "").asString();
        std::string key = p.get("appkey", "").asString();
        if (apiurl.empty() || pid.empty() || key.empty()) { r.errMsg = "彩虹易支付退款参数不完整"; return r; }
        std::string body = "pid=" + urlEncode(pid) + "&key=" + urlEncode(key) + "&refund_no=" + urlEncode(req.refundNo) +
                           "&trade_no=" + urlEncode(req.channelOrderNo.empty() ? req.orderId : req.channelOrderNo) +
                           "&money=" + urlEncode(fmtAmount(req.refundAmount));
        auto resp = SyncHttp::postForm(apiurl + "api.php?act=refund", body);
        r.rawResponse = resp.body;
        if (!resp.success) { r.errMsg = resp.errMsg; return r; }
        Json::Value j;
        if (!Json::Reader().parse(resp.body, j)) { r.errMsg = "彩虹易支付退款响应解析失败"; return r; }
        if (j.get("code", 0).asInt() == 1 || j.get("status", 0).asInt() == 1) {
            r.success = true;
            r.state = 1;
            r.channelRefundNo = j.get("refund_no", req.refundNo).asString();
        } else {
            r.errMsg = j.get("msg", "彩虹易支付退款失败").asString();
        }
        return r;
    }

private:
    static std::string sign(const std::map<std::string, std::string> &params, const std::string &key) {
        std::string s;
        for (auto &kv : params) {
            if (kv.first == "sign" || kv.first == "sign_type" || kv.second.empty()) continue;
            if (!s.empty()) s += "&";
            s += kv.first + "=" + kv.second;
        }
        std::string md = Md5Utils::md5(s + key);
        LOG_INFO << "[EpayPlugin] sign_str=" << s << " key_len=" << key.size() << " sign=" << md;
        return md;
    }

    // 去掉 order_id 开头的非数字字符（如 W 前缀），让上游接收纯数字订单号
    static std::string stripPrefix(const std::string &s) {
        size_t i = 0;
        while (i < s.size() && !std::isdigit((unsigned char)s[i])) ++i;
        return i == 0 ? s : s.substr(i);
    }

    static std::string mapPayType(const std::string &payType) {
        if (payType == "wxpay" || payType == "wx_qr") return "wxpay";
        if (payType == "qqpay") return "qqpay";
        if (payType == "bank" || payType == "ysf_qr") return "bank";
        if (payType == "jdpay") return "jdpay";
        return "alipay";
    }

    static std::string normalizeBase(std::string s) {
        if (!s.empty() && s.back() != '/') s += '/';
        return s;
    }

    static std::string fmtAmount(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string buildQuery(const std::map<std::string, std::string> &params) {
        std::string q;
        for (auto &kv : params) {
            if (!q.empty()) q += "&";
            q += kv.first + "=" + urlEncode(kv.second);
        }
        return q;
    }

    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }

    static std::string get(const std::map<std::string, std::string> &m, const std::string &k) {
        auto it = m.find(k);
        return it == m.end() ? "" : it->second;
    }
};

REGISTER_CHANNEL_PLUGIN(EpayPlugin);
