// WePay-Cpp — 管理后台: 商户门店管理控制器
// 职责：商户门店的增删改查等门店管理功能
//
// API 端点：
// GET    /admin/api/mchStore/list    列表
// POST   /admin/api/mchStore/add     新增
// POST   /admin/api/mchStore/edit    编辑
// DELETE /admin/api/mchStore/del     删除
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class MchStoreCtrl : public drogon::HttpController<MchStoreCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(MchStoreCtrl::list, "/admin/api/mchStore/list", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MchStoreCtrl::add,  "/admin/api/mchStore/add",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MchStoreCtrl::edit, "/admin/api/mchStore/edit", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MchStoreCtrl::del,  "/admin/api/mchStore/del",  drogon::Delete, "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:view");
        std::string mchId = req->getParameter("mch_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        auto rows = db.query("SELECT * FROM mch_store WHERE " + where + " ORDER BY id DESC", params);
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
        if (j.get("store_name", "").asString().empty()) {
            RESP_ERR(cb, "门店名称必填"); return;
        }
        PayDb::instance().exec(
            "INSERT INTO mch_store(mch_id,app_id,store_name,store_addr,contact,phone,remark,state,created_at) "
            "VALUES(?,?,?,?,?,?,?,1,?)",
            {j.get("mch_id", "0").asString(),
             j.get("app_id", "").asString(),
             j.get("store_name", "").asString(),
             j.get("store_addr", "").asString(),
             j.get("contact", "").asString(),
             j.get("phone", "").asString(),
             j.get("remark", "").asString(),
             std::to_string(std::time(nullptr))});
        RESP_MSG(cb, "已添加");
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        PayDb::instance().exec(
            "UPDATE mch_store SET store_name=?,store_addr=?,contact=?,phone=?,remark=?,state=? WHERE id=?",
            {j.get("store_name", "").asString(),
             j.get("store_addr", "").asString(),
             j.get("contact", "").asString(),
             j.get("phone", "").asString(),
             j.get("remark", "").asString(),
             std::to_string(j.get("state", 1).asInt()),
             j.get("id", "").asString()});
        RESP_MSG(cb, "已更新");
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:delete");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM mch_store WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }
};
