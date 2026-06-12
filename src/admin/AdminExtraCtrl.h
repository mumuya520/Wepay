// WePay-Cpp — 管理后台: 补充功能控制器
// 职责：退款管理、分账组、二维码模板、用户团队、操作日志、订单冻结等补充功能
//
// 包含功能：
// 1. Admin 端退款管理(独立页)
// 2. 分账接收组 DivisionGroup CRUD
// 3. 二维码模板 QrCodeShell CRUD
// 4. 用户团队 SysUserTeam CRUD
// 5. 操作日志 SysLog 查询(独立)
// 6. 订单冻结/解冻
//
// API 端点：
// GET    /admin/api/refund/list          退款单列表
// GET    /admin/api/refund/detail        详情
// POST   /admin/api/refund/retry         重试通道
// POST   /admin/api/refund/sync          同步通道状态
// GET    /admin/api/divisionGroup/list   分账组列表
// POST   /admin/api/divisionGroup/save   保存分账组
// DELETE /admin/api/divisionGroup/del    删除分账组
// GET    /admin/api/qrShell/list         二维码模板列表
// POST   /admin/api/qrShell/save         保存二维码模板
// DELETE /admin/api/qrShell/del          删除二维码模板
// GET    /admin/api/userTeam/list        用户团队列表
// POST   /admin/api/userTeam/save        保存用户团队
// DELETE /admin/api/userTeam/del         删除用户团队
// GET    /admin/api/sysLog/list          操作日志列表
// POST   /admin/api/order/freeze         冻结订单
// POST   /admin/api/order/unfreeze       解冻订单
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <random> // 随机数库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/PermCheck.h" // 权限检查
#include "../common/OplogService.h" // 操作日志服务
#include "../common/ChannelService.h" // 通道服务
#include "../channel/ChannelPlugin.h" // 通道插件
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

class AdminExtraCtrl : public drogon::HttpController<AdminExtraCtrl> {
public:
    METHOD_LIST_BEGIN
        // 退款
        ADD_METHOD_TO(AdminExtraCtrl::refundList,   "/admin/api/refund/list",   drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::refundDetail, "/admin/api/refund/detail", drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::refundRetry,  "/admin/api/refund/retry",  drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::refundSync,   "/admin/api/refund/sync",   drogon::Post, "AdminAuthFilter");
        // 分账组
        ADD_METHOD_TO(AdminExtraCtrl::dgList, "/admin/api/divisionGroup/list", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::dgSave, "/admin/api/divisionGroup/save", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::dgDel,  "/admin/api/divisionGroup/del",  drogon::Delete, "AdminAuthFilter");
        // 二维码模板
        ADD_METHOD_TO(AdminExtraCtrl::qsList, "/admin/api/qrShell/list", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::qsSave, "/admin/api/qrShell/save", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::qsDel,  "/admin/api/qrShell/del",  drogon::Delete, "AdminAuthFilter");
        // 用户团队
        ADD_METHOD_TO(AdminExtraCtrl::utList, "/admin/api/userTeam/list", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::utSave, "/admin/api/userTeam/save", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::utDel,  "/admin/api/userTeam/del",  drogon::Delete, "AdminAuthFilter");
        // 操作日志
        ADD_METHOD_TO(AdminExtraCtrl::logList, "/admin/api/sysLog/list", drogon::Get, "AdminAuthFilter");
        // 订单冻结/解冻
        ADD_METHOD_TO(AdminExtraCtrl::orderFreeze,   "/admin/api/order/freeze",   drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminExtraCtrl::orderUnfreeze, "/admin/api/order/unfreeze", drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    // ── 退款管理 ────────────────────────────────────────
    void refundList(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "order:view");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        std::string mchId = req->getParameter("mch_id");
        std::string state = req->getParameter("state");
        std::string keyword = req->getParameter("keyword");

        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        if (!state.empty()) { where += " AND state=?"; params.push_back(state); }
        if (!keyword.empty()) {
            where += " AND (refund_no LIKE ? OR order_id LIKE ?)";
            params.push_back("%" + keyword + "%");
            params.push_back("%" + keyword + "%");
        }
        auto cntR = db.query("SELECT COUNT(*) AS c FROM refund_order WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);
        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string((page - 1) * size));
        auto rows = db.query("SELECT * FROM refund_order WHERE " + where +
                              " ORDER BY id DESC LIMIT ? OFFSET ?", pp);
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

    void refundDetail(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "order:view");
        std::string no = req->getParameter("refund_no");
        auto r = PayDb::instance().queryOne("SELECT * FROM refund_order WHERE refund_no=?", {no});
        if (r.empty()) { RESP_ERR(cb, "退款单不存在"); return; }
        Json::Value data;
        for (auto &[k, v] : r) data[k] = v;
        data["state"] = std::stoi(r["state"]);
        RESP_OK(cb, data);
    }

