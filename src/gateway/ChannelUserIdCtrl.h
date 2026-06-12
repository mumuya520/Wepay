// WePay-Cpp — 获取渠道用户ID (微信openid / 支付宝buyer_id)
// 用于 JSAPI 支付前置授权流程
//
// GET  /gateway/channelUserId/:plugin   重定向到 OAuth2 授权页(再回调到下方接口)
// POST /gateway/channelUserId           前端提交 code 换取 user_id
#pragma once
#include <drogon/HttpController.h>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../channel/ChannelPlugin.h"

class ChannelUserIdCtrl : public drogon::HttpController<ChannelUserIdCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(ChannelUserIdCtrl::redirect, "/gateway/channelUserId/{plugin}", drogon::Get);
        ADD_METHOD_TO(ChannelUserIdCtrl::exchange, "/gateway/channelUserId",          drogon::Post);
    METHOD_LIST_END

    // 引导用户跳转到上游 OAuth2 授权页
    void redirect(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                  const std::string &plugin) {
        std::string mchId = req->getParameter("mch_id");
        std::string redirectUrl = req->getParameter("redirect_url");

        auto cfg = PayDb::instance().queryOne(
            "SELECT oauth_app_id,redirect_url FROM pay_oauth2_config "
            "WHERE mch_id=? AND state=1 LIMIT 1", {mchId});
        if (cfg.empty()) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setBody("OAuth2 配置不存在");
            r->setStatusCode(drogon::k400BadRequest);
            cb(r); return;
        }
        std::string redirectBack = redirectUrl.empty() ? cfg["redirect_url"] : redirectUrl;
        std::string url;
        if (plugin == "wxpay_native" || plugin == "wxpay_ext") {
            url = "https://open.weixin.qq.com/connect/oauth2/authorize"
                  "?appid=" + cfg["oauth_app_id"] +
                  "&redirect_uri=" + urlEncode(redirectBack) +
                  "&response_type=code&scope=snsapi_base&state=" + mchId +
                  "#wechat_redirect";
        } else if (plugin == "alipay" || plugin == "alipay_ext") {
            url = "https://openauth.alipay.com/oauth2/publicAppAuthorize.htm"
                  "?app_id=" + cfg["oauth_app_id"] +
                  "&scope=auth_base&redirect_uri=" + urlEncode(redirectBack) +
                  "&state=" + mchId;
        } else {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setBody("不支持的插件类型: " + plugin);
            cb(r); return;
        }
        auto r = drogon::HttpResponse::newRedirectionResponse(url);
        cb(r);
    }

    // 前端拿到 code 后提交此接口换取 user_id
    void exchange(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string code   = j.get("code", "").asString();
        std::string mchId  = j.get("mch_id", "").asString();
        std::string plugin = j.get("plugin", "").asString();

        if (code.empty() || mchId.empty() || plugin.empty()) {
            RESP_ERR(cb, "code/mch_id/plugin 必填"); return;
        }

        // 加载 OAuth2 配置
        auto cfg = PayDb::instance().queryOne(
            "SELECT oauth_app_id,oauth_secret FROM pay_oauth2_config "
            "WHERE mch_id=? AND state=1 LIMIT 1", {mchId});
        if (cfg.empty()) { RESP_ERR(cb, "OAuth2配置不存在"); return; }

        // 加载通道插件
        auto pluginObj = ChannelPluginRegistry::instance().create(plugin);
        if (!pluginObj) { RESP_ERR(cb, "通道插件不存在"); return; }

        Json::Value cp;
        // 微信用 secret，支付宝用应用私钥
        cp["appid"]  = cfg["oauth_app_id"];
        cp["secret"] = cfg["oauth_secret"];
        cp["private_key"] = cfg["oauth_secret"];  // 支付宝用同字段

        ChannelUserIdRequest ur;
        ur.code = code;
        ur.channelParams = cp;
        auto result = pluginObj->queryChannelUserId(ur);
        if (!result.success) { RESP_ERR(cb, result.errMsg); return; }

        Json::Value data;
        data["user_id"]  = result.userId;
        data["nickname"] = result.nickname;
        data["avatar"]   = result.avatar;
        RESP_OK(cb, data);
    }

private:
    static std::string urlEncode(const std::string &s) {
        std::string r; r.reserve(s.size());
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += c;
            else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                r += buf;
            }
        }
        return r;
    }
};
