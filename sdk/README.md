# WePay 多语言 SDK

WePay 支付网关的官方 SDK，支持 PHP、Python、Java、Go、C# 等多种编程语言。

## 功能特性

- ✅ 支持 MD5 和 RSA-SHA256 两种签名方式
- ✅ 统一下单接口
- ✅ 订单查询接口
- ✅ 退款申请接口
- ✅ 异步通知验证
- ✅ 完整的错误处理

## 快速开始

### PHP SDK

```php
require_once 'sdk/php/WePay.php';

$wepay = new WePay(
    'M515637',                          // 商户号
    'nca3twhvpqveixu2hdeutb6utpiet6k7', // 商户密钥
    'http://127.0.0.1:8088',            // API 基础 URL
    'MD5'                               // 签名方式
);

// 创建订单
$result = $wepay->createOrder([
    'out_trade_no' => 'order_' . time(),
    'pay_type' => 'wxpay',
    'amount' => '0.01',
    'subject' => '测试订单'
]);

echo json_encode($result);
```

### Python SDK

```python
from sdk.python.wepay import WePay

wepay = WePay(
    'M515637',                          # 商户号
    'nca3twhvpqveixu2hdeutb6utpiet6k7', # 商户密钥
    'http://127.0.0.1:8088',            # API 基础 URL
    'MD5'                               # 签名方式
)

# 创建订单
result = wepay.create_order({
    'out_trade_no': 'order_test_123',
    'pay_type': 'wxpay',
    'amount': '0.01',
    'subject': '测试订单'
})

print(result)
```

### Java SDK

```java
import com.wepay.WePay;

WePay wepay = new WePay(
    "M515637",
    "nca3twhvpqveixu2hdeutb6utpiet6k7",
    "http://127.0.0.1:8088",
    "MD5",
    ""
);

Map<String, String> params = new HashMap<>();
params.put("out_trade_no", "order_java_" + System.currentTimeMillis());
params.put("pay_type", "wxpay");
params.put("amount", "0.01");
params.put("subject", "测试订单");

JsonObject result = wepay.createOrder(params);
System.out.println(result);
```

### Go SDK

```go
package main

import (
    "fmt"
    "wepay"
)

func main() {
    client, _ := wepay.New(
        "M515637",
        "nca3twhvpqveixu2hdeutb6utpiet6k7",
        "http://127.0.0.1:8088",
        "MD5",
        "",
    )

    result, _ := client.CreateOrder(map[string]string{
        "out_trade_no": "order_go_123",
        "pay_type":     "wxpay",
        "amount":       "0.01",
        "subject":      "测试订单",
    })

    fmt.Println(result)
}
```

### C# SDK

```csharp
using WePay;

var wepay = new WePayClient(
    "M515637",
    "nca3twhvpqveixu2hdeutb6utpiet6k7",
    "http://127.0.0.1:8088",
    "MD5"
);

var result = await wepay.CreateOrderAsync(new Dictionary<string, string>
{
    { "out_trade_no", "order_csharp_" + DateTimeOffset.Now.ToUnixTimeMilliseconds() },
    { "pay_type", "wxpay" },
    { "amount", "0.01" },
    { "subject", "测试订单" }
});

Console.WriteLine(JsonConvert.SerializeObject(result));
```

## API 接口

### 统一下单

**请求参数：**

| 参数 | 类型 | 必需 | 说明 |
|------|------|------|------|
| mch_id | string | ✓ | 商户号 |
| out_trade_no | string | ✓ | 商户订单号 |
| pay_type | string | ✓ | 支付方式 (wxpay/alipay/qqpay) |
| amount | string | ✓ | 订单金额 |
| subject | string | ✓ | 订单标题 |
| notify_url | string | | 异步通知 URL |
| return_url | string | | 同步返回 URL |
| sign_type | string | | 签名方式 (MD5/RSA) |
| sign | string | ✓ | 签名值 |

**响应示例：**

```json
{
  "code": 1,
  "msg": "订单创建成功",
  "trade_no": "W20260530152204174895",
  "out_trade_no": "order_123",
  "pay_url": "/gateway/cashier/W20260530152204174895",
  "cashier_url": "/gateway/cashier/W20260530152204174895",
  "amount": "0.01",
  "pay_type": "wxpay"
}
```

### 查询订单

**请求参数：**

| 参数 | 类型 | 必需 | 说明 |
|------|------|------|------|
| mch_id | string | ✓ | 商户号 |
| out_trade_no | string | ✓ | 商户订单号 |
| sign_type | string | | 签名方式 |
| sign | string | ✓ | 签名值 |

### 申请退款

**请求参数：**

| 参数 | 类型 | 必需 | 说明 |
|------|------|------|------|
| mch_id | string | ✓ | 商户号 |
| trade_no | string | ✓ | 平台订单号 |
| refund_amount | string | ✓ | 退款金额 |
| sign_type | string | | 签名方式 |
| sign | string | ✓ | 签名值 |

## 签名方式

### MD5 签名

1. 对参数进行排序（按 key 字母顺序）
2. 排除 `sign` 和 `sign_type` 字段
3. 排除空值字段
4. 拼接为 `key=value&key=value` 格式
5. 追加商户密钥
6. 计算 MD5 哈希值

### RSA-SHA256 签名

1. 对参数进行排序（按 key 字母顺序）
2. 排除 `sign` 和 `sign_type` 字段
3. 排除空值字段
4. 拼接为 `key=value&key=value` 格式
5. 用 RSA 私钥进行 SHA256 签名
6. Base64 编码签名结果

## 异步通知

当订单状态变化时，WePay 会向商户的 `notify_url` 发送异步通知。

**通知参数：**

```json
{
  "trade_no": "W20260530152204174895",
  "out_trade_no": "order_123",
  "amount": "0.01",
  "status": "1",
  "pay_type": "wxpay",
  "sign": "xxx"
}
```

**验证通知：**

```php
$wepay = new WePay(...);
$params = $_POST; // 获取通知参数
$sign = $params['sign'];
unset($params['sign']);

if ($wepay->verifyNotify($params, $sign)) {
    // 签名验证通过，处理订单
    echo "success";
} else {
    // 签名验证失败
    echo "fail";
}
```

## 错误处理

所有 SDK 都提供了完整的错误处理机制。建议在生产环境中使用 try-catch 捕获异常。

```php
try {
    $result = $wepay->createOrder($params);
} catch (Exception $e) {
    echo "错误: " . $e->getMessage();
}
```

## 常见问题

### Q: 如何获取 RSA 私钥？

A: 在商户后台生成 RSA 密钥对时，系统会自动下载私钥文件。

### Q: 如何切换签名方式？

A: 在初始化 SDK 时指定 `signType` 参数即可。

### Q: 如何验证异步通知？

A: 使用 SDK 提供的 `verifyNotify` 方法验证签名。

## 支持

如有问题，请联系技术支持。

## 许可证

MIT License
