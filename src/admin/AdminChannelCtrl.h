// WePay-Cpp — 管理后台: 支付通道管理控制器
// 职责：支付通道的增删改查、启用禁用、商户绑定等管理功能
//
// API 端点：
// GET    /admin/api/channel/list        通道列表
// POST   /admin/api/channel/add         新增通道
// POST   /admin/api/channel/edit        编辑通道
// POST   /admin/api/channel/state       启用/禁用
// DELETE /admin/api/channel/del         删除通道
// GET    /admin/api/channel/bindList    商户-通道绑定列表
// POST   /admin/api/channel/bind       商户绑定通道
// POST   /admin/api/channel/unbind     商户解绑通道
// GET    /admin/api/paytype/list       支付类型列表
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../channel/ChannelPlugin.h" // 通道插件
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class AdminChannelCtrl : public drogon::HttpController<AdminChannelCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminChannelCtrl::list,      "/admin/api/channel/list",     drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::add,       "/admin/api/channel/add",      drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::edit,      "/admin/api/channel/edit",     drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::state,     "/admin/api/channel/state",    drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::del,       "/admin/api/channel/del",      drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::bindList,  "/admin/api/channel/bindList", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::bind,      "/admin/api/channel/bind",     drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::unbind,    "/admin/api/channel/unbind",   drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminChannelCtrl::paytypes,  "/admin/api/paytype/list",     drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query(
            "SELECT * FROM pay_channel ORDER BY sort_order ASC, id ASC", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]            = std::stoi(r.at("id"));
            item["channel_code"]  = r.at("channel_code");
            item["channel_name"]  = r.at("channel_name");
            item["pay_type"]      = r.at("pay_type");
            item["plugin"]        = r.at("plugin");
            item["rate"]          = r.at("rate");
            item["min_amount"]    = r.at("min_amount");
            item["max_amount"]    = r.at("max_amount");
            item["state"]         = std::stoi(r.at("state"));
            item["sort_order"]    = std::stoi(r.at("sort_order"));
            item["remark"]        = r.at("remark");
            item["select_mode"]   = r.count("select_mode") ? std::stoi(r.at("select_mode")) : 0;
            item["code_type"]     = r.count("code_type") ? r.at("code_type") : "";
            item["support_business_code"] = r.count("support_business_code") ? std::stoi(r.at("support_business_code")) : 0;
            arr.append(item);
        }
        Json::Value data; data["list"] = arr;
        RESP_OK(cb, data);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &j = *body;
        auto &db = PayDb::instance();

        std::string code    = j.get("channel_code", "").asString();
        std::string name    = j.get("channel_name", "").asString();
        std::string payType = j.get("pay_type", "").asString();
        std::string plugin  = j.get("plugin", "").asString();
        std::string rate    = j.get("rate", "1.00").asString();
        std::string params  = j.get("params_json", "{}").asString();
        std::string minAmt  = j.get("min_amount", "0.01").asString();
        std::string maxAmt  = j.get("max_amount", "50000").asString();
        std::string dayLim  = j.get("day_limit", "0").asString();
        int sortOrder       = j.get("sort_order", 0).asInt();
        int selectMode      = j.get("select_mode", 0).asInt();
        std::string codeType = j.get("code_type", "").asString();
        std::string supportBusinessCode = j.get("support_business_code", 0).asBool() ? "1" : "0";
        std::string remark  = j.get("remark", "").asString();

        if (code.empty() || name.empty()) { RESP_ERR(cb, "通道编码和名称不能为空"); return; }
        if (plugin.empty()) { RESP_ERR(cb, "插件不能为空"); return; }

        // 校验插件已安装
        if (!ChannelPluginRegistry::instance().isInstalled(plugin)) {
            RESP_ERR(cb, "插件 [" + plugin + "] 未安装，请先到「插件市场」安装该插件"); return;
        }

        auto exist = db.queryOne("SELECT id FROM pay_channel WHERE channel_code=?", {code});
        if (!exist.empty()) { RESP_ERR(cb, "通道编码已存在"); return; }

        long long now = std::time(nullptr);
        db.exec("INSERT INTO pay_channel(channel_code,channel_name,pay_type,plugin,"
                "rate,params_json,min_amount,max_amount,day_limit,state,sort_order,"
                "select_mode,code_type,support_business_code,"
                "remark,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?)",
                {code, name, payType, plugin, rate, params, minAmt, maxAmt,
                 dayLim, std::to_string(sortOrder), std::to_string(selectMode), codeType,
                 supportBusinessCode, remark,
                 std::to_string(now), std::to_string(now)});
        RESP_MSG(cb, "添加成功");
    }

    void edit(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        long long now = std::time(nullptr);
        PayDb::instance().exec(
            "UPDATE pay_channel SET channel_name=?,pay_type=?,plugin=?,"
            "rate=?,params_json=?,min_amount=?,max_amount=?,day_limit=?,"
            "sort_order=?,select_mode=?,code_type=?,support_business_code=?,remark=?,updated_at=? WHERE id=?",
            {j.get("channel_name","").asString(), j.get("pay_type","").asString(),
             j.get("plugin","").asString(), j.get("rate","1.00").asString(),
             j.get("params_json","{}").asString(), j.get("min_amount","0.01").asString(),
             j.get("max_amount","50000").asString(), j.get("day_limit","0").asString(),
             std::to_string(j.get("sort_order",0).asInt()),
             std::to_string(j.get("select_mode",0).asInt()),
             j.get("code_type","").asString(),
             j.get("support_business_code",0).asBool() ? "1" : "0",
             j.get("remark","").asString(), std::to_string(now), id});
        RESP_MSG(cb, "更新成功");
    }

    void state(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE pay_channel SET state=?,updated_at=? WHERE id=?",
            {std::to_string(s), std::to_string(std::time(nullptr)), id});
        RESP_MSG(cb, s ? "已启用" : "已禁用");
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM pay_channel WHERE id=?", {id});
        PayDb::instance().exec("DELETE FROM merchant_channel WHERE channel_id=?", {id});
        RESP_MSG(cb, "删除成功");
    }

    void bindList(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string mchId = req->getParameter("mch_id");
        auto rows = PayDb::instance().query(
            "SELECT mc.id,mc.channel_id,mc.rate,mc.state,mc.select_mode,mc.code_type,"
            "c.channel_code,c.channel_name,c.pay_type,c.rate AS channel_rate,c.select_mode AS channel_select_mode,c.code_type AS channel_code_type "
            "FROM merchant_channel mc "
            "JOIN pay_channel c ON c.id=mc.channel_id "
            "WHERE mc.mch_id=? ORDER BY c.sort_order ASC", {mchId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]            = std::stoi(r.at("id"));
            item["channel_id"]    = std::stoi(r.at("channel_id"));
            item["channel_code"]  = r.at("channel_code");
            item["channel_name"]  = r.at("channel_name");
            item["pay_type"]      = r.at("pay_type");
            item["channel_rate"]  = r.at("channel_rate");
            item["mch_rate"]      = r.at("rate");
            item["select_mode"]   = r.count("select_mode") && !r.at("select_mode").empty() ? std::stoi(r.at("select_mode")) : 0;
            item["code_type"]     = r.count("code_type") ? r.at("code_type") : "";
            item["channel_select_mode"] = r.count("channel_select_mode") && !r.at("channel_select_mode").empty() ? std::stoi(r.at("channel_select_mode")) : 0;
            item["channel_code_type"] = r.count("channel_code_type") ? r.at("channel_code_type") : "";
            item["state"]         = std::stoi(r.at("state"));
            arr.append(item);
        }
        Json::Value data; data["list"] = arr;
        RESP_OK(cb, data);
    }

    void bind(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string mchId = (*body).get("mch_id", "").asString();
        std::string channelId = (*body).get("channel_id", "").asString();
        std::string rate = (*body).get("rate", "").asString();
        std::string selectMode = (*body).isMember("select_mode") ? std::to_string((*body).get("select_mode", 0).asInt()) : "0";
        std::string codeType = (*body).get("code_type", "").asString();
        PayDb::instance().exec(
            "INSERT OR REPLACE INTO merchant_channel(mch_id,channel_id,rate,state,select_mode,code_type) VALUES(?,?,?,?,?,?)",
            {mchId, channelId, rate, "1", selectMode, codeType});
        RESP_MSG(cb, "绑定成功");
    }

    void unbind(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string mchId = (*body).get("mch_id", "").asString();
        std::string channelId = (*body).get("channel_id", "").asString();
        PayDb::instance().exec(
            "DELETE FROM merchant_channel WHERE mch_id=? AND channel_id=?",
            {mchId, channelId});
        RESP_MSG(cb, "解绑成功");
    }

    void paytypes(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query(
            "SELECT * FROM pay_type ORDER BY sort_order ASC, id ASC", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]   = std::stoi(r.at("id"));
            item["code"] = r.at("code");
            item["name"] = r.at("name");
            item["state"] = std::stoi(r.at("state"));
            arr.append(item);
        }
        Json::Value data; data["list"] = arr;
        RESP_OK(cb, data);
    }
};
