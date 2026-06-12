# WePay HTTP 直接调用示例

不使用 SDK，直接通过 HTTP 请求调用 WePay API。

## 前置条件

- 商户号 (mch_id): M515637
- 商户密钥 (mch_key): nca3twhvpqveixu2hdeutb6utpiet6k7
- API 基础 URL: http://127.0.0.1:8088

## 签名生成

### MD5 签名

1. 对所有参数按 key 字母顺序排序
2. 排除 `sign` 和 `sign_type` 字段
3. 排除空值字段
4. 拼接为 `key=value&key=value` 格式
5. 追加商户密钥
6. 计算 MD5 哈希

**示例：**

```
参数: amount=0.01, mch_id=M515637, out_trade_no=order_123, pay_type=wxpay, subject=测试
排序后: amount=0.01&mch_id=M515637&out_trade_no=order_123&pay_type=wxpay&subject=测试
追加密钥: amount=0.01&mch_id=M515637&out_trade_no=order_123&pay_type=wxpay&subject=测试nca3twhvpqveixu2hdeutb6utpiet6k7
MD5: 签名值
```

### RSA-SHA256 签名

1. 对所有参数按 key 字母顺序排序
2. 排除 `sign` 和 `sign_type` 字段
3. 排除空值字段
4. 拼接为 `key=value&key=value` 格式
5. 用 RSA 私钥进行 SHA256 签名
6. Base64 编码签名结果

## API 接口

### 统一下单

**请求方式：** POST

**请求 URL：** `/gateway/create`

**请求参数：**

```json
{
  "mch_id": "M515637",
  "out_trade_no": "order_123",
  "pay_type": "wxpay",
  "amount": "0.01",
  "subject": "测试订单",
  "sign_type": "MD5",
  "sign": "签名值"
}
```

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

**请求方式：** POST

**请求 URL：** `/gateway/query`

**请求参数：**

```json
{
  "mch_id": "M515637",
  "out_trade_no": "order_123",
  "sign_type": "MD5",
  "sign": "签名值"
}
```

### 申请退款

**请求方式：** POST

**请求 URL：** `/gateway/refund`

**请求参数：**

```json
{
  "mch_id": "M515637",
  "trade_no": "W20260530152204174895",
  "refund_amount": "0.01",
  "sign_type": "MD5",
  "sign": "签名值"
}
```

## 使用示例

### curl 命令

#### MD5 签名下单

```bash
curl -X POST http://127.0.0.1:8088/gateway/create \
  -H "Content-Type: application/json" \
  -d '{
    "mch_id": "M515637",
    "out_trade_no": "order_123",
    "pay_type": "wxpay",
    "amount": "0.01",
    "subject": "测试订单",
    "sign_type": "MD5",
    "sign": "计算出的MD5签名"
  }'
```

#### 查询订单

```bash
curl -X POST http://127.0.0.1:8088/gateway/query \
  -H "Content-Type: application/json" \
  -d '{
    "mch_id": "M515637",
    "out_trade_no": "order_123",
    "sign_type": "MD5",
    "sign": "计算出的MD5签名"
  }'
```

#### 申请退款

```bash
curl -X POST http://127.0.0.1:8088/gateway/refund \
  -H "Content-Type: application/json" \
  -d '{
    "mch_id": "M515637",
    "trade_no": "W20260530152204174895",
    "refund_amount": "0.01",
    "sign_type": "MD5",
    "sign": "计算出的MD5签名"
  }'
```

### JavaScript (Node.js)

```javascript
const crypto = require('crypto');
const https = require('https');

const mchId = 'M515637';
const mchKey = 'nca3twhvpqveixu2hdeutb6utpiet6k7';
const baseUrl = 'http://127.0.0.1:8088';

// MD5 签名
function signMd5(params) {
    const sorted = Object.keys(params)
        .filter(k => k !== 'sign' && k !== 'sign_type' && params[k])
        .sort()
        .map(k => `${k}=${params[k]}`)
        .join('&');
    
    const signStr = sorted + mchKey;
    return crypto.createHash('md5').update(signStr).digest('hex');
}

// 统一下单
async function createOrder() {
    const params = {
        mch_id: mchId,
        out_trade_no: 'order_js_' + Date.now(),
        pay_type: 'wxpay',
        amount: '0.01',
        subject: 'JavaScript 测试订单',
        sign_type: 'MD5'
    };

    params.sign = signMd5(params);

    const options = {
        hostname: '127.0.0.1',
        port: 8088,
        path: '/gateway/create',
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        }
    };

    return new Promise((resolve, reject) => {
        const req = https.request(options, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => resolve(JSON.parse(data)));
        });

        req.on('error', reject);
        req.write(JSON.stringify(params));
        req.end();
    });
}

// 运行示例
createOrder().then(result => {
    console.log('下单结果:', result);
}).catch(err => {
    console.error('错误:', err);
});
```

