#pragma once // 防止头文件重复包含
#include <string> // 标准字符串库
#include <map> // 标准映射容器库
#include <vector> // 标准向量容器库

// ── WePay 插件 SDK ────────────────────────────────────────────────
// 插件开发者只需包含此头文件，无需任何外部库（不依赖 Drogon / JsonCpp）
// 所有 HTTP 请求通过 handle() 转发

#ifdef _WIN32 // 如果是 Windows 平台
#  define PLUGIN_API extern "C" __declspec(dllexport) // 定义 Windows DLL 导出宏
#else // 否则是 Linux/Unix 平台
#  define PLUGIN_API extern "C" __attribute__((visibility("default"))) // 定义 Linux 共享库导出宏
#endif

// 插件元信息结构体
struct PluginMeta {
    std::string name; // 插件唯一标识（英文，与目录名一致）
    std::string version; // 版本号（如 1.0.0）
    std::string author; // 作者名称
    std::string description; // 插件功能描述
};

// HTTP 请求上下文结构体（无需 Drogon 头文件）
struct PluginRequest {
    std::string method; // HTTP 方法（GET POST PUT DELETE）
    std::string subPath; // /plugin/{name}/ 后面的剩余路径
    std::string body; // 请求体内容（JSON 字符串格式）
    std::string clientIp; // 客户端 IP 地址
    std::map<std::string, std::string> headers; // HTTP 请求头映射
    std::map<std::string, std::string> params; // URL 查询参数映射
};

// HTTP 响应结构体
struct PluginResponse {
    int statusCode = 200; // HTTP 状态码（默认 200 成功）
    std::string body; // 响应体内容（一般为 JSON 字符串）
    std::string contentType = "application/json; charset=utf-8"; // 响应内容类型
    std::map<std::string, std::string> headers; // 响应头映射
};

// 支付事件数据结构体
struct PayEvent {
    std::string orderId; // 订单 ID
    std::string amount; // 支付金额
    std::string currency; // 货币代码（如 CNY）
    std::string extra; // 额外数据（JSON 字符串，含其他字段）
};

// 插件接口基类
class IPlugin {
public:
    virtual ~IPlugin() = default; // 虚析构函数

    // 返回插件元信息
    virtual PluginMeta meta() const = 0;

    // 插件加载时调用（初始化资源）
    virtual void onLoad() = 0;

    // 插件卸载时调用（清理资源）
    virtual void onUnload() = 0;

    // HTTP 请求处理（所有 /plugin/{name}/{subPath} 请求都路由到这里）
    virtual void handle(const PluginRequest& req, PluginResponse& resp) = 0;

    // 可选：支付成功回调钩子（子类可重写此方法）
    virtual void onPaySuccess(const PayEvent& /*event*/) {}
};

// 每个插件必须导出这两个函数
// PLUGIN_API IPlugin* createPlugin(); // 创建插件实例
// PLUGIN_API void destroyPlugin(IPlugin* p); // 销毁插件实例
