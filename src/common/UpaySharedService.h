#pragma once

#ifdef WEPAY_HAS_UPAY_SHARED

#include <string>
#include <mutex>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <json/json.h>
#include <drogon/utils/Utilities.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

class UpaySharedService {
private:
    using FreeStringFn = void (*)(char*);
    using PingFn = char* (*)();
    using StartServerFn = char* (*)();
    using StartCronFn = char* (*)();
    using StartAllFn = char* (*)();
    using GetRuntimeStatusFn = char* (*)();
    using SetBaseURLFn = char* (*)(char*);
    using GetBaseURLFn = char* (*)();
    using GetSettingFn = char* (*)();
    using GetApiKeyFn = char* (*)();
    using GetUserNameFn = char* (*)();
    using GetOrderByOrderIDFn = char* (*)(char*);
    using GetWalletsByTypeFn = char* (*)(char*);
    using HTTPGetFn = char* (*)(char*, char*);
    using HTTPDeleteFn = char* (*)(char*, char*);
    using HTTPPostJSONFn = char* (*)(char*, char*, char*);
    using HTTPPutJSONFn = char* (*)(char*, char*, char*);

    struct Api {
        FreeStringFn freeString;
        PingFn ping;
        StartServerFn startServer;
        StartCronFn startCron;
        StartAllFn startAll;
        GetRuntimeStatusFn getRuntimeStatus;
        SetBaseURLFn setBaseURL;
        GetBaseURLFn getBaseURL;
        GetSettingFn getSetting;
        GetApiKeyFn getApiKey;
        GetUserNameFn getUserName;
        GetOrderByOrderIDFn getOrderByOrderID;
        GetWalletsByTypeFn getWalletsByType;
        HTTPGetFn httpGet;
        HTTPDeleteFn httpDelete;
        HTTPPostJSONFn httpPostJSON;
        HTTPPutJSONFn httpPutJSON;

        Api()
            : freeString(nullptr),
              ping(nullptr),
              startServer(nullptr),
              startCron(nullptr),
              startAll(nullptr),
              getRuntimeStatus(nullptr),
              setBaseURL(nullptr),
              getBaseURL(nullptr),
              getSetting(nullptr),
              getApiKey(nullptr),
              getUserName(nullptr),
              getOrderByOrderID(nullptr),
              getWalletsByType(nullptr),
              httpGet(nullptr),
              httpDelete(nullptr),
              httpPostJSON(nullptr),
              httpPutJSON(nullptr) {}
    };

    inline static std::mutex mutex_;
    inline static bool loaded_ = false;
    inline static bool initFailed_ = false;
    inline static std::string loadedPath_;
    inline static Api api_{};
#ifdef _WIN32
    inline static HMODULE module_ = nullptr;
#else
    inline static void* module_ = nullptr;
#endif

    template <typename T>
    static bool resolve(T& out, const char* symbol) {
#ifdef _WIN32
        out = reinterpret_cast<T>(GetProcAddress(module_, symbol));
#else
        out = reinterpret_cast<T>(dlsym(module_, symbol));
#endif
        if (!out) {
            LOG_ERROR << "[UPayShared] missing symbol: " << symbol;
            return false;
        }
        return true;
    }

    static bool resolveSymbols() {
        return resolve(api_.freeString, "UPay_FreeString") &&
               resolve(api_.ping, "UPay_Ping") &&
               resolve(api_.startServer, "UPay_StartServer") &&
               resolve(api_.startCron, "UPay_StartCron") &&
               resolve(api_.startAll, "UPay_StartAll") &&
               resolve(api_.getRuntimeStatus, "UPay_GetRuntimeStatus") &&
               resolve(api_.setBaseURL, "UPay_SetBaseURL") &&
               resolve(api_.getBaseURL, "UPay_GetBaseURL") &&
               resolve(api_.getSetting, "UPay_GetSetting") &&
               resolve(api_.getApiKey, "UPay_GetApiKey") &&
               resolve(api_.getUserName, "UPay_GetUserName") &&
               resolve(api_.getOrderByOrderID, "UPay_GetOrderByOrderID") &&
               resolve(api_.getWalletsByType, "UPay_GetWalletsByType") &&
               resolve(api_.httpGet, "UPay_HTTPGet") &&
               resolve(api_.httpDelete, "UPay_HTTPDelete") &&
               resolve(api_.httpPostJSON, "UPay_HTTPPostJSON") &&
               resolve(api_.httpPutJSON, "UPay_HTTPPutJSON");
    }

