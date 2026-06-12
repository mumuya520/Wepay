// WePay-Cpp — 管理后台: 商户应用管理控制器
// 职责：商户应用的增删改查、密钥重置等管理功能（一商户多套 AppID/Secret）
//
// API 端点：
// GET    /admin/api/mchApp/list      列表
// POST   /admin/api/mchApp/add       新增
// POST   /admin/api/mchApp/edit      编辑
// POST   /admin/api/mchApp/state     启用/禁用
// POST   /admin/api/mchApp/resetSecret  重置密钥
// DELETE /admin/api/mchApp/del       删除
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PasswordUtils.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class MchAppCtrl : public drogon::HttpController<MchAppCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(MchAppCtrl::list,         "/admin/api/mchApp/list",         drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MchAppCtrl::add,          "/admin/api/mchApp/add",          drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MchAppCtrl::edit,         "/admin/api/mchApp/edit",         drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MchAppCtrl::state,        "/admin/api/mchApp/state",        drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MchAppCtrl::resetSecret,  "/admin/api/mchApp/resetSecret",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MchAppCtrl::del,          "/admin/api/mchApp/del",          drogon::Delete, "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:view");
        std::string mchId = req->getParameter("mch_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        auto rows = db.query("SELECT * FROM mch_app_v2 WHERE " + where + " ORDER BY id DESC", params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;

        std::string mchId = j.get("mch_id", "0").asString();
        std::string appName = j.get("app_name", "").asString();
        if (appName.empty()) { RESP_ERR(cb, "应用名称必填"); return; }

        std::string appId = PasswordUtils::generateAppId();
        std::string secret = PasswordUtils::generateKey(32);
        long long now = std::time(nullptr);

        PayDb::instance().exec(
            "INSERT INTO mch_app_v2(mch_id,app_id,app_name,app_secret,sign_type,"
            "ip_white,notify_url,return_url,remark,state,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,1,?,?)",
            {mchId, appId, appName, secret,
             j.get("sign_type", "MD5").asString(),
             j.get("ip_white", "").asString(),
             j.get("notify_url", "").asString(),
             j.get("return_url", "").asString(),
             j.get("remark", "").asString(),
             std::to_string(now), std::to_string(now)});

        OplogService::adminLog(req, "merchant", "addApp", appId, "新增应用: " + appName);
        Json::Value data;
        data["app_id"]     = appId;
        data["app_secret"] = secret;
        RESP_OK(cb, data);
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();

        PayDb::instance().exec(
            "UPDATE mch_app_v2 SET app_name=?,sign_type=?,ip_white=?,"
            "notify_url=?,return_url=?,remark=?,updated_at=? WHERE id=?",
            {j.get("app_name", "").asString(),
             j.get("sign_type", "MD5").asString(),
             j.get("ip_white", "").asString(),
             j.get("notify_url", "").asString(),
             j.get("return_url", "").asString(),
             j.get("remark", "").asString(),
             std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "merchant", "editApp", id, "");
        RESP_MSG(cb, "已更新");
    }

    void state(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE mch_app_v2 SET state=?,updated_at=? WHERE id=?",
            {std::to_string(s), std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "merchant", "appState", id, s ? "启用" : "禁用");
        RESP_MSG(cb, "已更新");
    }

    void resetSecret(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string secret = PasswordUtils::generateKey(32);
        PayDb::instance().exec(
            "UPDATE mch_app_v2 SET app_secret=?,updated_at=? WHERE id=?",
            {secret, std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "merchant", "appResetSecret", id, "");
        Json::Value data; data["app_secret"] = secret;
        RESP_OK(cb, data);
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:delete");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM mch_app_v2 WHERE id=?", {id});
        OplogService::adminLog(req, "merchant", "delApp", id, "");
        RESP_MSG(cb, "已删除");
    }
};
