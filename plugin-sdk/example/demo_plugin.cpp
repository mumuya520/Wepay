#ifdef __has_include
#  if __has_include("IPlugin.h")
#    include "IPlugin.h"
#  else
#    include "../IPlugin.h"
#  endif
#else
#  include "../IPlugin.h"
#endif

// ── 示例插件 ──────────────────────────────────────────────────────
// 编译后放入 plugins/demo/demo.dll(.so)，在管理后台导入 demo.zip 即可使用
// 接口地址: /plugin/demo/hello  /plugin/demo/ping

class DemoPlugin : public IPlugin {
public:
    PluginMeta meta() const override {
        return {
            "demo",           // name（必须与目录名和 plugin.json 一致）
            "1.0.0",          // version
            "WePay Team",     // author
            "示例插件"         // description
        };
    }

    void onLoad() override {
        // 初始化资源，例如读取配置文件、建立连接等
        // 注意：不要在这里注册 Drogon 路由
    }

    void onUnload() override {
        // 清理资源
    }

    // 所有 /plugin/demo/{subPath} 的请求都转发到这里
    void handle(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& subPath) override
    {
        if (subPath == "hello" || subPath == "hello/") {
            Json::Value result;
            result["code"] = 0;
            result["msg"]  = "Hello from DemoPlugin!";
            result["path"] = subPath;
            cb(drogon::HttpResponse::newHttpJsonResponse(result));
            return;
        }

        if (subPath == "ping") {
            Json::Value result;
            result["code"] = 0;
            result["msg"]  = "pong";
            cb(drogon::HttpResponse::newHttpJsonResponse(result));
            return;
        }

        // 404 — 子路径不存在
        Json::Value err;
        err["code"] = 404;
        err["msg"]  = "Not found: " + subPath;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k404NotFound);
        cb(resp);
    }

    // 可选：监听支付成功事件
    void onPaySuccess(const std::string& orderId,
                      const Json::Value& data) override {
        // 例如：发送通知、更新第三方系统
    }
};

// 必须导出这两个函数
PLUGIN_API IPlugin* createPlugin()        { return new DemoPlugin(); }
PLUGIN_API void destroyPlugin(IPlugin* p) { delete p; }
