// WePay-Cpp — 管理后台: 系统用户管理控制器
// 职责：管理员账号的增删改查、密码重置、角色分配等管理功能
//
// API 端点：
// GET    /admin/api/sysuser/list         用户列表
// POST   /admin/api/sysuser/add          新增用户
// POST   /admin/api/sysuser/edit         编辑用户
// POST   /admin/api/sysuser/state        启用/禁用
// POST   /admin/api/sysuser/resetPwd     重置密码
// POST   /admin/api/sysuser/changePwd    修改自己的密码
// POST   /admin/api/sysuser/assignRoles  分配角色
// GET    /admin/api/sysuser/roles        查询用户的角色
// DELETE /admin/api/sysuser/del          删除用户
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/PasswordUtils.h"
#include "../common/RbacService.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class SysUserCtrl : public drogon::HttpController<SysUserCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SysUserCtrl::list,         "/admin/api/sysuser/list",         drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::add,          "/admin/api/sysuser/add",          drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::edit,         "/admin/api/sysuser/edit",         drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::state,        "/admin/api/sysuser/state",        drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::resetPwd,     "/admin/api/sysuser/resetPwd",     drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::changePwd,    "/admin/api/sysuser/changePwd",    drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::assignRoles,  "/admin/api/sysuser/assignRoles",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::roles,        "/admin/api/sysuser/roles",        drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::del,          "/admin/api/sysuser/del",          drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(SysUserCtrl::myInfo,       "/admin/api/sysuser/myInfo",       drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        int page = parseInt(req->getParameter("page"), 1);
        int size = parseInt(req->getParameter("size"), 20);
        std::string search = req->getParameter("search");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!search.empty()) {
            where += " AND (username LIKE ? OR real_name LIKE ?)";
            params.push_back("%" + search + "%");
            params.push_back("%" + search + "%");
        }
        auto cntR = db.query("SELECT COUNT(*) AS c FROM sys_user WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string((page - 1) * size));
        auto rows = db.query(
            "SELECT id,username,real_name,phone,email,is_super,state,"
            "last_login_ip,last_login_at,created_at FROM sys_user "
            "WHERE " + where + " ORDER BY id DESC LIMIT ? OFFSET ?", pp);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"]          = std::stoi(r["id"]);
            it["username"]    = r["username"];
            it["real_name"]   = r["real_name"];
            it["phone"]       = r["phone"];
            it["email"]       = r["email"];
            it["is_super"]    = std::stoi(r["is_super"]);
            it["state"]       = std::stoi(r["state"]);
            it["last_login_ip"]= r["last_login_ip"];
            it["last_login_at"]= (Json::Int64)std::stoll(r["last_login_at"]);
            it["created_at"]  = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &j = *body;
        std::string username = j.get("username", "").asString();
        std::string password = j.get("password", "").asString();
        if (username.empty() || password.empty()) { RESP_ERR(cb, "用户名和密码不能为空"); return; }
        if (password.size() < 6) { RESP_ERR(cb, "密码至少6位"); return; }

        auto &db = PayDb::instance();
        auto exist = db.queryOne("SELECT id FROM sys_user WHERE username=?", {username});
        if (!exist.empty()) { RESP_ERR(cb, "用户名已存在"); return; }

        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(password, salt);
        long long now = std::time(nullptr);
        db.exec("INSERT INTO sys_user(username,password,salt,real_name,phone,email,"
                "is_super,state,created_at,updated_at) VALUES(?,?,?,?,?,?,?,1,?,?)",
                {username, hash, salt,
                 j.get("real_name", "").asString(),
                 j.get("phone", "").asString(),
                 j.get("email", "").asString(),
                 j.get("is_super", 0).asInt() ? "1" : "0",
                 std::to_string(now), std::to_string(now)});
        OplogService::adminLog(req, "sysuser", "add", username, "新增管理员");
        RESP_MSG(cb, "创建成功");
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        if (id.empty()) { RESP_ERR(cb, "缺少ID"); return; }
        long long now = std::time(nullptr);
        PayDb::instance().exec(
            "UPDATE sys_user SET real_name=?,phone=?,email=?,is_super=?,updated_at=? WHERE id=?",
            {j.get("real_name", "").asString(),
             j.get("phone", "").asString(),
             j.get("email", "").asString(),
             j.get("is_super", 0).asInt() ? "1" : "0",
             std::to_string(now), id});
        OplogService::adminLog(req, "sysuser", "edit", id, "");
        RESP_MSG(cb, "更新成功");
    }

    void state(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE sys_user SET state=?,updated_at=? WHERE id=?",
            {std::to_string(s), std::to_string(std::time(nullptr)), id});
        int uid = 0; try { uid = std::stoi(id); } catch(...){}
        RbacService::invalidate(uid);
        OplogService::adminLog(req, "sysuser", "state", id, s ? "启用" : "禁用");
        RESP_MSG(cb, "已更新");
    }

    void resetPwd(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string pwd = (*body).get("password", "").asString();
        if (pwd.size() < 6) { RESP_ERR(cb, "密码至少6位"); return; }
        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(pwd, salt);
        PayDb::instance().exec(
            "UPDATE sys_user SET password=?,salt=?,updated_at=? WHERE id=?",
            {hash, salt, std::to_string(std::time(nullptr)), id});
        OplogService::adminLog(req, "sysuser", "resetPwd", id, "");
        RESP_MSG(cb, "密码已重置");
    }

    void changePwd(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string uid = req->getHeader("X-Admin-Id");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string oldPwd = (*body).get("old_password", "").asString();
        std::string newPwd = (*body).get("new_password", "").asString();
        if (newPwd.size() < 6) { RESP_ERR(cb, "新密码至少6位"); return; }

        auto &db = PayDb::instance();
        auto user = db.queryOne("SELECT password,salt FROM sys_user WHERE id=?", {uid});
        if (user.empty()) { RESP_ERR(cb, "用户不存在"); return; }

        bool passOk = !user["salt"].empty()
            ? PasswordUtils::verify(oldPwd, user["salt"], user["password"])
            : (oldPwd == user["password"]);
        if (!passOk) { RESP_ERR(cb, "原密码错误"); return; }

        std::string salt = PasswordUtils::generateSalt();
        std::string hash = PasswordUtils::hashPassword(newPwd, salt);
        db.exec("UPDATE sys_user SET password=?,salt=?,updated_at=? WHERE id=?",
                {hash, salt, std::to_string(std::time(nullptr)), uid});
        OplogService::adminLog(req, "sysuser", "changePwd", uid, "");
        RESP_MSG(cb, "密码修改成功");
    }

    void assignRoles(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        auto roleIds = (*body)["role_ids"];
        if (!roleIds.isArray()) { RESP_ERR(cb, "role_ids 必须为数组"); return; }

        auto &db = PayDb::instance();
        db.exec("DELETE FROM sys_user_role WHERE user_id=?", {id});
        for (auto &rid : roleIds) {
            db.exec("INSERT INTO sys_user_role(user_id,role_id) VALUES(?,?)",
                    {id, rid.asString()});
        }
        int uid = 0; try { uid = std::stoi(id); } catch(...){}
        RbacService::invalidate(uid);
        OplogService::adminLog(req, "sysuser", "assignRoles", id, "");
        RESP_MSG(cb, "已分配");
    }

    void roles(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        std::string id = req->getParameter("id");
        auto rows = PayDb::instance().query(
            "SELECT r.id,r.role_code,r.role_name FROM sys_role r "
            "INNER JOIN sys_user_role ur ON ur.role_id=r.id WHERE ur.user_id=?",
            {id});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"] = std::stoi(r["id"]);
            it["role_code"] = r["role_code"];
            it["role_name"] = r["role_name"];
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        std::string id = req->getParameter("id");
        auto u = PayDb::instance().queryOne("SELECT username,is_super FROM sys_user WHERE id=?", {id});
        if (u.empty()) { RESP_ERR(cb, "用户不存在"); return; }
        if (u["username"] == "admin") { RESP_ERR(cb, "默认管理员不可删除"); return; }
        PayDb::instance().exec("DELETE FROM sys_user WHERE id=?", {id});
        PayDb::instance().exec("DELETE FROM sys_user_role WHERE user_id=?", {id});
        int uid = 0; try { uid = std::stoi(id); } catch(...){}
        RbacService::invalidate(uid);
        OplogService::adminLog(req, "sysuser", "delete", id, "");
        RESP_MSG(cb, "已删除");
    }

    // 当前登录用户信息 + 权限列表
    void myInfo(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string uidS = req->getHeader("X-Admin-Id");
        std::string user = req->getHeader("X-Admin-User");
        bool isSuper = (req->getHeader("X-Admin-Super") == "1");
        int uid = 0; try { uid = std::stoi(uidS); } catch(...){}

        Json::Value data;
        data["id"] = uid;
        data["username"] = user;
        data["is_super"] = isSuper ? 1 : 0;

        if (uid > 0) {
            auto row = PayDb::instance().queryOne(
                "SELECT real_name,phone,email,avatar FROM sys_user WHERE id=?", {uidS});
            if (!row.empty()) {
                data["real_name"] = row["real_name"];
                data["phone"]     = row["phone"];
                data["email"]     = row["email"];
                data["avatar"]    = row["avatar"];
            }
            // 角色
            Json::Value roleArr(Json::arrayValue);
            for (auto &rc : RbacService::loadUserRoles(uid)) roleArr.append(rc);
            data["roles"] = roleArr;
            // 权限
            Json::Value permArr(Json::arrayValue);
            if (isSuper) permArr.append("*");
            else for (auto &p : RbacService::loadUserPermissions(uid)) permArr.append(p);
            data["permissions"] = permArr;
        } else {
            Json::Value star(Json::arrayValue); star.append("*");
            data["permissions"] = star;
            data["roles"] = Json::Value(Json::arrayValue);
        }
        RESP_OK(cb, data);
    }

private:
    static int parseInt(const std::string &s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }
};
