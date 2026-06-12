// WePay-Cpp — 管理后台: 支付配置管理控制器
// 职责：接口定义、实例配置、费率、OAuth2 等支付配置管理
//
// 包含四个独立配置：
// 1. 接口定义 - 只读，由系统初始化
// 2. 接口实例配置 - 商户/ISV/App 各自的真实参数
// 3. 费率配置 - 支付费率设置
// 4. OAuth2 授权配置 - 第三方授权配置
//
// API 端点：
// GET    /admin/api/payIface/defines       接口定义清单
// GET    /admin/api/payIface/configList    列表
// GET    /admin/api/payIface/config        单条详情
// POST   /admin/api/payIface/configSave    保存
// POST   /admin/api/payIface/configState   启用/禁用
// DELETE /admin/api/payIface/configDel     删除
// GET    /admin/api/payRate/list           列表
// POST   /admin/api/payRate/save           保存
// DELETE /admin/api/payRate/del            删除
//
// === OAuth2 授权配置 ===
// GET    /admin/api/payOauth/list          列表
// POST   /admin/api/payOauth/save          保存
// DELETE /admin/api/payOauth/del           删除
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class PayConfigCtrl : public drogon::HttpController<PayConfigCtrl> {
public:
    METHOD_LIST_BEGIN
        // 接口定义
        ADD_METHOD_TO(PayConfigCtrl::defines,      "/admin/api/payIface/defines",     drogon::Get, "AdminAuthFilter");
        // 接口实例配置
        ADD_METHOD_TO(PayConfigCtrl::configList,   "/admin/api/payIface/configList",  drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::configOne,    "/admin/api/payIface/config",      drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::configSave,   "/admin/api/payIface/configSave",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::configState,  "/admin/api/payIface/configState", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::configDel,    "/admin/api/payIface/configDel",   drogon::Delete, "AdminAuthFilter");
        // 费率
        ADD_METHOD_TO(PayConfigCtrl::rateList,     "/admin/api/payRate/list",         drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::rateSave,     "/admin/api/payRate/save",         drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::rateDel,      "/admin/api/payRate/del",          drogon::Delete, "AdminAuthFilter");
        // OAuth2
        ADD_METHOD_TO(PayConfigCtrl::oauthList,    "/admin/api/payOauth/list",        drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::oauthSave,    "/admin/api/payOauth/save",        drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PayConfigCtrl::oauthDel,     "/admin/api/payOauth/del",         drogon::Delete, "AdminAuthFilter");
    METHOD_LIST_END

    // ── 接口定义 ─────────────────────────────────────────────
    void defines(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        auto rows = PayDb::instance().query(
            "SELECT id,if_code,if_name,is_mch_mode,is_isv_mode,way_codes,config_params,icon,state,remark "
            "FROM pay_interface_define WHERE state=1 ORDER BY id", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["is_mch_mode"] = std::stoi(r["is_mch_mode"]);
            it["is_isv_mode"] = std::stoi(r["is_isv_mode"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    // ── 接口实例配置 ──────────────────────────────────────────
    void configList(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        std::string infoType = req->getParameter("info_type");
        std::string infoId   = req->getParameter("info_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!infoType.empty()) { where += " AND info_type=?"; params.push_back(infoType); }
        if (!infoId.empty())   { where += " AND info_id=?";   params.push_back(infoId); }

        auto rows = db.query(
            "SELECT id,info_type,info_id,if_code,if_rate,state,created_at,updated_at "
            "FROM pay_interface_config WHERE " + where + " ORDER BY id DESC", params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["info_type"] = std::stoi(r["info_type"]);
            it["state"] = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void configOne(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        std::string id = req->getParameter("id");
        auto r = PayDb::instance().queryOne("SELECT * FROM pay_interface_config WHERE id=?", {id});
        if (r.empty()) { RESP_ERR(cb, "配置不存在"); return; }
        Json::Value data;
        for (auto &[k, v] : r) data[k] = v;
        data["id"] = std::stoi(r["id"]);
        data["info_type"] = std::stoi(r["info_type"]);
        data["state"] = std::stoi(r["state"]);
        // 解析 if_params 为对象
        Json::Value params;
        if (Json::Reader().parse(r["if_params"], params)) data["if_params_obj"] = params;
        RESP_OK(cb, data);
    }

    void configSave(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        long long now = std::time(nullptr);

        // if_params 接受 JSON 字符串或对象
        std::string ifParams;
        if (j["if_params"].isString()) ifParams = j["if_params"].asString();
        else {
            Json::StreamWriterBuilder wb; wb["indentation"] = "";
            ifParams = Json::writeString(wb, j["if_params"]);
        }

        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO pay_interface_config(info_type,info_id,if_code,if_params,if_rate,"
                    "state,created_at,updated_at) VALUES(?,?,?,?,?,1,?,?)",
                    {std::to_string(j.get("info_type", 2).asInt()),
                     j.get("info_id", "").asString(),
                     j.get("if_code", "").asString(),
                     ifParams,
                     j.get("if_rate", "0.60").asString(),
                     std::to_string(now), std::to_string(now)});
            OplogService::adminLog(req, "channel", "configAdd",
                j.get("if_code", "").asString(), "info_id=" + j.get("info_id", "").asString());
        } else {
            db.exec("UPDATE pay_interface_config SET if_params=?,if_rate=?,updated_at=? WHERE id=?",
                    {ifParams, j.get("if_rate", "0.60").asString(),
                     std::to_string(now), id});
            OplogService::adminLog(req, "channel", "configEdit", id, "");
        }
        RESP_MSG(cb, "已保存");
    }

    void configState(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE pay_interface_config SET state=?,updated_at=? WHERE id=?",
            {std::to_string(s), std::to_string(std::time(nullptr)), id});
        RESP_MSG(cb, "已更新");
    }

    void configDel(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM pay_interface_config WHERE id=?", {id});
        OplogService::adminLog(req, "channel", "configDel", id, "");
        RESP_MSG(cb, "已删除");
    }

    // ── 费率配置 ──────────────────────────────────────────────
    void rateList(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        std::string mode = req->getParameter("config_mode");
        std::string infoId = req->getParameter("info_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mode.empty())   { where += " AND config_mode=?"; params.push_back(mode); }
        if (!infoId.empty()) { where += " AND info_id=?";     params.push_back(infoId); }
        auto rows = db.query("SELECT * FROM pay_rate_config WHERE " + where + " ORDER BY way_code", params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["config_mode"] = std::stoi(r["config_mode"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void rateSave(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        long long now = std::time(nullptr);
        std::string id = j.get("id", "").asString();
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO pay_rate_config(config_mode,info_id,way_code,rate,state,updated_at) "
                    "VALUES(?,?,?,?,1,?)",
                    {std::to_string(j.get("config_mode", 2).asInt()),
                     j.get("info_id", "").asString(),
                     j.get("way_code", "").asString(),
                     j.get("rate", "0.60").asString(),
                     std::to_string(now)});
        } else {
            db.exec("UPDATE pay_rate_config SET rate=?,state=?,updated_at=? WHERE id=?",
                    {j.get("rate", "0.60").asString(),
                     std::to_string(j.get("state", 1).asInt()),
                     std::to_string(now), id});
        }
        OplogService::adminLog(req, "channel", "rateSave", j.get("info_id", "").asString(),
            j.get("way_code", "").asString() + "=" + j.get("rate", "").asString());
        RESP_MSG(cb, "已保存");
    }

    void rateDel(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM pay_rate_config WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // ── OAuth2 授权配置 ───────────────────────────────────────
    void oauthList(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        std::string mchId = req->getParameter("mch_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        auto rows = db.query("SELECT * FROM pay_oauth2_config WHERE " + where + " ORDER BY id DESC", params);
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

    void oauthSave(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO pay_oauth2_config(mch_id,app_id,oauth_type,oauth_app_id,"
                    "oauth_secret,redirect_url,state) VALUES(?,?,?,?,?,?,1)",
                    {j.get("mch_id", "0").asString(),
                     j.get("app_id", "").asString(),
                     j.get("oauth_type", "").asString(),
                     j.get("oauth_app_id", "").asString(),
                     j.get("oauth_secret", "").asString(),
                     j.get("redirect_url", "").asString()});
        } else {
            db.exec("UPDATE pay_oauth2_config SET oauth_app_id=?,oauth_secret=?,redirect_url=?,state=? WHERE id=?",
                    {j.get("oauth_app_id", "").asString(),
                     j.get("oauth_secret", "").asString(),
                     j.get("redirect_url", "").asString(),
                     std::to_string(j.get("state", 1).asInt()), id});
        }
        RESP_MSG(cb, "已保存");
    }

    void oauthDel(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM pay_oauth2_config WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }
};