    // 重新请求通道退款(对状态=0或-1的退款单)
    void refundRetry(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "order:refund");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string refundNo = (*body).get("refund_no", "").asString();
        auto &db = PayDb::instance();
        auto rf = db.queryOne(
            "SELECT * FROM refund_order WHERE refund_no=?", {refundNo});
        if (rf.empty()) { RESP_ERR(cb, "退款单不存在"); return; }
        if (rf["state"] == "1") { RESP_ERR(cb, "退款已成功，无需重试"); return; }

        // 重新调用通道退款
        auto order = db.queryOne(
            "SELECT channel_order_no,amount FROM pay_order WHERE order_id=?",
            {rf["order_id"]});
        auto ch = db.queryOne(
            "SELECT plugin,params_json FROM pay_channel WHERE id=?", {rf["channel_id"]});
        if (ch.empty()) { RESP_ERR(cb, "通道不存在"); return; }
        auto plugin = ChannelPluginRegistry::instance().create(ch["plugin"]);
        if (!plugin) { RESP_ERR(cb, "插件未安装"); return; }

        Json::Value cp; Json::Reader().parse(ch["params_json"], cp);
        ChannelRefundRequest rr;
        rr.refundNo       = refundNo;
        rr.channelOrderNo = order.empty() ? "" : order["channel_order_no"];
        rr.orderId        = rf["order_id"];
        try { rr.paidAmount = std::stod(order["amount"]); } catch(...){}
        try { rr.refundAmount = std::stod(rf["refund_amount"]); } catch(...){}
        rr.reason = rf["reason"];
        rr.notifyUrl = "/notify/refund/" + ch["plugin"];
        rr.channelParams = cp;

