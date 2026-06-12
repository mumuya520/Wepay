// WePay-Cpp — Phase6B 代理端补充控制器
// 与 AgentSelfCtrl(自服务) 配合，提供:
//
// === 代理子账号(基于 sys_user 但 owner_type=3) ===
// GET    /agent/api/user/list
// POST   /agent/api/user/add
// POST   /agent/api/user/edit
// POST   /agent/api/user/state
// POST   /agent/api/user/resetPwd
// DELETE /agent/api/user/del
//
// === 代理查看下辖商户配置 ===
// GET    /agent/api/mch/list             下辖商户列表(分页)
// GET    /agent/api/mch/detail           商户详情
// GET    /agent/api/mch/payInterface     商户支付接口配置(只读)
// GET    /agent/api/mch/rate              商户费率(只读)
// GET    /agent/api/mch/passages          商户绑定的通道(只读)
//
// === 代理查看退款单 ===
// GET    /agent/api/refund/list          下辖商户退款列表
//
// === 代理查看应用 / 门店 ===
// GET    /agent/api/mch/apps             下辖商户的应用
// GET    /agent/api/mch/stores           下辖商户的门店
//
// === 代理公告(读取系统给代理的公告) ===
// GET    /agent/api/article/list
//
// === 代理查询支付方式 ===
// GET    /agent/api/payWay/list
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PasswordUtils.h"
#include "../filters/AgentAuthFilter.h"

