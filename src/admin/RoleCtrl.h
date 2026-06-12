// WePay-Cpp — 管理后台: 角色与权限管理控制器
// 职责：角色的增删改查、权限分配、权限清单等权限管理功能
//
// API 端点：
// GET    /admin/api/role/list              角色列表
// POST   /admin/api/role/add               新增角色
// POST   /admin/api/role/edit              编辑角色
// POST   /admin/api/role/assignPerms       分配权限
// GET    /admin/api/role/perms             查询角色权限
// DELETE /admin/api/role/del               删除角色
// GET    /admin/api/permission/list        全部权限清单(按模块分组)
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/RbacService.h" // 基于角色的访问控制
#include "../common/PermCheck.h" // 权限检查
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class RoleCtrl : public drogon::HttpController<RoleCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(RoleCtrl::list,        "/admin/api/role/list",        drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(RoleCtrl::add,         "/admin/api/role/add",         drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(RoleCtrl::edit,        "/admin/api/role/edit",        drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(RoleCtrl::assignPerms, "/admin/api/role/assignPerms", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(RoleCtrl::perms,       "/admin/api/role/perms",       drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(RoleCtrl::del,         "/admin/api/role/del",         drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(RoleCtrl::permList,    "/admin/api/permission/list",  drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "role:manage");
        auto rows = PayDb::instance().query(
            "SELECT id,role_code,role_name,remark,state,created_at "
            "FROM sys_role ORDER BY id ASC", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"]         = std::stoi(r["id"]);
            it["role_code"]  = r["role_code"];
            it["role_name"]  = r["role_name"];
            it["remark"]     = r["remark"];
            it["state"]      = std::stoi(r["state"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "role:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string code = (*body).get("role_code", "").asString();
        std::string name = (*body).get("role_name", "").asString();
        if (code.empty() || name.empty()) { RESP_ERR(cb, "编码和名称必填"); return; }

        auto &db = PayDb::instance();
        auto exist = db.queryOne("SELECT id FROM sys_role WHERE role_code=?", {code});
        if (!exist.empty()) { RESP_ERR(cb, "角色编码已存在"); return; }

        db.exec("INSERT INTO sys_role(role_code,role_name,remark,state,created_at) VALUES(?,?,?,1,?)",
                {code, name, (*body).get("remark", "").asString(),
                 std::to_string(std::time(nullptr))});
        OplogService::adminLog(req, "role", "add", code, "");
        RESP_MSG(cb, "创建成功");
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "role:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        PayDb::instance().exec(
            "UPDATE sys_role SET role_name=?,remark=?,state=? WHERE id=?",
            {(*body).get("role_name", "").asString(),
             (*body).get("remark", "").asString(),
             std::to_string((*body).get("state", 1).asInt()), id});
        RbacService::invalidateAll();
        OplogService::adminLog(req, "role", "edit", id, "");
        RESP_MSG(cb, "更新成功");
    }

    void assignPerms(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "role:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        auto permIds = (*body)["perm_ids"];
        if (!permIds.isArray()) { RESP_ERR(cb, "perm_ids 必须数组"); return; }

        auto &db = PayDb::instance();
        db.exec("DELETE FROM sys_role_permission WHERE role_id=?", {id});
        for (auto &pid : permIds) {
            db.exec("INSERT INTO sys_role_permission(role_id,perm_id) VALUES(?,?)",
                    {id, pid.asString()});
        }
        RbacService::invalidateAll();
        OplogService::adminLog(req, "role", "assignPerms", id,
            "数量:" + std::to_string(permIds.size()));
        RESP_MSG(cb, "已分配");
    }

    void perms(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "role:manage");
        std::string id = req->getParameter("id");
        auto rows = PayDb::instance().query(
            "SELECT p.id,p.perm_code,p.perm_name,p.module FROM sys_permission p "
            "INNER JOIN sys_role_permission rp ON rp.perm_id=p.id "
            "WHERE rp.role_id=?", {id});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            it["id"] = std::stoi(r["id"]);
            it["perm_code"] = r["perm_code"];
            it["perm_name"] = r["perm_name"];
            it["module"]    = r["module"];
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "role:manage");
        std::string id = req->getParameter("id");
        auto r = PayDb::instance().queryOne("SELECT role_code FROM sys_role WHERE id=?", {id});
        if (r.empty()) { RESP_ERR(cb, "角色不存在"); return; }
        if (r["role_code"] == "super_admin") { RESP_ERR(cb, "超级管理员角色不可删除"); return; }

        auto &db = PayDb::instance();
        db.exec("DELETE FROM sys_role WHERE id=?", {id});
        db.exec("DELETE FROM sys_role_permission WHERE role_id=?", {id});
        db.exec("DELETE FROM sys_user_role WHERE role_id=?", {id});
        RbacService::invalidateAll();
        OplogService::adminLog(req, "role", "delete", id, "");
        RESP_MSG(cb, "已删除");
    }

    // 全部权限清单，按模块分组
    void permList(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query(
            "SELECT id,perm_code,perm_name,module FROM sys_permission ORDER BY module,id", {});
        // group by module
        std::map<std::string, Json::Value> grouped;
        for (auto &r : rows) {
            Json::Value it;
            it["id"]        = std::stoi(r["id"]);
            it["perm_code"] = r["perm_code"];
            it["perm_name"] = r["perm_name"];
            grouped[r["module"]].append(it);
        }
        Json::Value arr(Json::arrayValue);
        for (auto &[m, list] : grouped) {
            Json::Value g;
            g["module"] = m;
            g["items"]  = list;
            arr.append(g);
        }
        RESP_OK(cb, arr);
    }
};