        auto result = plugin->refund(rr);
        long long now = std::time(nullptr);
        if (result.success) {
            db.exec("UPDATE refund_order SET state=?,channel_refund_no=?,err_msg='',finished_at=? WHERE refund_no=?",
                    {std::to_string(result.state == 1 ? 1 : 0),
                     result.channelRefundNo,
                     std::to_string(result.state == 1 ? now : 0), refundNo});
            OplogService::adminLog(req, "order", "refundRetry", refundNo, "");
            RESP_MSG(cb, "已重新发起退款");
        } else {
            db.exec("UPDATE refund_order SET err_msg=? WHERE refund_no=?",
                    {result.errMsg, refundNo});
            RESP_ERR(cb, "重试失败: " + result.errMsg);
        }
    }

    // 主动同步通道状态(轮询查询)
    void refundSync(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "order:refund");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string refundNo = (*body).get("refund_no", "").asString();
        auto &db = PayDb::instance();

        // 查询退款单
        auto rf = db.queryOne(
            "SELECT * FROM refund_order WHERE refund_no=?", {refundNo});
        if (rf.empty()) { RESP_ERR(cb, "退款单不存在"); return; }
        if (rf["state"] == "1") { RESP_MSG(cb, "退款已成功，无需同步"); return; }

        // 查询关联订单和通道
        auto order = db.queryOne(
            "SELECT channel_id,channel_order_no FROM pay_order WHERE order_id=?",
            {rf.count("order_id") ? rf.at("order_id") : ""});
        if (order.empty()) { RESP_ERR(cb, "关联订单不存在"); return; }

        std::string chId = order.count("channel_id") ? order.at("channel_id") : "0";
        auto ch = db.queryOne(
            "SELECT plugin,params_json FROM pay_channel WHERE id=?", {chId});
        if (ch.empty()) { RESP_ERR(cb, "通道不存在"); return; }

        auto plugin = ChannelPluginRegistry::instance().create(ch["plugin"]);
        if (!plugin) { RESP_ERR(cb, "插件未安装: " + ch["plugin"]); return; }

        Json::Value cp; Json::Reader().parse(ch["params_json"], cp);
        std::string channelRefundNo = rf.count("channel_refund_no") ? rf.at("channel_refund_no") : "";

        // 调用通道退款查询接口
        auto result = plugin->queryRefund(refundNo, channelRefundNo, cp);
        long long now = std::time(nullptr);

        if (result.success) {
            int newState = (result.state == 1) ? 1 : 0;
            std::string finished = (result.state == 1) ? std::to_string(now) : "0";
            if (!channelRefundNo.empty() && result.channelRefundNo.empty() == false) {
                db.exec("UPDATE refund_order SET state=?,channel_refund_no=?,err_msg='',finished_at=? WHERE refund_no=?",
                        {std::to_string(newState), result.channelRefundNo, finished, refundNo});
            } else {
                db.exec("UPDATE refund_order SET state=?,err_msg='',finished_at=? WHERE refund_no=?",
                        {std::to_string(newState), finished, refundNo});
            }
            LOG_INFO << "[AdminExtraCtrl] refundSync ok refundNo=" << refundNo
                     << " newState=" << newState;
            OplogService::adminLog(req, "order", "refundSync", refundNo,
                result.state == 1 ? "退款成功" : "退款处理中");
            RESP_MSG(cb, result.state == 1 ? "退款已成功" : "退款处理中，请稍后重试");
        } else {
            db.exec("UPDATE refund_order SET err_msg=? WHERE refund_no=?",
                    {result.errMsg, refundNo});
            RESP_ERR(cb, "同步失败: " + result.errMsg);
        }
    }

    // ── 分账组 ──────────────────────────────────────────
    void dgList(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:view");
        std::string mchId = req->getParameter("mch_id");
        auto &db = PayDb::instance();
        std::string where = "1=1"; std::vector<std::string> params;
        if (!mchId.empty()) { where += " AND mch_id=?"; params.push_back(mchId); }
        auto rows = db.query("SELECT * FROM division_receiver_group WHERE " + where +
                              " ORDER BY id DESC", params);
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["mch_id"] = std::stoi(r["mch_id"]);
            it["auto_div_flag"] = std::stoi(r["auto_div_flag"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void dgSave(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO division_receiver_group(mch_id,group_name,auto_div_flag,state,created_at) "
                    "VALUES(?,?,?,1,?)",
                    {j.get("mch_id", "0").asString(),
                     j.get("group_name", "").asString(),
                     std::to_string(j.get("auto_div_flag", 0).asInt()),
                     std::to_string(std::time(nullptr))});
        } else {
            db.exec("UPDATE division_receiver_group SET group_name=?,auto_div_flag=?,state=? WHERE id=?",
                    {j.get("group_name", "").asString(),
                     std::to_string(j.get("auto_div_flag", 0).asInt()),
                     std::to_string(j.get("state", 1).asInt()), id});
        }
        RESP_MSG(cb, "已保存");
    }

    void dgDel(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "division:manage");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM division_receiver_group WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // ── 二维码模板 ──────────────────────────────────────
    void qsList(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:view");
        auto rows = PayDb::instance().query(
            "SELECT * FROM qrcode_shell ORDER BY id DESC", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["qr_size"] = std::stoi(r["qr_size"]);
            it["qr_x"] = std::stoi(r["qr_x"]);
            it["qr_y"] = std::stoi(r["qr_y"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void qsSave(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            db.exec("INSERT INTO qrcode_shell(shell_name,bg_image,logo_image,qr_size,qr_x,qr_y,state,created_at) "
                    "VALUES(?,?,?,?,?,?,1,?)",
                    {j.get("shell_name", "").asString(),
                     j.get("bg_image", "").asString(),
                     j.get("logo_image", "").asString(),
                     std::to_string(j.get("qr_size", 300).asInt()),
                     std::to_string(j.get("qr_x", 0).asInt()),
                     std::to_string(j.get("qr_y", 0).asInt()),
                     std::to_string(std::time(nullptr))});
        } else {
            db.exec("UPDATE qrcode_shell SET shell_name=?,bg_image=?,logo_image=?,"
                    "qr_size=?,qr_x=?,qr_y=?,state=? WHERE id=?",
                    {j.get("shell_name", "").asString(),
                     j.get("bg_image", "").asString(),
                     j.get("logo_image", "").asString(),
                     std::to_string(j.get("qr_size", 300).asInt()),
                     std::to_string(j.get("qr_x", 0).asInt()),
                     std::to_string(j.get("qr_y", 0).asInt()),
                     std::to_string(j.get("state", 1).asInt()), id});
        }
        RESP_MSG(cb, "已保存");
    }

    void qsDel(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "merchant:edit");
        std::string id = req->getParameter("id");
        PayDb::instance().exec("DELETE FROM qrcode_shell WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // ── 用户团队 ──────────────────────────────────────────
    void utList(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        std::string ownerType = req->getParameter("owner_type");
        if (ownerType.empty()) ownerType = "1";
        auto rows = PayDb::instance().query(
            "SELECT * FROM sys_user_team WHERE owner_type=? ORDER BY id DESC", {ownerType});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"] = std::stoi(r["id"]);
            it["leader_user_id"] = std::stoi(r["leader_user_id"]);
            it["owner_type"] = std::stoi(r["owner_type"]);
            it["state"] = std::stoi(r["state"]);
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void utSave(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        auto &j = *body;
        std::string id = j.get("id", "").asString();
        long long now = std::time(nullptr);
        auto &db = PayDb::instance();
        if (id.empty() || id == "0") {
            std::string teamNo = generateTeamNo();
            db.exec("INSERT INTO sys_user_team(team_no,team_name,leader_user_id,owner_type,"
                    "owner_id,remark,state,created_at,updated_at) VALUES(?,?,?,?,?,?,1,?,?)",
                    {teamNo,
                     j.get("team_name", "").asString(),
                     std::to_string(j.get("leader_user_id", 0).asInt()),
                     std::to_string(j.get("owner_type", 1).asInt()),
                     j.get("owner_id", "").asString(),
                     j.get("remark", "").asString(),
                     std::to_string(now), std::to_string(now)});
        } else {
            db.exec("UPDATE sys_user_team SET team_name=?,leader_user_id=?,remark=?,state=?,updated_at=? "
                    "WHERE id=?",
                    {j.get("team_name", "").asString(),
                     std::to_string(j.get("leader_user_id", 0).asInt()),
                     j.get("remark", "").asString(),
                     std::to_string(j.get("state", 1).asInt()),
                     std::to_string(now), id});
        }
        RESP_MSG(cb, "已保存");
    }

    void utDel(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "sysuser:manage");
        std::string id = req->getParameter("id");
        // 防级联删除: 如果有 user.team_id 关联则禁止
        auto bound = PayDb::instance().queryOne(
            "SELECT id FROM sys_user WHERE team_id=? LIMIT 1", {id});
        if (!bound.empty()) { RESP_ERR(cb, "团队下还有成员，无法删除"); return; }
        PayDb::instance().exec("DELETE FROM sys_user_team WHERE id=?", {id});
        RESP_MSG(cb, "已删除");
    }

    // ── 操作日志 ──────────────────────────────────────────
    void logList(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "oplog:view");
        int page = pi(req->getParameter("page"), 1);
        int size = pi(req->getParameter("size"), 20);
        std::string module = req->getParameter("module");
        std::string operatorId = req->getParameter("operator_id");
        Json::Value data = OplogService::queryLogs(page, size, module, operatorId);
        RESP_OK(cb, data);
    }

    // ── 订单冻结 ──────────────────────────────────────────
    // POST /admin/api/order/freeze   body: {order_id, reason}
    // state: 1(已支付) → 3(已冻结), 同时冻结商户对应金额
    void orderFreeze(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "order:edit");
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string orderId = (*j)["order_id"].asString();
        std::string reason  = (*j).get("reason", "管理员冻结").asString();

        auto &db = PayDb::instance();
        auto order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (order.empty()) { RESP_ERR(cb, "订单不存在"); return; }
        if (order["state"] != "1") { RESP_ERR(cb, "只有已支付订单可冻结"); return; }

        int mchId = std::stoi(order["mch_id"]);
        double amount = 0;
        try { amount = std::stod(order["amount"]); } catch (...) {}
        double fee = 0;
        try { fee = std::stod(order.count("mch_fee_amount") ? order["mch_fee_amount"] : "0"); } catch (...) {}
        double mchAmount = amount - fee;

        // 冻结商户余额
        if (!ChannelService::changeMchBalance(mchId, 3, mchAmount, "freeze", orderId, reason)) {
            RESP_ERR(cb, "商户余额不足，无法冻结"); return;
        }

        long long now = std::time(nullptr);
        db.exec("UPDATE pay_order SET state=3,remark=?,updated_at=? WHERE order_id=?",
                {reason, std::to_string(now), orderId});

        OplogService::adminLog(req, OplogService::MOD_ORDER, "freeze", orderId, reason);
        RESP_MSG(cb, "订单已冻结");
    }

    // ── 订单解冻 ──────────────────────────────────────────
    // POST /admin/api/order/unfreeze   body: {order_id}
    // state: 3(已冻结) → 1(已支付), 解冻商户余额
    void orderUnfreeze(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "order:edit");
        auto j = req->getJsonObject();
        if (!j) { RESP_ERR(cb, "参数错误"); return; }
        std::string orderId = (*j)["order_id"].asString();

        auto &db = PayDb::instance();
        auto order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (order.empty()) { RESP_ERR(cb, "订单不存在"); return; }
        if (order["state"] != "3") { RESP_ERR(cb, "只有已冻结订单可解冻"); return; }

        int mchId = std::stoi(order["mch_id"]);
        double amount = 0;
        try { amount = std::stod(order["amount"]); } catch (...) {}
        double fee = 0;
        try { fee = std::stod(order.count("mch_fee_amount") ? order["mch_fee_amount"] : "0"); } catch (...) {}
        double mchAmount = amount - fee;

        // 解冻商户余额
        ChannelService::changeMchBalance(mchId, 4, mchAmount, "unfreeze", orderId, "解冻订单");

        long long now = std::time(nullptr);
        db.exec("UPDATE pay_order SET state=1,remark='',updated_at=? WHERE order_id=?",
                {std::to_string(now), orderId});

        OplogService::adminLog(req, OplogService::MOD_ORDER, "unfreeze", orderId, "");
        RESP_MSG(cb, "订单已解冻");
    }

private:
    static int pi(const std::string &s, int def) {
        try { return std::stoi(s); } catch(...) { return def; }
    }
    static std::string generateTeamNo() {
        static std::mt19937 rng((unsigned)std::random_device{}());
        char buf[16];
        std::snprintf(buf, sizeof(buf), "T%lld%03d",
            (long long)std::time(nullptr), (int)(rng() % 1000));
        return buf;
    }
};
