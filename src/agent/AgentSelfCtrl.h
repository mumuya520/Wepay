// WePay-Cpp — 代理后台自服务
// POST /agent/api/auth/login           代理登录
// POST /agent/api/auth/logout          退出
// GET  /agent/api/info                 个人信息
// POST /agent/api/changePwd            改密
// GET  /agent/api/dashboard            数据面板
// GET  /agent/api/merchants            下辖商户
// GET  /agent/api/orders               下辖商户订单
// GET  /agent/api/commissions          佣金流水
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PasswordUtils.h"
#include "../common/SimpleJwt.h"
#include "../filters/AgentAuthFilter.h"

class AgentSelfCtrl : public drogon::HttpController<AgentSelfCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AgentSelfCtrl::login,       "/agent/api/auth/login",  drogon::Post);
        ADD_METHOD_TO(AgentSelfCtrl::logout,      "/agent/api/auth/logout", drogon::Post);
        ADD_METHOD_TO(AgentSelfCtrl::info,        "/agent/api/info",        drogon::Get,  "AgentAuthFilter");
        ADD_METHOD_TO(AgentSelfCtrl::changePwd,   "/agent/api/changePwd",   drogon::Post, "AgentAuthFilter");
        ADD_METHOD_TO(AgentSelfCtrl::dashboard,   "/agent/api/dashboard",   drogon::Get,  "AgentAuthFilter");
        ADD_METHOD_TO(AgentSelfCtrl::merchants,   "/agent/api/merchants",   drogon::Get,  "AgentAuthFilter");
        ADD_METHOD_TO(AgentSelfCtrl::orders,      "/agent/api/orders",      drogon::Get,  "AgentAuthFilter");
        ADD_METHOD_TO(AgentSelfCtrl::commissions, "/agent/api/commissions", drogon::Get,  "AgentAuthFilter");
    METHOD_LIST_END

    void login(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        std::string username, password;
        if (body) {
            username = (*body).get("username", "").asString();
            password = (*body).get("password", "").asString();
        }
        if (username.empty() || password.empty()) {
            RESP_ERR(cb, "用户名密码不能为空"); return;
        }
        auto a = PayDb::instance().queryOne(
            "SELECT id,agent_no,username,password,salt,agent_name,state FROM agent WHERE username=?",
            {username});
        if (a.empty()) { RESP_ERR(cb, "账号或密码错误"); return; }
        if (a["state"] != "1") { RESP_ERR(cb, "账号已禁用"); return; }
        bool ok = !a["salt"].empty()
            ? PasswordUtils::verify(password, a["salt"], a["password"])
            : (password == a["password"]);
        if (!ok) { RESP_ERR(cb, "账号或密码错误"); return; }

        std::string sub = "agent:" + a["id"] + ":" + username;
        std::string token = SimpleJwt::sign(sub);

        Json::Value data;
        data["token"]      = token;
        data["agent_no"]   = a["agent_no"];
        data["agent_name"] = a["agent_name"];
        data["username"]   = username;
        RESP_JSON(cb, AjaxResult::success("登录成功", data));
    }

    void logout(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        RESP_MSG(cb, "退出成功");
    }

    void info(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getHeader("X-Agent-Id");
        auto a = PayDb::instance().queryOne(
            "SELECT id,agent_no,username,agent_name,contact,phone,email,parent_id,level,"
            "commission_rate,balance,total_commission,state,created_at FROM agent WHERE id=?",
            {id});
        if (a.empty()) { RESP_ERR(cb, "代理不存在"); return; }
        Json::Value data;
        for (auto &[k, v] : a) data[k] = v;
        data["id"] = std::stoi(a["id"]);
        RESP_OK(cb, data);
    }

    void changePwd(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getHeader("X-Agent-Id");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string oldP = (*body).get("old_password", "").asString();
        std::string newP = (*body).get("new_password", "").asString();
        if (newP.size() < 6) { RESP_ERR(cb, "新密码至少6位"); return; }

        auto &db = PayDb::instance();
        auto a = db.queryOne("SELECT password,salt FROM agent WHERE id=?", {id});
        if (a.empty()) { RESP_ERR(cb, "代理不存在"); return; }
        bool ok = !a["salt"].empty()
            ? PasswordUtils::verify(oldP, a["salt"], a["password"])
            : (oldP == a["password"]);
        if (!ok) { RESP_ERR(cb, "原密码错误"); return; }
        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(newP, salt);
        db.exec("UPDATE agent SET password=?,salt=?,updated_at=? WHERE id=?",
            {hash, salt, std::to_string(std::time(nullptr)), id});
        RESP_MSG(cb, "密码已修改");
    }

    void dashboard(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getHeader("X-Agent-Id");
        auto &db = PayDb::instance();
        auto a = db.queryOne(
            "SELECT balance,total_commission,commission_rate FROM agent WHERE id=?", {id});
        auto mchCnt = db.queryOne("SELECT COUNT(*) AS c FROM merchant WHERE agent_id=?", {id});

        // 今日佣金
        long long now = std::time(nullptr);
        long long dayStart = now - (now % 86400);
        auto today = db.queryOne(
            "SELECT COALESCE(SUM(CAST(commission AS REAL)),0) AS amt, COUNT(*) AS cnt "
            "FROM agent_commission_log WHERE agent_id=? AND created_at>=?",
            {id, std::to_string(dayStart)});

        Json::Value data;
        data["balance"]          = a.empty() ? "0.00" : a["balance"];
        data["total_commission"] = a.empty() ? "0.00" : a["total_commission"];
        data["commission_rate"]  = a.empty() ? "0.10" : a["commission_rate"];
        data["merchant_count"]   = mchCnt.empty() ? 0 : std::stoi(mchCnt["c"]);
        data["today_commission"] = today.empty() ? "0" : today["amt"];
        data["today_count"]      = today.empty() ? 0 : std::stoi(today["cnt"]);
        RESP_OK(cb, data);
    }

    void merchants(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getHeader("X-Agent-Id");
        auto rows = PayDb::instance().query(
            "SELECT id,mch_no,mch_name,balance,total_income,state,created_at "
            "FROM merchant WHERE agent_id=? ORDER BY id DESC", {id});
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

    void orders(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getHeader("X-Agent-Id");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);

        auto &db = PayDb::instance();
        // 仅看下辖商户的订单
        auto rows = db.query(
            "SELECT o.* FROM pay_order o INNER JOIN merchant m ON m.id=o.mch_id "
            "WHERE m.agent_id=? ORDER BY o.id DESC LIMIT ? OFFSET ?",
            {id, std::to_string(size), std::to_string((page - 1) * size)});
        auto cntR = db.query(
            "SELECT COUNT(*) AS c FROM pay_order o INNER JOIN merchant m ON m.id=o.mch_id "
            "WHERE m.agent_id=?", {id});
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["order_id"]     = r["order_id"];
            it["mch_order_no"] = r["mch_order_no"];
            it["mch_id"]       = std::stoi(r["mch_id"]);
            it["pay_type"]     = r["pay_type"];
            it["amount"]       = r["amount"];
            it["state"]        = std::stoi(r["state"]);
            it["created_at"]   = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void commissions(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getHeader("X-Agent-Id");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        auto &db = PayDb::instance();
        auto cntR = db.query("SELECT COUNT(*) AS c FROM agent_commission_log WHERE agent_id=?", {id});
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto rows = db.query(
            "SELECT * FROM agent_commission_log WHERE agent_id=? ORDER BY id DESC LIMIT ? OFFSET ?",
            {id, std::to_string(size), std::to_string((page - 1) * size)});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["state"] = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

private:
    static int pi(const std::string &s, int def) {
        try { return std::stoi(s); } catch(...) { return def; }
    }
};
