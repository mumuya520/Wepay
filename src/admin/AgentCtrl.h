// WePay-Cpp — 管理后台: 代理商管理控制器
// 职责：代理商的增删改查、密码重置、佣金管理、下辖商户等管理功能
//
// API 端点：
// GET    /admin/api/agent/list             代理商列表
// GET    /admin/api/agent/detail           代理商详情 + 下辖商户统计
// POST   /admin/api/agent/add              新增代理
// POST   /admin/api/agent/edit             编辑代理
// POST   /admin/api/agent/state            启用/禁用
// POST   /admin/api/agent/resetPwd         重置密码
// DELETE /admin/api/agent/del              删除代理
// GET    /admin/api/agent/commissionLogs   代理佣金流水
// GET    /admin/api/agent/merchants        代理下辖商户
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PasswordUtils.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class AgentCtrl : public drogon::HttpController<AgentCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AgentCtrl::list,           "/admin/api/agent/list",           drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::detail,         "/admin/api/agent/detail",         drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::add,            "/admin/api/agent/add",            drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::edit,           "/admin/api/agent/edit",           drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::state,          "/admin/api/agent/state",          drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::resetPwd,       "/admin/api/agent/resetPwd",       drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::del,            "/admin/api/agent/del",            drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::commissionLogs, "/admin/api/agent/commissionLogs", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AgentCtrl::merchants,      "/admin/api/agent/merchants",      drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:view");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        std::string search = req->getParameter("search");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!search.empty()) {
            where += " AND (agent_no LIKE ? OR username LIKE ? OR agent_name LIKE ?)";
            params.push_back("%" + search + "%");
            params.push_back("%" + search + "%");
            params.push_back("%" + search + "%");
        }
        auto cntR = db.query("SELECT COUNT(*) AS c FROM agent WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string((page - 1) * size));
        auto rows = db.query(
            "SELECT id,agent_no,username,agent_name,contact,phone,email,"
            "parent_id,level,commission_rate,balance,total_commission,state,created_at "
            "FROM agent WHERE " + where + " ORDER BY id DESC LIMIT ? OFFSET ?", pp);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["parent_id"] = std::stoi(r["parent_id"]);
            it["level"]     = std::stoi(r["level"]);
            it["state"]     = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void detail(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:view");
        std::string id = req->getParameter("id");
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM agent WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "代理不存在"); return; }

        Json::Value data;
        for (auto &[k, v] : row) data[k] = v;

        // 下辖商户统计
        auto mchCnt = db.queryOne(
            "SELECT COUNT(*) AS c, COALESCE(SUM(CAST(total_income AS REAL)),0) AS ti "
            "FROM merchant WHERE agent_id=?", {id});
        data["merchant_count"] = mchCnt.empty() ? 0 : std::stoi(mchCnt["c"]);
        data["merchant_total_income"] = mchCnt.empty() ? "0" : mchCnt["ti"];
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
        if (username.empty() || password.empty()) { RESP_ERR(cb, "用户名和密码必填"); return; }

        auto &db = PayDb::instance();
        auto exist = db.queryOne("SELECT id FROM agent WHERE username=?", {username});
        if (!exist.empty()) { RESP_ERR(cb, "用户名已存在"); return; }

        std::string agentNo = generateAgentNo();
        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(password, salt);
        long long now = std::time(nullptr);

        int parentId = j.get("parent_id", 0).asInt();
        int level = 1;
        if (parentId > 0) {
            auto p = db.queryOne("SELECT level FROM agent WHERE id=?", {std::to_string(parentId)});
            if (!p.empty()) { try { level = std::stoi(p["level"]) + 1; } catch(...){} }
        }

        db.exec("INSERT INTO agent(agent_no,username,password,salt,agent_name,contact,"
                "phone,email,parent_id,level,commission_rate,state,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,1,?,?)",
                {agentNo, username, hash, salt,
                 j.get("agent_name", "").asString(),
                 j.get("contact", "").asString(),
                 j.get("phone", "").asString(),
                 j.get("email", "").asString(),
                 std::to_string(parentId),
                 std::to_string(level),
                 j.get("commission_rate", "0.10").asString(),
                 std::to_string(now), std::to_string(now)});

        OplogService::adminLog(req, "agent", "add", agentNo, "新增代理: " + username);
        Json::Value data; data["agent_no"] = agentNo;
        RESP_OK(cb, data);
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        PayDb::instance().exec(
            "UPDATE agent SET agent_name=?,contact=?,phone=?,email=?,"
            "commission_rate=?,updated_at=? WHERE id=?",
            {j.get("agent_name", "").asString(),
             j.get("contact", "").asString(),
             j.get("phone", "").asString(),
             j.get("email", "").asString(),
             j.get("commission_rate", "0.10").asString(),
             std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "agent", "edit", id, "");
        RESP_MSG(cb, "更新成功");
    }

    void state(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE agent SET state=?,updated_at=? WHERE id=?",
            {std::to_string(s), std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "agent", "state", id, s ? "启用" : "禁用");
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
        PayDb::instance().exec(
            "UPDATE agent SET password=?,salt=?,updated_at=? WHERE id=?",
            {hash, salt, std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "agent", "resetPwd", id, "");
        RESP_MSG(cb, "密码已重置");
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:delete");
        std::string id = req->getParameter("id");
        auto &db = PayDb::instance();
        auto hasMch = db.queryOne("SELECT id FROM merchant WHERE agent_id=? LIMIT 1", {id});
        if (!hasMch.empty()) { RESP_ERR(cb, "该代理下还有商户，无法删除"); return; }
        auto hasChild = db.queryOne("SELECT id FROM agent WHERE parent_id=? LIMIT 1", {id});
        if (!hasChild.empty()) { RESP_ERR(cb, "该代理下还有下级代理"); return; }
        db.exec("DELETE FROM agent WHERE id=?", {id});
        OplogService::adminLog(req, "agent", "delete", id, "");
        RESP_MSG(cb, "已删除");
    }

    void commissionLogs(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:view");
        std::string agentId = req->getParameter("agent_id");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);

        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!agentId.empty()) { where += " AND agent_id=?"; params.push_back(agentId); }

        auto cntR = db.query("SELECT COUNT(*) AS c FROM agent_commission_log WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string((page - 1) * size));
        auto rows = db.query(
            "SELECT * FROM agent_commission_log WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", pp);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"]         = std::stoi(r["id"]);
            it["agent_id"]   = std::stoi(r["agent_id"]);
            it["mch_id"]     = std::stoi(r["mch_id"]);
            it["state"]      = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void merchants(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "agent:view");
        std::string agentId = req->getParameter("agent_id");
        auto rows = PayDb::instance().query(
            "SELECT id,mch_no,mch_name,balance,total_income,state,created_at "
            "FROM merchant WHERE agent_id=? ORDER BY id DESC", {agentId});
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
        try { return std::stoi(s); } catch (...) { return def; }
    }
    static std::string generateAgentNo() {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT MAX(id) AS m FROM agent", {});
        int next = 100001;
        if (!row.empty() && !row["m"].empty()) {
            try { next = std::max(100001, std::stoi(row["m"]) + 100001); } catch (...) {}
        }
        return "A" + std::to_string(next);
    }
};
