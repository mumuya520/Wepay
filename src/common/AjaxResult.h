// WePay-Cpp — AJAX 响应结果工具类 — 统一 JSON 响应格式
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h> // JSON 库
#include <string> // 字符串库

// AJAX 响应结果类，用于统一 HTTP API 的 JSON 响应格式
class AjaxResult {
public:
    // 成功状态码
    static constexpr int CODE_SUCCESS = 200;
    // 错误状态码
    static constexpr int CODE_ERROR   = 500;

    // 成功响应（无数据）
    // 返回：包含状态码和成功消息的 JSON 对象
    static Json::Value success() {
        // 创建 JSON 响应对象
        Json::Value r;
        // 设置状态码为 200（成功）
        r["code"] = CODE_SUCCESS;
        // 设置默认成功消息
        r["msg"] = "操作成功";
        // 返回响应对象
        return r;
    }

    // 成功响应（带自定义消息）
    // 参数 msg：自定义消息文本
    // 返回：包含状态码和消息的 JSON 对象
    static Json::Value success(const std::string &msg) {
        // 创建 JSON 响应对象
        Json::Value r;
        // 设置状态码为 200（成功）
        r["code"] = CODE_SUCCESS;
        // 设置自定义消息
        r["msg"] = msg;
        // 返回响应对象
        return r;
    }

    // 成功响应（带数据）
    // 参数 data：响应数据（JSON 对象）
    // 返回：包含状态码、消息和数据的 JSON 对象
    static Json::Value success(const Json::Value &data) {
        // 创建 JSON 响应对象
        Json::Value r;
        // 设置状态码为 200（成功）
        r["code"] = CODE_SUCCESS;
        // 设置默认成功消息
        r["msg"] = "操作成功";
        // 设置响应数据
        r["data"] = data;
        // 返回响应对象
        return r;
    }

    // 成功响应（带消息和数据）
    // 参数 msg：自定义消息文本
    // 参数 data：响应数据（JSON 对象）
    // 返回：包含状态码、消息和数据的 JSON 对象
    static Json::Value success(const std::string &msg, const Json::Value &data) {
        // 创建 JSON 响应对象
        Json::Value r;
        // 设置状态码为 200（成功）
        r["code"] = CODE_SUCCESS;
        // 设置自定义消息
        r["msg"] = msg;
        // 设置响应数据
        r["data"] = data;
        // 返回响应对象
        return r;
    }

    // 错误响应（带消息）
    // 参数 msg：错误消息文本
    // 返回：包含状态码和错误消息的 JSON 对象
    static Json::Value error(const std::string &msg) {
        // 创建 JSON 响应对象
        Json::Value r;
        // 设置状态码为 500（内部服务器错误）
        r["code"] = CODE_ERROR;
        // 设置错误消息
        r["msg"] = msg;
        // 返回响应对象
        return r;
    }

    // 错误响应（带自定义状态码和消息）
    // 参数 code：HTTP 状态码（如 400、401、403、404 等）
    // 参数 msg：错误消息文本
    // 返回：包含自定义状态码和错误消息的 JSON 对象
    static Json::Value error(int code, const std::string &msg) {
        // 创建 JSON 响应对象
        Json::Value r;
        // 设置自定义状态码
        r["code"] = code;
        // 设置错误消息
        r["msg"] = msg;
        // 返回响应对象
        return r;
    }
// 类定义结束
};

// 成功响应宏（带数据）
// 用法：RESP_OK(cb, data)
// 参数 cb：回调函数
// 参数 data：响应数据（JSON 对象）
#define RESP_OK(cb, data) \
    (cb)(drogon::HttpResponse::newHttpJsonResponse(AjaxResult::success(data)))

// 成功响应宏（带消息）
// 用法：RESP_MSG(cb, "消息")
// 参数 cb：回调函数
// 参数 msg：消息文本
#define RESP_MSG(cb, msg) \
    (cb)(drogon::HttpResponse::newHttpJsonResponse(AjaxResult::success(std::string(msg))))

// 错误响应宏（带消息）
// 用法：RESP_ERR(cb, "错误信息")
// 参数 cb：回调函数
// 参数 msg：错误消息文本
#define RESP_ERR(cb, msg) \
    (cb)(drogon::HttpResponse::newHttpJsonResponse(AjaxResult::error(std::string(msg))))

// 401 未授权响应宏
// 用法：RESP_401(cb)
// 参数 cb：回调函数
#define RESP_401(cb) \
    (cb)(drogon::HttpResponse::newHttpJsonResponse(AjaxResult::error(401, "未授权，请先登录")))

// JSON 响应宏（直接返回 JSON 对象）
// 用法：RESP_JSON(cb, json)
// 参数 cb：回调函数
// 参数 json：JSON 对象
#define RESP_JSON(cb, json) \
    (cb)(drogon::HttpResponse::newHttpJsonResponse(json))
