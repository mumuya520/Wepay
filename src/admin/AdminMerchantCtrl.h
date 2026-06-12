// WePay-Cpp — 管理后台: 商户管理控制器
// 职责：商户的增删改查、启用禁用、密钥重置、一键登录等管理功能
//
// API 端点：
// GET    /admin/api/merchant/list      分页列表
// GET    /admin/api/merchant/detail    商户详情
// POST   /admin/api/merchant/add       新增商户
// POST   /admin/api/merchant/edit      编辑商户
// POST   /admin/api/merchant/state     启用/禁用
// POST   /admin/api/merchant/resetKey  重置通讯密钥
// POST   /admin/api/merchant/quickLogin 管理员一键登录商户后台
// DELETE /admin/api/merchant/del       删除商户
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/PasswordUtils.h" // 密码工具
#include "../common/ChannelService.h" // 通道服务
#include "../common/OplogService.h" // 操作日志服务
#include "../common/SimpleJwt.h" // JWT 令牌
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class AdminMerchantCtrl : public drogon::HttpController<AdminMerchantCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminMerchantCtrl::list,     "/admin/api/merchant/list",       drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminMerchantCtrl::detail,   "/admin/api/merchant/detail",     drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminMerchantCtrl::add,      "/admin/api/merchant/add",        drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminMerchantCtrl::edit,     "/admin/api/merchant/edit",       drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminMerchantCtrl::state,    "/admin/api/merchant/state",      drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminMerchantCtrl::resetKey, "/admin/api/merchant/resetKey",   drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminMerchantCtrl::quickLogin,"/admin/api/merchant/quickLogin",drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminMerchantCtrl::del,      "/admin/api/merchant/del",         drogon::Delete, "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int page = safeInt(req->getParameter("page"), 1);
        int size = safeInt(req->getParameter("size"), 20);
        if (page < 1) page = 1;
        if (size < 1 || size > 100) size = 20;
        int offset = (page - 1) * size;

        std::string search = req->getParameter("search");
        std::string where = "1=1";
        std::vector<std::string> params;
        if (!search.empty()) {
            where += " AND (mch_no LIKE ? OR mch_name LIKE ? OR username LIKE ?)";
            std::string like = "%" + search + "%";
            params.insert(params.end(), {like, like, like});
        }

        auto cntR = db.query("SELECT COUNT(*) AS c FROM merchant WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0].at("c"));

        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT id,mch_no,username,mch_name,contact,phone,email,"
            "balance,frozen,total_income,rate,state,created_at "
            "FROM merchant WHERE " + where + " ORDER BY id DESC LIMIT ? OFFSET ?", pp);

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]           = std::stoi(r.at("id"));
            item["mch_no"]       = r.at("mch_no");
            item["username"]     = r.at("username");
            item["mch_name"]     = r.at("mch_name");
            item["contact"]      = r.at("contact");
            item["phone"]        = r.at("phone");
            item["balance"]      = r.at("balance");
            item["frozen"]       = r.at("frozen");
            item["total_income"] = r.at("total_income");
            item["rate"]         = r.at("rate");
            item["state"]        = std::stoi(r.at("state"));
            item["created_at"]   = (Json::Int64)std::stoll(r.at("created_at"));
            arr.append(item);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        data["page"] = page; data["size"] = size;
        RESP_OK(cb, data);
    }

    void detail(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getParameter("id");
        auto row = PayDb::instance().queryOne(
            "SELECT id,mch_no,username,mch_name,contact,phone,email,mch_key,"
            "sign_type,balance,frozen,total_income,rate,settle_type,"
            "settle_account,ip_white,notify_url,return_url,state,created_at,updated_at "
            "FROM merchant WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "商户不存在"); return; }

        Json::Value data;
        for (auto &[k, v] : row) data[k] = v;
        data["id"] = std::stoi(row["id"]);
        data["state"] = std::stoi(row["state"]);
        RESP_OK(cb, data);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &j = *body;
        auto &db = PayDb::instance();

        std::string username = j.get("username", "").asString();
        std::string password = j.get("password", "").asString();
        std::string mchName  = j.get("mch_name", "").asString();
        std::string contact  = j.get("contact", "").asString();
        std::string phone    = j.get("phone", "").asString();
        std::string email    = j.get("email", "").asString();
        std::string rate     = j.get("rate", "1.00").asString();

        if (username.empty() || password.empty()) {
            RESP_ERR(cb, "用户名和密码不能为空"); return;
        }

        auto exist = db.queryOne("SELECT id FROM merchant WHERE username=?", {username});
        if (!exist.empty()) { RESP_ERR(cb, "用户名已存在"); return; }

        std::string mchNo  = PasswordUtils::generateMchNo();
        std::string mchKey = PasswordUtils::generateKey(32);
        std::string salt   = PasswordUtils::generateSalt();
        std::string hash   = PasswordUtils::hashPassword(password, salt);
        long long now = std::time(nullptr);

        db.exec("INSERT INTO merchant(mch_no,username,password,salt,mch_name,"
                "contact,phone,email,mch_key,rate,state,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,1,?,?)",
                {mchNo, username, hash, salt, mchName, contact, phone, email,
                 mchKey, rate, std::to_string(now), std::to_string(now)});

        OplogService::adminLog(req, OplogService::MOD_MERCHANT, "add", mchNo, "新增商户: " + username);

        Json::Value data;
        data["mch_no"]  = mchNo;
        data["mch_key"] = mchKey;
        RESP_OK(cb, data);
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &j = *body;
        auto &db = PayDb::instance();

        std::string id = j.get("id", "").asString();
        auto mch = db.queryOne("SELECT id FROM merchant WHERE id=?", {id});
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; }

        long long now = std::time(nullptr);
        std::string mchName = j.get("mch_name", "").asString();
        std::string contact = j.get("contact", "").asString();
        std::string phone   = j.get("phone", "").asString();
        std::string email   = j.get("email", "").asString();
        std::string rate    = j.get("rate", "").asString();
        std::string ipWhite = j.get("ip_white", "").asString();
        std::string notifyUrl = j.get("notify_url", "").asString();
        std::string returnUrl = j.get("return_url", "").asString();
        int settleType = j.get("settle_type", 0).asInt();
        std::string settleAccount = j.get("settle_account", "").asString();

        db.exec("UPDATE merchant SET mch_name=?,contact=?,phone=?,email=?,"
                "rate=?,ip_white=?,notify_url=?,return_url=?,"
                "settle_type=?,settle_account=?,updated_at=? WHERE id=?",
                {mchName, contact, phone, email, rate, ipWhite,
                 notifyUrl, returnUrl, std::to_string(settleType),
                 settleAccount, std::to_string(now), id});

        // 若有新密码
        std::string newPwd = j.get("password", "").asString();
        if (!newPwd.empty()) {
            std::string salt = PasswordUtils::generateSalt();
            std::string hash = PasswordUtils::hashPassword(newPwd, salt);
            db.exec("UPDATE merchant SET password=?,salt=? WHERE id=?", {hash, salt, id});
        }

        OplogService::adminLog(req, OplogService::MOD_MERCHANT, "edit", id, "编辑商户");
        RESP_MSG(cb, "更新成功");
    }

    void state(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int newState = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE merchant SET state=?,updated_at=? WHERE id=?",
            {std::to_string(newState), std::to_string(std::time(nullptr)), id});
        RESP_MSG(cb, newState ? "已启用" : "已禁用");
    }

    void resetKey(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        std::string newKey = PasswordUtils::generateKey(32);
        PayDb::instance().exec("UPDATE merchant SET mch_key=?,updated_at=? WHERE id=?",
            {newKey, std::to_string(std::time(nullptr)), id});
        Json::Value data; data["mch_key"] = newKey;
        RESP_OK(cb, data);
    }

    void quickLogin(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string mchId = (*body).get("mch_id", "").asString();
        if (mchId.empty()) { RESP_ERR(cb, "缺少商户ID"); return; }

        auto &db = PayDb::instance();
        auto mch = db.queryOne(
            "SELECT id,mch_no,username,mch_name,state FROM merchant WHERE id=?",
            {mchId});
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; }
        if (mch["state"] == "0") { RESP_ERR(cb, "商户已被禁用"); return; }

        std::string sub = "mch:" + mch["id"] + ":" + mch["username"];
        std::string token = SimpleJwt::sign(sub);

        OplogService::adminLog(req, OplogService::MOD_MERCHANT, "quickLogin",
                               mch["mch_no"], "管理员一键登录商户: " + mch["username"]);

        Json::Value data;
        data["token"]    = token;
        data["mch_no"]   = mch["mch_no"];
        data["mch_name"] = mch["mch_name"];
        data["username"] = mch["username"];
        RESP_JSON(cb, AjaxResult::success("登录成功", data));
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getParameter("id");
        if (id.empty()) { RESP_ERR(cb, "缺少id"); return; }
        PayDb::instance().exec("DELETE FROM merchant WHERE id=?", {id});
        PayDb::instance().exec("DELETE FROM merchant_channel WHERE mch_id=?", {id});
        RESP_MSG(cb, "删除成功");
    }

private:
    static int safeInt(const std::string &s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }
};
