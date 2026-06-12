// WePay-Cpp — 插件市场
// 用户在后台"插件市场"页面：
//   - 浏览所有可用插件(内置 + 已导入的外置)
//   - 安装/卸载 (免费)
//   - 导入自备 .dll 外置插件
//
// GET    /admin/api/plugin/list      插件市场列表
// GET    /admin/api/plugin/installed 已安装插件列表
// POST   /admin/api/plugin/install   安装
// POST   /admin/api/plugin/uninstall 卸载
// POST   /admin/api/plugin/import    导入外置插件(表单上传.dll)
// DELETE /admin/api/plugin/del       删除外置插件记录
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include <filesystem>
#include <random>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/PermCheck.h"
#include "../common/OplogService.h"
#include "../common/Md5Utils.h"
#include "../common/ChannelService.h"
#include "../filters/AdminAuthFilter.h"
#include "../common/SmtpUtils.h"
#include "ChannelPlugin.h"

class PluginMarketCtrl : public drogon::HttpController<PluginMarketCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(PluginMarketCtrl::list,      "/admin/api/plugin/list",      drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::installed, "/admin/api/plugin/installed", drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::install,   "/admin/api/plugin/install",   drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::uninstall, "/admin/api/plugin/uninstall", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::importPlg, "/admin/api/plugin/import",    drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::del,       "/admin/api/plugin/del",       drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::apiMeta,   "/admin/api/plugin/apiMeta",   drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::schema,     "/admin/api/plugin/schema",     drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::testOrder,  "/admin/api/plugin/testOrder",  drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::saveConfig, "/admin/api/plugin/saveConfig", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(PluginMarketCtrl::getConfig,  "/admin/api/plugin/getConfig",  drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        auto rows = PayDb::instance().query(
            "SELECT * FROM plugin_store WHERE state=1 ORDER BY plugin_type,id", {});
        Json::Value arr(Json::arrayValue);
        auto &reg = ChannelPluginRegistry::instance();
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            it["id"]          = std::stoi(r["id"]);
            it["plugin_type"] = std::stoi(r["plugin_type"]);
            it["installed"]   = std::stoi(r["installed"]);
            it["state"]       = std::stoi(r["state"]);
            it["price"]       = std::stoi(r["price"]);
            // 是否已编译(用于区分"可安装"和"占位未实现")
            // wepay_v3 是 Drogon 原生插件，不注册到 ChannelPluginRegistry，特判
            bool builtIn = reg.hasBuiltIn(r["plugin_code"]);
#ifdef WEPAY_HAS_V3
            if (r["plugin_code"] == "wepay_v3") builtIn = true;
#endif
            it["has_builtin"] = builtIn;
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void installed(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        auto rows = PayDb::instance().query(
            "SELECT plugin_code,display_name,icon,supported_ways,version FROM plugin_store "
            "WHERE installed=1 AND state=1 ORDER BY display_name", {});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value it;
            for (auto &[k, v] : r) it[k] = v;
            arr.append(it);
        }
        RESP_OK(cb, arr);
    }

    void install(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string code = (*body).get("plugin_code", "").asString();
        if (code.empty()) { RESP_ERR(cb, "plugin_code 必填"); return; }

        auto &db = PayDb::instance();
        auto p = db.queryOne("SELECT plugin_code,plugin_type FROM plugin_store WHERE plugin_code=?", {code});
        if (p.empty()) { RESP_ERR(cb, "插件不存在"); return; }

        auto &reg = ChannelPluginRegistry::instance();
        // 内置插件必须已编译；wepay_v3 是 Drogon 原生插件，跳过 ChannelPluginRegistry 检查
        bool isMonitorPlugin = (code == "wepay_v3" || code == "wepay");
        if (p["plugin_type"] == "1" && !isMonitorPlugin && !reg.hasBuiltIn(code)) {
            RESP_ERR(cb, "该插件未编译入系统，请使用支持该插件的版本"); return;
        }

        db.exec("UPDATE plugin_store SET installed=1,installed_at=? WHERE plugin_code=?",
                {std::to_string(std::time(nullptr)), code});
        reg.markInstalled(code, true);
        OplogService::adminLog(req, "channel", "pluginInstall", code, "");
        RESP_MSG(cb, "已安装: " + code);
    }

    void uninstall(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string code = (*body).get("plugin_code", "").asString();

        auto &db = PayDb::instance();
        // 统计该插件下通道数量 (供提示)
        auto cntRow = db.queryOne(
            "SELECT COUNT(*) AS c FROM pay_channel WHERE plugin=? AND state=1", {code});
        int affectedChannels = 0;
        try { affectedChannels = std::stoi(cntRow["c"]); } catch (...) {}

        // 停用该插件所有通道 + 所有商户绑定 (保留记录便于重新安装恢复)
        db.exec("UPDATE pay_channel SET state=0 WHERE plugin=?", {code});
        db.exec(
            "UPDATE merchant_channel SET state=0 "
            "WHERE channel_id IN (SELECT id FROM pay_channel WHERE plugin=?)", {code});

        db.exec("UPDATE plugin_store SET installed=0,installed_at=0 WHERE plugin_code=?", {code});
        ChannelPluginRegistry::instance().markInstalled(code, false);
        OplogService::adminLog(req, "channel", "pluginUninstall", code,
                               std::to_string(affectedChannels));
        std::string msg = "已卸载: " + code;
        if (affectedChannels > 0) msg += "（同时停用 " + std::to_string(affectedChannels) + " 个通道）";
        RESP_MSG(cb, msg);
    }

    // 导入自备 .dll 外置插件
    // 请求格式: multipart/form-data, 字段 file=插件.dll, plugin_code, display_name等
    void importPlg(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");

        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0) { RESP_ERR(cb, "表单解析失败"); return; }

        auto &files = parser.getFiles();
        auto &params = parser.getParameters();
        if (files.empty()) { RESP_ERR(cb, "未上传文件"); return; }

        std::string pluginCode = params.count("plugin_code") ? params.at("plugin_code") : "";
        std::string displayName = params.count("display_name") ? params.at("display_name") : "";
        if (pluginCode.empty() || displayName.empty()) {
            RESP_ERR(cb, "plugin_code 和 display_name 必填"); return;
        }

        // 保存到 plugins 目录
        std::filesystem::create_directories("plugins");
        std::string libPath = "plugins/" + pluginCode + ".dll";
        try {
            files[0].saveAs(libPath);
        } catch (const std::exception &e) {
            RESP_ERR(cb, std::string("保存失败: ") + e.what()); return;
        }

        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        db.exec("INSERT OR REPLACE INTO plugin_store(plugin_code,display_name,description,"
                "vendor,version,plugin_type,supported_ways,lib_path,installed,state,created_at) "
                "VALUES(?,?,?,?,?,2,?,?,0,1,?)",
                {pluginCode, displayName,
                 params.count("description") ? params.at("description") : "",
                 params.count("vendor") ? params.at("vendor") : "用户自备",
                 params.count("version") ? params.at("version") : "1.0.0",
                 params.count("supported_ways") ? params.at("supported_ways") : "",
                 libPath, std::to_string(now)});

        OplogService::adminLog(req, "channel", "pluginImport", pluginCode, displayName);
        Json::Value data;
        data["plugin_code"] = pluginCode;
        data["lib_path"] = libPath;
        data["hint"] = "外置.dll 插件已保存到 plugins 目录。当前版本暂未加载 .dll 动态模块，"
                       "后续版本将支持 C API 热加载。建议先使用内置插件。";
        RESP_OK(cb, data);
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        std::string code = req->getParameter("plugin_code");
        auto &db = PayDb::instance();
        auto p = db.queryOne("SELECT plugin_type,lib_path FROM plugin_store WHERE plugin_code=?", {code});
        if (p.empty()) { RESP_ERR(cb, "插件不存在"); return; }
        if (p["plugin_type"] != "2") { RESP_ERR(cb, "内置插件不可删除，只能卸载"); return; }
        // 外置插件: 删除记录 + 文件
        if (!p["lib_path"].empty()) {
            try { std::filesystem::remove(p["lib_path"]); } catch (...) {}
        }
        db.exec("DELETE FROM plugin_store WHERE plugin_code=?", {code});
        ChannelPluginRegistry::instance().markInstalled(code, false);
        OplogService::adminLog(req, "channel", "pluginDel", code, "");
        RESP_MSG(cb, "已删除");
    }

    void apiMeta(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string code = req->getParameter("plugin_code");
        if (code.empty()) { RESP_ERR(cb, "plugin_code 必填"); return; }
        std::string base = PayDb::instance().getSetting("site_url", "");
        if (base.empty()) base = requestBaseUrl(req);
        Json::Value rows(Json::arrayValue);
        addApi(rows, "统一下单", "POST", base + "/gateway/create", "测试订单和正式订单统一入口");
        addApi(rows, "统一查询", "POST", base + "/gateway/query", "统一订单查询");
        addApi(rows, "插件回调", "POST/GET", base + "/notify/channel/" + code, "当前插件专属异步通知入口");
        if (code == "vmq") {
            addApi(rows, "V免签心跳", "POST/GET", base + "/appHeart", "V免签监听App心跳");
            addApi(rows, "V免签推送", "POST/GET", base + "/appPush", "V免签监听App收款推送");
            addApi(rows, "新版心跳", "POST", base + "/api/monitor/heart", "新版监听端心跳");
            addApi(rows, "新版推送", "POST", base + "/api/monitor/push", "新版监听端收款推送");
        } else if (code == "wepay") {
            addApi(rows, "🔒 WePay心跳", "POST", base + "/api/wepay/heart",
                   "HMAC-SHA256签名, JSON: {t,device_id,nonce,sign}");
            addApi(rows, "🔒 WePay推送", "POST", base + "/api/wepay/push",
                   "精确order_id匹配, JSON: {t,type,price,order_id?,device_id,nonce,sign}");
            addApi(rows, "🔒 待支付列表", "POST", base + "/api/wepay/pending",
                   "拉取待支付订单列表, JSON: {t,device_id,nonce,sign}");
            addApi(rows, "设备管理", "GET", base + "/admin/api/wepay/devices",
                   "查看已注册监控设备列表（需管理员JWT）");
            addApi(rows, "兼容心跳", "POST", base + "/api/monitor/heart",
                   "兼容旧版MD5签名心跳");
            addApi(rows, "兼容推送", "POST", base + "/api/monitor/push",
                   "兼容旧版MD5签名推送");
        } else if (code == "codepay") {
            addApi(rows, "码支付取单", "GET",
                   base + "/checkOrder/{pid}/{sign}",
                   "pid=mch_no, sign=md5(pid+mch_key); pid=0/platform 走平台密钥");
            addApi(rows, "码支付确认", "GET/POST",
                   base + "/checkPayResult?pid=&price=&type=",
                   "type=wxpay/alipay/qqpay; 仅匹配 codepay 通道订单");
            addApi(rows, "SmsForwarder", "POST",
                   base + "/mpayNotify",
                   "短信转发器: action=mpay/mpaypc, data={pid,price,payway}");
        } else if (code == "epay_upstream") {
            addApi(rows, "易支付上游通知", "POST/GET", base + "/notify/channel/" + code, "易支付上游异步通知");
        } else if (code == "wepay_v3") {
            addApi(rows, "设备注册",   "POST", base + "/api/wepay/v3/device/register", "安卓 App 首次启动时注册设备");
            addApi(rows, "设备解绑",   "POST", base + "/api/wepay/v3/device/unbind",   "解除设备与商户绑定");
            addApi(rows, "设备信息",   "GET",  base + "/api/wepay/v3/device/info",     "查询设备在线状态");
            addApi(rows, "心跳上报",   "POST", base + "/api/wepay/v3/heart",           "安卓 HeartbeatService 每30s上报");
            addApi(rows, "通知上报",   "POST", base + "/api/wepay/v3/notify",          "安卓通知栏解析后上报，触发订单匹配");
            addApi(rows, "待推送订单", "GET",  base + "/api/wepay/v3/device/pending",  "设备拉取未处理订单列表");
            std::string wsBase = base; if (wsBase.substr(0,5) == "https") wsBase = "wss" + wsBase.substr(5); else if (wsBase.substr(0,4) == "http") wsBase = "ws" + wsBase.substr(4);
            addApi(rows, "WebSocket",  "WS",   wsBase + "/api/wepay/v3/ws",            "实时推送订单到设备 / 面板");
            addApi(rows, "手动回调",   "POST", base + "/admin/api/v3/order/callback",  "管理员手动触发商户回调");
        }
        Json::Value data;
        data["plugin_code"] = code;
        data["base_url"] = base;
        data["rows"] = rows;
        RESP_OK(cb, data);
    }

    // 返回插件参数 schema (供通道管理页动态渲染表单)
    void schema(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string code = req->getParameter("plugin_code");
        if (code.empty()) { RESP_ERR(cb, "缺少 plugin_code"); return; }
        auto plg = ChannelPluginRegistry::instance().createRaw(code);
        if (!plg) { RESP_ERR(cb, "插件不存在"); return; }
        Json::Value data;
        data["plugin_code"] = code;
        data["fields"] = plg->paramSchema();
        RESP_OK(cb, data);
    }

    // 管理员快速创建测试订单 -> 返回收银台 URL
    void testOrder(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "参数错误"); return; }
        int mchId = (*body).get("mch_id", 0).asInt();
        std::string payType = (*body).get("pay_type", "wxpay").asString();
        std::string amount = (*body).get("amount", "0.01").asString();
        std::string subject = (*body).get("subject", "管理员测试订单").asString();
        std::string notifyUrl = (*body).get("notify_url", "").asString();
        std::string returnUrl = (*body).get("return_url", "").asString();
        std::string notifyEmail = (*body).get("notify_email", "").asString();
        auto &db = PayDb::instance();
        PayDb::Row mch;
        if (mchId > 0) mch = db.queryOne("SELECT * FROM merchant WHERE id=?", {std::to_string(mchId)});
        else mch = db.queryOne("SELECT * FROM merchant WHERE state=1 ORDER BY id ASC LIMIT 1", {});
        if (mch.empty()) { RESP_ERR(cb, "没有可用商户，请先创建一个商户"); return; }
        std::string mchNo = mch["mch_no"];
        std::string mchKey = mch["mch_key"];
        std::string pluginCode = (*body).get("plugin_code", "vmq").asString();
        // 如果该 pay_type 没有任何 state=1 通道, 自动为当前插件创建一条并绑定商户
        ensureDefaultChannel(db, std::stoi(mch["id"]), pluginCode, payType);
        std::mt19937_64 rng((unsigned long long)std::time(nullptr));
        char outNo[40]; std::snprintf(outNo, sizeof(outNo), "TEST%lld%06u", (long long)std::time(nullptr), (unsigned)(rng() % 1000000));
        // 构造与 /gateway/create 一致的签名字符串
        std::map<std::string, std::string> params = {
            {"mch_id", mchNo}, {"out_trade_no", outNo}, {"pay_type", payType},
            {"amount", amount}, {"subject", subject},
            {"notify_url", notifyUrl}, {"return_url", returnUrl},
            {"notify_email", notifyEmail}
        };
        std::string src;
        for (auto &kv : params) if (!kv.second.empty()) { if (!src.empty()) src += "&"; src += kv.first + "=" + kv.second; }
        src += mchKey;
        std::string sign = Md5Utils::md5(src);
        Json::Value data;
        data["gateway_url"] = "/gateway/create";
        data["mch_id"] = mchNo;
        data["out_trade_no"] = outNo;
        data["pay_type"] = payType;
        data["amount"] = amount;
        data["subject"] = subject;
        data["notify_url"] = notifyUrl;
        data["return_url"] = returnUrl;
        data["notify_email"] = notifyEmail;
        data["sign"] = sign;
        data["hint"] = "POST 上述参数到 /gateway/create 即可完成下单，或使用下方 cashier_url 直接打开收银台预览";
        // 若填了通知邮箱，立即发一封测试通知邮件（验证 SMTP 配置）
        if (!notifyEmail.empty() && SmtpUtils::instance().isConfigured()) {
            std::string mailBody =
                "订单号: " + std::string(outNo) + "\n" +
                "支付类型: " + payType + "\n" +
                "金额: " + amount + " 元\n" +
                "商户: " + mchNo + "\n" +
                "说明: 这是一封测试下单通知邮件，订单已成功创建，等待支付。";
            SmtpUtils::instance().send(notifyEmail, "[WePay] 测试订单已创建 " + std::string(outNo), mailBody);
        }
        RESP_OK(cb, data);
    }

