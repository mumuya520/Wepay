// WePay-Cpp — Bepusdt 库加载器
// BepusdtLib.h — 加载 bepusdt.dll (Go cgo) 并封装 C 函数
// BEpusdt: 多链加密货币支付网关 (USDT/ETH/BNB/USDC 等)
// 原始 3 函数: BepusdtStart / BepusdtStop / BepusdtVersion
// FFI 扩展 3 函数: BepusdtBuildEngine / BepusdtHandleHTTPRequest / BepusdtFreeString
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <map> // 映射容器
#include <mutex> // 互斥锁
#include <atomic>
#include <drogon/drogon.h>
#include <json/json.h>

#ifdef _WIN32
#  include <windows.h>
#  define BP_LIB_HANDLE HMODULE
#  define BP_LOAD_LIB(p) LoadLibraryA(p)
#  define BP_GET_PROC GetProcAddress
#  define BP_FREE_LIB FreeLibrary
#else
#  include <dlfcn.h>
#  define BP_LIB_HANDLE void*
#  define BP_LOAD_LIB(p) dlopen(p, RTLD_LAZY|RTLD_GLOBAL)
#  define BP_GET_PROC dlsym
#  define BP_FREE_LIB dlclose
#endif

struct BepusdtHTTPResponse {
    int status{500};
    std::map<std::string, std::string> headers;
    std::string body;
    bool ok{false};
    std::string err;
};

class BepusdtLib {
public:
    static BepusdtLib &instance() {
        static BepusdtLib inst;
        return inst;
    }

    bool load(const std::string &dllPath) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (handle_) return true;
        handle_ = BP_LOAD_LIB(dllPath.c_str());
        if (!handle_) {
            LOG_ERROR << "[BepusdtLib] load failed: " << dllPath
#ifdef _WIN32
                      << " err=" << GetLastError();
#else
                      << " err=" << dlerror();
#endif
            return false;
        }
        fn_Start            = (Fn_Start)           BP_GET_PROC(handle_, "BepusdtStart");
        fn_Stop             = (Fn_Stop)            BP_GET_PROC(handle_, "BepusdtStop");
        fn_Version          = (Fn_Version)         BP_GET_PROC(handle_, "BepusdtVersion");
        fn_BuildEngine      = (Fn_BuildEngine)     BP_GET_PROC(handle_, "BepusdtBuildEngine");
        fn_HandleHTTPRequest= (Fn_HandleHTTPRequest)BP_GET_PROC(handle_, "BepusdtHandleHTTPRequest");
        fn_IssueToken       = (Fn_IssueToken)      BP_GET_PROC(handle_, "BepusdtIssueToken");
        fn_GetSecurePath    = (Fn_GetSecurePath)   BP_GET_PROC(handle_, "BepusdtGetSecurePath");
        fn_FreeString       = (Fn_FreeString)      BP_GET_PROC(handle_, "BepusdtFreeString");

