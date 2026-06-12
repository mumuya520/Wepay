# 变更日志

本文档记录 WePay-Cpp 项目的所有重要变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### 计划中
- 支持更多支付通道
- 优化数据库查询性能
- 添加单元测试
- 完善文档

## [1.0.0] - 2026-06-12

### 新增
- 多商户管理系统
- 支付通道插件化架构
- 统一支付网关接口
- 订单管理系统
- 结算管理系统
- 资金管理系统
- JWT 认证
- API 签名验证
- 异步回调通知
- 操作日志记录
- 数据统计面板
- SQLite / PostgreSQL 双数据库支持
- Redis 缓存支持（可选）
- RabbitMQ 消息队列支持（可选）
- Docker 部署支持
- Windows / Linux 跨平台支持
- 插件 SDK
- 客户端 SDK（C++ / HTTP）
- 完整的 API 文档
- 商户接入文档

### 支付通道
- 支付宝 (Alipay)
- 微信支付 (WeChat Pay)
- QQ钱包
- 自建设备 (OCR/安卓监听/挂机)
- GoPay 集成
- UPay 集成
- Alipay-Lib 集成

### 安全
- 数据库加密存储
- 密码哈希存储
- API 密钥管理
- 请求签名验证
- CORS 跨域配置
- 限流保护

### 文档
- README.md
- API.md
- 接入文档.md
- 接口实现总结.md
- 接口对比分析.md
- CONTRIBUTING.md
- LICENSE

---

## 版本说明

### 版本号格式
- 主版本号：不兼容的 API 修改
- 次版本号：向下兼容的功能性新增
- 修订号：向下兼容的问题修正

### 变更类型
- **新增**: 新功能
- **变更**: 现有功能的变更
- **弃用**: 即将移除的功能
- **移除**: 已移除的功能
- **修复**: Bug 修复
- **安全**: 安全相关的修复
