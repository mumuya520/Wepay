// WePay-Cpp — 管理后台: 通知任务管理控制器
// 职责：通知任务的列表、日志查询、手动重试等通知管理功能
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/NotifyTaskService.h" // 通知任务服务
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class NotifyMgrCtrl : public drogon::HttpController<NotifyMgrCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(NotifyMgrCtrl::list,  "/admin/api/notify/list",      drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(NotifyMgrCtrl::logs,  "/admin/api/notify/logs/{1}",  drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(NotifyMgrCtrl::retry, "/admin/api/notify/retry/{1}", drogon::Post, "AdminAuthFilter");
        // 旧路由兼容
        ADD_METHOD_TO(NotifyMgrCtrl::list,  "/api/notify/list",          drogon::Get,  "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int page = 1, size = 20;
        try { page = std::stoi(req->getParameter("page")); } catch (...) {}
        try { size = std::stoi(req->getParameter("size")); } catch (...) {}
        if (page < 1) page = 1;
        if (size < 1 || size > 100) size = 20;
        int offset = (page - 1) * size;
        std::string statusParam = req->getParameter("status");

        std::string where = "1=1";
        std::vector<std::string> params;
        if (!statusParam.empty()) { where += " AND status=?"; params.push_back(statusParam); }

        auto cntRow = db.query(
            "SELECT COUNT(*) AS c FROM pay_notify_task WHERE " + where, params);
        int total = cntRow.empty() ? 0 : std::stoi(cntRow[0].at("c"));

        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT id,order_id,plugin,status,retry_cnt,next_retry_at,last_response,"
            "created_at,updated_at,notify_full_url AS notify_url "
            "FROM pay_notify_task WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", pp);

        Json::Value list(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]            = r.at("id");
            item["order_id"]      = r.at("order_id");
            item["plugin"]        = r.count("plugin") ? r.at("plugin") : "";
            item["status"]        = std::stoi(r.at("status"));
            item["retry_cnt"]     = std::stoi(r.at("retry_cnt"));
            item["notify_url"]    = r.at("notify_url");
            item["last_response"] = r.at("last_response");
            item["next_retry_at"] = (Json::Int64)std::stoll(r.at("next_retry_at"));
            item["created_at"]    = (Json::Int64)std::stoll(r.at("created_at"));
            item["updated_at"]    = (Json::Int64)std::stoll(r.at("updated_at"));
            list.append(item);
        }
        Json::Value data;
        data["list"] = list; data["total"] = total;
        data["page"] = page; data["size"] = size;
        RESP_OK(cb, data);
    }

    void logs(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb,
              std::string orderId) {
        auto &db = PayDb::instance();
        auto rows = db.query(
            "SELECT id,notify_url,http_status,response,success,created_at "
            "FROM pay_callback_log WHERE order_id=? ORDER BY id DESC LIMIT 100",
            {orderId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value j;
            j["id"]          = r.at("id");
            j["notify_url"]  = r.at("notify_url");
            j["http_status"] = std::stoi(r.at("http_status"));
            j["response"]    = r.at("response");
            j["success"]     = std::stoi(r.at("success")) != 0;
            j["created_at"]  = (Json::Int64)std::stoll(r.at("created_at"));
            arr.append(j);
        }
        Json::Value d; d["logs"] = arr;
        RESP_OK(cb, d);
    }

    void retry(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb,
               std::string orderId) {
        NotifyTaskService::retryNow(orderId);
        RESP_MSG(cb, "已加入重试队列");
    }
};
