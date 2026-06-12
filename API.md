# WePay-Cpp 第三方支付系统 API 文档

## 路由体系总览

| 前缀 | 用途 | 鉴权方式 |
|------|------|----------|
| `/admin/api/` | 管理员后台接口 | JWT (AdminAuthFilter) |
| `/merchant/api/` | 商户后台接口 | JWT (MerchantAuthFilter) |
| `/gateway/` | 统一支付网关(下单/查询/退款/关闭) | 商户密钥签名 |
| `/notify/` | 第三方异步回调接收地址 | 通道验签 |
| `/device/` | 自建设备上报(OCR/安卓监听/挂机) | 设备密钥 |

---

## 1. 管理员后台 `/admin/api/`

### 1.1 登录
```
POST /admin/api/auth/login
Body: { "username": "admin", "password": "admin" }
Response: { "code": 200, "data": { "token": "...", "username": "admin" } }
```

### 1.2 商户管理
```
GET    /admin/api/merchant/list?page=1&size=20&search=xxx   分页列表
GET    /admin/api/merchant/detail?id=1                       详情
POST   /admin/api/merchant/add         新增 { username, password, mch_name, rate, ... }
POST   /admin/api/merchant/edit        编辑 { id, mch_name, rate, password(可选), ... }
POST   /admin/api/merchant/state       启用/禁用 { id, state: 0|1 }
POST   /admin/api/merchant/resetKey    重置密钥 { id }
DELETE /admin/api/merchant/del?id=1    删除
```

### 1.3 通道管理
```
GET    /admin/api/channel/list                 通道列表
POST   /admin/api/channel/add                  新增 { channel_code, channel_name, pay_type, plugin, rate, params_json }
POST   /admin/api/channel/edit                 编辑
POST   /admin/api/channel/state                启用/禁用 { id, state }
DELETE /admin/api/channel/del?id=1             删除
GET    /admin/api/channel/bindList?mch_id=1    商户绑定的通道列表
POST   /admin/api/channel/bind                 绑定 { mch_id, channel_id, rate }
POST   /admin/api/channel/unbind               解绑 { mch_id, channel_id }
GET    /admin/api/paytype/list                 支付类型列表
```

### 1.4 订单管理
```
GET    /admin/api/order/list?page=1&limit=10&state=1&type=wxpay   订单列表
GET    /admin/api/order/detail/{id}        详情
POST   /admin/api/order/close/{id}         关闭
POST   /admin/api/order/supplement/{id}    补单(标记已支付+回调)
POST   /admin/api/order/reissue/{id}       重发回调
DELETE /admin/api/order/{id}               删除
DELETE /admin/api/order/batch              批量删除
```

### 1.5 结算管理
```
GET  /admin/api/settle/list?page=1&state=0&mch_id=1   结算列表
POST /admin/api/settle/approve    审核通过 { id, remark }
POST /admin/api/settle/reject     驳回 { id, remark }
```

### 1.6 资金管理
```
GET  /admin/api/money/logs?page=1&mch_id=1   资金日志
POST /admin/api/money/manual                  手动调账 { mch_id, change_type, amount, remark }
```
change_type: 1=收入 2=支出 3=冻结 4=解冻

### 1.7 通知管理
```
GET  /admin/api/notify/list?page=1&status=0   通知任务列表
GET  /admin/api/notify/logs/{orderId}          通知日志
POST /admin/api/notify/retry/{orderId}         重试
```

### 1.8 系统配置
```
GET  /admin/api/config/get            获取配置
POST /admin/api/config/save           保存配置
GET  /admin/api/config/status         系统状态
POST /admin/api/config/resetKey       重置API密钥
POST /admin/api/user/changePassword   修改密码
GET  /admin/api/dashboard             数据面板
```

---

## 2. 商户后台 `/merchant/api/`

### 2.1 登录
```
POST /merchant/api/auth/login
Body: { "username": "mch001", "password": "xxx" }
Response: { "data": { "token": "...", "mch_no": "M123456", "mch_name": "..." } }
```

### 2.2 个人信息
```
GET  /merchant/api/info               商户信息(余额/费率/密钥等)
POST /merchant/api/changePwd          修改密码 { old_password, new_password }
POST /merchant/api/resetKey           重置通讯密钥
```

