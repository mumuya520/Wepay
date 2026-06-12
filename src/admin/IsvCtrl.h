// WePay-Cpp — 管理后台: 服务商(ISV)管理控制器
// 职责：ISV 的增删改查、密码重置、下辖商户和代理管理等功能
//
// 分销模型：平台 → ISV(服务商) → 代理/商户
//
// API 端点：
// GET    /admin/api/isv/list        列表
// POST   /admin/api/isv/add         新增
// POST   /admin/api/isv/edit        编辑
// POST   /admin/api/isv/state       启用/禁用
// POST   /admin/api/isv/resetPwd    重置密码
// DELETE /admin/api/isv/del         删除
// GET    /admin/api/isv/merchants    下辖商户
// GET    /admin/api/isv/agents       下辖代理
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PasswordUtils.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class IsvCtrl : public drogon::HttpController<IsvCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(IsvCtrl::list,      "/admin/api/isv/list",      drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(IsvCtrl::add,       "/admin/api/isv/add",       drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(IsvCtrl::edit,      "/admin/api/isv/edit",      drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(IsvCtrl::state,     "/admin/api/isv/state",     drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(IsvCtrl::resetPwd,  "/admin/api/isv/resetPwd",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(IsvCtrl::del,       "/admin/api/isv/del",       drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(IsvCtrl::merchants, "/admin/api/isv/merchants", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(IsvCtrl::agents,    "/admin/api/isv/agents",    drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:view");  // 复用代理查看权限
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        std::string search = req->getParameter("search");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!search.empty()) {
            where += " AND (isv_no LIKE ? OR username LIKE ? OR isv_name LIKE ?)";
            params.push_back("%" + search + "%");
            params.push_back("%" + search + "%");
            params.push_back("%" + search + "%");
        }
        auto cntR = db.query("SELECT COUNT(*) AS c FROM isv WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string((page - 1) * size));
        auto rows = db.query(
            "SELECT id,isv_no,username,isv_name,contact,phone,email,state,created_at "
            "FROM isv WHERE " + where + " ORDER BY id DESC LIMIT ? OFFSET ?", pp);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["state"] = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:add");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string username = j.get("username", "").asString();
        std::string password = j.get("password", "").asString();
        if (username.empty() || password.empty()) {
            RESP_ERR(cb, "用户名密码必填"); return;
        }

        auto &db = PayDb::instance();
        auto exist = db.queryOne("SELECT id FROM isv WHERE username=?", {username});
        if (!exist.empty()) { RESP_ERR(cb, "用户名已存在"); return; }

        std::string isvNo = generateIsvNo();
        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(password, salt);
        long long now = std::time(nullptr);

        db.exec("INSERT INTO isv(isv_no,username,password,salt,isv_name,contact,phone,email,"
                "remark,state,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,1,?,?)",
                {isvNo, username, hash, salt,
                 j.get("isv_name", "").asString(),
                 j.get("contact", "").asString(),
                 j.get("phone", "").asString(),
                 j.get("email", "").asString(),
                 j.get("remark", "").asString(),
                 std::to_string(now), std::to_string(now)});

        OplogService::adminLog(req, "agent", "addIsv", isvNo, "新增ISV: " + username);
        Json::Value data; data["isv_no"] = isvNo;
        RESP_OK(cb, data);
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        PayDb::instance().exec(
            "UPDATE isv SET isv_name=?,contact=?,phone=?,email=?,remark=?,updated_at=? WHERE id=?",
            {j.get("isv_name", "").asString(),
             j.get("contact", "").asString(),
             j.get("phone", "").asString(),
             j.get("email", "").asString(),
             j.get("remark", "").asString(),
             std::to_string(std::time(nullptr)),
             j.get("id", "").asString()});
        RESP_MSG(cb, "已更新");
    }

    void state(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE isv SET state=?,updated_at=? WHERE id=?",
            {std::to_string(s), std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "agent", "isvState", id, s ? "启用" : "禁用");
        RESP_MSG(cb, "已更新");
    }

    void resetPwd(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string pwd = (*body).get("password", "").asString();
        if (pwd.size() < 6) { RESP_ERR(cb, "密码至少6位"); return; }
        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(pwd, salt);
        PayDb::instance().exec("UPDATE isv SET password=?,salt=?,updated_at=? WHERE id=?",
            {hash, salt, std::to_string(std::time(nullptr)), id});
        RESP_MSG(cb, "密码已重置");
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:delete");
        std::string id = req->getParameter("id");
        auto &db = PayDb::instance();
        auto hasMch = db.queryOne("SELECT id FROM merchant WHERE isv_id=? LIMIT 1", {id});
        if (!hasMch.empty()) { RESP_ERR(cb, "该ISV下还有商户，无法删除"); return; }
        auto hasAgent = db.queryOne("SELECT id FROM agent WHERE isv_id=? LIMIT 1", {id});
        if (!hasAgent.empty()) { RESP_ERR(cb, "该ISV下还有代理，无法删除"); return; }
        db.exec("DELETE FROM isv WHERE id=?", {id});
        OplogService::adminLog(req, "agent", "delIsv", id, "");
        RESP_MSG(cb, "已删除");
    }

    void merchants(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:view");
        std::string isvId = req->getParameter("isv_id");
        auto rows = PayDb::instance().query(
            "SELECT id,mch_no,mch_name,balance,total_income,state FROM merchant "
            "WHERE isv_id=? ORDER BY id DESC", {isvId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void agents(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:view");
        std::string isvId = req->getParameter("isv_id");
        auto rows = PayDb::instance().query(
            "SELECT id,agent_no,agent_name,commission_rate,state FROM agent "
            "WHERE isv_id=? ORDER BY id DESC", {isvId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

private:
    static int pi(const std::string &s, int def) {
        try { return std::stoi(s); } catch(...) { return def; }
    }
    static std::string generateIsvNo() {
        auto row = PayDb::instance().queryOne("SELECT MAX(id) AS m FROM isv", {});
        int next = 100001;
        if (!row.empty() && !row["m"].empty()) {
            try { next = std::max(100001, std::stoi(row["m"]) + 100001); } catch(...) {}
        }
        return "I" + std::to_string(next);
    }
};
