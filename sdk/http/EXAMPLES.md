# WePay HTTP 调用示例

不使用 SDK，直接通过 HTTP 请求调用 WePay API 的完整示例。

## 📁 文件清单

| 文件 | 说明 | 运行方式 |
|------|------|--------|
| `curl_example.sh` | Bash/curl 示例 | `bash curl_example.sh` |
| `nodejs_example.js` | Node.js 示例 | `node nodejs_example.js` |
| `python_example.py` | Python requests 示例 | `python python_example.py` |
| `browser_example.html` | 浏览器示例 | 用浏览器打开 |

## 🚀 快速开始

### 1. Bash/curl 示例

**前置条件：**
- bash
- curl
- jq (可选，用于格式化 JSON)

**运行：**

```bash
bash curl_example.sh
```

**功能：**
- ✅ MD5 签名下单
- ✅ 查询订单
- ✅ 异步通知验证
- ✅ 不同支付方式测试

### 2. Node.js 示例

**前置条件：**
- Node.js 12+
- npm

**安装依赖：**

```bash
npm install axios
```

**运行：**

```bash
node nodejs_example.js
```

**功能：**
- ✅ MD5 签名下单
- ✅ 查询订单
- ✅ 异步通知验证
- ✅ 不同支付方式测试

### 3. Python 示例

**前置条件：**
- Python 3.6+
- pip

**安装依赖：**

```bash
pip install requests
```

**运行：**

```bash
python python_example.py
```

**功能：**
- ✅ MD5 签名下单
- ✅ 查询订单
- ✅ 异步通知验证
- ✅ 不同支付方式测试
- ✅ 错误处理示例

### 4. 浏览器示例

**前置条件：**
- 现代浏览器（Chrome、Firefox、Safari 等）

**运行：**

1. 用浏览器打开 `browser_example.html`
2. 在 UI 中填写参数
3. 点击按钮执行操作

**功能：**
- ✅ 可视化界面
- ✅ MD5 签名下单
- ✅ 查询订单
- ✅ 异步通知验证
- ✅ 批量测试

## 📝 示例输出

### 下单成功

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

### 下单失败

```json
{
  "code": -1,
  "msg": "参数不完整"
}
```

## 🔐 签名说明

### MD5 签名步骤

1. **排序参数**
   ```
   amount, mch_id, out_trade_no, pay_type, subject
   ```

2. **构造签名字符串**
   ```
   amount=0.01&mch_id=M515637&out_trade_no=order_123&pay_type=wxpay&subject=测试
   ```

3. **追加商户密钥**
   ```
   amount=0.01&mch_id=M515637&out_trade_no=order_123&pay_type=wxpay&subject=测试nca3twhvpqveixu2hdeutb6utpiet6k7
   ```

4. **计算 MD5**
   ```
   sign = MD5(上述字符串)
   ```

## 📊 支持的支付方式

- `wxpay` - 微信支付
- `alipay` - 支付宝
- `qqpay` - QQ 钱包

## 🧪 测试场景

### 场景 1：基本下单

```bash
# Bash
bash curl_example.sh

# Node.js
node nodejs_example.js

# Python
python python_example.py
```

### 场景 2：浏览器测试

1. 打开 `browser_example.html`
2. 修改订单参数
3. 点击"统一下单"按钮
4. 查看响应结果

### 场景 3：自定义请求

**curl 命令：**

```bash
curl -X POST http://127.0.0.1:8088/gateway/create \
  -H "Content-Type: application/json" \
  -d '{
    "mch_id": "M515637",
    "out_trade_no": "order_123",
    "pay_type": "wxpay",
    "amount": "0.01",
    "subject": "测试",
    "sign_type": "MD5",
    "sign": "计算出的MD5签名"
  }'
```

## 🔧 常见问题

### Q: 如何修改商户号或密钥？

A: 编辑脚本中的 `MCH_ID` 和 `MCH_KEY` 变量。

### Q: 如何使用 RSA 签名？

A: 修改 `sign_type` 为 `RSA`，并计算 RSA-SHA256 签名。

### Q: 如何处理 CORS 错误？

A: 浏览器示例需要后端支持 CORS。如果出现 CORS 错误，可以：
1. 使用 curl 或 Node.js 示例
2. 配置后端 CORS 头
3. 使用代理服务器

### Q: 如何验证异步通知？

A: 使用相同的签名方式验证通知中的 `sign` 字段。

## 📚 更多信息

- 完整 API 文档：`README.md`
- SDK 文档：`../README.md`
- C++ SDK 文档：`../cpp/README.md`

## 🎯 下一步

1. 选择适合的示例运行
2. 修改参数进行测试
3. 集成到自己的项目中
4. 处理异步通知

## 📞 技术支持

如有问题，请联系技术支持团队。