        if (!fn_Start || !fn_Stop || !fn_Version) {
            LOG_ERROR << "[BepusdtLib] missing basic exports in " << dllPath;
            BP_FREE_LIB(handle_);
            handle_ = nullptr;
            return false;
        }
        if (!fn_BuildEngine || !fn_HandleHTTPRequest || !fn_FreeString) {
            LOG_WARN << "[BepusdtLib] FFI exports not found — FFI 模式不可用，将使用独立端口";
        }
        loaded_ = true;
        LOG_INFO << "[BepusdtLib] loaded " << dllPath;
        return true;
    }

    bool isLoaded() const { return loaded_.load(); }

    bool hasFFI() const {
        return fn_BuildEngine != nullptr && fn_HandleHTTPRequest != nullptr && fn_FreeString != nullptr;
    }

    bool hasSSO() const {
        return fn_IssueToken != nullptr && fn_FreeString != nullptr;
    }

    // HTTP 模式: 启动独立端口
    std::string start(const std::string &listen,
                      const std::string &sqlitePath = "",
                      const std::string &mysqlDsn = "",
                      const std::string &postgresDsn = "",
                      const std::string &logPath = "") {
        if (!loaded_) return "error: dll not loaded";
        char *r = fn_Start(
            const_cast<char*>(listen.c_str()),
            const_cast<char*>(sqlitePath.c_str()),
            const_cast<char*>(mysqlDsn.c_str()),
            const_cast<char*>(postgresDsn.c_str()),
            const_cast<char*>(logPath.c_str()));
        std::string result = r ? r : "error: null return";
        if (fn_FreeString && r) fn_FreeString(r);
        return result;
    }

    // FFI 模式: 构建 gin engine（不监听端口）
    std::string buildEngine(const std::string &sqlitePath = "",
                            const std::string &mysqlDsn = "",
                            const std::string &postgresDsn = "",
                            const std::string &logPath = "") {
        if (!loaded_ || !fn_BuildEngine) return "error: FFI not available";
        char *r = fn_BuildEngine(
            const_cast<char*>(sqlitePath.c_str()),
            const_cast<char*>(mysqlDsn.c_str()),
            const_cast<char*>(postgresDsn.c_str()),
            const_cast<char*>(logPath.c_str()));
        std::string result = r ? r : "error: null return";
        if (fn_FreeString && r) fn_FreeString(r);
        return result;
    }

    // FFI HTTP 派发: 直接调用 gin engine（不经过 TCP）
    BepusdtHTTPResponse handleRequest(const std::string &method,
                                       const std::string &path,
                                       const std::string &rawQuery,
                                       const std::map<std::string, std::string> &headers,
                                       const std::string &body) {
        BepusdtHTTPResponse resp;
        if (!loaded_ || !fn_HandleHTTPRequest) {
            resp.err = "FFI not available";
            return resp;
        }
        Json::Value h(Json::objectValue);
        for (auto &kv : headers) h[kv.first] = kv.second;
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string headersJson = Json::writeString(wb, h);

        char *out = fn_HandleHTTPRequest(
            const_cast<char*>(method.c_str()),
            const_cast<char*>(path.c_str()),
            const_cast<char*>(rawQuery.c_str()),
            const_cast<char*>(headersJson.c_str()),
            body.empty() ? nullptr : (void*)body.data(),
            (int)body.size());
        if (!out) {
            resp.err = "null response from FFI";
            return resp;
        }
        std::string outStr = out;
        fn_FreeString(out);

        Json::Value j;
        if (!Json::Reader().parse(outStr, j)) {
            resp.err = "parse response json failed";
            return resp;
        }
        resp.status = j.get("status", 500).asInt();
        if (j.isMember("headers") && j["headers"].isObject()) {
            for (auto &k : j["headers"].getMemberNames()) {
                resp.headers[k] = j["headers"][k].asString();
            }
        }
        std::string b64 = j.get("body_b64", "").asString();
        if (!b64.empty()) resp.body = base64Decode(b64);
        resp.ok = true;
        return resp;
    }

    // 获取安全入口路径（如 /b1d8b50648）
    std::string getSecurePath() {
        if (!loaded_ || !fn_GetSecurePath) return "";
        char *r = fn_GetSecurePath();
        std::string result = r ? r : "";
        if (fn_FreeString && r) fn_FreeString(r);
        return result;
    }

    // SSO: 签发 BEpusdt 管理 token
    std::string issueToken(const std::string &username) {
        if (!loaded_ || !fn_IssueToken) return "";
        char *r = fn_IssueToken(const_cast<char*>(username.c_str()));
        std::string result = r ? r : "";
        if (fn_FreeString && r) fn_FreeString(r);
        return result;
    }

    std::string stop() {
        if (!loaded_) return "error: dll not loaded";
        char *r = fn_Stop();
        std::string result = r ? r : "error: null return";
        if (fn_FreeString && r) fn_FreeString(r);
        return result;
    }

    std::string version() {
        if (!loaded_) return "";
        char *r = fn_Version();
        std::string result = r ? r : "";
        if (fn_FreeString && r) fn_FreeString(r);
        return result;
    }

private:
    BepusdtLib() = default;
    ~BepusdtLib() {
        if (handle_) BP_FREE_LIB(handle_);
    }
    BepusdtLib(const BepusdtLib&) = delete;
    BepusdtLib &operator=(const BepusdtLib&) = delete;

    using Fn_Start            = char* (*)(char*, char*, char*, char*, char*);
    using Fn_Stop             = char* (*)();
    using Fn_Version          = char* (*)();
    using Fn_BuildEngine      = char* (*)(char*, char*, char*, char*);
    using Fn_HandleHTTPRequest= char* (*)(char*, char*, char*, char*, void*, int);
    using Fn_IssueToken       = char* (*)(char*);
    using Fn_GetSecurePath    = char* (*)();
    using Fn_FreeString       = void  (*)(char*);

    BP_LIB_HANDLE handle_{nullptr};
    std::atomic<bool> loaded_{false};
    std::mutex mtx_;

    Fn_Start             fn_Start{nullptr};
    Fn_Stop              fn_Stop{nullptr};
    Fn_Version           fn_Version{nullptr};
    Fn_BuildEngine       fn_BuildEngine{nullptr};
    Fn_HandleHTTPRequest fn_HandleHTTPRequest{nullptr};
    Fn_IssueToken        fn_IssueToken{nullptr};
    Fn_GetSecurePath     fn_GetSecurePath{nullptr};
    Fn_FreeString        fn_FreeString{nullptr};

    static std::string base64Decode(const std::string &in) {
        static const std::string chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -8;
        for (unsigned char c : in) {
            if (c == '=') break;
            auto pos = chars.find((char)c);
            if (pos == std::string::npos) {
                if (c == '\n' || c == '\r' || c == ' ') continue;
                return out;
            }
            val = (val << 6) | (int)pos;
            valb += 6;
            if (valb >= 0) {
                out.push_back((char)((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }
};