### 2.3 订单
```
GET /merchant/api/order/list?page=1&size=20&state=1&pay_type=wxpay  我的订单
GET /merchant/api/order/detail?order_id=W20260503...                  订单详情
```

### 2.4 结算
```
POST /merchant/api/settle/apply       申请结算 { "amount": "100.00" }
GET  /merchant/api/settle/list        结算记录
```

### 2.5 数据统计
```
GET /merchant/api/dashboard           面板(余额/今日/总计/待结算)
GET /merchant/api/money/logs          资金日志
```

---

## 3. 统一支付网关 `/gateway/`

### 3.1 统一下单
```
POST /gateway/create
参数:
  mch_id         商户ID或商户号(必填)
  out_trade_no   商户订单号(必填)
  pay_type       支付类型: wxpay/alipay/qqpay(必填)
  amount         金额(必填)
  subject        商品名称
  notify_url     异步通知地址
  return_url     同步跳转地址
  body           商品描述
  param          透传参数
  sign           签名(必填)

返回:
  { "code": 1, "trade_no": "W...", "cashier_url": "/gateway/cashier/W...", ... }
```

### 3.2 查询订单
```
POST /gateway/query
参数: mch_id, trade_no 或 out_trade_no, sign
返回: { "code": 1, "status": 0|1|-1, "amount": "10.00", ... }
```

### 3.3 关闭订单
```
POST /gateway/close
参数: mch_id, trade_no 或 out_trade_no, sign
```

### 3.4 申请退款
```
POST /gateway/refund
参数: mch_id, trade_no 或 out_trade_no, refund_amount, reason, sign
返回: { "code": 1, "refund_no": "R...", "refund_amount": "10.00" }
```

### 3.5 收银台
```
GET /gateway/cashier/{orderId}    HTML 收银台页面(支持多支付方式选择)
```

### 3.6 易支付兼容
```
GET/POST /submit.php   跳转支付 → 重定向收银台
POST     /mapi.php     API支付 → 返回JSON
GET      /api.php?act=order&pid=1&out_trade_no=xxx   查询
```

### 签名算法
所有参数按 key 升序排列，拼接为 `key=value&key=value`（跳过 sign/sign_type/空值），
后追加 `&key={mch_key}` 做 MD5。

---

## 4. 第三方异步回调 `/notify/`

```
POST/GET /notify/channel/{channelCode}   支付成功回调(各上游通道调用)
POST     /notify/refund/{channelCode}    退款回调
```
系统返回 `success` 表示已处理。

---

## 5. 设备上报 `/device/`

### 5.1 新版接口
```
POST /device/heart       心跳 { device_no, t, sign }
POST /device/push        收款推送 { device_no, pay_type, amount, sign }
POST /device/register    注册 { device_no, device_name, device_type }
GET  /device/orders/{no} 拉取待支付订单
```
device_type: 1=安卓监听 2=PC挂机 3=OCR 4=云端

### 5.2 V免签兼容
```
POST /appHeart   心跳 { t, sign }
POST /appPush    推送 { t, type, price, sign }
```

### 5.3 码支付兼容
```
GET  /checkOrder/{pid}/{sign}    轮询待支付订单
GET  /checkPayResult?price=&type=  提交收款
POST /mpayNotify                   SmsForwarder 推送
```

---

## 数据库表结构

| 表名 | 说明 |
|------|------|
| `setting` | KV 配置 |
| `pay_type` | 支付类型(wxpay/alipay/...) |
| `merchant` | 商户(多商户体系) |
| `merchant_app` | 商户应用 |
| `pay_channel` | 支付通道(插件化) |
| `pay_channel_account` | 通道账户(轮询) |
| `merchant_channel` | 商户-通道绑定 |
| `pay_order` | 支付订单 |
| `refund_order` | 退款订单 |
| `settle_order` | 结算订单 |
| `money_log` | 资金日志 |
| `device` | 设备 |
| `pay_qrcode` | 收款码(兼容) |
| `tmp_price` | 临时金额锁 |
| `pay_notify_task` | 通知队列 |
| `pay_callback_log` | 回调日志 |
| `oplog` | 操作日志 |
| `schema_version` | 迁移版本 |
