// WePay-Cpp — Vault 代理
// VaultProxy.h — HashiCorp Vault 子进程托管 + 自动解封 + 管理接口
// 读取 config.json["vault"]，启动 vault.exe server，等待就绪后自动解封（Shamir unseal）
// 管理接口（JWT 鉴权）:
//   GET  /admin/api/vault/status       — 状态（sealed/unsealed/standby）
//   POST /admin/api/vault/unseal       — 手动解封
//   POST /admin/api/vault/restart      — 重启 Vault 进程
//   GET  /admin/api/vault/secret/{path} — 读取 KV（调试用，生产可按需关闭）
//   POST /admin/api/vault/secret/{path} — 写入 KV
//   GET  /admin/api/vault/secrets       — 列出 secret/ 根路径下的 key
// Vault 使用 PostgreSQL 存储: configcenter 库
#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <drogon/HttpController.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include "ProcessManager.h"
#include "SimpleJwt.h"
#include "AjaxResult.h"
#include "SyncHttp.h"
#include "VaultClient.h"

#ifdef _WIN32
#include <windows.h>
#endif

struct VaultProxyCfg {
        bool        enabled        = false;
        std::string exePath        = "./vault/vault.exe";  // vault.exe 相对于 exe 目录
        std::string configFile     = "./vault/vault-config.json";
        std::string addr           = "http://127.0.0.1:8200";
        std::string token;                    // Root token
        std::vector<std::string> unsealKeys;  // Shamir 解封密钥（threshold 片数）
        bool        autoUnseal     = true;
        bool        autoRestart    = true;
        bool        showWindow     = false;
        std::string workDir;
};

class VaultProxy : public drogon::HttpController<VaultProxy> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(VaultProxy::vaultStatus,   "/admin/api/vault/status",          drogon::Get);
        ADD_METHOD_TO(VaultProxy::vaultUnseal,   "/admin/api/vault/unseal",          drogon::Post);
        ADD_METHOD_TO(VaultProxy::vaultRestart,  "/admin/api/vault/restart",         drogon::Post);
        ADD_METHOD_TO(VaultProxy::secretGet,     "/admin/api/vault/secret/{path}",   drogon::Get);
        ADD_METHOD_TO(VaultProxy::secretPut,     "/admin/api/vault/secret/{path}",   drogon::Post);
        ADD_METHOD_TO(VaultProxy::secretList,    "/admin/api/vault/secrets",         drogon::Get);
    METHOD_LIST_END

    using Config = VaultProxyCfg;

    VaultProxy() = default;
    static VaultProxy& instance() { static VaultProxy i; return i; }

    static void setup(const Json::Value& appCfg) {
        if (!appCfg.isMember("vault")) return;
        auto& mgr = instance();
        const auto& v = appCfg["vault"];

        mgr.cfg_.enabled    = v.get("enabled",      false).asBool();
        if (!mgr.cfg_.enabled) return;

        mgr.cfg_.exePath    = v.get("exe",           "./vault/vault.exe").asString();
        mgr.cfg_.configFile = v.get("config",        "./vault/vault-config.json").asString();
        mgr.cfg_.addr       = v.get("addr",          "http://127.0.0.1:8200").asString();
        mgr.cfg_.token      = v.get("token",         "").asString();
        mgr.cfg_.autoUnseal = v.get("auto_unseal",   true).asBool();
        mgr.cfg_.autoRestart= v.get("auto_restart",  true).asBool();
        mgr.cfg_.showWindow = v.get("show_window",   false).asBool();
        mgr.cfg_.workDir    = v.get("work_dir",      "").asString();
        if (v.isMember("unseal_keys"))
            for (auto& k : v["unseal_keys"]) mgr.cfg_.unsealKeys.push_back(k.asString());

        // 同步配置到 VaultClient
        VaultClient::instance().configure(appCfg);

        mgr.doStart();
        LOG_INFO << "[VaultProxy] 启动，addr=" << mgr.cfg_.addr;
    }

    // ── 管理 API ─────────────────────────────────────────────────────────────
    void vaultStatus(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!auth(req)) { RESP_401(cb); return; }
        auto r = SyncHttp::get(cfg_.addr + "/v1/sys/seal-status",
                               {{"X-Vault-Token", cfg_.token}}, 3);
        Json::Value d;
#ifdef _WIN32
        d["process_running"] = (proc_ != INVALID_HANDLE_VALUE && proc_ != nullptr);
#else
        d["process_running"] = false;