private:
    // 若 pluginCode + payType 没有任何启用通道, 自动创建一个并绑定到 mchId
    static void ensureDefaultChannel(PayDb &db, int mchId, const std::string &pluginCode,
                                     const std::string &payType) {
        auto row = db.queryOne(
            "SELECT id FROM pay_channel WHERE plugin=? AND pay_type=? AND state=1 LIMIT 1",
            {pluginCode, payType});
        std::string channelId;
        if (!row.empty()) {
            channelId = row["id"];
        } else {
            // 创建默认通道
            std::string code = pluginCode + "_" + payType + "_default";
            std::string name = pluginCode + " 默认 " + payType;
            long long now = std::time(nullptr);
            db.exec(
                "INSERT INTO pay_channel(channel_code,channel_name,pay_type,plugin,rate,"
                "params_json,min_amount,max_amount,state,sort_order,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,1,-1,?,?)",
                {code, name, payType, pluginCode, "1.00", "{}", "0.01", "50000",
                 std::to_string(now), std::to_string(now)});
            auto created = db.queryOne(
                "SELECT id FROM pay_channel WHERE channel_code=?", {code});
            if (created.empty()) return;
            channelId = created["id"];
        }
        long long now = std::time(nullptr);
        // 1) 把该商户在同 pay_type 下指向其他插件的绑定禁用，避免抢单
        db.exec(
            "UPDATE merchant_channel SET state=0 "
            "WHERE mch_id=? AND channel_id IN ("
            "  SELECT id FROM pay_channel WHERE pay_type=? AND plugin<>?"
            ")",
            {std::to_string(mchId), payType, pluginCode});
        // 2) 确保对当前插件通道的绑定存在并启用
        auto bind = db.queryOne(
            "SELECT id FROM merchant_channel WHERE mch_id=? AND channel_id=?",
            {std::to_string(mchId), channelId});
        if (bind.empty()) {
            db.exec(
                "INSERT INTO merchant_channel(mch_id,channel_id,rate,state) "
                "VALUES(?,?,?,1)",
                {std::to_string(mchId), channelId, ""});
        } else {
            db.exec("UPDATE merchant_channel SET state=1 WHERE id=?", {bind["id"]});
        }
        (void)now;
    }

    static void addApi(Json::Value &rows, const std::string &name, const std::string &method,
                       const std::string &url, const std::string &remark) {
        Json::Value row;
        row["name"] = name;
        row["method"] = method;
        row["url"] = url;
        row["remark"] = remark;
        rows.append(row);
    }
    static std::string requestBaseUrl(const drogon::HttpRequestPtr &req) {
        std::string host = req->getHeader("host");
        if (host.empty()) host = "127.0.0.1";
        std::string proto = req->getHeader("x-forwarded-proto");
        if (proto.empty()) proto = "http";
        return proto + "://" + host;
    }

