// WePay-Cpp — 管理后台: 插件管理控制器
// 职责：插件的列表、上传、热重载、卸载、删除等插件管理功能
//
// API 端点：
// GET  /admin/api/plugin/list     列出所有已加载插件
// POST /admin/api/plugin/upload   上传 zip 包并自动加载
// POST /admin/api/plugin/reload   热重载指定插件
// POST /admin/api/plugin/unload   卸载指定插件
// POST /admin/api/plugin/delete   卸载并删除插件文件
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <filesystem> // 文件系统库
#include <fstream> // 文件流库
#include "../common/AjaxResult.h"
#include "../filters/AdminAuthFilter.h"
#include "../plugin/PluginManager.h"

class PluginCtrl : public drogon::HttpController<PluginCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(PluginCtrl::list,   "/admin/api/plugin/list",   drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(PluginCtrl::upload, "/admin/api/plugin/upload", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(PluginCtrl::reload, "/admin/api/plugin/reload", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(PluginCtrl::unload, "/admin/api/plugin/unload", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(PluginCtrl::remove,  "/admin/api/plugin/delete",   drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    // GET /admin/api/plugin/list
    void list(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb)
    {
        auto& pm = PluginManager::instance();
        Json::Value arr(Json::arrayValue);
        for (auto& p : pm.list()) {
            Json::Value item;
            item["name"]        = p.meta.name;
            item["version"]     = p.meta.version;
            item["author"]      = p.meta.author;
            item["description"] = p.meta.description;
            item["enabled"]     = p.enabled;
            arr.append(item);
        }
        RESP_OK(cb, arr);
    }

    // POST /admin/api/plugin/upload  (multipart/form-data, 字段名 file)
    void upload(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
    {
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().empty()) {
            RESP_ERR(cb, "请上传 zip 文件"); return;
        }

        auto& file = parser.getFiles()[0];
        std::string fname = file.getFileName();
        // 只允许 .zip
        if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".zip") {
            RESP_ERR(cb, "只支持 .zip 格式"); return;
        }
        // 限制 50MB
        if (file.fileLength() > 50 * 1024 * 1024) {
            RESP_ERR(cb, "插件包不能超过 50MB"); return;
        }

        auto& pm = PluginManager::instance();
        std::string pluginsDir = pm.pluginsDir();
        std::filesystem::create_directories(pluginsDir);

        // 保存 zip
        std::string zipPath = pluginsDir + "/" + fname;
        file.saveAs(zipPath);

        // 解压 zip 到 plugins/ 目录
        std::string cmd;
#ifdef _WIN32
        // Windows 10+ 自带 tar 支持 zip
        cmd = "tar -xf \"" + zipPath + "\" -C \"" + pluginsDir + "\"";
#else
        cmd = "unzip -o \"" + zipPath + "\" -d \"" + pluginsDir + "\"";
#endif
        int ret = std::system(cmd.c_str());
        std::filesystem::remove(zipPath);  // 删除 zip

        if (ret != 0) {
            RESP_ERR(cb, "解压失败，请确保 zip 格式正确"); return;
        }

        // 从文件名推断插件名（去掉 .zip 和版本号，如 alipay-1.0.0.zip → alipay）
        std::string pluginName = fname.substr(0, fname.size() - 4);
        auto dashPos = pluginName.find('-');
        if (dashPos != std::string::npos) pluginName = pluginName.substr(0, dashPos);

        // 如果已加载则先卸载再重新加载
        if (pm.isLoaded(pluginName)) pm.unload(pluginName);
        bool ok = pm.load(pluginName);
        if (!ok) {
            RESP_ERR(cb, "插件解压成功但加载失败，请检查 .dll/.so 文件是否存在"); return;
        }

        Json::Value data;
        data["name"] = pluginName;
        data["msg"]  = "插件导入成功，已自动加载";
        RESP_OK(cb, data);
    }

    // POST /admin/api/plugin/reload  body: {"name":"xxx"}
    void reload(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
    {
        auto body = req->getJsonObject();
        if (!body || !(*body).isMember("name")) { RESP_ERR(cb, "name 必填"); return; }
        std::string name = (*body)["name"].asString();

        bool ok = PluginManager::instance().reload(name);
        ok ? RESP_MSG(cb, "热重载成功") : RESP_ERR(cb, "重载失败");
    }

    // POST /admin/api/plugin/unload  body: {"name":"xxx"}
    void unload(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
    {
        auto body = req->getJsonObject();
        if (!body || !(*body).isMember("name")) { RESP_ERR(cb, "name 必填"); return; }
        std::string name = (*body)["name"].asString();

        bool ok = PluginManager::instance().unload(name);
        ok ? RESP_MSG(cb, "已卸载") : RESP_ERR(cb, "插件不存在");
    }

    // POST /admin/api/plugin/delete  body: {"name":"xxx"}
    void remove(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
    {
        auto body = req->getJsonObject();
        if (!body || !(*body).isMember("name")) { RESP_ERR(cb, "name 必填"); return; }
        std::string name = (*body)["name"].asString();

        // 安全检查，禁止路径穿越
        if (name.find("..") != std::string::npos || name.find('/') != std::string::npos) {
            RESP_ERR(cb, "非法插件名"); return;
        }

        PluginManager::instance().unload(name);

        // 删除插件目录
        std::string dir = PluginManager::instance().pluginsDir() + "/" + name;
        if (std::filesystem::exists(dir)) {
            std::filesystem::remove_all(dir);
        }
        RESP_MSG(cb, "已删除");
    }

    // GET /admin/api/plugin/apiMeta?plugin_code=xxx
    // 返回当前插件的接口列表，URL 由后端用请求 Host 自动生成
    void apiMeta(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb)
    {
        std::string pluginCode = req->getParameter("plugin_code");

        // 从请求头推断后端 base URL
        std::string proto = req->getHeader("x-forwarded-proto");
        if (proto.empty()) proto = "http";
        std::string host = req->getHeader("x-forwarded-host");
        if (host.empty()) host = req->getHeader("host");
        if (host.empty()) host = "127.0.0.1:8888";
        std::string base = proto + "://" + host;

        Json::Value rows(Json::arrayValue);

        // 通用接口（所有插件共享）
        auto addRow = [&](const std::string& name, const std::string& method,
                          const std::string& url, const std::string& remark) {
            Json::Value r;
            r["name"] = name; r["method"] = method;
            r["url"]  = url;  r["remark"] = remark;
            rows.append(r);
        };
        addRow("统一下单",  "POST", base + "/gateway/create",    "测试订单和正式订单统一入口");
        addRow("统一查询",  "POST", base + "/gateway/query",     "统一订单查询");
        addRow("插件回调",  "POST/GET", base + "/notify/channel/" + pluginCode, "当前插件专属异步通知入口");

        // wepay_v3 设备/安卓监听专属接口
        if (pluginCode == "wepay_v3") {
            addRow("设备注册",   "POST", base + "/api/wepay/v3/device/register", "安卓 App 首次启动时注册设备");
            addRow("设备解绑",   "POST", base + "/api/wepay/v3/device/unbind",   "解除设备与商户绑定");
            addRow("设备信息",   "GET",  base + "/api/wepay/v3/device/info",     "查询设备在线状态");
            addRow("心跳上报",   "POST", base + "/api/wepay/v3/heart",           "安卓 HeartbeatService 每30s上报");
            addRow("通知上报",   "POST", base + "/api/wepay/v3/notify",          "安卓通知栏解析后上报，触发订单匹配");
            addRow("待推送订单", "GET",  base + "/api/wepay/v3/device/pending",  "设备拉取未处理订单列表");
            addRow("WebSocket",  "WS",
                   (proto == "https" ? "wss" : "ws") + std::string("://") + host + "/api/wepay/v3/ws",
                   "实时推送订单到设备 / 面板");
            addRow("手动回调",   "POST", base + "/admin/api/v3/order/callback",  "管理员手动触发商户回调");
        }

        Json::Value data;
        data["rows"] = rows;
        data["debug_plugin_code"] = pluginCode;
        RESP_OK(cb, data);
    }
};