#endif
        d["addr"]            = cfg_.addr;
        if (r.success && r.status == 200) {
            Json::Value s; Json::Reader rd; rd.parse(r.body, s);
            d["sealed"]      = s.get("sealed",      true);
            d["initialized"] = s.get("initialized", false);
            d["standby"]     = s.get("standby",     false);
            d["version"]     = s.get("version",     "");
        } else {
            d["sealed"] = true; d["error"] = "Vault 不可达";
        }
        RESP_OK(cb, d);
    }

    void vaultUnseal(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!auth(req)) { RESP_401(cb); return; }
        doUnseal();
        auto r = SyncHttp::get(cfg_.addr + "/v1/sys/seal-status",
                               {{"X-Vault-Token", cfg_.token}}, 3);
        Json::Value d; d["triggered"] = true;
        if (r.success) {
            Json::Value s; Json::Reader rd; rd.parse(r.body, s);
            d["sealed"] = s.get("sealed", true);
        }
        RESP_OK(cb, d);
    }

    void vaultRestart(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!auth(req)) { RESP_401(cb); return; }
        doStop();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        doStart();
        Json::Value d; d["restarted"] = true;
        RESP_OK(cb, d);
    }

    void secretGet(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                   const std::string& path) {
        if (!auth(req)) { RESP_401(cb); return; }
        auto fields = VaultClient::instance().getAll(path);
        Json::Value d;
        for (auto& [k, v] : fields) d[k] = v;
        RESP_OK(cb, d);
    }

    void secretPut(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                   const std::string& path) {
        if (!auth(req)) { RESP_401(cb); return; }
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::map<std::string, std::string> fields;
        for (auto& k : (*body).getMemberNames())
            fields[k] = (*body)[k].asString();
        bool ok = VaultClient::instance().put(path, fields);
        Json::Value d; d["saved"] = ok;
        if (ok) RESP_OK(cb, d); else RESP_ERR(cb, "写入 Vault 失败");
    }

    void secretList(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!auth(req)) { RESP_401(cb); return; }
        std::string prefix = req->getParameter("prefix");
        auto keys = VaultClient::instance().list(prefix);
        Json::Value list(Json::arrayValue);
        for (auto& k : keys) list.append(k);
        RESP_OK(cb, list);
    }

private:
    inline static Config            cfg_;
#ifdef _WIN32
    inline static HANDLE            proc_           = INVALID_HANDLE_VALUE;
#endif
    inline static std::atomic<bool> watchdogActive_;

    void doStart() {
        if (!std::filesystem::exists(cfg_.exePath)) {
            LOG_WARN << "[VaultProxy] vault.exe 不存在: " << cfg_.exePath;
            return;
        }
#ifdef _WIN32
        // vault.exe server -config=<configFile>
        std::string args = "\"" + cfg_.exePath + "\" server -config=\"" + cfg_.configFile + "\"";
        proc_ = ProcessManager::spawn("", args, cfg_.workDir, cfg_.showWindow, "vault");
        if (proc_ == INVALID_HANDLE_VALUE || proc_ == nullptr) {
            LOG_WARN << "[VaultProxy] 启动失败"; return;
        }
#else
        ProcessManager::spawn(cfg_.exePath, "server -config=\"" + cfg_.configFile + "\"", cfg_.workDir, false, "vault");
#endif

        // 异步等待 Vault 就绪再解封
        if (cfg_.autoUnseal && !cfg_.unsealKeys.empty()) {
            std::thread([this] {
                for (int i = 0; i < 30; ++i) {  // 最多等 30s
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    auto r = SyncHttp::get(cfg_.addr + "/v1/sys/seal-status", {}, 2);
                    if (r.success && r.status == 200) { doUnseal(); break; }
                }
            }).detach();
        }

        if (cfg_.autoRestart) startWatchdog();
    }

    void doStop() {
        watchdogActive_ = false;
#ifdef _WIN32
        if (proc_ != INVALID_HANDLE_VALUE && proc_ != nullptr) {
            TerminateProcess(proc_, 0);
            CloseHandle(proc_);
            proc_ = INVALID_HANDLE_VALUE;
            LOG_INFO << "[VaultProxy] 进程已停止";
        }
#endif
    }

    void doUnseal() {
        for (auto& key : cfg_.unsealKeys) {
            Json::Value body; body["key"] = key;
            Json::FastWriter fw;
            SyncHttp::postJson(cfg_.addr + "/v1/sys/unseal", fw.write(body),
                               {{"X-Vault-Token", cfg_.token}}, 3);
        }
        // 检查结果
        auto r = SyncHttp::get(cfg_.addr + "/v1/sys/seal-status",
                               {{"X-Vault-Token", cfg_.token}}, 3);
        if (r.success) {
            Json::Value s; Json::Reader rd; rd.parse(r.body, s);
            bool sealed = s.get("sealed", true).asBool();
            LOG_INFO << "[VaultProxy] 解封完成，sealed=" << (sealed ? "true" : "false");
        }
    }

    void startWatchdog() {
        watchdogActive_ = true;
        std::thread([this] {
            while (watchdogActive_) {
                std::this_thread::sleep_for(std::chrono::seconds(20));
                if (!watchdogActive_) break;
                auto r = SyncHttp::get(cfg_.addr + "/v1/sys/health", {}, 2);
                if (!r.success || (r.status != 200 && r.status != 429)) {
                    LOG_WARN << "[VaultProxy] Vault 不可达，自动重启...";
                    doStop();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    doStart();
                }
            }
        }).detach();
    }

    static bool auth(const drogon::HttpRequestPtr& req) {
        std::string a = req->getHeader("Authorization");
        if (a.empty()) a = req->getHeader("authorization");
        if (a.empty()) return false;
        try { SimpleJwt::verify(SimpleJwt::fromHeader(a)); return true; }
        catch (...) { return false; }
    }
};
