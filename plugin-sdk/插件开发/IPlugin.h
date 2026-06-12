#pragma once
#include <string>
#include <map>
#include <vector>

// ── WePay 插件 SDK ────────────────────────────────────────────────
// 插件开发者只需包含此头文件，无需任何外部库（不依赖 Drogon / JsonCpp）
// 所有 HTTP 请求通过 handle() 转发

#ifdef _WIN32
#  define PLUGIN_API extern "C" __declspec(dllexport)
#else
#  define PLUGIN_API extern "C" __attribute__((visibility("default")))
#endif

// 插件元信息
struct PluginMeta {
    std::string name;         // 插件唯一标识（英文，与目录名一致）
    std::string version;      // 版本号
    std::string author;       // 作者
    std::string description;  // 描述
};

// HTTP 请求上下文（无需 Drogon 头文件）
struct PluginRequest {
    std::string method;       // GET POST PUT DELETE
    std::string subPath;      // /plugin/{name}/ 后面的剩余路径
    std::string body;         // 请求体（JSON 字符串）
    std::string clientIp;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> params;   // URL 查询参数
};

// HTTP 响应
struct PluginResponse {
    int         statusCode = 200;
    std::string body;         // 返回内容（一般为 JSON 字符串）
    std::string contentType = "application/json; charset=utf-8";
    std::map<std::string, std::string> headers;
};

// 支付事件数据
struct PayEvent {
    std::string orderId;
    std::string amount;
    std::string currency;
    std::string extra;        // JSON 字符串，含其他字段
};

class IPlugin {
public:
    virtual ~IPlugin() = default;

    // 返回插件元信息
    virtual PluginMeta meta() const = 0;

    // 插件加载时调用（初始化资源）
    virtual void onLoad() = 0;

    // 插件卸载时调用（清理资源）
    virtual void onUnload() = 0;

    // HTTP 请求处理（所有 /plugin/{name}/{subPath} 请求都路由到这里）
    virtual void handle(const PluginRequest& req, PluginResponse& resp) = 0;

    // 可选：支付成功回调钩子
    virtual void onPaySuccess(const PayEvent& /*event*/) {}
};

// 每个插件必须导出这两个函数
// PLUGIN_API IPlugin* createPlugin();
// PLUGIN_API void destroyPlugin(IPlugin* p);
