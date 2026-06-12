#include "IPlugin.h"

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
    void handle(const PluginRequest& req, PluginResponse& resp) override {
        if (req.subPath == "hello") {
            resp.body = R"({"code":0,"msg":"Hello from DemoPlugin!"})";
            return;
        }
        if (req.subPath == "ping") {
            resp.body = R"({"code":0,"msg":"pong"})";
            return;
        }
        // 404
        resp.statusCode = 404;
        resp.body = "{\"code\":404,\"msg\":\"Not found: " + req.subPath + "\"}";
    }

    // 可选：监听支付成功事件
    void onPaySuccess(const PayEvent& event) override {
        // event.orderId, event.amount 等
    }
};

// 必须导出这两个函数
PLUGIN_API IPlugin* createPlugin()        { return new DemoPlugin(); }
PLUGIN_API void destroyPlugin(IPlugin* p) { delete p; }
