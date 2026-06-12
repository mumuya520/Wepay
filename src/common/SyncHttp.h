// WePay-Cpp — 同步 HTTP 客户端 (基于 libcurl)
// 用于通道插件调用上游支付 API，返回结果后才能响应商户
// 支持自定义 headers、超时、TLS
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <map> // 映射容器
#include <curl/curl.h> // libcurl 库

// 同步 HTTP 客户端类
// 职责：
//   1. 基于 libcurl 的同步 HTTP 请求库
//   2. 支持 GET、POST、PUT、DELETE、PATCH 等方法
//   3. 用于通道插件调用上游支付 API
//   4. 支持自定义请求头、超时、TLS 配置
class SyncHttp {
public:
    // HTTP 响应结构体
    struct Response {
        // 请求是否成功
        bool success = false;
        // HTTP 状态码
        long status = 0;
        // 响应体
        std::string body;
        // 错误消息
        std::string errMsg;
    };

    // 全局初始化（在 main.cc 启动时调用一次）
    // 功能：初始化 libcurl 全局资源
    static void globalInit() {
        // 初始化标志
        static bool initDone = false;
        // 检查是否已初始化
        if (!initDone) {
            // 初始化 libcurl
            curl_global_init(CURL_GLOBAL_DEFAULT);
            // 标记已初始化
            initDone = true;
        }
    }

    // POST JSON 请求
    // 参数 url：请求 URL
    // 参数 body：JSON 请求体
    // 参数 headers：自定义请求头（可选）
    // 参数 timeoutSec：超时时间（秒，默认 15）
    // 返回：HTTP 响应
    static Response postJson(const std::string &url,
                              const std::string &body,
                              const std::map<std::string, std::string> &headers = {},
                              long timeoutSec = 15) {
        // 复制请求头
        std::map<std::string, std::string> h = headers;
        // 如果未指定 Content-Type，设置为 application/json
        if (h.find("Content-Type") == h.end())
            h["Content-Type"] = "application/json";
        // 执行 POST 请求
        return doRequest("POST", url, body, h, timeoutSec);
    }

    // POST form-urlencoded 请求
    // 参数 url：请求 URL
    // 参数 formBody：表单编码的请求体
    // 参数 timeoutSec：超时时间（秒，默认 15）
    // 返回：HTTP 响应
    static Response postForm(const std::string &url,
                              const std::string &formBody,
                              long timeoutSec = 15) {
        // 创建请求头
        std::map<std::string, std::string> h;
        // 设置 Content-Type
        h["Content-Type"] = "application/x-www-form-urlencoded";
        // 执行 POST 请求
        return doRequest("POST", url, formBody, h, timeoutSec);
    }

    // GET 请求
    // 参数 url：请求 URL
    // 参数 headers：自定义请求头（可选）
    // 参数 timeoutSec：超时时间（秒，默认 15）
    // 返回：HTTP 响应
    static Response get(const std::string &url,
                         const std::map<std::string, std::string> &headers = {},
                         long timeoutSec = 15) {
        // 执行 GET 请求
        return doRequest("GET", url, "", headers, timeoutSec);
    }

    // 自定义 HTTP 方法请求
    // 参数 method：HTTP 方法（LIST / DELETE / PUT / PATCH 等）
    // 参数 url：请求 URL
    // 参数 body：请求体（可选）
    // 参数 headers：自定义请求头（可选）
    // 参数 timeoutSec：超时时间（秒，默认 15）
    // 返回：HTTP 响应
    // 说明：供 VaultClient 等需要自定义方法的客户端使用
    static Response doRequestPublic(const std::string &method,
                                    const std::string &url,
                                    const std::string &body,
                                    const std::map<std::string, std::string> &headers = {},
                                    long timeoutSec = 15) {
        // 执行自定义方法请求
        return doRequest(method, url, body, headers, timeoutSec);
    }

// 私有区域
private:
    // libcurl 写入回调函数
    // 参数 data：响应数据指针
    // 参数 size：数据块大小
    // 参数 nmemb：数据块数量
    // 参数 userp：用户指针（指向 std::string）
    // 返回：处理的字节数
    // 说明：libcurl 会多次调用此函数来接收响应体
    static size_t writeCallback(void *data, size_t size, size_t nmemb, void *userp) {
        // 将用户指针转换为 std::string 指针
        auto *s = static_cast<std::string*>(userp);
        // 将数据追加到字符串
        s->append(static_cast<char*>(data), size * nmemb);
        // 返回处理的字节数
        return size * nmemb;
    }

    // 执行 HTTP 请求（内部实现）
    // 参数 method：HTTP 方法（GET / POST / PUT / DELETE / PATCH 等）
    // 参数 url：请求 URL
    // 参数 body：请求体
    // 参数 headers：请求头
    // 参数 timeoutSec：超时时间（秒）
    // 返回：HTTP 响应
    static Response doRequest(const std::string &method,
                               const std::string &url,
                               const std::string &body,
                               const std::map<std::string, std::string> &headers,
                               long timeoutSec) {
        // ── 初始化响应 ──────────────────────────────
        // 创建响应对象
        Response r;
        // 初始化 libcurl 句柄
        CURL *curl = curl_easy_init();
        // 检查初始化是否成功
        if (!curl) {
            // 设置错误消息
            r.errMsg = "curl init failed";
            // 返回错误响应
            return r;
        }

        // ── 设置基本选项 ──────────────────────────────
        // 设置请求 URL
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // 设置总超时时间
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
        // 设置连接超时时间（最多 10 秒）
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)std::min<long>(timeoutSec, 10));
        // 启用重定向跟踪
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        // 禁用 SSL 证书验证（生产环境应改为 1L 并配置 CA 证书）
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        // 禁用主机名验证
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        // 设置写入回调函数
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &SyncHttp::writeCallback);
        // 设置写入回调的用户数据（响应体字符串）
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
        // 设置 User-Agent
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "WePay-Cpp/2.0");

        // ── 设置请求方法和请求体 ──────────────────────────────
        // 根据方法类型设置选项
        if (method == "POST") {
            // 设置为 POST 请求
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            // 设置请求体
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            // 设置请求体大小
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        } else if (method == "GET") {
            // 设置为 GET 请求
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        } else {
            // ── 自定义方法（LIST / DELETE / PUT / PATCH）──────────────────────────────
            // 设置自定义 HTTP 方法
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
            // 如果有请求体
            if (!body.empty()) {
                // 设置请求体
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                // 设置请求体大小
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
            }
        }

        // ── 设置请求头 ──────────────────────────────
        // 创建请求头链表
        struct curl_slist *hlist = nullptr;
        // 遍历所有请求头
        for (auto &[k, v] : headers) {
            // 添加请求头到链表
            hlist = curl_slist_append(hlist, (k + ": " + v).c_str());
        }
        // 如果有请求头，设置到 curl 句柄
        if (hlist)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

        // ── 执行请求 ──────────────────────────────
        // 执行 HTTP 请求
        CURLcode res = curl_easy_perform(curl);
        // 检查请求是否成功
        if (res == CURLE_OK) {
            // 标记成功
            r.success = true;
            // 获取 HTTP 状态码
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        } else {
            // 获取错误消息
            r.errMsg = curl_easy_strerror(res);
        }

        // ── 清理资源 ──────────────────────────────
        // 释放请求头链表
        if (hlist)
            curl_slist_free_all(hlist);
        // 清理 curl 句柄
        curl_easy_cleanup(curl);
        // 返回响应
        return r;
    }
};