public:
    void saveConfig(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:edit");
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体无效"); return; }
        std::string pluginCode = (*body).get("plugin_code", "").asString();
        std::string params     = (*body).get("params_json", "{}").asString();
        if (pluginCode.empty()) { RESP_ERR(cb, "plugin_code 不能为空"); return; }
        Json::Value test;
        if (!Json::Reader().parse(params, test)) { RESP_ERR(cb, "params_json 格式错误"); return; }
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT id FROM plugin_store WHERE plugin_code=?", {pluginCode});
        if (row.empty()) { RESP_ERR(cb, "插件不存在"); return; }
        db.exec("UPDATE plugin_store SET default_params=? WHERE plugin_code=?", {params, pluginCode});
        RESP_MSG(cb, "已保存");
    }

    void getConfig(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        REQUIRE_PERM(cb, "channel:view");
        std::string pluginCode = req->getParameter("plugin_code");
        if (pluginCode.empty()) { RESP_ERR(cb, "plugin_code 不能为空"); return; }
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT default_params FROM plugin_store WHERE plugin_code=?", {pluginCode});
        if (row.empty()) { RESP_ERR(cb, "插件不存在"); return; }
        std::string raw = row.count("default_params") ? row["default_params"] : "{}";
        Json::Value data;
        if (!Json::Reader().parse(raw, data)) data = Json::objectValue;
        Json::Value j; j["code"] = 0; j["data"] = data;
        cb(drogon::HttpResponse::newHttpJsonResponse(j));
    }
};
