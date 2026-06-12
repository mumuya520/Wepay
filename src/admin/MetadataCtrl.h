// WePay-Cpp — 管理后台: 元数据管理控制器
// 职责：支付方式、菜单、聚合二维码、商户配置等元数据管理
//
// 包含功能：
// 1. 支付方式管理 - 支持的支付方式列表
// 2. 菜单管理 - 管理后台菜单树
// 3. 聚合二维码 - 多个支付方式的聚合二维码
// 4. 商户配置 - 商户级别的配置项
//
// API 端点：
// GET    /admin/api/payWay/list    列表
// GET    /admin/api/payWay/public  公开列表(登录页前可用)
// POST   /admin/api/payWay/save    新增/编辑
// DELETE /admin/api/payWay/del     删除
// GET    /admin/api/menu/tree         菜单树
// GET    /admin/api/menu/myMenu       当前用户可见菜单(按权限过滤)
// POST   /admin/api/menu/save
// DELETE /admin/api/menu/del
// === 聚合二维码 ===
// GET    /admin/api/aggQr/list        列表
// POST   /admin/api/aggQr/save        新增/编辑
// POST   /admin/api/aggQr/state       启用禁用
// DELETE /admin/api/aggQr/del
// GET    /qr/:qr_code                 扫码入口(公开，跳转收银台)
//
// === 商户扩展配置 ===
// GET    /admin/api/mchConfig/list    列表
// POST   /admin/api/mchConfig/save
// DELETE /admin/api/mchConfig/del
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include <random>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../filters/AdminAuthFilter.h"

class MetadataCtrl : public drogon::HttpController<MetadataCtrl> {
public:
    METHOD_LIST_BEGIN
        // 支付方式
        ADD_METHOD_TO(MetadataCtrl::payWayList,   "/admin/api/payWay/list",   drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::payWayPublic, "/admin/api/payWay/public", drogon::Get);
        ADD_METHOD_TO(MetadataCtrl::payWaySave,   "/admin/api/payWay/save",   drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::payWayDel,    "/admin/api/payWay/del",    drogon::Delete, "AdminAuthFilter");
        // 菜单
        ADD_METHOD_TO(MetadataCtrl::menuTree,     "/admin/api/menu/tree",     drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::myMenu,       "/admin/api/menu/myMenu",   drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::menuSave,     "/admin/api/menu/save",     drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::menuDel,      "/admin/api/menu/del",      drogon::Delete, "AdminAuthFilter");
        // 聚合二维码
        ADD_METHOD_TO(MetadataCtrl::aggQrList,    "/admin/api/aggQr/list",    drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::aggQrSave,    "/admin/api/aggQr/save",    drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::aggQrState,   "/admin/api/aggQr/state",   drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::aggQrDel,     "/admin/api/aggQr/del",     drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::aggQrScan,    "/qr/{qr_code}",            drogon::Get);
        // 商户扩展配置
        ADD_METHOD_TO(MetadataCtrl::mchCfgList,   "/admin/api/mchConfig/list", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::mchCfgSave,   "/admin/api/mchConfig/save", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MetadataCtrl::mchCfgDel,    "/admin/api/mchConfig/del",  drogon::Delete, "AdminAuthFilter");
    METHOD_LIST_END

    // ── 支付方式 ──────────────────────────────────────────
    void payWayList(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        RESP_OK(cb, fetchPayWays(false));
    }

    void payWayPublic(const drogon::HttpRequestPtr &,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        RESP_OK(cb, fetchPayWays(true));  // 仅启用
    }

