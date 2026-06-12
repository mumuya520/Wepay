// WePay-Cpp — 前端运行时配置 API
// 提供前端动态读取 / 管理员动态修改 入口路径的能力
//
// GET  /app-config.json                        公开，前端启动时拉取
// POST /admin/api/config/appConfig             管理员修改入口路径
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include "AjaxResult.h" // AJAX 响应结果
#include "PayDb.h"
#include "PermCheck.h"
#include "OplogService.h"
#include "../filters/AdminAuthFilter.h"

class AppConfigCtrl : public drogon::HttpController<AppConfigCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AppConfigCtrl::publicConfig, "/app-config.json", drogon::Get);
        ADD_METHOD_TO(AppConfigCtrl::getAppConfig, "/admin/api/config/appConfig", drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AppConfigCtrl::saveAppConfig,"/admin/api/config/appConfig", drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    // 公开接口: 返回前端运行时配置
    void publicConfig(const drogon::HttpRequestPtr &,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        Json::Value data = loadConfig();
        // 响应为 JSON, 允许跨域
        auto r = drogon::HttpResponse::newHttpJsonResponse(data);
        r->addHeader("Cache-Control", "no-store");
        cb(r);
    }

    void getAppConfig(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:view");
        RESP_OK(cb, loadConfig());
    }

    void saveAppConfig(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;

        auto &db = PayDb::instance();
        // 管理员路径 (不允许为空 / 不能包含斜杠)
        std::string adminPath = trimPath(j.get("adminPath", "").asString());
        if (adminPath.empty()) {
            RESP_ERR(cb, "adminPath 不能为空"); return;
        }
        if (adminPath.find('/') != std::string::npos || adminPath.find(' ') != std::string::npos) {
            RESP_ERR(cb, "adminPath 不能包含斜杠或空格"); return;
        }

        std::string merchantPath = trimPath(j.get("merchantPath", "merchant").asString());
        if (merchantPath.empty()) merchantPath = "merchant";
        if (merchantPath.find('/') != std::string::npos) {
            RESP_ERR(cb, "merchantPath 不能包含斜杠"); return;
        }
        if (merchantPath == adminPath) {
            RESP_ERR(cb, "管理端和商户端路径不能相同"); return;
        }

        std::string siteName = j.get("siteName", "WePay").asString();
        std::string primary  = j.get("primaryColor", "#409EFF").asString();

        db.setSetting("app_admin_path",    adminPath);
        db.setSetting("app_merchant_path", merchantPath);
        db.setSetting("app_site_name",     siteName);
        db.setSetting("app_primary_color", primary);

        OplogService::adminLog(req, OplogService::MOD_CONFIG, "appConfig", "",
            "adminPath=" + adminPath + " merchantPath=" + merchantPath);
        RESP_MSG(cb, "保存成功，下次访问将生效");
    }

private:
    static Json::Value loadConfig() {
        auto &db = PayDb::instance();
        Json::Value d;
        d["siteName"]     = db.getSetting("app_site_name",     "WePay");
        d["adminPath"]    = db.getSetting("app_admin_path",    "manage");
        d["merchantPath"] = db.getSetting("app_merchant_path", "merchant");
        d["apiBase"]      = "";
        d["primaryColor"] = db.getSetting("app_primary_color", "#409EFF");
        return d;
    }
    // 去除开头和结尾的空格、斜杠
    static std::string trimPath(std::string s) {
        while (!s.empty() && (s.front() == '/' || s.front() == ' ')) s.erase(0, 1);
        while (!s.empty() && (s.back()  == '/' || s.back()  == ' ')) s.pop_back();
        return s;
    }
};
