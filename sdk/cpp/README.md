# WePay C++ SDK

WePay 支付网关的 C++ SDK，支持 MD5 和 RSA-SHA256 两种签名方式。

## 依赖

- C++17 或更高版本
- OpenSSL（用于 MD5、RSA 签名）
- libcurl（用于 HTTP 请求）
- nlohmann/json（用于 JSON 处理）

## 安装依赖

### Windows (VCPKG)

```bash
vcpkg install openssl:x64-windows
vcpkg install curl:x64-windows
vcpkg install nlohmann-json:x64-windows
```

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install libssl-dev libcurl4-openssl-dev nlohmann-json3-dev
```

### macOS (Homebrew)

```bash
brew install openssl curl nlohmann-json
```

## 编译

### 使用 CMake

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### 编译后运行示例

```bash
./wepay_example
```

## 使用方法

### 基本使用

```cpp
#include "WePay.h"
#include <iostream>

int main() {
    // 初始化 SDK（MD5 签名）
    WePay wepay(
        "M515637",                          // 商户号
        "nca3twhvpqveixu2hdeutb6utpiet6k7", // 商户密钥
        "http://127.0.0.1:8088",            // API 基础 URL
        "MD5"                               // 签名方式
    );

    // 创建订单
    std::map<std::string, std::string> params;
    params["out_trade_no"] = "order_cpp_123";
    params["pay_type"] = "wxpay";
    params["amount"] = "0.01";
    params["subject"] = "测试订单";

    try {
        auto result = wepay.createOrder(params);
        std::cout << "下单结果: " << result.dump(2) << std::endl;
    } catch (const std::exception& e) {
        std::cout << "错误: " << e.what() << std::endl;
    }

    return 0;
}
```

### RSA 签名

```cpp
std::string privateKeyPem = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDM/CnqlQujQq7y
...
-----END PRIVATE KEY-----)";

WePay wepay(
    "M515637",
    "nca3twhvpqveixu2hdeutb6utpiet6k7",
    "http://127.0.0.1:8088",
    "RSA",
    privateKeyPem
);

auto result = wepay.createOrder(params);
```

### 查询订单

```cpp
auto result = wepay.queryOrder("order_cpp_123");
std::cout << "查询结果: " << result.dump(2) << std::endl;
```

### 申请退款

```cpp
auto result = wepay.refund("W20260530152204174895", "0.01");
std::cout << "退款结果: " << result.dump(2) << std::endl;
```

### 验证异步通知

```cpp
std::map<std::string, std::string> notifyData;
notifyData["trade_no"] = "W20260530152204174895";
notifyData["out_trade_no"] = "order_cpp_123";
notifyData["amount"] = "0.01";
notifyData["status"] = "1";

std::string sign = "从通知中获取的签名";

if (wepay.verifyNotify(notifyData, sign)) {
    std::cout << "签名验证通过" << std::endl;
} else {
    std::cout << "签名验证失败" << std::endl;
}
```

## API 文档

### 类：WePay

#### 构造函数

```cpp
WePay(const std::string& mchId, 
      const std::string& mchKey, 
      const std::string& baseUrl = "http://127.0.0.1:8088",
      const std::string& signType = "MD5", 
      const std::string& privateKeyPem = "")
```

#### 方法

##### sign()

生成签名

```cpp
std::string sign(std::map<std::string, std::string>& params)
```

##### createOrder()

统一下单

```cpp
json createOrder(std::map<std::string, std::string> params)
```

##### queryOrder()

查询订单

```cpp
json queryOrder(const std::string& outTradeNo)
```

##### refund()

申请退款

```cpp
json refund(const std::string& tradeNo, const std::string& refundAmount)
```

##### verifyNotify()

验证异步通知签名

```cpp
bool verifyNotify(std::map<std::string, std::string>& params, 
                  const std::string& sign)
```

## 示例

完整的示例代码见 `example.cpp`

运行示例：

```bash
./wepay_example
```

## 错误处理

SDK 使用 C++ 异常处理错误：

```cpp
try {
    auto result = wepay.createOrder(params);
} catch (const std::exception& e) {
    std::cerr << "错误: " << e.what() << std::endl;
}
```

## 常见问题

### Q: 如何集成到我的项目中？

A: 只需要包含 `WePay.h` 头文件，并链接 OpenSSL 和 libcurl 库。

### Q: 支持哪些编译器？

A: 支持任何支持 C++17 的编译器（MSVC、GCC、Clang）。

### Q: 如何处理 JSON 响应？

A: 使用 nlohmann/json 库的 API，例如：
```cpp
auto result = wepay.createOrder(params);
int code = result["code"];
std::string msg = result["msg"];
std::string tradeNo = result["trade_no"];
```

## 许可证

MIT License
