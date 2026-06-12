// ReverseProxy.h — C++ 反向代理 PHP 服务，生成支付二维码
// 流程：C++ 生成订单 → 调 PHP mapi.php → PHP 生成二维码返回 base64
//       C++ 解码 base64 → PNG 返回前端
#pragma once
#include <string>
#include <map>
#include "Utils/SyncHttp.h"
#include "Utils/Md5Utils.h"

namespace Gateway {

// 反向代理器：代理 PHP 码支付后端
class ReverseProxy {
public:
    // PHP 基础地址（mapi.php 所在服务）
    static std::string phpBase() {
        return "http://localhost:61111";
    }

    // 构造 mapi.php 签名参数（彩虹易支付协议）
    // pid/key: 商户号和密钥
    // outTradeNo: C++ 生成的订单号
    // amount: 金额（元，字符串）
    // subject: 商品名称
    // notifyUrl: 回调地址（C++ sync-status）
    // returnUrl: 支付完成后跳转地址
    static std::string buildMapiUrl(const std::string& pid,
                                     const std::string& key,
                                     const std::string& outTradeNo,
                                     const std::string& amount,
                                     const std::string& subject,
                                     const std::string& notifyUrl,
                                     const std::string& returnUrl,
                                     const std::string& sitename = "alipay_direct") {
        // 1. 构造签名字符串（按 key 升序）
        std::map<std::string, std::string> signMap;
        auto add = [&](const std::string& k, const std::string& v) {
            if (!v.empty()) signMap[k] = v;
        };
        add("money",       amount);
        add("name",        subject.empty() ? "支付宝扫码支付" : subject);
        add("notify_url",  notifyUrl);
        add("out_trade_no", outTradeNo);
        add("pid",         pid);
        add("return_url",  returnUrl);
        add("sitename",    sitename);
        signMap["type"] = "alipay";

        // 签名字符串 = k1=v1&k2=v2...&key
        std::ostringstream signSrc;
        for (auto& kv : signMap) {
            if (&kv != &*signMap.begin()) signSrc << "&";
            signSrc << kv.first << "=" << kv.second;
        }
        signSrc << key;
        std::string sign = Md5Utils::md5(signSrc.str());

        // 2. 构造请求 URL
        std::ostringstream url;
        for (auto& kv : signMap) {
            if (&kv != &*signMap.begin()) url << "&";
            url << kv.first << "=" << urlEncode(kv.second);
        }
        url << "&sign=" << sign << "&sign_type=MD5";

        return phpBase() + "/mapi.php?" + url.str();
    }

    // 调用 mapi.php，返回响应体字符串
    // 出错返回空字符串
    static std::string callMapi(const std::string& url) {
        auto resp = SyncHttp::get(url);
        return resp.success ? resp.body : "";
    }

    // 解析 mapi.php 返回的 JSON，提取二维码 base64
    // 返回 { qr: base64字符串, payment_url: 支付链接, trade_no: PHP内部单号 }
    struct MapiResult {
        std::string qr;           // base64 图片数据（不含 data:image 前缀）
        std::string paymentUrl;    // 支付链接
        std::string tradeNo;      // PHP 内部交易号
        int         code = 0;     // 1=成功，其他失败
        std::string msg;          // 错误信息
    };

    static MapiResult parseMapiResponse(const std::string& body) {
        MapiResult r;
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(body, root)) return r;

        r.code = root.get("code", 0).asInt();
        if (r.code != 1) {
            r.msg = root.get("msg", "").asString();
            return r;
        }

        r.paymentUrl = root.get("payment_url", "").asString();
        r.tradeNo    = root.get("trade_no",    "").asString();

        // 提取 qr_code 或 qr.value
        if (root["qr_code"].isString() && !root["qr_code"].asString().empty()) {
            r.qr = root["qr_code"].asString();
        } else if (root["qr"].isObject() && root["qr"]["value"].isString()) {
            r.qr = root["qr"]["value"].asString();
        }

        // 去掉 data:image/png;base64, 前缀
        if (r.qr.find("data:") != std::string::npos) {
            size_t comma = r.qr.find(',');
            if (comma != std::string::npos) r.qr = r.qr.substr(comma + 1);
        }

        return r;
    }

    // Base64 解码（标准 URL-safe + 标准 Base64）
    static std::string decodeBase64(const std::string& b64) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string raw;
        raw.reserve(b64.size() * 3 / 4);

        int val = 0, bits = -8;
        for (unsigned char c : b64) {
            if (c == '=') break;
            int v = -1;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+' || c == '-') v = 62;
            else if (c == '/' || c == '_') v = 63;
            else continue;

            val = (val << 6) | v;
            bits += 6;
            if (bits >= 0) {
                raw.push_back(char((val >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return raw;
    }

    // URL 编码（保留 isalnum 和 -_.&= 不编码）
    static std::string urlEncode(const std::string& str) {
        std::string result;
        for (unsigned char uc : str) {
            if (std::isalnum(uc) || uc == '-' || uc == '_' || uc == '.' || uc == '~'
                || uc == '&' || uc == '=') {
                result += char(uc);
            } else {
                result += '%';
                result += "0123456789ABCDEF"[uc >> 4];
                result += "0123456789ABCDEF"[uc & 15];
            }
        }
        return result;
    }
};

} // namespace Gateway
