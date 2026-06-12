// WePay-Cpp — 管理后台: 监听端 APK 分发控制器
// 职责：APK 的上传、列表、删除、公开下载等 APK 分发功能
//
// API 端点：
// POST /admin/api/monitor/uploadApk      管理员上传 APK
// GET  /admin/api/monitor/apkList         查看已上传的 APK
// DELETE /admin/api/monitor/apk/{plugin}  删除某插件 APK
// GET  /m/apk/{plugin_code}               公开下载 APK (浏览器直接打开会触发下载)
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <drogon/MultiPart.h> // Drogon 多部分表单
#include <filesystem> // 文件系统库
#include <fstream> // 文件流库
#include <ctime>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../filters/AdminAuthFilter.h"

class MonitorApkCtrl : public drogon::HttpController<MonitorApkCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(MonitorApkCtrl::upload,    "/admin/api/monitor/uploadApk",   drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(MonitorApkCtrl::list,      "/admin/api/monitor/apkList",     drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(MonitorApkCtrl::remove,    "/admin/api/monitor/apk/{plugin}",drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(MonitorApkCtrl::download,  "/m/apk/{plugin}",                drogon::Get);
    METHOD_LIST_END

    static const std::filesystem::path &storeDir() {
        static std::filesystem::path d = std::filesystem::path("upload") / "monitor_apk";
        std::error_code ec; std::filesystem::create_directories(d, ec);
        return d;
    }

    void upload(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0) { RESP_ERR(cb, "表单解析失败"); return; }
        std::string plugin = req->getParameter("plugin");
        if (plugin.empty()) plugin = "vmq";
        if (plugin != "vmq" && plugin != "codepay" && plugin != "mpay") {
            RESP_ERR(cb, "不支持的插件类型"); return;
        }
        auto &files = parser.getFiles();
        if (files.empty()) { RESP_ERR(cb, "没有上传文件"); return; }
        const auto &f = files[0];
        // 简单格式校验
        std::string fname = f.getFileName();
        std::string lower = fname;
        for (auto &c : lower) c = std::tolower((unsigned char)c);
        if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".apk") {
            RESP_ERR(cb, "请上传 .apk 文件"); return;
        }
        std::filesystem::path target = storeDir() / (plugin + ".apk");
        try {
            f.saveAs(target.string());
        } catch (const std::exception &e) {
            RESP_ERR(cb, std::string("保存失败: ") + e.what()); return;
        }
        // 同步更新 download_url 设置
        auto &db = PayDb::instance();
        std::string url = "/m/apk/" + plugin;
        db.setSetting("monitor_android_download_url_" + plugin, url);
        if (plugin == "vmq")
            db.setSetting("monitor_android_download_url", url); // 兼容旧字段
        Json::Value data;
        data["plugin"] = plugin;
        data["file_name"] = fname;
        data["size"] = (Json::Int64)f.fileLength();
        data["download_url"] = url;
        RESP_OK(cb, data);
    }

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        Json::Value rows(Json::arrayValue);
        for (const std::string &plg : {"vmq", "codepay"}) {
            std::filesystem::path p = storeDir() / (plg + ".apk");
            if (!std::filesystem::exists(p)) continue;
            Json::Value v;
            v["plugin"] = plg;
            v["size"] = (Json::Int64)std::filesystem::file_size(p);
            auto t = std::filesystem::last_write_time(p);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            v["mtime"] = (Json::Int64)std::chrono::system_clock::to_time_t(sctp);
            v["download_url"] = "/m/apk/" + plg;
            rows.append(v);
        }
        RESP_OK(cb, rows);
    }

    void remove(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                const std::string &plugin) {
        std::filesystem::path p = storeDir() / (plugin + ".apk");
        std::error_code ec;
        std::filesystem::remove(p, ec);
        auto &db = PayDb::instance();
        db.setSetting("monitor_android_download_url_" + plugin, "");
        if (plugin == "vmq") db.setSetting("monitor_android_download_url", "");
        RESP_MSG(cb, "已删除");
    }

    // 公开下载: GET /m/apk/{plugin}
    // 浏览器/Android 直接打开会触发 APK 下载
    void download(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                  const std::string &plugin) {
        std::filesystem::path p = storeDir() / (plugin + ".apk");
        if (!std::filesystem::exists(p)) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k404NotFound);
            r->setBody("APK 未上传，请联系管理员"); cb(r); return;
        }
        auto resp = drogon::HttpResponse::newFileResponse(
            p.string(), plugin + ".apk", drogon::CT_CUSTOM);
        resp->addHeader("Content-Type", "application/vnd.android.package-archive");
        cb(resp);
    }
};
