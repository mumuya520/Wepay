// WePay-Cpp — 图形验证码 API
// GET  /captcha?type=image    返回 SVG 图形验证码 + 缓存 token
// POST /captcha/verify        验证(后端比对)
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <random> // 随机数库
#include "AjaxResult.h" // AJAX 响应结果
#include "Utilities.h" // 工具集
#include "CacheService.h" // 缓存服务

class CaptchaCtrl : public drogon::HttpController<CaptchaCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(CaptchaCtrl::generate, "/captcha",        drogon::Get);
        ADD_METHOD_TO(CaptchaCtrl::verify,   "/captcha/verify", drogon::Post);
    METHOD_LIST_END

    void generate(const drogon::HttpRequestPtr &,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto c = CaptchaUtil::generate();
        // 生成 token，缓存 5 分钟
        std::string token = randomToken();
        CacheService::instance().set("captcha:" + token, c.code, 300);

        Json::Value data;
        data["token"] = token;
        data["svg"]   = c.svg;
        // 也提供 dataUrl 形式方便前端直接 <img src=...>
        // SVG 支持 data:image/svg+xml;base64,
        data["data_url"] = "data:image/svg+xml;utf8," + urlEncode(c.svg);
        RESP_OK(cb, data);
    }

    void verify(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string token = (*body).get("token", "").asString();
        std::string code  = (*body).get("code", "").asString();
        if (token.empty() || code.empty()) { RESP_ERR(cb, "token/code 必填"); return; }

        auto cached = CacheService::instance().get("captcha:" + token);
        if (cached.empty()) { RESP_ERR(cb, "验证码已过期"); return; }
        // 一次性使用
        CacheService::instance().del("captcha:" + token);

        std::string upper = code;
        for (auto &ch : upper) ch = (char)std::toupper((unsigned char)ch);
        if (upper != cached) { RESP_ERR(cb, "验证码错误"); return; }
        RESP_MSG(cb, "验证通过");
    }

private:
    static std::string randomToken() {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < 24; ++i) s += cs[rng() % 36];
        return s;
    }
    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                oss << buf;
            }
        }
        return oss.str();
    }
};
