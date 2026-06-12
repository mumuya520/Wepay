// WePay-Cpp — HTTP 异步调用工具
// 基于 Drogon HttpClient 实现异步 GET/POST，支持可选回调
// 用法：HttpCaller::asyncGet(url) / HttpCaller::asyncPost(url, body, contentType)
#pragma once // 防止头文件重复包含
#include <drogon/HttpClient.h> // Drogon HTTP 客户端
#include <drogon/HttpRequest.h> // Drogon HTTP 请求
#include <drogon/HttpAppFramework.h> // Drogon 应用框架
#include <string> // 字符串库
#include <functional> // 函数库
#include <regex> // 正则表达式库
#include <iostream> // 输入输出库

// HTTP 异步调用工具类，基于 Drogon HttpClient 实现异步 HTTP 请求
class HttpCaller {
public:
    // 回调函数类型别名
    // 参数 ok：请求是否成功
    // 参数 httpStatus：HTTP 状态码
    // 参数 body：响应体内容
    using Cb = std::function<void(bool ok, int httpStatus, const std::string &body)>;

    // 异步 GET 方法，回调可选（为空时即"发了不管"）
    // 参数 url：完整的 HTTP(S) URL
    // 参数 cb：可选的回调函数，为 nullptr 时不处理响应
    static void asyncGet(const std::string &url, Cb cb = nullptr) {
        // 用于存储 URL 的基础部分（协议 + 主机）
        std::string base;
        // 用于存储 URL 的路径部分
        std::string path;
        // 解析 URL，分离基础部分和路径部分
        if (!splitUrl(url, base, path)) {
            // 如果 URL 格式不正确
            if (cb)
                // 调用回调函数，传递错误信息
                cb(false, 0, "invalid url");
            // 函数返回
            return;
        }
        // 创建 HTTP 客户端实例
        auto client = drogon::HttpClient::newHttpClient(base);
        // 设置 User-Agent 请求头
        client->setUserAgent("WePay-Cpp/1.0");
        // 创建新的 HTTP 请求对象
        auto req = drogon::HttpRequest::newHttpRequest();
        // 设置请求方法为 GET
        req->setMethod(drogon::Get);
        // 设置请求路径
        req->setPath(path);
        // 发送请求，并指定回调函数处理响应
        client->sendRequest(req,
            // Lambda 回调函数：处理请求结果和响应
            [cb](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) {
                // 如果指定了回调函数
                if (cb) {
                    // 检查请求是否失败或响应为空
                    if (result != drogon::ReqResult::Ok || !resp) {
                        // 调用回调函数，传递请求失败信息
                        cb(false, 0, "request failed");
                    } else {
                        // 调用回调函数，传递成功状态、HTTP 状态码和响应体
                        cb(true, (int)resp->statusCode(),
                           std::string(resp->getBody().data(), resp->getBody().size()));
                    }
                }
            },
            // 设置请求超时时间为 15 秒
            /*timeout*/ 15.0);
    }

    // 异步 POST 方法，支持自定义请求体和 Content-Type
    // 参数 url：完整的 HTTP(S) URL
    // 参数 body：POST 请求体内容
    // 参数 contentType：Content-Type 请求头值，默认为 "application/json"
    // 参数 cb：可选的回调函数，为 nullptr 时不处理响应
    static void asyncPost(const std::string &url, const std::string &body,
                          const std::string &contentType = "application/json",
                          Cb cb = nullptr) {
        // 用于存储 URL 的基础部分（协议 + 主机）
        std::string base;
        // 用于存储 URL 的路径部分
        std::string path;
        // 解析 URL，分离基础部分和路径部分
        if (!splitUrl(url, base, path)) {
            // 如果 URL 格式不正确
            if (cb)
                // 调用回调函数，传递错误信息
                cb(false, 0, "invalid url");
            // 函数返回
            return;
        }
        // 创建 HTTP 客户端实例
        auto client = drogon::HttpClient::newHttpClient(base);
        // 设置 User-Agent 请求头
        client->setUserAgent("WePay-Cpp/1.0");
        // 创建新的 HTTP 请求对象
        auto req = drogon::HttpRequest::newHttpRequest();
        // 设置请求方法为 POST
        req->setMethod(drogon::Post);
        // 设置请求路径
        req->setPath(path);
        // 设置请求体内容
        req->setBody(body);
        // 添加 Content-Type 请求头
        req->addHeader("Content-Type", contentType);
        // 发送请求，并指定回调函数处理响应
        client->sendRequest(req,
            // Lambda 回调函数：处理请求结果和响应
            [cb](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) {
                // 如果指定了回调函数
                if (cb) {
                    // 检查请求是否失败或响应为空
                    if (result != drogon::ReqResult::Ok || !resp) {
                        // 调用回调函数，传递请求失败信息
                        cb(false, 0, "request failed");
                    } else {
                        // 调用回调函数，传递成功状态、HTTP 状态码和响应体
                        cb(true, (int)resp->statusCode(),
                           std::string(resp->getBody().data(), resp->getBody().size()));
                    }
                }
            },
            // 设置请求超时时间为 15 秒
            /*timeout*/ 15.0);
    }

// 私有方法区域
private:
    // 解析 URL，分离基础部分和路径部分
    // 参数 url：完整的 URL 字符串
    // 参数 base：输出参数，存储 URL 的基础部分（协议 + 主机）
    // 参数 path：输出参数，存储 URL 的路径部分
    // 返回：true 表示 URL 格式正确，false 表示格式错误
    static bool splitUrl(const std::string &url, std::string &base, std::string &path) {
        // 创建正则表达式用于匹配 URL 格式
        // 匹配模式：http(s)://主机名/路径
        std::regex re(R"(^(https?://[^/]+)(/.*)?$)", std::regex::icase);
        // 用于存储正则表达式匹配结果
        std::smatch m;
        // 检查 URL 是否匹配正则表达式
        if (!std::regex_match(url, m, re))
            // 如果不匹配，返回 false
            return false;
        // 提取匹配的第一个分组（基础部分：协议 + 主机）
        base = m[1].str();
        // 提取匹配的第二个分组（路径部分），如果不存在则默认为 "/"
        path = m[2].matched ? m[2].str() : "/";
        // 返回 true 表示解析成功
        return true;
    }
// 类定义结束
};
