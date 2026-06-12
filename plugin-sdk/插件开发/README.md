# WePay 插件开发指南

## 快速开始

### 1. 准备文件

```
my_plugin/
├── IPlugin.h            ← 从 SDK 复制，不要修改
├── my_plugin_plugin.cpp ← 你的插件代码
├── plugin.json          ← 插件元信息
└── CMakeLists.txt       ← 修改 PLUGIN_NAME 后直接用
```

### 2. 修改 plugin.json

```json
{
    "name": "my_plugin",
    "version": "1.0.0",
    "author": "你的名字",
    "description": "插件功能描述",
    "entry": "my_plugin"
}
```

> `name` 必须是英文，与目录名、编译产物名一致。

### 3. 实现插件

```cpp
#include "IPlugin.h"

class MyPlugin : public IPlugin {
public:
    PluginMeta meta() const override {
        return {"my_plugin", "1.0.0", "作者", "描述"};
    }

    void onLoad() override {
        // 初始化（不要注册路由）
    }

    void onUnload() override {
        // 清理资源
    }

    // 所有 /plugin/my_plugin/{subPath} 请求都在这里处理
    void handle(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& subPath) override {
        Json::Value result;
        result["code"] = 0;
        result["msg"]  = "ok";
        cb(drogon::HttpResponse::newHttpJsonResponse(result));
    }
};

PLUGIN_API IPlugin* createPlugin()        { return new MyPlugin(); }
PLUGIN_API void destroyPlugin(IPlugin* p) { delete p; }
```

### 4. 编译

**Linux:**
```bash
mkdir build && cd build
cmake .. -DPLUGIN_NAME=my_plugin
make
make package   # 生成 my_plugin.zip
```

**Windows (Visual Studio):**
```powershell
mkdir build; cd build
cmake .. -DPLUGIN_NAME=my_plugin
cmake --build . --config Release
# 手动将 my_plugin.dll + plugin.json 打包为 my_plugin.zip
```

### 5. 导入

在 WePay 管理后台 → **插件管理** → **导入插件**，上传 `my_plugin.zip`，立即生效。

---

## 接口路由规则

上传后你的插件接口自动可用：

```
/plugin/{name}/{任意子路径}
```

例如 `name=alipay` 时：
- `GET  /plugin/alipay/pay`
- `POST /plugin/alipay/notify`
- `GET  /plugin/alipay/query?orderId=xxx`

在 `handle()` 里通过 `subPath` 参数区分路径。

---

## 监听支付事件（可选）

```cpp
void onPaySuccess(const std::string& orderId,
                  const Json::Value& data) override {
    // 订单支付成功时触发
    // 可用于发送通知、对接第三方系统等
}
```

---

## zip 打包结构

```
my_plugin.zip
└── my_plugin/
    ├── my_plugin.dll   (Windows)
    ├── my_plugin.so    (Linux)
    └── plugin.json
```

> zip 内必须有与插件同名的子目录。