    static Json::Value parseJson(const std::string& payload) {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream stream(payload);
        if (!Json::parseFromStream(builder, stream, &root, &errors)) {
            Json::Value fallback;
            fallback["code"] = -1;
            fallback["message"] = "invalid json from upay shared";
            fallback["raw"] = payload;
            fallback["errors"] = errors;
            return fallback;
        }
        return root;
    }

    static Json::Value call0(char* (*fn)()) {
        if (!fn) {
            Json::Value root;
            root["code"] = -1;
            root["message"] = "function not loaded";
            return root;
        }
        char* raw = fn();
        std::string payload = raw ? raw : "";
        if (raw && api_.freeString) {
            api_.freeString(raw);
        }
        return parseJson(payload);
    }

    static char* dupCString(const std::string& value) {
#ifdef _WIN32
        return _strdup(value.c_str());
#else
        return strdup(value.c_str());
#endif
    }

    static Json::Value call1(char* (*fn)(char*), const std::string& arg1) {
        if (!fn) {
            Json::Value root;
            root["code"] = -1;
            root["message"] = "function not loaded";
            return root;
        }
        char* c1 = dupCString(arg1);
        char* raw = fn(c1);
        free(c1);
        std::string payload = raw ? raw : "";
        if (raw && api_.freeString) {
            api_.freeString(raw);
        }
        return parseJson(payload);
    }

    static Json::Value call2(char* (*fn)(char*, char*), const std::string& arg1, const std::string& arg2) {
        if (!fn) {
            Json::Value root;
            root["code"] = -1;
            root["message"] = "function not loaded";
            return root;
        }
        char* c1 = dupCString(arg1);
        char* c2 = dupCString(arg2);
        char* raw = fn(c1, c2);
        free(c1);
        free(c2);
        std::string payload = raw ? raw : "";
        if (raw && api_.freeString) {
            api_.freeString(raw);
        }
        return parseJson(payload);
    }

    static Json::Value call3(char* (*fn)(char*, char*, char*), const std::string& arg1, const std::string& arg2, const std::string& arg3) {
        if (!fn) {
            Json::Value root;
            root["code"] = -1;
            root["message"] = "function not loaded";
            return root;
        }
        char* c1 = dupCString(arg1);
        char* c2 = dupCString(arg2);
        char* c3 = dupCString(arg3);
        char* raw = fn(c1, c2, c3);
        free(c1);
        free(c2);
        free(c3);
        std::string payload = raw ? raw : "";
        if (raw && api_.freeString) {
            api_.freeString(raw);
        }
        return parseJson(payload);
    }

public:
    static bool load(const std::string& libraryPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_) {
            return true;
        }
        if (initFailed_) {
            return false;
        }

