// WePay-Cpp — 管理后台: 自定义表单控制器
// 职责：自定义表单的增删改查、提交记录管理等功能（基于 form-create）
//
// 管理员接口（鉴权）：
// GET    /admin/api/customForm/list                    列表（支持分页搜索）
// GET    /admin/api/customForm/get?id=                 取单个
// POST   /admin/api/customForm/save                    创建/更新（按 code）
// DELETE /admin/api/customForm/del?id=                 删除
// GET    /admin/api/customForm/submissions?formId=     提交记录列表
// DELETE /admin/api/customForm/submission/del?id=      删除提交记录
//
// === 公开接口 (无鉴权) ===
// GET    /api/customForm/byCode?code=                  公开取表单 schema
// POST   /api/customForm/submit                        提交表单
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include <regex>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class CustomFormCtrl : public drogon::HttpController<CustomFormCtrl> {
public:
    METHOD_LIST_BEGIN
        // 管理员
        ADD_METHOD_TO(CustomFormCtrl::list,         "/admin/api/customForm/list",            drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(CustomFormCtrl::get,          "/admin/api/customForm/get",             drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(CustomFormCtrl::save,         "/admin/api/customForm/save",            drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(CustomFormCtrl::del,          "/admin/api/customForm/del",             drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(CustomFormCtrl::submissions,  "/admin/api/customForm/submissions",     drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(CustomFormCtrl::deleteSub,    "/admin/api/customForm/submission/del",  drogon::Delete, "AdminAuthFilter");
        // 公开
        ADD_METHOD_TO(CustomFormCtrl::publicGet,    "/api/customForm/byCode",                drogon::Get);
        ADD_METHOD_TO(CustomFormCtrl::publicSubmit, "/api/customForm/submit",                drogon::Post);
    METHOD_LIST_END

    // ───── 管理员：列表 ─────
    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:view");
        std::string kw = req->getParameter("keyword");
        int page  = std::max(1, atoiSafe(req->getParameter("page"), 1));
        int size  = std::min(100, std::max(1, atoiSafe(req->getParameter("size"), 20)));
        int offs  = (page - 1) * size;

        std::string where = "1=1";
        std::vector<std::string> params;
        if (!kw.empty()) {
            where += " AND (code LIKE ? OR title LIKE ?)";
            params.push_back("%" + kw + "%");
            params.push_back("%" + kw + "%");
        }

        auto &db = PayDb::instance();
        auto cntRow = db.query("SELECT COUNT(*) AS c FROM custom_form WHERE " + where, params);
        int total = cntRow.empty() ? 0 : std::stoi(cntRow[0]["c"]);

        params.push_back(std::to_string(size));
        params.push_back(std::to_string(offs));
        auto rows = db.query(
            "SELECT id,code,title,description,state,require_login,created_by,created_at,updated_at "
            "FROM custom_form WHERE " + where + " ORDER BY id DESC LIMIT ? OFFSET ?",
            params);

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"]            = std::stoi(r["id"]);
            it["state"]         = std::stoi(r["state"]);
            it["require_login"] = std::stoi(r["require_login"]);
            it["created_at"]    = (Json::Int64)std::stoll(r["created_at"]);
            it["updated_at"]    = (Json::Int64)std::stoll(r["updated_at"]);
            // 统计提交数
            auto sc = db.query("SELECT COUNT(*) AS c FROM custom_form_submission WHERE form_id=?",
                               {r["id"]});
            it["submission_count"] = sc.empty() ? 0 : std::stoi(sc[0]["c"]);
            arr.append(it);
        }
        Json::Value out;
        out["list"]  = arr;
        out["total"] = total;
        out["page"]  = page;
        out["size"]  = size;
        RESP_OK(cb, out);
    }

    // ───── 管理员：取单个（带 schema_json） ─────
    void get(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:view");
        std::string id   = req->getParameter("id");
        std::string code = req->getParameter("code");
        if (id.empty() && code.empty()) { RESP_ERR(cb, "缺少参数"); return; }

        auto &db = PayDb::instance();
        auto rows = !id.empty()
            ? db.query("SELECT * FROM custom_form WHERE id=?", {id})
            : db.query("SELECT * FROM custom_form WHERE code=?", {code});

        if (rows.empty()) { RESP_ERR(cb, "未找到"); return; }
        auto &r = rows[0];
        Json::Value it;
        for (auto &[k, v] : r) it[k] = v;
        it["id"]            = std::stoi(r["id"]);
        it["state"]         = std::stoi(r["state"]);
        it["require_login"] = std::stoi(r["require_login"]);
        it["created_at"]    = (Json::Int64)std::stoll(r["created_at"]);
        it["updated_at"]    = (Json::Int64)std::stoll(r["updated_at"]);
        RESP_OK(cb, it);
    }

    // ───── 管理员：创建/更新（按 code 视为 upsert） ─────
    void save(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;

        std::string code  = j.get("code", "").asString();
        std::string title = j.get("title", "").asString();
        if (code.empty())  { RESP_ERR(cb, "code 不能为空"); return; }
        if (title.empty()) { RESP_ERR(cb, "title 不能为空"); return; }
        if (!std::regex_match(code, std::regex("^[A-Za-z0-9_-]+$"))) {
            RESP_ERR(cb, "code 只能字母/数字/下划线/短横线"); return;
        }

        Json::FastWriter fw;
        std::string formFields = j.isMember("form_fields") ? fw.write(j["form_fields"]) : "[]";
        std::string formConf   = j.isMember("form_conf")   ? fw.write(j["form_conf"])   : "{}";
        if (!formFields.empty() && formFields.back() == '\n') formFields.pop_back();
        if (!formConf.empty()   && formConf.back()   == '\n') formConf.pop_back();

        std::string desc       = j.get("description", "").asString();
        std::string state      = std::to_string(j.get("state", 1).asInt());
        std::string reqLogin   = std::to_string(j.get("require_login", 0).asInt());
        std::string now        = std::to_string(std::time(nullptr));
        std::string createdBy  = req->session() ? req->session()->get<std::string>("admin_username") : "";

        auto &db = PayDb::instance();
        std::string id = j.get("id", "").asString();
        if (id.empty() || id == "0") {
            // 检查 code 重复
            auto exist = db.query("SELECT id FROM custom_form WHERE code=?", {code});
            if (!exist.empty()) { RESP_ERR(cb, "code 已存在"); return; }
            db.exec(
                "INSERT INTO custom_form(code,title,description,form_fields,form_conf,"
                "state,require_login,created_by,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?)",
                {code, title, desc, formFields, formConf,
                 state, reqLogin, createdBy, now, now});
            OplogService::adminLog(req, OplogService::MOD_CONFIG, "customForm.add", code,
                                    "title=" + title);
            RESP_MSG(cb, "已创建");
        } else {
            db.exec(
                "UPDATE custom_form SET code=?,title=?,description=?,form_fields=?,"
                "form_conf=?,state=?,require_login=?,updated_at=? WHERE id=?",
                {code, title, desc, formFields, formConf,
                 state, reqLogin, now, id});
            OplogService::adminLog(req, OplogService::MOD_CONFIG, "customForm.edit", code,
                                    "title=" + title);
            RESP_MSG(cb, "已更新");
        }
    }

    // ───── 管理员：删除 ─────
    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:edit");
        std::string id = req->getParameter("id");
        if (id.empty()) { RESP_ERR(cb, "缺少 id"); return; }
        auto &db = PayDb::instance();
        db.exec("DELETE FROM custom_form_submission WHERE form_id=?", {id});
        db.exec("DELETE FROM custom_form WHERE id=?", {id});
        OplogService::adminLog(req, OplogService::MOD_CONFIG, "customForm.del", id, "");
        RESP_MSG(cb, "已删除");
    }

    // ───── 管理员：查看提交记录 ─────
    void submissions(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:view");
        std::string formId = req->getParameter("formId");
        if (formId.empty()) { RESP_ERR(cb, "缺少 formId"); return; }
        int page = std::max(1, atoiSafe(req->getParameter("page"), 1));
        int size = std::min(200, std::max(1, atoiSafe(req->getParameter("size"), 50)));
        int offs = (page - 1) * size;

        auto &db = PayDb::instance();
        auto cntRow = db.query("SELECT COUNT(*) AS c FROM custom_form_submission WHERE form_id=?",
                               {formId});
        int total = cntRow.empty() ? 0 : std::stoi(cntRow[0]["c"]);

        auto rows = db.query(
            "SELECT * FROM custom_form_submission WHERE form_id=? ORDER BY id DESC LIMIT ? OFFSET ?",
            {formId, std::to_string(size), std::to_string(offs)});

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"]         = std::stoi(r["id"]);
            it["form_id"]    = std::stoi(r["form_id"]);
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]);
            arr.append(it);
        }
        Json::Value out;
        out["list"]  = arr;
        out["total"] = total;
        RESP_OK(cb, out);
    }

    // ───── 管理员：删除提交记录 ─────
    void deleteSub(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "config:edit");
        std::string id = req->getParameter("id");
        if (id.empty()) { RESP_ERR(cb, "缺少 id"); return; }
        PayDb::instance().exec("DELETE FROM custom_form_submission WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // ───── 公开：取表单 schema ─────
    void publicGet(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string code = req->getParameter("code");
        if (code.empty()) { RESP_ERR(cb, "缺少 code"); return; }

        auto &db = PayDb::instance();
        auto rows = db.query("SELECT * FROM custom_form WHERE code=? AND state=1", {code});
        if (rows.empty()) { RESP_ERR(cb, "表单不存在或已停用"); return; }
        auto &r = rows[0];
        Json::Value it;
        it["id"]            = std::stoi(r["id"]);
        it["code"]          = r["code"];
        it["title"]         = r["title"];
        it["description"]   = r["description"];
        it["form_fields"]   = r["form_fields"];
        it["form_conf"]     = r["form_conf"];
        it["require_login"] = std::stoi(r["require_login"]);
        RESP_OK(cb, it);
    }

    // ───── 公开：提交表单 ─────
    void publicSubmit(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string code = j.get("code", "").asString();
        if (code.empty()) { RESP_ERR(cb, "缺少 code"); return; }

        auto &db = PayDb::instance();
        auto rows = db.query("SELECT id, require_login FROM custom_form WHERE code=? AND state=1",
                             {code});
        if (rows.empty()) { RESP_ERR(cb, "表单不存在或已停用"); return; }
        std::string formId = rows[0]["id"];
        int reqLogin = std::stoi(rows[0]["require_login"]);

        std::string submitter;
        if (req->session()) {
            try { submitter = req->session()->get<std::string>("admin_username"); } catch(...) {}
            if (submitter.empty()) try { submitter = req->session()->get<std::string>("merchant_username"); } catch(...) {}
        }
        if (reqLogin && submitter.empty()) {
            RESP_ERR(cb, "本表单需要登录后才能提交"); return;
        }

        Json::FastWriter fw;
        std::string payloadJson = j.isMember("payload") ? fw.write(j["payload"]) : "{}";
        if (!payloadJson.empty() && payloadJson.back() == '\n') payloadJson.pop_back();

        std::string ip = req->getHeader("X-Real-IP");
        if (ip.empty()) ip = req->getHeader("X-Forwarded-For");
        if (ip.empty()) ip = req->peerAddr().toIp();
        std::string now = std::to_string(std::time(nullptr));

        db.exec(
            "INSERT INTO custom_form_submission(form_id,form_code,submitter,submitter_ip,"
            "payload_json,created_at) VALUES(?,?,?,?,?,?)",
            {formId, code, submitter, ip, payloadJson, now});
        RESP_MSG(cb, "提交成功");
    }

private:
    static int atoiSafe(const std::string &s, int def) {
        if (s.empty()) return def;
        try { return std::stoi(s); } catch (...) { return def; }
    }
};
