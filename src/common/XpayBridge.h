// WePay-Cpp — xpay-go 桥接层 (子进程 HTTP 代理模式)
// xpay-go 作为独立子进程运行，XpayBridge 通过 HTTP 转发请求
// (避免与 dujiao.dll 的 Go runtime 冲突)
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <map> // 映射容器
#include <mutex> // 互斥锁
#include <sstream> // 字符串流库
#include <vector>
#include <fstream>
#include <cstring>
#include <json/json.h>
#include <drogon/drogon.h>
#include "SyncHttp.h"

class XpayBridge {
public:
    static XpayBridge &instance() {
        static XpayBridge b;
        return b;
    }

    // 配置后端 URL (在启动子进程后调用)
    void configure(const std::string &backendUrl) {
        std::lock_guard<std::mutex> lk(mu_);
        baseUrl_ = backendUrl;
        initialized_ = !backendUrl.empty();
        LOG_INFO << "[XpayBridge] HTTP 代理模式, backend=" << backendUrl;
    }

    bool init(const std::string & = "xpay-go") {
        return initialized_;
    }

    bool ready() {
        std::lock_guard<std::mutex> lk(mu_);
        return initialized_;
    }

    struct Response {
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
        bool ok() const { return status >= 200 && status < 300; }
    };

    // 代理到 xpay-go 子进程
    Response request(const std::string &method,
                     const std::string &path,
                     const std::string &rawQuery = "",
                     const std::string &reqBody = "",
                     const std::map<std::string, std::string> &reqHeaders = {}) {
        Response r;
        if (!ready()) { r.status = 0; return r; }

        std::string url = baseUrl_ + path;
        if (!rawQuery.empty()) url += "?" + rawQuery;

        std::map<std::string, std::string> fwdHeaders;
        for (auto &[k, v] : reqHeaders) fwdHeaders[k] = v;

        SyncHttp::Response hr;
        if (method == "GET" || method == "HEAD") {
            hr = SyncHttp::get(url, fwdHeaders);
        } else {
            std::string ct = reqHeaders.count("Content-Type")
                             ? reqHeaders.at("Content-Type")
                             : "application/x-www-form-urlencoded";
            if (ct.find("json") != std::string::npos)
                hr = SyncHttp::postJson(url, reqBody, fwdHeaders);
            else
                hr = SyncHttp::doRequestPublic(method, url, reqBody, fwdHeaders);
        }
        r.status = (int)hr.status;
        r.body   = hr.body;
        return r;
    }

    Response get(const std::string &path, const std::string &rawQuery = "") {
        return request("GET", path, rawQuery);
    }
    Response postForm(const std::string &path, const std::string &form) {
        return request("POST", path, "", form,
                       {{"Content-Type","application/x-www-form-urlencoded"}});
    }
    Response postJson(const std::string &path, const std::string &json) {
        return request("POST", path, "", json,
                       {{"Content-Type","application/json"}});
    }

    std::string lastError() { return cachedErr_; }

private:
    std::mutex mu_;
    bool initialized_ = false;
    std::string baseUrl_;
    std::string cachedErr_;
};