        std::filesystem::path path = std::filesystem::absolute(libraryPath);
        if (!std::filesystem::exists(path)) {
            LOG_WARN << "[UPayShared] library not found, skipping optional bridge: " << path.string();
            return false;
        }
#ifdef _WIN32
        module_ = LoadLibraryA(path.string().c_str());
#else
        module_ = dlopen(path.string().c_str(), RTLD_NOW);
#endif
        if (!module_) {
            LOG_ERROR << "[UPayShared] failed to load library: " << path.string();
            initFailed_ = true;
            return false;
        }
        if (!resolveSymbols()) {
            initFailed_ = true;
            return false;
        }
        loaded_ = true;
        loadedPath_ = path.string();
        LOG_INFO << "[UPayShared] loaded: " << loadedPath_;
        return true;
    }

    static bool start(const std::string& libraryPath, const std::string& baseUrl, bool startServer, bool startCron) {
        if (!load(libraryPath)) {
            return false;
        }

        if (!baseUrl.empty()) {
            Json::Value result = call1(api_.setBaseURL, baseUrl);
            if (result.get("code", -1).asInt() != 0) {
                LOG_ERROR << "[UPayShared] set base url failed: " << result.toStyledString();
                return false;
            }
        }

        Json::Value startResult;
        if (startServer && startCron) {
            startResult = call0(api_.startAll);
        } else if (startServer) {
            startResult = call0(api_.startServer);
        } else if (startCron) {
            startResult = call0(api_.startCron);
        } else {
            startResult = call0(api_.getRuntimeStatus);
        }

        if (startResult.get("code", -1).asInt() != 0) {
            LOG_ERROR << "[UPayShared] start failed: " << startResult.toStyledString();
            return false;
        }

        LOG_INFO << "[UPayShared] started with baseUrl=" << baseUrl;
        return true;
    }

    static bool isLoaded() {
        std::lock_guard<std::mutex> lock(mutex_);
        return loaded_;
    }

    static std::string libraryPath() {
        std::lock_guard<std::mutex> lock(mutex_);
        return loadedPath_;
    }

    static Json::Value ping() { return call0(api_.ping); }
    static Json::Value getRuntimeStatus() { return call0(api_.getRuntimeStatus); }
    static Json::Value getBaseURL() { return call0(api_.getBaseURL); }
    static Json::Value getSetting() { return call0(api_.getSetting); }
    static Json::Value getApiKey() { return call0(api_.getApiKey); }
    static Json::Value getUserName() { return call0(api_.getUserName); }
    static Json::Value getOrderByOrderID(const std::string& orderId) { return call1(api_.getOrderByOrderID, orderId); }
    static Json::Value getWalletsByType(const std::string& typeName) { return call1(api_.getWalletsByType, typeName); }
    static Json::Value httpGet(const std::string& path, const std::string& cookie = "") { return call2(api_.httpGet, path, cookie); }
    static Json::Value httpDelete(const std::string& path, const std::string& cookie = "") { return call2(api_.httpDelete, path, cookie); }
    static Json::Value httpPostJson(const std::string& path, const std::string& body, const std::string& cookie = "") { return call3(api_.httpPostJSON, path, body, cookie); }
    static Json::Value httpPutJson(const std::string& path, const std::string& body, const std::string& cookie = "") { return call3(api_.httpPutJSON, path, body, cookie); }
};

#else

#include <string>
#include <json/json.h>
#include <drogon/utils/Utilities.h>

class UpaySharedService {
public:
    static bool load(const std::string&) {
        LOG_WARN << "UPay shared support not compiled in (WEPAY_HAS_UPAY_SHARED not defined)";
        return false;
    }

    static bool start(const std::string&, const std::string&, bool, bool) {
        LOG_WARN << "UPay shared support not compiled in (WEPAY_HAS_UPAY_SHARED not defined)";
        return false;
    }

    static bool isLoaded() { return false; }
    static std::string libraryPath() { return {}; }
    static Json::Value ping() { return Json::Value(); }
    static Json::Value getRuntimeStatus() { return Json::Value(); }
    static Json::Value getBaseURL() { return Json::Value(); }
    static Json::Value getSetting() { return Json::Value(); }
    static Json::Value getApiKey() { return Json::Value(); }
    static Json::Value getUserName() { return Json::Value(); }
    static Json::Value getOrderByOrderID(const std::string&) { return Json::Value(); }
    static Json::Value getWalletsByType(const std::string&) { return Json::Value(); }
    static Json::Value httpGet(const std::string&, const std::string& = "") { return Json::Value(); }
    static Json::Value httpDelete(const std::string&, const std::string& = "") { return Json::Value(); }
    static Json::Value httpPostJson(const std::string&, const std::string&, const std::string& = "") { return Json::Value(); }
    static Json::Value httpPutJson(const std::string&, const std::string&, const std::string& = "") { return Json::Value(); }
};

#endif