### JavaScript (浏览器)

```javascript
const mchId = 'M515637';
const mchKey = 'nca3twhvpqveixu2hdeutb6utpiet6k7';
const baseUrl = 'http://127.0.0.1:8088';

// MD5 签名
function signMd5(params) {
    const sorted = Object.keys(params)
        .filter(k => k !== 'sign' && k !== 'sign_type' && params[k])
        .sort()
        .map(k => `${k}=${params[k]}`)
        .join('&');
    
    const signStr = sorted + mchKey;
    return md5(signStr); // 使用 md5.js 库
}

// 统一下单
async function createOrder() {
    const params = {
        mch_id: mchId,
        out_trade_no: 'order_browser_' + Date.now(),
        pay_type: 'wxpay',
        amount: '0.01',
        subject: '浏览器测试订单',
        sign_type: 'MD5'
    };

    params.sign = signMd5(params);

    const response = await fetch(`${baseUrl}/gateway/create`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(params)
    });

    return await response.json();
}

// 运行示例
createOrder().then(result => {
    console.log('下单结果:', result);
}).catch(err => {
    console.error('错误:', err);
});
```

### Python (requests)

```python
import requests
import hashlib
import json
from datetime import datetime

mch_id = 'M515637'
mch_key = 'nca3twhvpqveixu2hdeutb6utpiet6k7'
base_url = 'http://127.0.0.1:8088'

def sign_md5(params):
    sorted_params = sorted([
        (k, v) for k, v in params.items() 
        if k not in ['sign', 'sign_type'] and v
    ])
    sign_str = '&'.join([f'{k}={v}' for k, v in sorted_params])
    sign_str += mch_key
    return hashlib.md5(sign_str.encode()).hexdigest()

def create_order():
    params = {
        'mch_id': mch_id,
        'out_trade_no': f'order_py_{int(datetime.now().timestamp() * 1000)}',
        'pay_type': 'wxpay',
        'amount': '0.01',
        'subject': 'Python 测试订单',
        'sign_type': 'MD5'
    }
    
    params['sign'] = sign_md5(params)
    
    response = requests.post(
        f'{base_url}/gateway/create',
        json=params,
        headers={'Content-Type': 'application/json'}
    )
    
    return response.json()

# 运行示例
result = create_order()
print('下单结果:', json.dumps(result, ensure_ascii=False, indent=2))
```

### Bash (curl + openssl)

```bash
#!/bin/bash

MCH_ID="M515637"
MCH_KEY="nca3twhvpqveixu2hdeutb6utpiet6k7"
BASE_URL="http://127.0.0.1:8088"

# 生成 MD5 签名
sign_md5() {
    local params="$1"
    local sign_str="${params}${MCH_KEY}"
    echo -n "$sign_str" | md5sum | awk '{print $1}'
}

# 统一下单
create_order() {
    local out_trade_no="order_bash_$(date +%s%N)"
    local params="amount=0.01&mch_id=${MCH_ID}&out_trade_no=${out_trade_no}&pay_type=wxpay&subject=Bash测试订单"
    local sign=$(sign_md5 "$params")
    
    local json_data=$(cat <<EOF
{
    "mch_id": "${MCH_ID}",
    "out_trade_no": "${out_trade_no}",
    "pay_type": "wxpay",
    "amount": "0.01",
    "subject": "Bash 测试订单",
    "sign_type": "MD5",
    "sign": "${sign}"
}
EOF
)
    
    curl -X POST "${BASE_URL}/gateway/create" \
        -H "Content-Type: application/json" \
        -d "$json_data"
}

# 运行示例
create_order
```

## 常见问题

### Q: 如何计算 MD5 签名？

A: 参考上面的签名生成说明。

### Q: 如何处理 RSA 签名？

A: 使用 RSA 私钥对签名字符串进行 SHA256 签名，然后 Base64 编码。

### Q: 如何验证异步通知？

A: 使用相同的签名方式验证通知中的签名字段。

## 支持

如有问题，请联系技术支持。
