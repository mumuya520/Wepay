// WePay-Cpp — 系统健康检查控制器
// SystemHealthCtrl.h — 统一健康检查接口
// GET /health                              公开，主进程存活探针
// GET /admin/api/system/health             管理员，主进程 + 子进程详情
// POST /admin/api/system/restartProcess    管理员，重启指定子进程（终止后由 watchdog 自动重启）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include "ProcessManager.h" // 进程管理器
#include "HttpStatus.h" // HTTP 状态码
#include "AjaxResult.h" // AJAX 响应结果
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class SystemHealthCtrl : public drogon::HttpController<SystemHealthCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SystemHealthCtrl::publicHealth,  "/health",                          drogon::Get);
        ADD_METHOD_TO(SystemHealthCtrl::adminHealth,   "/admin/api/system/health",         drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(SystemHealthCtrl::restart,       "/admin/api/system/restartProcess", drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    // 公开探针：始终返回 200 {"status":"ok"}
    void publicHealth(const drogon::HttpRequestPtr &,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        Json::Value v;
        v["status"] = "ok";
        auto r = drogon::HttpResponse::newHttpJsonResponse(v);
        r->addHeader("Cache-Control", "no-store");
        cb(r);
    }

    // 管理员详情：主进程 uptime + 所有子进程动态状态
    void adminHealth(const drogon::HttpRequestPtr &,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        RESP_OK(cb, ProcessManager::getStatusJson());
    }

    // 重启指定子进程：终止后由各模块的 watchdog 自动重启
    // body: { "name": "nginx" } 或  ?name=nginx
    void restart(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string name = req->getParameter("name");
        if (name.empty()) {
            auto j = req->getJsonObject();
            if (j && j->isMember("name")) name = (*j)["name"].asString();
        }
        if (name.empty()) { RESP_ERR(cb, "缺少参数 name"); return; }
        bool ok = ProcessManager::killByName(name);
        if (!ok) { RESP_ERR(cb, "未找到进程或终止失败: " + name); return; }
        Json::Value v;
        v["name"]   = name;
        v["killed"] = true;
        v["msg"]    = "已发送终止信号，watchdog 将在数秒内重启";
        RESP_OK(cb, v);
    }
};