    void payWaySave(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        long long now = std::time(nullptr);
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO pay_way(way_code,way_name,icon,bg_color,if_code,sort_order,state,remark,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?)",
                    {j.get("way_code", "").asString(),
                     j.get("way_name", "").asString(),
                     j.get("icon", "").asString(),
                     j.get("bg_color", "").asString(),
                     j.get("if_code", "").asString(),
                     std::to_string(j.get("sort_order", 0).asInt()),
                     std::to_string(j.get("state", 1).asInt()),
                     j.get("remark", "").asString(),
                     std::to_string(now)});
        } else {
            db.exec("UPDATE pay_way SET way_name=?,icon=?,bg_color=?,if_code=?,"
                    "sort_order=?,state=?,remark=?,updated_at=? WHERE id=?",
                    {j.get("way_name", "").asString(),
                     j.get("icon", "").asString(),
                     j.get("bg_color", "").asString(),
                     j.get("if_code", "").asString(),
                     std::to_string(j.get("sort_order", 0).asInt()),
                     std::to_string(j.get("state", 1).asInt()),
                     j.get("remark", "").asString(),
                     std::to_string(now), id});
        }
        RESP_MSG(cb, "已保存");
    }

    void payWayDel(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM pay_way WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // ── 菜单 ──────────────────────────────────────────────
    void menuTree(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string target = req->getParameter("target");
        if (target.empty()) target = "admin";
        RESP_OK(cb, buildMenuTree(target, std::set<std::string>{"*"}));
    }

    void myMenu(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        bool isSuper = (req->getHeader("X-Admin-Super") == "1");
        std::set<std::string> perms;
        if (isSuper) {
            perms.insert("*");
        } else {
            int uid = 0;
            try { uid = std::stoi(req->getHeader("X-Admin-Id")); } catch(...) {}
            perms = RbacService::loadUserPermissions(uid);
        }
        RESP_OK(cb, buildMenuTree("admin", perms));
    }

    void menuSave(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO sys_menu(parent_id,menu_name,menu_path,menu_icon,perm_code,"
                    "sort_order,target,state) VALUES(?,?,?,?,?,?,?,?)",
                    {std::to_string(j.get("parent_id", 0).asInt()),
                     j.get("menu_name", "").asString(),
                     j.get("menu_path", "").asString(),
                     j.get("menu_icon", "").asString(),
                     j.get("perm_code", "").asString(),
                     std::to_string(j.get("sort_order", 0).asInt()),
                     j.get("target", "admin").asString(),
                     std::to_string(j.get("state", 1).asInt())});
        } else {
            db.exec("UPDATE sys_menu SET parent_id=?,menu_name=?,menu_path=?,menu_icon=?,"
                    "perm_code=?,sort_order=?,target=?,state=? WHERE id=?",
                    {std::to_string(j.get("parent_id", 0).asInt()),
                     j.get("menu_name", "").asString(),
                     j.get("menu_path", "").asString(),
                     j.get("menu_icon", "").asString(),
                     j.get("perm_code", "").asString(),
                     std::to_string(j.get("sort_order", 0).asInt()),
                     j.get("target", "admin").asString(),
                     std::to_string(j.get("state", 1).asInt()), id});
        }
        RESP_MSG(cb, "已保存");
    }

    void menuDel(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        std::string id = req->getParameter("id");
        auto &db = PayDb::instance();
        auto child = db.queryOne("SELECT id FROM sys_menu WHERE parent_id=? LIMIT 1", {id});
        if (!child.empty()) { RESP_ERR(cb, "该菜单下还有子项，无法删除"); return; }
        db.exec("DELETE FROM sys_menu WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // ── 聚合二维码 ─────────────────────────────────────────
    void aggQrList(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:view");
        std::string mchId = req->getParameter("mch_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        auto rows = db.query(
            "SELECT * FROM agg_qrcode WHERE " + where + " ORDER BY id DESC LIMIT 500", params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["store_id"] = std::stoi(r["store_id"]);
            it["qr_type"] = std::stoi(r["qr_type"]);
            it["shell_id"] = std::stoi(r["shell_id"]);
            it["total_count"] = std::stoi(r["total_count"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void aggQrSave(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        long long now = std::time(nullptr);
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            std::string qrCode = generateQrCode();
            db.exec("INSERT INTO agg_qrcode(qr_code,mch_id,app_id,store_id,qr_name,qr_type,"
                    "fixed_amount,subject,body,way_codes,shell_id,state,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,1,?,?)",
                    {qrCode,
                     j.get("mch_id", "0").asString(),
                     j.get("app_id", "").asString(),
                     std::to_string(j.get("store_id", 0).asInt()),
                     j.get("qr_name", "").asString(),
                     std::to_string(j.get("qr_type", 1).asInt()),
                     j.get("fixed_amount", "0.00").asString(),
                     j.get("subject", "").asString(),
                     j.get("body", "").asString(),
                     j.get("way_codes", "").asString(),
                     std::to_string(j.get("shell_id", 0).asInt()),
                     std::to_string(now), std::to_string(now)});
            Json::Value data; data["qr_code"] = qrCode;
            RESP_OK(cb, data); return;
        } else {
            db.exec("UPDATE agg_qrcode SET qr_name=?,qr_type=?,fixed_amount=?,subject=?,body=?,"
                    "way_codes=?,shell_id=?,updated_at=? WHERE id=?",
                    {j.get("qr_name", "").asString(),
                     std::to_string(j.get("qr_type", 1).asInt()),
                     j.get("fixed_amount", "0.00").asString(),
                     j.get("subject", "").asString(),
                     j.get("body", "").asString(),
                     j.get("way_codes", "").asString(),
                     std::to_string(j.get("shell_id", 0).asInt()),
                     std::to_string(now), id});
            RESP_MSG(cb, "已更新");
        }
    }

    void aggQrState(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string id = (*body).get("id", "").asString();
        int s = (*body).get("state", 0).asInt();
        PayDb::instance().exec("UPDATE agg_qrcode SET state=?,updated_at=? WHERE id=?",
            {std::to_string(s), std::to_string(std::time(nullptr)), id});
        RESP_MSG(cb, "已更新");
    }

    void aggQrDel(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM agg_qrcode WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // 公开: 扫码入口 - 渲染选择支付方式的 HTML 页
    void aggQrScan(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                   const std::string &qrCode) {
        auto q = PayDb::instance().queryOne(
            "SELECT * FROM agg_qrcode WHERE qr_code=? AND state=1", {qrCode});
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_HTML);
        if (q.empty()) {
            resp->setBody("<html><body style='text-align:center;padding:100px'>"
                          "<h2>二维码不存在或已停用</h2></body></html>");
            cb(resp); return;
        }

        std::string qrName = q["qr_name"].empty() ? "商品支付" : q["qr_name"];
        std::string subject = q["subject"];
        std::string fixedAmt = q["fixed_amount"];
        std::string wayCodes = q["way_codes"];
        std::string mchId = q["mch_id"];
        std::string qType = q["qr_type"];

        // 简化HTML页面 - 根据 UA 自动选择微信/支付宝
        std::string html =
            "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
            "<title>" + qrName + " - WePay 收银台</title>"
            "<style>body{margin:0;font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
            "background:linear-gradient(180deg,#667eea,#764ba2);min-height:100vh;"
            "display:flex;align-items:center;justify-content:center}"
            ".card{background:#fff;border-radius:16px;padding:40px 30px;width:90%;max-width:420px;"
            "box-shadow:0 20px 60px rgba(0,0,0,.2);text-align:center}"
            ".name{font-size:18px;color:#666;margin-bottom:8px}"
            ".amt{font-size:42px;color:#f56c6c;font-weight:700;margin:10px 0 30px}"
            ".amt small{font-size:18px;font-weight:400}"
            ".input{width:100%;padding:12px;border:1px solid #ddd;border-radius:8px;"
            "font-size:22px;text-align:center;margin-bottom:20px;box-sizing:border-box}"
            ".btn{width:100%;padding:14px;border:none;border-radius:8px;color:#fff;"
            "font-size:16px;cursor:pointer;margin-bottom:10px}"
            ".btn-wx{background:#07C160}.btn-ali{background:#1677FF}.btn-qq{background:#12B7F5}"
            "</style></head><body>"
            "<div class='card'>"
            "<div class='name'>" + subject + "</div>";

        if (qType == "1") {  // 固定金额
            html += "<div class='amt'><small>￥</small>" + fixedAmt + "</div>";
        } else {
            html += "<input id='amt' class='input' type='number' placeholder='输入金额' step='0.01'>";
        }

        // 根据 way_codes 渲染按钮
        auto hasWay = [&](const std::string &key) {
            return wayCodes.find(key) != std::string::npos;
        };
        if (hasWay("wx")) {
            html += "<button class='btn btn-wx' onclick='pay(\"wx_jsapi\")'>微信支付</button>";
        }
        if (hasWay("ali")) {
            html += "<button class='btn btn-ali' onclick='pay(\"ali_jsapi\")'>支付宝支付</button>";
        }
        if (hasWay("ysf")) {
            html += "<button class='btn btn-qq' onclick='pay(\"ysf_jsapi\")'>云闪付</button>";
        }

        html += "<script>"
                "function pay(way){"
                "  var amt = " + (qType == "1" ? ("\"" + fixedAmt + "\"") : "document.getElementById('amt').value") + ";"
                "  if(!amt || amt<=0){ alert('请输入支付金额'); return; }"
                "  var ua = navigator.userAgent.toLowerCase();"
                "  var body = {mch_id:" + mchId + ",qr_code:'" + qrCode + "',"
                "              way_code:way,amount:amt,ua:ua};"
                "  fetch('/gateway/aggQrOrder',{method:'POST',headers:{'Content-Type':'application/json'},"
                "    body:JSON.stringify(body)})"
                "    .then(r=>r.json()).then(d=>{"
                "      if(d.code==1 && d.data && d.data.pay_url){ location.href=d.data.pay_url; }"
                "      else alert(d.msg || '下单失败');"
                "    });"
                "}"
                "</script></div></body></html>";
        resp->setBody(html);
        cb(resp);
    }

    // ── 商户扩展配置 ───────────────────────────────────────
    void mchCfgList(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:view");
        std::string mchId = req->getParameter("mch_id");
        auto rows = PayDb::instance().query(
            "SELECT * FROM mch_config WHERE mch_id=? ORDER BY config_key", {mchId});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void mchCfgSave(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        PayDb::instance().exec(
            "INSERT OR REPLACE INTO mch_config(mch_id,config_key,config_value,remark,updated_at) "
            "VALUES(?,?,?,?,?)",
            {j.get("mch_id", "0").asString(),
             j.get("config_key", "").asString(),
             j.get("config_value", "").asString(),
             j.get("remark", "").asString(),
             std::to_string(std::time(nullptr))});
        RESP_MSG(cb, "已保存");
    }

    void mchCfgDel(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM mch_config WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

private:
    static Json::Value fetchPayWays(bool onlyActive) {
        auto &db = PayDb::instance();
        auto rows = db.query(
            std::string("SELECT * FROM pay_way ") +
            (onlyActive ? "WHERE state=1 " : "") +
            "ORDER BY sort_order, id", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["sort_order"] = std::stoi(r["sort_order"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        return arr;
    }

    // 构建菜单树(递归) - perms 为 "*" 表示超管放行所有
    static Json::Value buildMenuTree(const std::string &target,
                                      const std::set<std::string> &perms) {
        auto rows = PayDb::instance().query(
            "SELECT id,parent_id,menu_name,menu_path,menu_icon,perm_code,sort_order,state,target "
            "FROM sys_menu WHERE target=? AND state=1 ORDER BY sort_order, id", {target});

        // 分组
        std::map<int, PayDb::Rows> byParent;
        for (auto &r : rows) {
            // 权限过滤
            auto pcIt = r.find("perm_code");
            std::string pc = (pcIt == r.end()) ? "" : pcIt->second;
            if (!pc.empty() && !perms.count("*") && !perms.count(pc)) continue;
            int pid = 0; try { pid = std::stoi(r.at("parent_id")); } catch(...) {}
            byParent[pid].push_back(r);
        }

        std::function<Json::Value(int)> build = [&](int parentId) -> Json::Value {
            Json::Value arr(Json::arrayValue);
            auto it = byParent.find(parentId);
            if (it == byParent.end()) return arr;
            for (auto &r : it->second) {
                Json::Value node;
                int id = 0; try { id = std::stoi(r.at("id")); } catch(...) {}
                node["id"]        = id;
                try { node["parent_id"] = std::stoi(r.at("parent_id")); } catch(...){ node["parent_id"] = 0; }
                node["menu_name"] = r.count("menu_name") ? r.at("menu_name") : "";
                node["menu_path"] = r.count("menu_path") ? r.at("menu_path") : "";
                node["menu_icon"] = r.count("menu_icon") ? r.at("menu_icon") : "";
                node["perm_code"] = r.count("perm_code") ? r.at("perm_code") : "";
                Json::Value children = build(id);
                if (!children.empty()) node["children"] = children;
                arr.append(node);
            }
            return arr;
        };
        return build(0);
    }

    static std::string generateQrCode() {
        static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::string s;
        for (int i = 0; i < 16; ++i) s += cs[rng() % 36];
        return s;
    }
};