class AgentExtraCtrl : public drogon::HttpController<AgentExtraCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AgentExtraCtrl::userList,     "/agent/api/user/list",     drogon::Get,    "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::userAdd,      "/agent/api/user/add",      drogon::Post,   "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::userEdit,     "/agent/api/user/edit",     drogon::Post,   "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::userState,    "/agent/api/user/state",    drogon::Post,   "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::userResetPwd, "/agent/api/user/resetPwd", drogon::Post,   "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::userDel,      "/agent/api/user/del",      drogon::Delete, "AgentAuthFilter");

        ADD_METHOD_TO(AgentExtraCtrl::mchList,       "/agent/api/mch/list",         drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::mchDetail,     "/agent/api/mch/detail",       drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::mchPayInterface,"/agent/api/mch/payInterface",drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::mchRate,       "/agent/api/mch/rate",         drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::mchPassages,   "/agent/api/mch/passages",     drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::mchApps,       "/agent/api/mch/apps",         drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::mchStores,     "/agent/api/mch/stores",       drogon::Get, "AgentAuthFilter");

        ADD_METHOD_TO(AgentExtraCtrl::refundList,    "/agent/api/refund/list",      drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::articleList,   "/agent/api/article/list",     drogon::Get, "AgentAuthFilter");
        ADD_METHOD_TO(AgentExtraCtrl::payWayList,    "/agent/api/payWay/list",      drogon::Get, "AgentAuthFilter");
    METHOD_LIST_END

    // ── 代理子账号 ──────────────────────────────────────
    void userList(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        // sys_user 增加 team_id 概念，但跨层使用 mch_user 风格不太合适
        // 简化方案: 用 mch_user 表+扩展 mch_id=0 + 在 username 加前缀 a-{agentId}-
        // 或者创建独立 agent_user 表更清晰。这里复用现有 mch_user 表结构,
        // 用 mch_id=0 + 用户名空间分隔
        auto rows = PayDb::instance().query(
            "SELECT id,username,real_name,phone,email,is_admin,permissions,state,created_at "
            "FROM mch_user WHERE mch_id=0 AND username LIKE ? ORDER BY id DESC",
            {"a-" + agentId + "-%"});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"] = std::stoi(r["id"]);
            // 移除前缀显示给前端
            std::string prefix = "a-" + agentId + "-";
            std::string un = r["username"];
            if (un.rfind(prefix, 0) == 0) un = un.substr(prefix.size());
            it["username"]   = un;
            it["real_name"]  = r["real_name"];
            it["phone"]      = r["phone"];
            it["email"]      = r["email"];
            it["is_admin"]   = std::stoi(r["is_admin"]);
            it["state"]      = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            Json::Value perms;
            if (Json::Reader().parse(r["permissions"], perms)) it["permissions"] = perms;
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void userAdd(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string username = j.get("username", "").asString();
        std::string password = j.get("password", "").asString();
        if (username.empty() || password.size() < 6) {
            RESP_ERR(cb, "用户名必填，密码至少6位"); return;
        }
        std::string fullUn = "a-" + agentId + "-" + username;
        auto &db = PayDb::instance();
        auto exist = db.queryOne("SELECT id FROM mch_user WHERE mch_id=0 AND username=?", {fullUn});
        if (!exist.empty()) { RESP_ERR(cb, "用户名已存在"); return; }

        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(password, salt);

        Json::Value perms = j["permissions"];
        if (!perms.isArray()) perms = Json::Value(Json::arrayValue);
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string permsStr = Json::writeString(wb, perms);

        db.exec("INSERT INTO mch_user(mch_id,username,password,salt,real_name,phone,email,"
                "is_admin,permissions,state,created_at) VALUES(0,?,?,?,?,?,?,?,?,1,?)",
                {fullUn, hash, salt,
                 j.get("real_name", "").asString(),
                 j.get("phone", "").asString(),
                 j.get("email", "").asString(),
                 std::to_string(j.get("is_admin", 0).asInt()),
                 permsStr,
                 std::to_string(std::time(nullptr))});
        RESP_MSG(cb, "已创建");
    }

    void userEdit(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();

        Json::Value perms = j["permissions"];
        if (!perms.isArray()) perms = Json::Value(Json::arrayValue);
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string permsStr = Json::writeString(wb, perms);

        std::string prefix = "a-" + agentId + "-";
        PayDb::instance().exec(
            "UPDATE mch_user SET real_name=?,phone=?,email=?,is_admin=?,permissions=? "
            "WHERE id=? AND mch_id=0 AND username LIKE ?",
            {j.get("real_name", "").asString(),
             j.get("phone", "").asString(),
             j.get("email", "").asString(),
             std::to_string(j.get("is_admin", 0).asInt()),
             permsStr, id, prefix + "%"});
        RESP_MSG(cb, "已更新");
    }

    void userState(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        std::string prefix = "a-" + agentId + "-";
        PayDb::instance().exec(
            "UPDATE mch_user SET state=? WHERE id=? AND mch_id=0 AND username LIKE ?",
            {std::to_string(s), id, prefix + "%"});
        RESP_MSG(cb, "已更新");
    }

    void userResetPwd(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string pwd = (*body).get("password", "").asString();
        if (pwd.size() < 6) { RESP_ERR(cb, "密码至少6位"); return; }
        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(pwd, salt);
        std::string prefix = "a-" + agentId + "-";
        PayDb::instance().exec(
            "UPDATE mch_user SET password=?,salt=? WHERE id=? AND mch_id=0 AND username LIKE ?",
            {hash, salt, id, prefix + "%"});
        RESP_MSG(cb, "已重置");
    }

    void userDel(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        std::string id = req->getParameter("id");
        std::string prefix = "a-" + agentId + "-";
        PayDb::instance().exec(
            "DELETE FROM mch_user WHERE id=? AND mch_id=0 AND username LIKE ?",
            {id, prefix + "%"});
        RESP_MSG(cb, "已删除");
    }

    // ── 代理查看下辖商户 ──────────────────────────────────
    void mchList(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        auto &db = PayDb::instance();
        auto cntR = db.query("SELECT COUNT(*) AS c FROM merchant WHERE agent_id=?", {agentId});
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto rows = db.query(
            "SELECT id,mch_no,mch_name,contact,phone,balance,total_income,rate,state,created_at "
            "FROM merchant WHERE agent_id=? ORDER BY id DESC LIMIT ? OFFSET ?",
            {agentId, std::to_string(size), std::to_string((page - 1) * size)});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void mchDetail(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        std::string mchId = req->getParameter("mch_id");
        auto r = PayDb::instance().queryOne(
            "SELECT * FROM merchant WHERE id=? AND agent_id=?", {mchId, agentId});
        if (r.empty()) { RESP_ERR(cb, "商户不存在或非下辖"); return; }
        Json::Value data;
        for (auto &[k, v] : r) data[k] = v;
        // 屏蔽密码字段
        data.removeMember("password");
        data.removeMember("salt");
        data["id"] = std::stoi(r["id"]);
        RESP_OK(cb, data);
    }

    void mchPayInterface(const drogon::HttpRequestPtr &req,
                         std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        std::string mchId = req->getParameter("mch_id");
        // 验证商户归属
        if (!verifyOwnership(agentId, mchId)) { RESP_ERR(cb, "非下辖商户"); return; }
        auto rows = PayDb::instance().query(
            "SELECT id,if_code,if_rate,state FROM pay_interface_config "
            "WHERE info_type=2 AND info_id=? ORDER BY id", {mchId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"] = std::stoi(r["id"]);
            it["if_code"] = r["if_code"];
            it["if_rate"] = r["if_rate"];
            it["state"]   = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void mchRate(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        std::string mchId = req->getParameter("mch_id");
        if (!verifyOwnership(agentId, mchId)) { RESP_ERR(cb, "非下辖商户"); return; }
        auto rows = PayDb::instance().query(
            "SELECT id,way_code,rate,state FROM pay_rate_config "
            "WHERE config_mode=2 AND info_id=? ORDER BY way_code", {mchId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"] = std::stoi(r["id"]);
            it["way_code"] = r["way_code"];
            it["rate"] = r["rate"];
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void mchPassages(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        std::string mchId = req->getParameter("mch_id");
        if (!verifyOwnership(agentId, mchId)) { RESP_ERR(cb, "非下辖商户"); return; }
        auto rows = PayDb::instance().query(
            "SELECT c.id,c.channel_code,c.channel_name,c.pay_type,mc.rate AS bind_rate "
            "FROM pay_channel c INNER JOIN merchant_channel mc ON mc.channel_id=c.id "
            "WHERE mc.mch_id=? ORDER BY c.sort_order", {mchId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void mchApps(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        std::string mchId = req->getParameter("mch_id");
        if (!verifyOwnership(agentId, mchId)) { RESP_ERR(cb, "非下辖商户"); return; }
        auto rows = PayDb::instance().query(
            "SELECT id,app_id,app_name,sign_type,state,created_at FROM mch_app_v2 "
            "WHERE mch_id=? ORDER BY id DESC", {mchId});
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

    void mchStores(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        std::string mchId = req->getParameter("mch_id");
        if (!verifyOwnership(agentId, mchId)) { RESP_ERR(cb, "非下辖商户"); return; }
        auto rows = PayDb::instance().query(
            "SELECT * FROM mch_store WHERE mch_id=? ORDER BY id DESC", {mchId});
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

    // ── 代理查看下辖商户的退款 ────────────────────────────
    void refundList(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string agentId = req->getHeader("X-Agent-Id");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        auto &db = PayDb::instance();
        auto cntR = db.query(
            "SELECT COUNT(*) AS c FROM refund_order r "
            "INNER JOIN merchant m ON m.id=r.mch_id WHERE m.agent_id=?", {agentId});
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto rows = db.query(
            "SELECT r.* FROM refund_order r INNER JOIN merchant m ON m.id=r.mch_id "
            "WHERE m.agent_id=? ORDER BY r.id DESC LIMIT ? OFFSET ?",
            {agentId, std::to_string(size), std::to_string((page - 1) * size)});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void articleList(const drogon::HttpRequestPtr &,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query(
            "SELECT id,title,article_type,is_top,publish_at FROM sys_article "
            "WHERE state=1 AND (target='all' OR target='agent') "
            "ORDER BY is_top DESC, id DESC LIMIT 20", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"] = std::stoi(r["id"]);
            it["title"] = r["title"];
            it["article_type"] = std::stoi(r["article_type"]);
            it["is_top"] = std::stoi(r["is_top"]);
            it["publish_at"] = (Json::Int64)std::stoll(r["publish_at"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void payWayList(const drogon::HttpRequestPtr &,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query(
            "SELECT way_code,way_name,icon,if_code FROM pay_way "
            "WHERE state=1 ORDER BY sort_order, id", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

private:
    static int pi(const std::string &s, int def) {
        try { return std::stoi(s); } catch(...) { return def; }
    }
    static bool verifyOwnership(const std::string &agentId, const std::string &mchId) {
        auto r = PayDb::instance().queryOne(
            "SELECT id FROM merchant WHERE id=? AND agent_id=?", {mchId, agentId});
        return !r.empty();
    }
};
